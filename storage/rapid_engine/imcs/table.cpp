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

#include "include/ut0dbg.h"  //ut_a
#include "sql/field.h"       //field
#include "sql/key.h"         //COPY_KEY
#include "sql/table.h"       //TABLE
#include "storage/innobase/include/mach0data.h"

#include "storage/rapid_engine/imcs/cu.h"              //CU
#include "storage/rapid_engine/include/rapid_const.h"  // INVALID_ROW_ID

#include "storage/rapid_engine/imcs/index/encoder.h"
#include "storage/rapid_engine/imcs/predicate.h"  //predicate
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_status.h"
#include "storage/rapid_engine/utils/memory_pool.h"  //Blob
#include "storage/rapid_engine/utils/utils.h"        //Blob
namespace ShannonBase {
// global memory pool
extern std::shared_ptr<ShannonBase::Utils::MemoryPool> g_rpd_memory_pool;
namespace Imcs {

/**
 * @brief Encode a row buffer into a contiguous key buffer suitable for indexing.
 *
 * This function constructs a binary key representation from a MySQL row record
 * according to the specified KEY metadata. It handles null flags, variable-length
 * fields, BLOBs, and numeric types, ensuring proper encoding for index comparison.
 *
 * @param[out] to_key      Pointer to pre-allocated key buffer to write encoded key.
 * @param[in]  from_record Pointer to row data buffer containing raw field values.
 * @param[in]  key_info    Pointer to MySQL KEY structure describing key parts.
 * @param[in]  key_len     Total length of the key buffer.
 *
 * @note
 *   - Handles null indicators for columns that have a null bit.
 *   - Numeric types (DOUBLE, FLOAT, DECIMAL, NEWDECIMAL, LONG) are encoded
 *     in sortable binary format using Index::Encoder.
 *   - Fixed-length, variable-length, and BLOB columns are encoded according
 *     to MySQL key conventions (HA_KEY_BLOB_LENGTH for BLOBs).
 *   - The function does not modify the input record.
 */
static void encode_row_key(uchar *to_key, const uchar *from_record, const KEY *key_info, uint key_len,
                           const RowBuffer *row_data) {
  if (!to_key || !from_record || !key_info || key_len == 0) return;

  std::memset(to_key, 0x0, key_len);

  uint length{0u};
  auto remain_len = key_len;

  for (KEY_PART_INFO *key_part = key_info->key_part; remain_len > 0; key_part++) {
    Field *field = key_part->field;
    const CHARSET_INFO *cs = field->charset();

    auto col_val = row_data->get_column(field->field_index());
    field->set_field_ptr(const_cast<uchar *>(col_val->data));
    if (key_part->null_bit) {
      *to_key++ = (col_val->flags.is_null ? 1 : 0);
      remain_len--;
    }

    length = std::min<uint>(remain_len, key_part->length);
    if (key_part->key_part_flag & HA_BLOB_PART || key_part->key_part_flag & HA_VAR_LENGTH_PART) {
      remain_len -= HA_KEY_BLOB_LENGTH;
      length = std::min<uint>(remain_len, key_part->length);
      field->get_key_image(to_key, length, Field::itRAW);
      to_key += HA_KEY_BLOB_LENGTH;
    } else {
      Utils::ColumnMapGuard guard(key_info->table);
      switch (field->type()) {
        case MYSQL_TYPE_DOUBLE:
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL: {
          uchar encoding[8] = {0};
          Index::Encoder<double>::EncodeData(field->val_real(), encoding);
          std::memcpy(to_key, encoding, length);  // decimal stored length: 5 not 8.
        } break;
        case MYSQL_TYPE_LONG: {
          ut_a(length == sizeof(int32_t));
          uchar encoding[4] = {0};
          Index::Encoder<int32_t>::EncodeData((int32_t)field->val_int(), encoding);
          std::memcpy(to_key, encoding, length);
        } break;
        default: {
          ut_a(length == field->pack_length());
          size_t bytes = field->get_key_image(to_key, length, Field::itRAW);
          if (bytes < length) {
            cs->cset->fill(cs, reinterpret_cast<char *>(to_key + bytes), length - bytes, ' ');
          }
        } break;
      }
    }

    to_key += length;
    remain_len -= length;
  }
}

Table::Table(const TABLE *&mysql_table, const TableConfig &config) : RpdTable(mysql_table, config) {
  m_memory_pool = ShannonBase::Utils::MemoryPool::create_from_parent(ShannonBase::g_rpd_memory_pool, config.tenant_name,
                                                                     config.max_table_mem_size);
  m_metadata.db_name = mysql_table->s->db.str;
  m_metadata.table_name = mysql_table->s->table_name.str;
  m_metadata.table_id = generate_table_id();
  m_metadata.rows_per_imcu = config.rows_per_imcu;
  m_metadata.max_imcu_size_mb = config.max_imcu_size_mb;

  // from MySQL TABLE get fields infor.
  m_metadata.num_columns = mysql_table->s->fields;
  m_metadata.fields.reserve(m_metadata.num_columns);

  for (uint32_t ind = 0; ind < m_metadata.num_columns; ind++) {
    Field *field = mysql_table->field[ind];

    std::string comment;
    if (field->comment.str && field->comment.length > 0) {
      comment = std::string(field->comment.str, field->comment.length);
      std::transform(comment.begin(), comment.end(), comment.begin(), ::toupper);
    }

    Compress::Encoding_type encoding = Compress::Encoding_type::NONE;
    const char *const patt_str = "RAPID_COLUMN\\s*=\\s*ENCODING\\s*=\\s*(SORTED|VARLEN)";
    std::regex column_encoding_patt(patt_str, std::regex_constants::nosubs | std::regex_constants::icase);

    if (std::regex_search(comment, column_encoding_patt)) {
      if (comment.find("SORTED") != std::string::npos)
        encoding = Compress::Encoding_type::SORTED;
      else if (comment.find("VARLEN") != std::string::npos)
        encoding = Compress::Encoding_type::VARLEN;
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
        .encoding = encoding,
        .charset = field->charset(),
        .dictionary = is_string_type(field->type()) ? std::make_shared<Compress::Dictionary>(encoding) : nullptr,
        .global_min = 0.0,
        .global_max = 0.0,
        .distinct_count = 0,
        .null_ratio = 0.0});
  }

  // [TODO] intial global compoent.
  // m_txn_coordinator = std::make_unique<Transaction_Coordinator>();
  // m_version_manager = std::make_unique<Global_Version_Manager>();
  // m_bg_workers = std::make_unique<Background_Worker_Pool>(config.background_worker_threads);

  // create intial IMCU
  create_initial_imcu();
}

Table::~Table() {
  if (m_memory_pool) {
    m_memory_pool.get()->reset();
    m_memory_pool = nullptr;
  }
}

int Table::build_user_defined_index_memo(const Rapid_load_context *context) {
  auto source = context->m_table;

  for (auto ind = 0u; ind < source->s->keys; ind++) {
    auto key_info = source->key_info + ind;
    std::vector<std::string> key_parts_names;
    for (uint i = 0u; i < key_info->user_defined_key_parts /**actual_key_parts*/; i++) {
      key_parts_names.push_back(key_info->key_part[i].field->field_name);
    }

    // m_source_keys.emplace(key_info->name, std::make_pair(key_info->key_length, key_parts_names));
    m_indexes.emplace(key_info->name, std::make_unique<Index::Index<uchar, row_id_t>>(key_info->name));
    m_index_mutexes.emplace(key_info->name, std::make_unique<std::mutex>());
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Table::build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid, const RowBuffer *row_data) {
  return build_index_impl(context, key, rowid, row_data);
}

int Table::build_index_impl(const Rapid_load_context *context, const KEY *key, row_id_t rowid,
                            const RowBuffer *row_data) {
  // this is come from ha_innodb.cc postion(), when postion() changed, the part should be changed respondingly.
  // why we dont not change the impl of postion() directly? because the postion() is impled in innodb engine.
  // we want to decouple with innodb engine.
  // ref: void key_copy(uchar *to_key, const uchar *from_record, const KEY *key_info,
  //            uint key_length). Due to we should encoding the float/double/decimal types.
  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = key->key_length;
  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff = std::make_unique<uchar[]>(key->key_length);
  memset(context->m_extra_info.m_key_buff.get(), 0x0, key->key_length);
  auto to_key = context->m_extra_info.m_key_buff.get();

  encode_row_key(to_key, context->m_table->record[0], key, key->key_length, row_data);

  {
    std::lock_guard<std::mutex> lock(*m_index_mutexes[key->name].get());
    m_indexes[key->name].get()->insert(context->m_extra_info.m_key_buff.get(), context->m_extra_info.m_key_len, &rowid,
                                       sizeof(rowid));
  }
  return SHANNON_SUCCESS;
}

int Table::build_key_info(const Rapid_load_context *context, const KEY *key, uchar *rowdata,
                          const RowBuffer *row_data) {
  // this is come from ha_innodb.cc postion(), when postion() changed, the part should be changed respondingly.
  // why we dont not change the impl of postion() directly? because the postion() is impled in innodb engine.
  // we want to decouple with innodb engine.
  /* Copy primary key as the row reference */
  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = key->key_length;
  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff = std::make_unique<uchar[]>(key->key_length);
  std::memset(context->m_extra_info.m_key_buff.get(), 0x0, key->key_length);
  auto to_key = context->m_extra_info.m_key_buff.get();
  std::shared_mutex key_buff_mutex;
  encode_row_key(to_key, context->m_table->record[0], key, key->key_length, row_data);

  return SHANNON_SUCCESS;
}

int Table::create_index_memo(const Rapid_load_context *context) {
  auto source = context->m_table;
  ut_a(source);

  build_user_defined_index_memo(context);
  return ShannonBase::SHANNON_SUCCESS;
}

int Table::register_transaction(Transaction *trx) {
  assert(trx);
  for (auto &imcu : m_imcus) {
    trx->register_imcu_modification(imcu.get());
  }
  return ShannonBase::SHANNON_SUCCESS;
}

row_id_t Table::insert_row(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets,
                           size_t n_cols, ulong *null_byte_offsets, ulong *null_bitmasks) {
  ut_a(context->m_table->s->fields == n_cols);

  RowBuffer row_data(n_cols);
  row_data.copy_from_mysql_fields(context, context->m_table->field, n_cols, rowdata, col_offsets, null_byte_offsets,
                                  null_bitmasks);

  return insert_row_impl(context, row_data);
}

row_id_t Table::insert_row_impl(const Rapid_load_context *context, const RowBuffer &row_data) {
  Imcu *current_imcu = get_or_create_write_imcu();
  if (!current_imcu) {
    return INVALID_ROW_ID;
  }

  row_id_t local_row_id = current_imcu->insert_row(context, row_data);
  if (local_row_id == INVALID_ROW_ID) {  // full.
    current_imcu = get_or_create_write_imcu();
    if (!current_imcu) return INVALID_ROW_ID;
    local_row_id = current_imcu->insert_row(context, row_data);
  }

  row_id_t global_row_id = current_imcu->get_start_row() + local_row_id;
  for (auto index = 0u; index < context->m_table->s->keys; index++) {  // user defined indexes.
    auto key_info = context->m_table->key_info + index;
    if (build_index(context, key_info, global_row_id, &row_data)) return HA_ERR_GENERIC;
  }

  m_metadata.total_rows.fetch_add(1);

  return global_row_id;
}

int Table::delete_row(const Rapid_load_context *context, row_id_t global_row_id) {
  // 1. locate IMCU
  Imcu *imcu = locate_imcu_by_rowid(global_row_id);
  if (!imcu) return HA_ERR_KEY_NOT_FOUND;

  // 2. calc row_id
  assert((imcu->get_start_row() % m_metadata.rows_per_imcu) == 0);
  row_id_t local_row_id = global_row_id - imcu->get_start_row();

  // 3. delete row from IMCU.
  auto success = imcu->delete_row(context, local_row_id);

  if (success) return success;  // return on error.

  // 4. update statistics if delete operation succeeded.
  m_metadata.deleted_rows.fetch_add(1);
  m_metadata.version_count.fetch_add(1);

  return ShannonBase::SHANNON_SUCCESS;
}

size_t Table::delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &row_ids) {
  // 1. the IMCU candidate group.
  std::unordered_map<Imcu *, std::vector<row_id_t>> imcu_groups;

  for (row_id_t global_row_id : row_ids) {
    Imcu *imcu = locate_imcu_by_rowid(global_row_id);
    if (imcu) {
      row_id_t local_row_id = global_row_id - imcu->get_start_row();
      imcu_groups[imcu].push_back(local_row_id);
    }
  }

  // 2. delete rows in IMCU.
  size_t total_deleted = 0;

  for (auto &[imcu, local_ids] : imcu_groups) total_deleted += imcu->delete_rows(context, local_ids);

  // 3. update statistics.
  m_metadata.deleted_rows.fetch_add(total_deleted);

  return total_deleted;
}

int Table::update_row(const Rapid_load_context *context, row_id_t global_row_id,
                      const std::unordered_map<uint32_t, RowBuffer::ColumnValue> &updates) {
  // 1. locate IMCU.
  Imcu *imcu = locate_imcu_by_rowid(global_row_id);
  if (!imcu) return false;

  // 2. calc row_id.
  row_id_t local_row_id = global_row_id - imcu->get_start_row();

  // 3. update.
  return imcu->update_row(context, local_row_id, updates);
}

row_id_t Table::locate_row(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets,
                           size_t n_cols, ulong *null_byte_offsets, ulong *null_bitmasks) {
  std::string sch_tb_name = context->m_schema_name;
  sch_tb_name.append(":").append(context->m_table_name);

  RowBuffer row_data(n_cols);
  row_data.copy_from_mysql_fields(context, context->m_table->field, n_cols, rowdata, col_offsets, null_byte_offsets,
                                  null_bitmasks);

  for (auto index = 0u; index < context->m_table->s->keys; index++) {
    auto key_info = context->m_table->key_info + index;
    if (build_key_info(context, key_info, rowdata, &row_data)) return HA_ERR_GENERIC;
    break;
  }

  auto rowid = m_indexes[ShannonBase::SHANNON_PRIMARY_KEY_NAME].get()->lookup(context->m_extra_info.m_key_buff.get(),
                                                                              context->m_extra_info.m_key_len);
  auto global_row_id = rowid ? *rowid : INVALID_ROW_ID;
  return global_row_id;
}

int Table::scan_table(Rapid_scan_context *context, const std::vector<std::unique_ptr<Predicate>> &predicates,
                      const std::vector<uint32_t> &projection, RowCallback callback) {
  // 1. travel all IMCUs.
  for (auto &imcu : m_imcus) {
    // 1.1 Storage Index to filter（skip IMCU ）
    if (imcu->can_skip_imcu(predicates)) continue;  // skip IMCU

    // 1.2 scan IMCU
    imcu->scan(context, predicates, projection, callback);

    // 1.3 check LIMIT oper.
    if (context->limit > 0 && context->rows_returned >= context->limit) break;
  }

  return ShannonBase::SHANNON_SUCCESS;
}

size_t Table::scan_table(Rapid_scan_context *context, row_id_t start_offset, size_t limit,
                         const std::vector<std::unique_ptr<Predicate>> &predicates,
                         const std::vector<uint32_t> &projection, RowCallback callback) {
  std::shared_lock lock(m_table_mutex);

  if (limit == 0 || m_imcus.empty()) return 0;

  size_t total_scanned = 0;
  size_t remaining = limit;

  // Calculate which IMCU contains start_offset
  size_t rows_per_imcu = m_metadata.rows_per_imcu;
  size_t start_imcu_idx = start_offset / rows_per_imcu;
  size_t offset_in_imcu = start_offset % rows_per_imcu;

  // Scan from start_imcu_idx to end
  for (size_t imcu_idx = start_imcu_idx; imcu_idx < m_imcus.size() && remaining > 0; imcu_idx++) {
    auto &imcu = m_imcus[imcu_idx];

    // Storage Index filtering
    if (imcu->can_skip_imcu(predicates)) {
      offset_in_imcu = 0;  // Reset for next IMCU
      continue;
    }

    // Scan range within this IMCU
    size_t scanned = imcu->scan_range(context,
                                      offset_in_imcu,  // Start from offset only for first IMCU
                                      remaining,       // How many more rows we need
                                      predicates, projection, callback);

    total_scanned += scanned;
    remaining -= scanned;
    offset_in_imcu = 0;  // Subsequent IMCUs start from 0

    // Check context limit
    if (context->limit > 0 && context->rows_returned >= context->limit) break;
  }

  return total_scanned;
}

bool Table::read(Rapid_scan_context *context, const uchar *key_value, Row_Result &result) { return false; }

bool Table::range_scan(Rapid_scan_context *context, const uchar *start_key, const uchar *end_key,
                       RowCallback callback) {
  assert(context && start_key && end_key);
  return false;
}

uint64_t Table::get_row_count(const Rapid_scan_context *context) const {
  assert(context);
  return 0;
}

ColumnStatistics Table::get_column_stats(uint32_t col_idx) const {
  assert(col_idx);

  ColumnStatistics col_stat(col_idx, "col_name", MYSQL_TYPE_NULL);
  return col_stat;
}

void Table::update_statistics(bool force) {
  std::unique_lock lock(m_table_mutex);

  for (uint32_t col_idx = 0; col_idx < m_metadata.num_columns; col_idx++) {
    auto stats = std::make_unique<ColumnStatistics>(col_idx, m_metadata.fields[col_idx].field_name,
                                                    m_metadata.fields[col_idx].type);

    // check all IMCUs
    for (auto &imcu : m_imcus) {
      assert(imcu);
      // imcu->update_statistic();
      //  [TODO]: collection statistics.
    }

    stats->finalize();
    // m_column_stats[col_idx] = std::move(stats);
  }
}

size_t Table::garbage_collect(uint64_t min_active_scn) {
  size_t total_freed = 0;

  // 1. perform GC on each IMCU.
  for (auto &imcu : m_imcus) {
    total_freed += imcu->garbage_collect(min_active_scn);
  }

  // 2. update global version count.
  m_metadata.version_count.fetch_sub(total_freed);

  return total_freed;
}

size_t Table::compact(double delete_ratio_threshold) {
  size_t total_freed = 0;

  std::vector<std::shared_ptr<Imcu>> new_imcus;
  for (auto &imcu : m_imcus) {
    if (imcu->needs_compaction() && imcu->get_delete_ratio() >= delete_ratio_threshold) {
      // compact the IMCU.
      auto compacted = imcu->compact();
      if (compacted) {
        new_imcus.emplace_back(compacted);
        total_freed += imcu->estimate_size() - compacted->estimate_size();
      } else {
        new_imcus.emplace_back(imcu);
      }
    } else {
      new_imcus.emplace_back(imcu);
    }
  }

  // change atomically.
  {
    std::unique_lock lock(m_table_mutex);
    m_imcus = std::move(new_imcus);
    build_imcu_index();
  }

  return total_freed;
}

bool Table::reorganize() { return false; }

int PartTable::register_transaction(Transaction *trx) {
  for (const auto &[_, table_ptr] : m_partitions) {
    if (table_ptr) table_ptr.get()->register_transaction(trx);
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int PartTable::build_partitions(const Rapid_load_context *context) {
  auto ret{ShannonBase::SHANNON_SUCCESS};
  assert(context->m_table);

  // start to add partitions.
  for (auto &[part_name, part_id] : context->m_extra_info.m_partition_infos) {
    auto part_key = part_name;
    part_key.append("#").append(std::to_string(part_id));
    // each sub-part table is a normal rpd table. But using small table mem size.
    TableConfig config;
    config.tenant_name = part_key;
    config.max_table_mem_size = SHANNON_SMALL_TABLE_MEMRORY_SIZE;
    auto sub_part_table = std::make_unique<Table>(m_source_table, config);

    // step 1: build indexes.
    if ((ret = sub_part_table.get()->create_index_memo(context))) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Build indexes memo for partition failed");
      return ret;
    }

    // step 2: set load type.
    sub_part_table.get()->set_load_type(LoadType::USER_LOADED);

    // step 4: Adding the Table meta obj into partitions table meta information.
    m_partitions.emplace(part_key, std::move(sub_part_table));
  }

  return ShannonBase::SHANNON_SUCCESS;
}
}  // namespace Imcs
}  // namespace ShannonBase
