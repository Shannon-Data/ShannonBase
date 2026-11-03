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

   Copyright (c) 2026, Shannon Data AI and/or its affiliates.

   Data processing unit: the vocabulary shared by every hardware backend.
*/

#ifndef __SHANNONBASE_DPU_KERNEL_H__
#define __SHANNONBASE_DPU_KERNEL_H__

#include <cstddef>
#include <cstdint>

namespace ShannonBase {
namespace Dpu {

/**
 * Kernels a backend may claim. One id per shape, never per SQL function: the
 * same SUM serves COUNT rewrites and SUM(DISTINCT) de-duplication afterwards.
 */
enum class KernelId : uint8_t {
  SUM = 0,
  MIN,
  MAX,
  COUNT_NON_NULL,
  FILTER,   // selection only, no value output
  PROJECT,  // copy or gather under an existing selection
  COUNT
};

/**
 * Element classes the batch layer can hand over without decoding. Everything
 * else (varlen payloads, dictionary ids, blobs, charset-dependent strings) stays
 * on the host; see dpu/readme.txt.
 */
enum class ElementType : uint8_t {
  NONE = 0,
  INT8,
  INT16,
  INT32,
  INT64,
  UINT8,
  UINT16,
  UINT32,
  UINT64,
  FLOAT,
  DOUBLE,
  DECIMAL_INT64,  // my_decimal widened to int64 by the caller, scale preserved
  COUNT
};

inline size_t ElementSize(ElementType type) noexcept {
  switch (type) {
    case ElementType::INT8:
    case ElementType::UINT8:
      return 1;
    case ElementType::INT16:
    case ElementType::UINT16:
      return 2;
    case ElementType::INT32:
    case ElementType::UINT32:
    case ElementType::FLOAT:
      return 4;
    case ElementType::INT64:
    case ElementType::UINT64:
    case ElementType::DOUBLE:
    case ElementType::DECIMAL_INT64:
      return 8;
    case ElementType::NONE:
    case ElementType::COUNT:
      break;
  }
  return 0;
}

inline const char *ElementTypeName(ElementType type) noexcept {
  switch (type) {
    case ElementType::NONE:
      return "none";
    case ElementType::INT8:
      return "int8";
    case ElementType::INT16:
      return "int16";
    case ElementType::INT32:
      return "int32";
    case ElementType::INT64:
      return "int64";
    case ElementType::UINT8:
      return "uint8";
    case ElementType::UINT16:
      return "uint16";
    case ElementType::UINT32:
      return "uint32";
    case ElementType::UINT64:
      return "uint64";
    case ElementType::FLOAT:
      return "float";
    case ElementType::DOUBLE:
      return "double";
    case ElementType::DECIMAL_INT64:
      return "decimal_int64";
    case ElementType::COUNT:
      break;
  }
  return "unknown";
}

inline const char *KernelName(KernelId kernel) noexcept {
  switch (kernel) {
    case KernelId::SUM:
      return "sum";
    case KernelId::MIN:
      return "min";
    case KernelId::MAX:
      return "max";
    case KernelId::COUNT_NON_NULL:
      return "count_non_null";
    case KernelId::FILTER:
      return "filter";
    case KernelId::PROJECT:
      return "project";
    case KernelId::COUNT:
      break;
  }
  return "unknown";
}

/**
 * One column of a batch.
 *
 * The layout is the one ColumnChunk already uses: `data` holds `rows * stride`
 * bytes, `null_mask` is a bit array where a SET bit means NULL (the same
 * convention as Utils::SIMD), and a null `null_mask` means the column has no
 * NULLs at all.
 *
 * `data` and `null_mask` are host memory owned by the caller for the duration of
 * the call. A backend may copy them out, it must never retain the pointers.
 */
struct ColumnView {
  const void *data{nullptr};
  const uint8_t *null_mask{nullptr};
  size_t rows{0};
  size_t stride{0};
  ElementType type{ElementType::NONE};

  bool Usable() const {
    return data != nullptr && rows != 0 && stride != 0 && type != ElementType::NONE && stride == ElementSize(type) &&
           rows <= SIZE_MAX / stride;
  }
};

/** Sparse row indexes into the input columns. Empty means the rows are dense. */
struct SelectionView {
  const uint32_t *rows{nullptr};
  size_t count{0};

  bool Sparse() const { return rows != nullptr && count != 0; }
};

/**
 * A complete, self-describing kernel call.
 *
 * `inputs` points at `input_count` ColumnViews for the duration of the call.
 * A request is immutable against a backend, so the optimizer can probe exactly
 * the object the executor will later execute.
 */
struct KernelRequest {
  KernelId kernel{KernelId::SUM};
  const ColumnView *inputs{nullptr};
  size_t input_count{0};
  SelectionView selection{};
  ElementType result_type{ElementType::NONE};
};

/**
 * Host output buffer owned by the caller.
 *
 * A backend fills it completely or reports UNCLAIMED without touching it.
 * Partially writing is a contract violation: the host path would then run on a
 * buffer that already holds part of the answer.
 */
struct KernelResult {
  void *out{nullptr};
  size_t out_bytes{0};
};

}  // namespace Dpu
}  // namespace ShannonBase

#endif  // __SHANNONBASE_DPU_KERNEL_H__
