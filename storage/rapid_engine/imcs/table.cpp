/**
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. Rapid Table.
*/

/**DataTable to mock a table hehaviors. We can use a DataTable to open the IMCS
 * with sepecific table information. After the Cu belongs to this table were found
 * , we can use this DataTable object to read/write, etc., just like a normal innodb
 * table.
 */
#include "storage/rapid_engine/imcs/table.h"

#include <regex>
#include <sstream>
#include <thread>

#include "include/ut0dbg.h"  //ut_a
#include "sql/field.h"       //field
#include "sql/key.h"         //COPY_KEY
#include "sql/table.h"       //TABLE
#include "storage/innobase/include/mach0data.h"

#include "storage/innobase/handler/ha_innodb.h"

#include "storage/rapid_engine/imcs/cu_recovery.h"
#include "storage/rapid_engine/imcs/index/encoder.h"
#include "storage/rapid_engine/imcs/index/key_codec.h"
#include "storage/rapid_engine/include/rapid_const.h"  // INVALID_ROW_ID
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/recovery/recovery.h"
#include "storage/rapid_engine/utils/memory_pool.h"  //Blob
#include "storage/rapid_engine/utils/utils.h"        //Blob
namespace ShannonBase {
// global memory pool
extern std::shared_ptr<ShannonBase::Utils::MemoryPool> shannon_rpd_memory_pool;
namespace Imcs {

namespace {

// Field clones are shared by all compiled index descriptors of a Rapid table.
// Any path that temporarily retargets Field::field_ptr() therefore needs one
// codec-level guard; a mutex stored per key part would not protect the same
// Field when it is referenced by another index.
std::mutex &KeyCodecFieldMutex() {
  static std::mutex mutex;
  return mutex;
}

size_t MaterializeFieldKeyImage(Field *field, const uchar *source, uchar *dst, uint length) {
  if (field == nullptr || source == nullptr || dst == nullptr || length == 0) return 0;

  std::lock_guard<std::mutex> guard(KeyCodecFieldMutex());
  uchar *old_ptr = field->field_ptr();
  field->set_field_ptr(const_cast<uchar *>(source));
  const size_t written = field->get_key_image(dst, length, Field::itRAW);
  field->set_field_ptr(old_ptr);
  return written;
}

}  // namespace

bool Index::RapidKeyCodec::IsCollatedTextField(const Field *field) {
  if (field == nullptr) return false;
  if (field->real_type() == MYSQL_TYPE_ENUM || field->real_type() == MYSQL_TYPE_SET) return false;

  switch (field->type()) {
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
      return true;
    default:
      return false;
  }
}

bool Index::RapidKeyCodec::IsBlobField(const Field *field) {
  if (field == nullptr) return false;
  switch (field->type()) {
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
      return true;
    default:
      return false;
  }
}

uint32 Index::RapidKeyCodec::PrefixCharacters(const Field *field, uint16_t part_length) {
  if (field == nullptr || part_length == 0) return 0;
  const CHARSET_INFO *cs = field->charset();
  const uint mbmaxlen = (cs != nullptr && cs->mbmaxlen != 0) ? cs->mbmaxlen : 1;
  return std::max<uint32>(1, static_cast<uint32>(part_length / mbmaxlen));
}

bool Index::RapidKeyCodec::TransformCollation(const ArtKeyPartDescriptor &part, const uchar *src, size_t src_len,
                                              KeyBuffer *out) {
  if (part.source_field == nullptr || src == nullptr || out == nullptr) return false;
  const CHARSET_INFO *cs = part.source_field->charset();
  if (cs == nullptr || cs->coll == nullptr || cs->coll->strnxfrm == nullptr || part.encoded_capacity == 0) {
    return false;
  }

  // strnxfrm() requires that the source contain at most num_codepoints.  MySQL
  // KEY_PART_INFO::length is a maximum byte length, so trim at a character
  // boundary before asking the collation for the canonical weights.
  const uint32 max_chars = std::max<uint32>(1, part.prefix_characters);
  const uchar *end = src + src_len;
  size_t trimmed_len = static_cast<size_t>(my_charpos(cs, src, end, max_chars));
  trimmed_len = std::min(trimmed_len, src_len);

  size_t capacity = part.encoded_capacity;
  if (capacity & 1U) ++capacity;  // strnxfrm() requires an even destination size.
  out->assign(capacity, 0);
  const size_t written = cs->coll->strnxfrm(cs, out->data(), capacity, max_chars, src, trimmed_len, 0);
  if (written > capacity) return false;
  out->resize(written);
  return true;
}

bool Index::RapidKeyCodec::EncodeSortableValue(const ArtKeyPartDescriptor &part, const uchar *source,
                                               bool db_low_byte_first, bool row_image, KeyBuffer *out) {
  Field *field = part.source_field;
  const uint16_t length = static_cast<uint16_t>(part.payload_length);
  if (field == nullptr || source == nullptr || out == nullptr || length == 0) return false;
  out->assign(length, 0);

  switch (field->type()) {
    case MYSQL_TYPE_TINY:
      if (length != sizeof(uint8_t)) return false;
      if (field->is_unsigned()) {
        Encoder<uint8_t>::Encode(Utils::Util::get_field_numeric<uint8_t>(field, source, nullptr, db_low_byte_first),
                                 out->data());
      } else {
        Encoder<int8_t>::Encode(Utils::Util::get_field_numeric<int8_t>(field, source, nullptr, db_low_byte_first),
                                out->data());
      }
      return true;

    case MYSQL_TYPE_SHORT:
      if (length != sizeof(uint16_t)) return false;
      if (field->is_unsigned()) {
        Encoder<uint16_t>::Encode(Utils::Util::get_field_numeric<uint16_t>(field, source, nullptr, db_low_byte_first),
                                  out->data());
      } else {
        Encoder<int16_t>::Encode(Utils::Util::get_field_numeric<int16_t>(field, source, nullptr, db_low_byte_first),
                                 out->data());
      }
      return true;

    case MYSQL_TYPE_LONG:
      if (length != sizeof(uint32_t)) return false;
      if (field->is_unsigned()) {
        Encoder<uint32_t>::Encode(Utils::Util::get_field_numeric<uint32_t>(field, source, nullptr, db_low_byte_first),
                                  out->data());
      } else {
        Encoder<int32_t>::Encode(Utils::Util::get_field_numeric<int32_t>(field, source, nullptr, db_low_byte_first),
                                 out->data());
      }
      return true;

    case MYSQL_TYPE_LONGLONG:
      if (length != sizeof(uint64_t)) return false;
      if (field->is_unsigned()) {
        Encoder<uint64_t>::Encode(Utils::Util::get_field_numeric<uint64_t>(field, source, nullptr, db_low_byte_first),
                                  out->data());
      } else {
        Encoder<int64_t>::Encode(Utils::Util::get_field_numeric<int64_t>(field, source, nullptr, db_low_byte_first),
                                 out->data());
      }
      return true;

    case MYSQL_TYPE_FLOAT: {
      if (length != sizeof(float)) return false;
      const double value = Utils::Util::get_field_numeric<double>(field, source, nullptr, db_low_byte_first);
      Encoder<float>::Encode(static_cast<float>(value), out->data());
      return true;
    }

    case MYSQL_TYPE_DOUBLE: {
      if (length != sizeof(double)) return false;
      const double value = Utils::Util::get_field_numeric<double>(field, source, nullptr, db_low_byte_first);
      Encoder<double>::Encode(value, out->data());
      return true;
    }

    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_DECIMAL:
      if (!row_image) {
        std::memcpy(out->data(), source, length);
        return true;
      } else {
        // Decimal is the only ordered numeric path that still needs Field to
        // materialize the exact MySQL key image.
        return MaterializeFieldKeyImage(field, source, out->data(), length) == length;
      }

    default:
      return false;
  }
}

bool Index::RapidKeyCodec::CompilePart(ArtKeyPartDescriptor *part, ArtKeyMode *index_mode) {
  if (part == nullptr || index_mode == nullptr || part->source_field == nullptr || part->payload_length == 0 ||
      part->store_length == 0) {
    return false;
  }

  Field *field = part->source_field;
  part->codec = ArtKeyPartCodec::KEY_IMAGE;
  part->prefix_characters = 0;
  part->encoded_capacity = part->store_length - (part->nullable ? 1U : 0U);
  if (part->encoded_capacity == 0) return false;

  if (IsCollatedTextField(field) && field->real_type() != MYSQL_TYPE_JSON) {
    const CHARSET_INFO *cs = field->charset();
    // A raw string key image is not an equality key for non-binary collations
    // (for example 'a' and 'A' may compare equal). Fail closed instead of
    // silently creating an ART that can miss SQL-equal values.
    if (cs == nullptr || cs->coll == nullptr || cs->coll->strnxfrm == nullptr || cs->coll->strnxfrmlen == nullptr) {
      return false;
    }

    part->codec = ArtKeyPartCodec::COLLATION_WEIGHT;
    part->prefix_characters = PrefixCharacters(field, static_cast<uint16_t>(part->payload_length));
    part->encoded_capacity = cs->coll->strnxfrmlen(cs, part->payload_length);
    if (part->encoded_capacity & 1U) ++part->encoded_capacity;
    return part->encoded_capacity != 0;
  }

  const bool full_width = !part->variable_length && part->payload_length == field->pack_length();
  switch (field->type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_DECIMAL:
      if (full_width) {
        part->codec = ArtKeyPartCodec::SORTABLE_VALUE;
        part->encoded_capacity = part->payload_length;
        return true;
      }
      break;
    default:
      break;
  }

  // KEY_IMAGE is the thin exact-only fallback: row-build materializes MySQL's
  // key image and handler lookup consumes the same bytes. It is deliberately
  // not promoted to ordered/range access until that type has an audited codec.
  *index_mode = ArtKeyMode::EXACT;
  return true;
}

void Index::RapidKeyCodec::AppendPart(const KeyBuffer &part_bytes, bool is_null, bool descending, KeyBuffer *out) {
  if (out == nullptr) return;
  const size_t begin = out->size();
  out->push_back(is_null ? 0x00 : 0x01);
  if (!is_null) {
    for (uchar b : part_bytes) {
      out->push_back(0x01);
      out->push_back(b);
    }
    out->push_back(0x00);
  }

  if (descending) {
    for (size_t i = begin; i < out->size(); ++i) (*out)[i] = static_cast<uchar>(~(*out)[i]);
  }
}

bool Index::RapidKeyCodec::EncodeRowPart(const ArtIndexDescriptor &index_desc, size_t part_no, const uchar *source,
                                         KeyBuffer *part_bytes) {
  if (part_no >= index_desc.parts.size() || source == nullptr || part_bytes == nullptr) return false;
  const auto &part = index_desc.parts[part_no];
  Field *field = part.source_field;
  if (field == nullptr) return false;

  switch (part.codec) {
    case ArtKeyPartCodec::SORTABLE_VALUE:
      return EncodeSortableValue(part, source, index_desc.db_low_byte_first, true, part_bytes);

    case ArtKeyPartCodec::COLLATION_WEIGHT: {
      const uchar *data = source;
      size_t data_len = 0;

      if (field->type() == MYSQL_TYPE_VARCHAR || field->type() == MYSQL_TYPE_VAR_STRING) {
        const uint len_bytes = field->get_length_bytes();
        data_len = (len_bytes == 1) ? static_cast<size_t>(source[0]) : static_cast<size_t>(uint2korr(source));
        data = source + len_bytes;
      } else if (IsBlobField(field)) {
        // Blob row storage contains a pointer; ask Field only to produce its
        // ordinary MySQL key image, then feed those bytes into the same stateless
        // collation transform used by handler keys.
        KeyBuffer image(part.payload_length + HA_KEY_BLOB_LENGTH, 0);
        const size_t written = MaterializeFieldKeyImage(field, source, image.data(), part.payload_length);
        if (image.size() < HA_KEY_BLOB_LENGTH) return false;
        data_len = std::min<size_t>(uint2korr(image.data()), part.payload_length);
        if (written > 0) data_len = std::min(data_len, written);
        data = image.data() + HA_KEY_BLOB_LENGTH;
        return TransformCollation(part, data, data_len, part_bytes);
      } else {
        data_len = std::min<size_t>(field->pack_length(), part.payload_length);
      }

      return TransformCollation(part, data, data_len, part_bytes);
    }

    case ArtKeyPartCodec::KEY_IMAGE: {
      part_bytes->assign(part.encoded_capacity, 0);
      const size_t written = MaterializeFieldKeyImage(field, source, part_bytes->data(), part.payload_length);
      // Keep the compiled MySQL key-image width, including any varlen framing
      // and padding, so the handler-side full key image normalizes identically.
      if (!part.variable_length && written != part.payload_length) return false;
      return true;
    }
  }
  return false;
}

bool Index::RapidKeyCodec::EncodeSearchPart(const ArtIndexDescriptor &index_desc, size_t part_no, const uchar *source,
                                            uint source_len, KeyBuffer *part_bytes) {
  if (part_no >= index_desc.parts.size() || source == nullptr || part_bytes == nullptr) return false;
  const auto &part = index_desc.parts[part_no];
  Field *field = part.source_field;
  if (field == nullptr) return false;

  switch (part.codec) {
    case ArtKeyPartCodec::SORTABLE_VALUE:
      if (source_len < part.payload_length) return false;
      return EncodeSortableValue(part, source, index_desc.db_low_byte_first, false, part_bytes);

    case ArtKeyPartCodec::COLLATION_WEIGHT: {
      const uchar *data = source;
      size_t data_len = source_len;
      if (part.variable_length) {
        if (source_len < HA_KEY_BLOB_LENGTH) return false;
        data_len = std::min<size_t>(uint2korr(source), source_len - HA_KEY_BLOB_LENGTH);
        data_len = std::min<size_t>(data_len, part.payload_length);
        data = source + HA_KEY_BLOB_LENGTH;
      } else {
        data_len = std::min<size_t>(data_len, part.payload_length);
      }
      return TransformCollation(part, data, data_len, part_bytes);
    }

    case ArtKeyPartCodec::KEY_IMAGE:
      part_bytes->assign(source, source + source_len);
      return true;
  }
  return false;
}

bool Index::RapidKeyCodec::EncodeRowKey(const ArtIndexDescriptor &index_desc, const uchar *rowdata,
                                        const ulong *col_offsets, const ulong *null_byte_offsets,
                                        const ulong *null_bitmasks, KeyBuffer *out) {
  if (rowdata == nullptr || col_offsets == nullptr || null_byte_offsets == nullptr || null_bitmasks == nullptr ||
      out == nullptr) {
    return false;
  }

  out->clear();
  out->reserve(index_desc.max_art_key_length);
  KeyBuffer part_bytes;

  for (size_t part_no = 0; part_no < index_desc.parts.size(); ++part_no) {
    const auto &part = index_desc.parts[part_no];
    const uint32 field_index = part.field_index;
    const bool is_null = part.nullable && ((rowdata[null_byte_offsets[field_index]] & null_bitmasks[field_index]) != 0);

    part_bytes.clear();
    if (!is_null && !EncodeRowPart(index_desc, part_no, rowdata + col_offsets[field_index], &part_bytes)) {
      return false;
    }
    AppendPart(part_bytes, is_null, part.descending, out);
  }
  return !out->empty();
}

bool Index::RapidKeyCodec::RequiredSearchImageLength(const ArtIndexDescriptor &index_desc, const uchar *mysql_key,
                                                     uint mysql_key_len, uint *image_len) {
  if (mysql_key == nullptr || image_len == nullptr || mysql_key_len == 0 || mysql_key_len > index_desc.key_length) {
    return false;
  }

  uint required = 0;
  uint expected_offset = 0;
  size_t touched_parts = 0;

  for (size_t part_no = 0; part_no < index_desc.parts.size(); ++part_no) {
    const auto &part = index_desc.parts[part_no];
    if (part.mysql_key_offset != expected_offset || part.store_length == 0) return false;
    expected_offset += part.store_length;
    if (expected_offset > index_desc.key_length) return false;

    // Same participation test as key_cmp(): key_length is checked only at the
    // beginning of a KEY_PART_INFO. Once this byte is in range, Field::key_cmp()
    // defines how many bytes of this part are actually inspected.
    if (part.mysql_key_offset >= mysql_key_len) break;
    ++touched_parts;

    const bool next_part_touched =
        part_no + 1 < index_desc.parts.size() && index_desc.parts[part_no + 1].mysql_key_offset < mysql_key_len;

    const uchar *source = mysql_key + part.mysql_key_offset;
    uint part_required = 0;
    uint framing = 0;
    bool is_null = false;

    if (part.nullable) {
      // key_cmp() always reads the NULL marker first. A NULL part needs no
      // payload bytes unless a following key part also participates.
      framing = 1;
      part_required = 1;
      is_null = source[0] != 0;
      ++source;
    }

    if (!is_null) {
      if (part.variable_length) {
        // MySQL handler key images for VARCHAR/BLOB-family key parts use the
        // fixed HA_KEY_BLOB_LENGTH (2-byte) length prefix. Field::key_cmp()
        // reads exactly that prefix plus the encoded payload, not the unused
        // zero-padding at the tail of store_length.
        const uint payload_capacity = part.store_length - framing;
        if (payload_capacity < HA_KEY_BLOB_LENGTH) return false;

        const uint data_len = static_cast<uint>(uint2korr(source));
        if (data_len > part.payload_length || HA_KEY_BLOB_LENGTH + data_len > payload_capacity) {
          return false;
        }
        part_required = framing + HA_KEY_BLOB_LENGTH + data_len;
      } else {
        // Fixed-width Field::key_cmp() consumes the full key-part image.
        part_required = part.store_length;
      }
    }

    // To reach a following participating part, the backing image necessarily
    // spans this part's complete store_length even if this part is NULL or a
    // compact variable-length image.
    if (next_part_touched) part_required = part.store_length;

    required = std::max(required, part.mysql_key_offset + part_required);
  }

  if (touched_parts == 0 || required == 0 || required > index_desc.key_length) return false;
  *image_len = required;
  return true;
}

bool Index::RapidKeyCodec::EncodeSearchKey(const ArtIndexDescriptor &index_desc, const uchar *mysql_key,
                                           uint logical_key_len, uint backing_image_len, KeyBuffer *out) {
  if (mysql_key == nullptr || out == nullptr || logical_key_len == 0 || logical_key_len > index_desc.key_length ||
      backing_image_len == 0 || backing_image_len > index_desc.key_length) {
    return false;
  }

  out->clear();
  out->reserve(index_desc.max_art_key_length);
  KeyBuffer part_bytes;
  size_t encoded_parts = 0;

  for (size_t part_no = 0; part_no < index_desc.parts.size(); ++part_no) {
    const auto &part = index_desc.parts[part_no];

    // MySQL's logical key length decides whether this KEY_PART_INFO
    // participates in the comparison. Do not substitute the amount of backing
    // storage copied for that semantic boundary.
    if (part.mysql_key_offset >= logical_key_len) break;

    // The handler may expand the start key through keypart_map, while a copied
    // final VARCHAR/BLOB range boundary may contain only length-prefix+payload.
    // Never read beyond the actual backing image in either case.
    if (part.mysql_key_offset >= backing_image_len) return false;

    const uint part_end = part.mysql_key_offset + part.store_length;
    const bool logical_complete_part = part_end <= logical_key_len;
    if (!logical_complete_part && part.codec != ArtKeyPartCodec::COLLATION_WEIGHT) {
      // A compact final collation endpoint is self-describing through its
      // handler length prefix. Numeric/raw partial parts are not.
      return false;
    }

    const uchar *source = mysql_key + part.mysql_key_offset;
    uint source_len = std::min<uint>(part.store_length, backing_image_len - part.mysql_key_offset);
    bool is_null = false;
    if (part.nullable) {
      if (source_len == 0) return false;
      is_null = source[0] != 0;
      ++source;
      --source_len;
    }

    part_bytes.clear();
    if (!is_null && !EncodeSearchPart(index_desc, part_no, source, source_len, &part_bytes)) return false;
    AppendPart(part_bytes, is_null, part.descending, out);
    ++encoded_parts;
  }

  return encoded_parts != 0 && !out->empty();
}

RpdTable::RpdTable(const TABLE *&mysql_table, const TableConfig &config)
    : m_mem_root(std::make_unique<MEM_ROOT>()), m_source_table(mysql_table) {
  m_memory_pool = ShannonBase::Utils::MemoryPool::create_from_parent(
      ShannonBase::shannon_rpd_memory_pool,
      config.tenant_name + "." + mysql_table->s->db.str + "." + mysql_table->s->table_name.str,
      config.max_table_mem_size);
  m_metadata.db_name = mysql_table->s->db.str;
  m_metadata.table_name = mysql_table->s->table_name.str;
  m_metadata.table_id = mysql_table->file->get_table_id();
  m_metadata.rows_per_imcu = config.rows_per_imcu;

  // from MySQL TABLE get fields infor.
  m_metadata.db_low_byte_first = mysql_table->s->db_low_byte_first;
  m_metadata.num_columns = mysql_table->s->fields;
  m_metadata.col_offsets.resize(m_metadata.num_columns);
  m_metadata.null_byte_offsets.resize(m_metadata.num_columns);
  m_metadata.null_bitmasks.resize(m_metadata.num_columns);

  m_metadata.fields.reserve(m_metadata.num_columns);
  for (uint32 ind = 0; ind < m_metadata.num_columns; ind++) {
    Field *field = mysql_table->field[ind];

    m_metadata.col_offsets[ind] = field->offset(mysql_table->record[0]);
    if (field->is_nullable()) {
      m_metadata.null_byte_offsets[ind] = field->null_offset();
      m_metadata.null_bitmasks[ind] = field->null_bit;
    }

    std::string comment;
    if (field->comment.str && field->comment.length > 0) {
      comment = std::string(field->comment.str, field->comment.length);
      std::transform(comment.begin(), comment.end(), comment.begin(), ::toupper);
    }

    Compress::ENCODING_TYPE encoding = Compress::ENCODING_TYPE::NONE;
    const char *const patt_str = "RAPID_COLUMN\\s*=\\s*ENCODING\\s*=\\s*(SORTED|VARLEN)";
    std::regex column_encoding_patt(patt_str, std::regex_constants::nosubs | std::regex_constants::icase);

    if (std::regex_search(comment, column_encoding_patt)) {
      if (comment.find("SORTED") != std::string::npos)
        encoding = Compress::ENCODING_TYPE::SORTED;
      else if (comment.find("VARLEN") != std::string::npos)
        encoding = Compress::ENCODING_TYPE::VARLEN;
    }

    m_metadata.fields.emplace_back(FieldMetadata{
        .source_fld = field->clone(m_mem_root.get()),
        .field_id = ind,
        .field_name = (field->field_name && field->field_name[0] != '\0') ? std::string(field->field_name) : "unknown",
        .type = field->type(),
        .pack_length = field->pack_length(),
        .normalized_length = Utils::Util::normalized_length(field),
        .nullable = field->is_nullable(),
        .is_key = field->is_flag_set(PRI_KEY_FLAG),
        .is_secondary_field = !field->is_flag_set(NOT_SECONDARY_FLAG),
        .compression_level = Compress::COMPRESS_LEVEL::DEFAULT,
        .encoding = encoding,
        .charset = field->charset(),
        .dictionary = (is_string_type(field->type()) && !Utils::Util::is_varlen(field->type()) &&
                       field->real_type() != MYSQL_TYPE_ENUM && field->real_type() != MYSQL_TYPE_SET)
                          ? std::make_shared<Compress::Dictionary>(encoding)
                          : nullptr,
        .global_min = 0.0,
        .global_max = 0.0,
        .distinct_count = 0,
        .null_ratio = 0.0,
        .statistics = std::make_unique<ColumnStatistics>(ind, field->field_name, field->type())});
  }
}

Table::Table(const TABLE *&mysql_table, const TableConfig &config) : RpdTable(mysql_table, config) {
  // create intial IMCU
  create_initial_imcu();

  // Wire the shared per-table WAL/checkpoint manager when the recovery
  // scheduler is active so DML appends WAL records before mutating memory.
  if (auto *sched = Recovery::CheckpointScheduler::global()) {
    if (auto *rmgr = sched->recovery_manager()) {
      m_recovery_manager = rmgr->table_manager(m_metadata.db_name, m_metadata.table_name);
    }
  }
}

Table::~Table() {
  std::scoped_lock lk(m_table_mutex);
  m_imcus.clear();
  m_imcu_index.clear();
  if (m_memory_pool) m_memory_pool.reset();
}

int Table::build_user_defined_index_memo(const Rapid_load_context *context) {
  auto source = context->m_table;

  m_art_index_descriptors.clear();
  m_art_index_descriptors.reserve(source->s->keys);

  for (auto ind = 0u; ind < source->s->keys; ind++) {
    auto key_info = source->key_info + ind;

    Key key;
    key.key_name = key_info->name;
    key.key_length = key_info->key_length;

    ArtIndexDescriptor descriptor;
    descriptor.key_name = key_info->name;
    descriptor.key_length = key_info->key_length;
    descriptor.db_low_byte_first = m_metadata.db_low_byte_first;
    descriptor.mode = Index::ArtKeyMode::ORDERED;
    descriptor.parts.reserve(key_info->user_defined_key_parts);

    // KEY_INFO/KEY_PART_INFO is interpreted exactly once here. Every execution
    // path below this point uses ArtIndexDescriptor instead of reopening the
    // handler TABLE metadata.
    uint mysql_key_offset = 0;

    for (uint i = 0u; i < key_info->user_defined_key_parts /**actual_key_parts*/; i++) {
      const KEY_PART_INFO &mysql_part = key_info->key_part[i];
      Field *field = mysql_part.field;
      if (field == nullptr) return HA_ERR_INTERNAL_ERROR;

      const uint field_index = field->field_index();
      if (field_index >= m_metadata.fields.size() || m_metadata.fields[field_index].source_fld == nullptr) {
        return HA_ERR_INTERNAL_ERROR;
      }

      KeyPart key_part;
      key_part.key_field_ind = field_index;
      key_part.null_bit = mysql_part.null_bit;
      key_part.key_part_flag = mysql_part.key_part_flag;
      key_part.length = mysql_part.length;

      const bool variable_length = (mysql_part.key_part_flag & (HA_BLOB_PART | HA_VAR_LENGTH_PART)) != 0;
      const bool descending = (mysql_part.key_part_flag & HA_REVERSE_SORT) != 0;

      ArtKeyPartDescriptor part_descriptor;
      part_descriptor.field_index = field_index;
      // Bind execution to RpdTable's stable Field clone, not to the live
      // handler TABLE's Field object used while compiling this descriptor.
      part_descriptor.source_field = m_metadata.fields[field_index].source_fld;
      part_descriptor.mysql_key_offset = mysql_key_offset;
      part_descriptor.store_length = mysql_part.store_length;
      part_descriptor.payload_length = mysql_part.length;
      part_descriptor.nullable = mysql_part.null_bit != 0;
      part_descriptor.variable_length = variable_length;
      part_descriptor.descending = descending;

      if (!Index::RapidKeyCodec::CompilePart(&part_descriptor, &descriptor.mode)) {
        return HA_ERR_INTERNAL_ERROR;
      }

      // marker + escaped part bytes + terminator. NULL is shorter, so this is
      // the maximum contribution of this part to one physical ART key.
      descriptor.max_art_key_length += 2 + 2 * part_descriptor.encoded_capacity;

      key.key_parts.emplace_back(std::move(key_part));
      descriptor.parts.emplace_back(std::move(part_descriptor));

      // store_length, not Field::pack_length(), is the packed handler key-part
      // width. It includes nullable and variable-length framing bytes.
      mysql_key_offset += mysql_part.store_length;
    }

    if (descriptor.parts.empty()) return HA_ERR_INTERNAL_ERROR;

    // Keep the legacy TableMetadata capability in sync for existing users; the
    // compiled descriptor is now the execution-time source of truth.
    key.art_ordering_preserving = descriptor.supports_ordered_access();

    m_metadata.keys.push_back(std::move(key));
    m_art_index_descriptors.emplace_back(std::move(descriptor));

    // Physical index inventory mirrors MySQL's KEY inventory. Equality/ref
    // lookup and internal PRIMARY-key row location are independent from whether
    // this byte representation has been certified for ordered/range access.
    m_indexes.emplace(key_info->name, std::make_unique<Index::Index<uchar, row_id_t>>(key_info->name));
    m_index_mutexes.emplace(key_info->name, std::make_unique<std::mutex>());
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Table::build_index(const Rapid_load_context *context, const ArtIndexDescriptor &index_desc, row_id_t rowid,
                       uchar *rowdata, ulong *col_offsets, ulong *null_byte_offsets, ulong *null_bitmasks) {
  (void)context;

  Index::RapidKeyCodec::KeyBuffer key_buffer;
  if (!Index::RapidKeyCodec::EncodeRowKey(index_desc, rowdata, col_offsets, null_byte_offsets, null_bitmasks,
                                          &key_buffer)) {
    return HA_ERR_INTERNAL_ERROR;
  }

  {
    std::lock_guard<std::mutex> idx_lock(*m_index_mutexes.at(index_desc.key_name));
    m_indexes[index_desc.key_name].get()->insert(key_buffer.data(), key_buffer.size(), &rowid, sizeof(rowid));
  }
  return SHANNON_SUCCESS;
}

int Table::create_index_memo(const Rapid_load_context *context) {
  ut_a(context != nullptr && context->m_table != nullptr);
  return build_user_defined_index_memo(context);
}

int Table::register_transaction(Transaction *trx) {
  // Transaction registration is not wired to the per-IMCU transaction journal
  // yet.  Returning a non-success code keeps this API honest instead of
  // pretending every IMCU registered the transaction (stubs must not create a
  // false success path).
  (void)trx;
  return HA_ERR_UNSUPPORTED;
}

Result<row_id_t> Table::insert_row(const Rapid_load_context *context, uchar *rowdata) {
  SHANNON_THREAD_LOCAL RowBuffer row_data(m_metadata.num_columns);
  row_data.resize(m_metadata.num_columns);

  if (context->m_extra_info.m_oper == Rapid_context::extra_info_t::OperType::LOAD)
    row_data.zero_copy_from_mysql_fields(context, rowdata, m_metadata.fields, m_metadata.col_offsets.data(),
                                         m_metadata.null_byte_offsets.data(), m_metadata.null_bitmasks.data());
  else
    row_data.copy_from_mysql_fields(context, rowdata, m_metadata.fields, m_metadata.col_offsets.data(),
                                    m_metadata.null_byte_offsets.data(), m_metadata.null_bitmasks.data());

  while (true) {
    auto current_imcu = get_or_create_write_imcu();
    if (!current_imcu) return {ErrorCode::NO_SPACE, INVALID_ROW_ID};

    if (!current_imcu->try_acquire_reader()) continue;

    row_id_t local_row_id = current_imcu->insert_row(context, row_data);
    if (local_row_id == INVALID_ROW_ID) {
      // INVALID_ROW_ID also represents WAL/CU failures.  Retry on a new IMCU
      // only when the current one is actually full; otherwise a durability or
      // allocation error must not be misreported as NO_SPACE (or duplicated by
      // a second insert attempt).
      const bool was_full = current_imcu->is_full();
      current_imcu->release_reader();
      if (!was_full) return {ErrorCode::INTERNAL, INVALID_ROW_ID};

      current_imcu = get_or_create_write_imcu();
      if (!current_imcu) return {ErrorCode::NO_SPACE, INVALID_ROW_ID};
      if (!current_imcu->try_acquire_reader()) continue;
      local_row_id = current_imcu->insert_row(context, row_data);
      if (local_row_id == INVALID_ROW_ID) {
        const bool retry_imcu_full = current_imcu->is_full();
        current_imcu->release_reader();
        return {retry_imcu_full ? ErrorCode::NO_SPACE : ErrorCode::INTERNAL, INVALID_ROW_ID};
      }
    }

    // global current rowid.
    auto rowid = current_imcu->get_start_row() + local_row_id;

    // Build user-defined indexes while still holding the reader pin.  The pin
    // spans CU write -> index commit -> (on failure) rollback, so compact()
    // cannot copy this row out of the IMCU before the index is settled.
    std::vector<const ArtIndexDescriptor *> built_indexes;
    built_indexes.reserve(m_art_index_descriptors.size());

    bool index_failed = false;
    for (const auto &index_desc : m_art_index_descriptors) {
      // Every MySQL key owns a physical Rapid ART. Missing index state is an
      // internal invariant violation, not an unsupported key-layout case.
      if (m_indexes.find(index_desc.key_name) == m_indexes.end() ||
          m_index_mutexes.find(index_desc.key_name) == m_index_mutexes.end()) {
        index_failed = true;
        break;
      }

      if (build_index(context, index_desc, rowid, rowdata, m_metadata.col_offsets.data(),
                      m_metadata.null_byte_offsets.data(), m_metadata.null_bitmasks.data())) {
        index_failed = true;
        break;
      }
      built_indexes.push_back(&index_desc);
    }

    if (index_failed) {
      // Undo indexes already built for this row.  The removal must take the
      // same per-index mutex build_index() uses so rollback never races a
      // concurrent insert/lookup on the same key.
      for (const auto *index_desc : built_indexes) {
        if (index_desc == nullptr) continue;
        Index::RapidKeyCodec::KeyBuffer key_buffer;
        if (!Index::RapidKeyCodec::EncodeRowKey(*index_desc, rowdata, m_metadata.col_offsets.data(),
                                                m_metadata.null_byte_offsets.data(), m_metadata.null_bitmasks.data(),
                                                &key_buffer)) {
          continue;
        }
        std::lock_guard<std::mutex> idx_lock(*m_index_mutexes.at(index_desc->key_name));
        // Value-selective remove: for a non-unique secondary index the same
        // key may map to several rowids; drop only this row's entry.
        m_indexes[index_desc->key_name]->remove(key_buffer.data(), key_buffer.size(), &rowid, sizeof(rowid));
      }
      // The IMCU insert has already durably committed its ROW_PREPARE/ROW_COMMIT
      // before table-level index construction.  For normal DML, compensate
      // with a durable DELETE so crash recovery cannot resurrect a row whose
      // SQL insert returned an error.  LOAD bypasses row WAL, so a local hide
      // is sufficient there.
      if (context->m_extra_info.m_oper == Rapid_context::extra_info_t::OperType::LOAD) {
        current_imcu->rollback_inserted_row(local_row_id);
      } else if (current_imcu->delete_row(context, local_row_id) != ShannonBase::SHANNON_SUCCESS) {
        // Best-effort local visibility repair.  If the compensating DELETE's
        // COMMIT durability outcome is ambiguous, log_row_commit() also puts
        // the recovery manager into recovery_required; a clean PREPARE/append
        // failure is uncommitted and recovery will ignore it.
        current_imcu->rollback_inserted_row(local_row_id);
      }
      current_imcu->release_reader();
      return {ErrorCode::INTERNAL, INVALID_ROW_ID};
    }

    // Row and indexes are now committed to this IMCU; release the pin.
    current_imcu->release_reader();

    m_metadata.total_rows.fetch_add(1);
    if (context->m_extra_info.m_oper == Rapid_context::extra_info_t::OperType::LOAD) {
      if ((m_metadata.total_rows.load(std::memory_order_relaxed) & 0x3FFF) == 0) m_metadata.update_stat_n_rows();
    } else {
      m_metadata.update_stat_n_rows();
    }
    return {ErrorCode::OK, rowid};
  }
}

int Table::delete_row(const Rapid_load_context *context, row_id_t global_row_id) {
  while (true) {
    auto imcu = locate_imcu_by_rowid(global_row_id);
    if (!imcu) return HA_ERR_KEY_NOT_FOUND;

    if (!imcu->try_acquire_reader()) {
      std::this_thread::yield();
      continue;
    }

    // 2. calc row_id
    assert((imcu->get_start_row() % m_metadata.rows_per_imcu) == 0);
    row_id_t local_row_id = global_row_id - imcu->get_start_row();

    // 3. delete row from IMCU.
    auto success = imcu->delete_row(context, local_row_id);

    imcu->release_reader();

    if (success) return success;  // return on error.

    // 4. update statistics if delete operation succeeded.
    m_metadata.deleted_rows.fetch_add(1);
    m_metadata.version_count.fetch_add(1);
    m_metadata.update_stat_n_rows();

    return ShannonBase::SHANNON_SUCCESS;
  }
}

size_t Table::delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &row_ids) {
  // 1. group global row IDs by their IMCU index (not by shared_ptr — a
  //    concurrent compact may swap the IMCU out and re-map local row ids).
  std::unordered_map<size_t, std::vector<row_id_t>> imcu_groups;

  for (row_id_t global_row_id : row_ids) {
    size_t imcu_idx = global_row_id / m_metadata.rows_per_imcu;
    imcu_groups[imcu_idx].push_back(global_row_id);
  }

  // 2. delete rows in each IMCU with reader protection.
  size_t total_deleted = 0;

  for (auto &[imcu_idx, global_ids] : imcu_groups) {
    while (true) {
      auto imcu = locate_imcu(imcu_idx);
      if (!imcu) break;

      // Pin the IMCU only while ACTIVE; a concurrent compact makes it
      // non-ACTIVE, in which case wait and re-locate (the swap re-maps rows).
      if (!imcu->try_acquire_reader()) {
        std::this_thread::yield();
        continue;
      }

      // Compute local row ids after pinning: compact preserves row order but
      // re-maps local ids.
      std::vector<row_id_t> local_ids;
      local_ids.reserve(global_ids.size());
      for (row_id_t gid : global_ids) {
        local_ids.push_back(gid - imcu->get_start_row());
      }

      total_deleted += imcu->delete_rows(context, local_ids);
      imcu->release_reader();
      break;
    }
  }

  // 3. update statistics.
  m_metadata.deleted_rows.fetch_add(total_deleted);
  m_metadata.update_stat_n_rows();

  return total_deleted;
}

int Table::update_row(const Rapid_load_context *context, row_id_t global_row_id,
                      const std::unordered_map<uint32, RowBuffer::ColumnValue> &updates) {
  // This only mutates CU column data (MVCC-versioned per column already).
  // Callers whose updates touch an indexed column must swap that index's ART
  // entry (remove old key, insert new key, same rowid) themselves before
  // calling this -- see CopyInfoParser::parse_and_apply_update() -- since
  // that requires the raw old/new row images this function does not have.

  // Retry loop: wait out any concurrent compact that makes the owning IMCU
  // non-ACTIVE, then re-locate the (possibly swapped-in) IMCU.
  while (true) {
    auto imcu = locate_imcu_by_rowid(global_row_id);
    if (!imcu) return HA_ERR_KEY_NOT_FOUND;

    if (!imcu->try_acquire_reader()) {
      std::this_thread::yield();
      continue;
    }

    // 2. calc row_id.
    row_id_t local_row_id = global_row_id - imcu->get_start_row();

    // 3. update.
    int ret = imcu->update_row(context, local_row_id, updates);

    imcu->release_reader();
    return ret;
  }
}

row_id_t Table::locate_row(const Rapid_load_context *context, uchar *rowdata) {
  const auto *primary_desc = get_art_index_descriptor(ShannonBase::SHANNON_PRIMARY_KEY_NAME);
  if (primary_desc == nullptr) return INVALID_ROW_ID;

  Index::RapidKeyCodec::KeyBuffer primary_key;
  if (!Index::RapidKeyCodec::EncodeRowKey(*primary_desc, rowdata, m_metadata.col_offsets.data(),
                                          m_metadata.null_byte_offsets.data(), m_metadata.null_bitmasks.data(),
                                          &primary_key)) {
    return INVALID_ROW_ID;
  }

  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = primary_key.size();
  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff = std::make_unique<uchar[]>(primary_key.size());
  std::memcpy(context->m_extra_info.m_key_buff.get(), primary_key.data(), primary_key.size());

  // A loaded Rapid table with a PRIMARY KEY must own the corresponding
  // physical ART regardless of its ordered/range capability. Keep the lookup
  // fail-closed in production; a missing PRIMARY ART is an invariant violation.
  auto index_it = m_indexes.find(primary_desc->key_name);
  if (index_it == m_indexes.end() || !index_it->second) return INVALID_ROW_ID;

  auto rowid = index_it->second->lookup(context->m_extra_info.m_key_buff.get(), context->m_extra_info.m_key_len);
  return rowid ? *rowid : INVALID_ROW_ID;
}

ColumnStatistics *Table::get_column_stats(uint32 col_idx) const {
  if (col_idx >= m_metadata.fields.size()) return nullptr;
  return m_metadata.fields[col_idx].statistics.get();
}

void Table::update_statistics(bool force) {
  std::unique_lock lock(m_table_mutex);
  for (auto &imcu : m_imcus) {
    assert(imcu);
    imcu->update_statistics();
  }
  for (const auto &col_stat : m_metadata.fields) col_stat.statistics->finalize();
}

size_t Table::garbage_collect(uint64 min_active_scn) {
  size_t total_freed = 0;

  // 1. Snapshot the IMCU list under a shared lock to avoid data race with compact().
  std::vector<std::shared_ptr<Imcu>> snapshot;
  {
    std::shared_lock lock(m_table_mutex);
    snapshot = m_imcus;
  }

  // 2. perform GC on each IMCU OUTSIDE the lock.
  for (auto &imcu : snapshot) {
    // Skip IMCUs that have active readers (e.g. a foreground DML is
    // in the middle of delete_row / update_row on this IMCU).
    if (imcu->has_active_readers()) continue;
    total_freed += imcu->garbage_collect(min_active_scn);
  }

  // 3. update global version count.
  m_metadata.version_count.fetch_sub(total_freed);

  return total_freed;
}

size_t Table::compact(double delete_ratio_threshold) {
  size_t total_freed = 0;
  size_t total_physically_removed = 0;

  // 1. Snapshot the IMCU list under a shared lock — readers can still proceed.
  std::vector<std::shared_ptr<Imcu>> old_imcus;
  {
    std::shared_lock lock(m_table_mutex);
    old_imcus = m_imcus;
  }

  // 2. Compact IMCUs OUTSIDE the lock — the expensive part.
  std::vector<std::shared_ptr<Imcu>> new_imcus;
  new_imcus.reserve(old_imcus.size());
  for (auto &imcu : old_imcus) {
    if (!imcu->needs_compaction() || imcu->get_delete_ratio() < delete_ratio_threshold || imcu->has_active_readers()) {
      new_imcus.emplace_back(imcu);
      continue;
    }

    const size_t rows_before = imcu->get_row_count();
    auto compacted = imcu->compact();
    if (!compacted) {
      new_imcus.emplace_back(imcu);
      continue;
    }

    const size_t rows_after = compacted->get_row_count();
    total_freed += imcu->estimate_size() - compacted->estimate_size();
    total_physically_removed += rows_before - rows_after;
    new_imcus.emplace_back(std::move(compacted));
  }

  // 3. Atomically swap the IMCU list — brief exclusive lock.
  //    Preserve any IMCUs that were concurrently added between the snapshot
  //    and the swap (e.g. by get_or_create_write_imcu).
  {
    std::unique_lock lock(m_table_mutex);
    // Merge any new IMCUs that appeared after we took the snapshot.
    if (m_imcus.size() > old_imcus.size()) {
      for (size_t i = old_imcus.size(); i < m_imcus.size(); ++i) {
        new_imcus.push_back(m_imcus[i]);
      }
    }
    m_imcus = std::move(new_imcus);
    build_imcu_index();
  }

  if (total_physically_removed > 0) {
    m_metadata.total_rows.fetch_sub(total_physically_removed, std::memory_order_release);
    m_metadata.deleted_rows.fetch_sub(total_physically_removed, std::memory_order_release);
    m_metadata.update_stat_n_rows();
  }

  return total_freed;
}

void Table::compact_imcu(std::shared_ptr<Imcu> imcu) {
  if (!imcu || imcu->has_active_readers() || !imcu->needs_compaction()) return;

  const size_t rows_before = imcu->get_row_count();
  auto compacted = imcu->compact();
  if (!compacted) return;  // compaction disabled (stable row-id model) or nothing to do.

  const size_t rows_after = compacted->get_row_count();

  // Replace the old IMCU with the compacted one and rebuild the shadow index
  // atomically under the table lock.
  {
    std::unique_lock lock(m_table_mutex);
    for (auto &slot : m_imcus) {
      if (slot.get() == imcu.get()) {
        slot = std::move(compacted);
        break;
      }
    }
    build_imcu_index();
  }

  if (rows_after < rows_before) {
    m_metadata.total_rows.fetch_sub(rows_before - rows_after, std::memory_order_release);
    m_metadata.deleted_rows.fetch_sub(rows_before - rows_after, std::memory_order_release);
    m_metadata.update_stat_n_rows();
  }
}

// Reorganize is not implemented yet; returning false (failure) keeps this
// honest so callers never assume the table was actually reorganized.
bool Table::reorganize() { return false; }

int PartTable::register_transaction(Transaction *trx) {
  std::shared_lock lock(m_partitions_mutex);
  for (const auto &[_, table_ptr] : m_partitions) {
    if (table_ptr) {
      int rc = table_ptr.get()->register_transaction(trx);
      if (rc != ShannonBase::SHANNON_SUCCESS) return rc;
    }
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int PartTable::build_partitions(const Rapid_load_context *context) {
  auto ret{ShannonBase::SHANNON_SUCCESS};
  assert(context->m_table);
  m_part_key = context->m_sch_tb_name;

  // start to add partitions.
  for (auto &[part_name, part_id] : context->m_extra_info.m_partition_infos) {
    auto part_key = part_name;
    part_key.append("#").append(std::to_string(part_id));
    // each sub-part table is a normal rpd table. But using small table mem size.
    TableConfig config;
    config.tenant_name = part_key;
    config.max_table_mem_size = SHANNON_SMALL_TABLE_MEMRORY_SIZE;

    const TABLE *mysql_source = context->m_table;
    auto sub_part_table = std::make_unique<Table>(mysql_source, config);
    if (!sub_part_table->has_memory_pool()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "Out of Rapid memory while reserving a partition sub-pool. Raise rapid_memory_size_max or unload "
               "tables.");
      return HA_ERR_GENERIC;
    }

    // step 1: build indexes.
    if ((ret = sub_part_table.get()->create_index_memo(context))) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Build indexes memo for partition failed");
      return ret;
    }

    // step 2: set load type.
    sub_part_table.get()->set_load_type(load_type_t::USER);

    // step 4: Adding the Table meta obj into partitions table meta information.
    std::unique_lock lock(m_partitions_mutex);
    m_partitions.emplace(part_key, std::move(sub_part_table));
  }

  return ShannonBase::SHANNON_SUCCESS;
}

row_id_t PartTable::rows(const Rapid_context *) { return static_cast<row_id_t>(count_total_rows()); }

size_t PartTable::garbage_collect(uint64 min_active_scn) {
  std::shared_lock lock(m_partitions_mutex);
  size_t total_freed = 0;
  for (const auto &[_, table_ptr] : m_partitions) {
    if (table_ptr) total_freed += table_ptr->garbage_collect(min_active_scn);
  }
  return total_freed;
}

size_t PartTable::compact(double delete_ratio_threshold) {
  std::shared_lock lock(m_partitions_mutex);
  size_t total_freed = 0;
  for (const auto &[_, table_ptr] : m_partitions) {
    if (table_ptr) total_freed += table_ptr->compact(delete_ratio_threshold);
  }
  return total_freed;
}

void PartTable::compact_imcu(std::shared_ptr<Imcu> imcu) {
  if (!imcu) return;
  // Route to the owning partition table (a normal Table), not the parent.
  auto *owner = imcu->owner();
  if (owner && owner != this) {
    owner->compact_imcu(std::move(imcu));
  }
}

void PartTable::update_statistics(bool force) {
  std::shared_lock lock(m_partitions_mutex);
  for (const auto &[_, table_ptr] : m_partitions) {
    if (table_ptr) table_ptr->update_statistics(force);
  }
}

void PartTable::foreach_imcu(std::function<void(Imcu *)> func) {
  std::shared_lock lock(m_partitions_mutex);
  for (const auto &[_, table_ptr] : m_partitions) {
    if (table_ptr) table_ptr->foreach_imcu(func);
  }
}
}  // namespace Imcs
}  // namespace ShannonBase
