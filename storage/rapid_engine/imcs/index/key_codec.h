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

   Copyright (c) 2023, 2026, Shannon Data AI and/or its affiliates.

   Thin compiled MySQL row/KEY -> Rapid ART key codec.

   KEY_INFO/KEY_PART_INFO is interpreted once while the Rapid table metadata is
   built.  Execution then sees only ArtIndexDescriptor and two index-level
   capabilities:

    EXACT   - encoded equality is safe for point/ref lookup.
    ORDERED - EXACT plus byte-wise ART order is equivalent to MySQL key order.

   The physical ART exists independently of ORDERED capability.  Codec-specific
   details remain private implementation details rather than leaking into the
   handler/optimizer paths.
*/
#ifndef __SHANNONBASE_RAPID_KEY_CODEC_H__
#define __SHANNONBASE_RAPID_KEY_CODEC_H__

#include <cstddef>
#include <cstdint>
#include <vector>

#include "my_inttypes.h"
#include "sql/field.h"

namespace ShannonBase {
namespace Imcs {

struct ArtKeyPartDescriptor;
struct ArtIndexDescriptor;

namespace Index {

/** Capability of the complete compiled ART key. */
enum class ArtKeyMode : uint8_t {
  EXACT = 0,
  ORDERED,
};

/** Private implementation choice compiled for one MySQL key part. */
enum class ArtKeyPartCodec : uint8_t {
  // Preserve MySQL's key image for equality. Ordering is not advertised.
  KEY_IMAGE = 0,
  // Fixed-width numeric/decimal bytes normalized for lexicographic order.
  SORTABLE_VALUE,
  // MySQL collation weight string produced by strnxfrm().
  COLLATION_WEIGHT,
};

/**
 * Canonical Rapid ART key codec.
 *
 * Each logical key part is framed as:
 *
 *   ASC:  [NULL=00 | NONNULL=01] [01,b0] [01,b1] ... [00]
 *   DESC: bitwise NOT of the complete ASC part
 *
 * Framing is shared by EXACT and ORDERED indexes. It makes multipart-prefix
 * equality unambiguous; when every part is ORDERED it also preserves MySQL
 * range/order semantics.
 */
class RapidKeyCodec final {
 public:
  using KeyBuffer = std::vector<uchar>;

  // Compile codec-specific fields of one already-laid-out key part. index_mode
  // must start as ORDERED and is downgraded to EXACT when this part only has an
  // equality-safe representation.
  static bool CompilePart(ArtKeyPartDescriptor *part, ArtKeyMode *index_mode);

  // Row/index-build side.
  static bool EncodeRowKey(const ArtIndexDescriptor &index_desc, const uchar *rowdata, const ulong *col_offsets,
                           const ulong *null_byte_offsets, const ulong *null_bitmasks, KeyBuffer *out);

  // Return the minimum number of backing handler-key bytes that MySQL key_cmp()
  // may inspect for this logical key length. This is used only to deep-copy
  // range boundaries safely; normal point lookup callers do not need it.
  static bool RequiredSearchImageLength(const ArtIndexDescriptor &index_desc, const uchar *mysql_key,
                                        uint mysql_key_len, uint *image_len);

  // Handler point/range side. logical_key_len preserves MySQL key_range
  // semantics (which key parts participate); backing_image_len is only a
  // memory-safety bound for the actual handler-owned/copied key image.
  static bool EncodeSearchKey(const ArtIndexDescriptor &index_desc, const uchar *mysql_key, uint logical_key_len,
                              uint backing_image_len, KeyBuffer *out);

 private:
  static bool IsCollatedTextField(const Field *field);
  static bool IsBlobField(const Field *field);
  static uint32 PrefixCharacters(const Field *field, uint16_t part_length);
  static bool TransformCollation(const ArtKeyPartDescriptor &part, const uchar *src, size_t src_len, KeyBuffer *out);
  static bool EncodeSortableValue(const ArtKeyPartDescriptor &part, const uchar *source, bool db_low_byte_first,
                                  bool row_image, KeyBuffer *out);
  static bool EncodeRowPart(const ArtIndexDescriptor &index_desc, size_t part_no, const uchar *source,
                            KeyBuffer *part_bytes);
  static bool EncodeSearchPart(const ArtIndexDescriptor &index_desc, size_t part_no, const uchar *source,
                               uint source_len, KeyBuffer *part_bytes);
  static void AppendPart(const KeyBuffer &part_bytes, bool is_null, bool descending, KeyBuffer *out);
};

}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase

#endif  // __SHANNONBASE_RAPID_KEY_CODEC_H__
