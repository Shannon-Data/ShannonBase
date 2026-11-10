/**
   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   Differential tests for the hand-written kernels in utils/SIMD_Impl.h.

   Every SIMD specialization is compared against an independent scalar
   reference over sizes that cross every lane/byte boundary, NULL densities from
   none to all, and bitmaps whose padding bits past `row_count` are garbage (the
   API does not require them to be zero). The kernels are called directly, not
   through the dispatcher, so both ISA families are covered on one machine.

   Microbenchmarks.SimdNullMaskPath measures the same entry points against the
   scalar path; it is named Microbenchmarks so a CI run can skip it with
   --gtest_filter=-Microbenchmarks.* (the convention of gunit/benchmark.h).
*/

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <type_traits>
#include <vector>

#include "storage/rapid_engine/utils/SIMD.h"

namespace ShannonBase {
namespace {

using Utils::SIMD::CompareOp;

// ---------------------------------------------------------------------------
// Scalar reference. Bit set = NULL, matching ColumnChunk's null bitmap.
// ---------------------------------------------------------------------------

inline bool RefIsNull(const uint8_t *mask, size_t i) { return mask != nullptr && ((mask[i >> 3] >> (i & 7)) & 1u); }

template <typename T>
T RefSum(const T *data, const uint8_t *mask, size_t rows) {
  T total = 0;
  for (size_t i = 0; i < rows; ++i)
    if (!RefIsNull(mask, i)) total = static_cast<T>(total + data[i]);
  return total;
}

// The kernels return the fold identity when every row is NULL, so the reference
// must too: that is the contract being pinned, not a SQL NULL.
template <typename T>
T RefMin(const T *data, const uint8_t *mask, size_t rows) {
  T best = std::numeric_limits<T>::max();
  for (size_t i = 0; i < rows; ++i)
    if (!RefIsNull(mask, i) && data[i] < best) best = data[i];
  return best;
}

template <typename T>
T RefMax(const T *data, const uint8_t *mask, size_t rows) {
  T best = std::numeric_limits<T>::lowest();
  for (size_t i = 0; i < rows; ++i)
    if (!RefIsNull(mask, i) && data[i] > best) best = data[i];
  return best;
}

inline size_t RefCountNonNull(const uint8_t *mask, size_t rows) {
  if (mask == nullptr) return rows;
  size_t present = 0;
  for (size_t i = 0; i < rows; ++i)
    if (!RefIsNull(mask, i)) ++present;
  return present;
}

template <typename T>
bool Same(T actual, T expected) {
  if constexpr (std::is_floating_point_v<T>) {
    // SIMD reduction order differs from the scalar one, so allow the last bits.
    const double scale = std::max(1.0, std::fabs(static_cast<double>(expected)));
    return std::fabs(static_cast<double>(actual) - static_cast<double>(expected)) <= 1e-6 * scale;
  } else {
    return actual == expected;
  }
}

// ---------------------------------------------------------------------------
// Test data: lane-boundary sizes, every bitmap shape, garbage padding bits.
// ---------------------------------------------------------------------------

constexpr size_t kSizes[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 200};

// 0x00 = nothing NULL, 0xFF = everything NULL; the rest exercise partial bytes.
constexpr uint8_t kPatterns[] = {0x00, 0xFF, 0x01, 0x80, 0x55, 0xAA, 0xFE, 0x7F, 0x0F, 0xF0, 0x3C, 0xC3};

template <typename T>
std::vector<T> MakeValues(size_t rows) {
  std::vector<T> values(rows);
  for (size_t i = 0; i < rows; ++i) {
    if constexpr (std::is_floating_point_v<T>)
      values[i] = static_cast<T>((i % 97) * 0.25 - 12.0);
    else
      values[i] = static_cast<T>(static_cast<int64_t>(i % 97) - 48);
  }
  return values;
}

// `pattern` fills the bytes that actually hold rows; the padding bits of the
// last byte are forced to 1 so a kernel that counts them fails.
std::vector<uint8_t> MakeMask(size_t rows, uint8_t pattern, bool garbage_padding) {
  std::vector<uint8_t> mask((rows + 7) / 8, pattern);
  if (garbage_padding && rows % 8 != 0 && !mask.empty()) {
    const unsigned tail = static_cast<unsigned>(rows % 8);
    mask.back() |= static_cast<uint8_t>(~((1u << tail) - 1u));
  }
  return mask;
}

// ---------------------------------------------------------------------------
// Aggregate kernels: each specialization against the reference.
// ---------------------------------------------------------------------------

template <typename T, typename SumFn, typename MinFn, typename MaxFn>
void CheckAggregates(const char *name, SumFn sum_fn, MinFn min_fn, MaxFn max_fn) {
  for (size_t rows : kSizes) {
    const auto values = MakeValues<T>(rows);
    const T *data = values.empty() ? nullptr : values.data();
    const T ref_sum = RefSum(data, nullptr, rows);
    const T ref_min = RefMin(data, nullptr, rows);
    const T ref_max = RefMax(data, nullptr, rows);

    EXPECT_TRUE(Same(sum_fn(data, nullptr, rows), ref_sum)) << name << " sum/no mask/rows=" << rows;
    EXPECT_TRUE(Same(min_fn(data, nullptr, rows), ref_min)) << name << " min/no mask/rows=" << rows;
    EXPECT_TRUE(Same(max_fn(data, nullptr, rows), ref_max)) << name << " max/no mask/rows=" << rows;

    for (uint8_t pattern : kPatterns) {
      for (bool garbage : {false, true}) {
        const auto mask = MakeMask(rows, pattern, garbage);
        const uint8_t *m = mask.empty() ? nullptr : mask.data();
        EXPECT_TRUE(Same(sum_fn(data, m, rows), RefSum(data, m, rows)))
            << name << " sum/pattern=" << int(pattern) << "/rows=" << rows;
        EXPECT_TRUE(Same(min_fn(data, m, rows), RefMin(data, m, rows)))
            << name << " min/pattern=" << int(pattern) << "/rows=" << rows;
        EXPECT_TRUE(Same(max_fn(data, m, rows), RefMax(data, m, rows)))
            << name << " max/pattern=" << int(pattern) << "/rows=" << rows;
      }
    }
  }
}

template <typename T, typename CountFn>
void CheckCountNonNull(const char *name, CountFn count_fn) {
  for (size_t rows : kSizes) {
    EXPECT_EQ(count_fn(nullptr, rows), rows) << name << " no mask/rows=" << rows;
    for (uint8_t pattern : kPatterns) {
      for (bool garbage : {false, true}) {
        const auto mask = MakeMask(rows, pattern, garbage);
        const uint8_t *m = mask.empty() ? nullptr : mask.data();
        EXPECT_EQ(count_fn(m, rows), RefCountNonNull(m, rows))
            << name << " pattern=" << int(pattern) << "/rows=" << rows;
      }
    }
  }
}

template <typename T>
void CheckAgainstAllIsas(const char *name) {
  using SumFn = T (*)(const T *, const uint8_t *, size_t);
  CheckAggregates<T>(name, static_cast<SumFn>(&Utils::SIMD::sum<T>),
                     static_cast<SumFn>(&Utils::SIMD::min<T>), static_cast<SumFn>(&Utils::SIMD::max<T>));
}

// ---------------------------------------------------------------------------
// filter_compare_*: every comparison operator, sparse and dense.
// ---------------------------------------------------------------------------

template <typename T, typename FilterFn>
void CheckFilter(const char *name, FilterFn filter_fn) {
  const CompareOp ops[] = {CompareOp::EQ, CompareOp::NE, CompareOp::LT,
                           CompareOp::LE, CompareOp::GT, CompareOp::GE};
  for (size_t rows : kSizes) {
    auto values = MakeValues<T>(rows);
    const T *data = values.empty() ? nullptr : values.data();
    for (uint8_t pattern : {uint8_t{0x00}, uint8_t{0x55}, uint8_t{0xFF}}) {
      const auto mask = MakeMask(rows, pattern, true);
      const uint8_t *m = mask.empty() ? nullptr : mask.data();
      for (CompareOp op : ops) {
        for (T target : {T{0}, T{7}, T{-5}, T{100}}) {
          std::vector<size_t> expected;
          for (size_t i = 0; i < rows; ++i) {
            if (RefIsNull(m, i)) continue;
            const T v = data[i];
            const bool hit = op == CompareOp::EQ   ? v == target
                             : op == CompareOp::NE ? v != target
                             : op == CompareOp::LT ? v < target
                             : op == CompareOp::LE ? v <= target
                             : op == CompareOp::GT ? v > target
                                                   : v >= target;
            if (hit) expected.push_back(i);
          }
          std::vector<size_t> actual;
          filter_fn(data, m, rows, target, op, actual);
          EXPECT_EQ(actual, expected) << name << " rows=" << rows << " pattern=" << int(pattern)
                                      << " op=" << int(op);
        }
      }
    }
  }
}

TEST(SimdKernels, AggregatesMatchScalarReference) {
  // Dispatch path, then every specialization that this build can emit.
  CheckAggregates<float>("float/dispatch", &Utils::SIMD::sum<float>, &Utils::SIMD::min<float>,
                         &Utils::SIMD::max<float>);
  CheckAggregates<double>("double/dispatch", &Utils::SIMD::sum<double>, &Utils::SIMD::min<double>,
                          &Utils::SIMD::max<double>);
  CheckAggregates<int32_t>("int32/dispatch", &Utils::SIMD::sum<int32_t>, &Utils::SIMD::min<int32_t>,
                           &Utils::SIMD::max<int32_t>);
  CheckAggregates<int64_t>("int64/dispatch", &Utils::SIMD::sum<int64_t>, &Utils::SIMD::min<int64_t>,
                           &Utils::SIMD::max<int64_t>);
  CheckAggregates<float>("float/sse2", &Utils::SIMD::sum_sse2_float, &Utils::SIMD::min_sse2_float,
                         &Utils::SIMD::max_sse2_float);
  CheckAggregates<double>("double/sse2", &Utils::SIMD::sum_sse2_double, &Utils::SIMD::min_sse2_double,
                          &Utils::SIMD::max_sse2_double);
  CheckAggregates<int32_t>("int32/sse2", &Utils::SIMD::sum_sse2_int32, &Utils::SIMD::min_sse2_int32,
                           &Utils::SIMD::max_sse2_int32);
  CheckAggregates<int64_t>("int64/sse2", &Utils::SIMD::sum_sse2_int64, &Utils::SIMD::min_sse2_int64,
                           &Utils::SIMD::max_sse2_int64);
  CheckAggregates<float>("float/avx2", &Utils::SIMD::sum_avx2_float, &Utils::SIMD::min_avx2_float,
                         &Utils::SIMD::max_avx2_float);
  CheckAggregates<double>("double/avx2", &Utils::SIMD::sum_avx2_double, &Utils::SIMD::min_avx2_double,
                          &Utils::SIMD::max_avx2_double);
  CheckAggregates<int32_t>("int32/avx2", &Utils::SIMD::sum_avx2_int32, &Utils::SIMD::min_avx2_int32,
                           &Utils::SIMD::max_avx2_int32);
  CheckAggregates<int64_t>("int64/avx2", &Utils::SIMD::sum_avx2_int64, &Utils::SIMD::min_avx2_int64,
                           &Utils::SIMD::max_avx2_int64);
}

TEST(SimdKernels, CountNonNullMatchesScalarReference) {
  CheckCountNonNull<int>("dispatch", static_cast<size_t (*)(const uint8_t *, size_t)>(&Utils::SIMD::count_non_null));
#if defined(SHANNON_HAVE_SSE2)
  CheckCountNonNull<int>("sse2", &Utils::SIMD::count_non_null_sse2);
#endif
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  CheckCountNonNull<int>("avx2", &Utils::SIMD::count_non_null_avx2);
#endif
}

TEST(SimdKernels, FilterCompareMatchesScalarReference) {
  CheckFilter<int32_t>("int32", [](const int32_t *d, const uint8_t *m, size_t r, int32_t v, CompareOp op,
                                   std::vector<size_t> &out) {
    Utils::SIMD::filter_compare_int32(d, m, r, v, op, out);
  });
  CheckFilter<int64_t>("int64", [](const int64_t *d, const uint8_t *m, size_t r, int64_t v, CompareOp op,
                                   std::vector<size_t> &out) {
    Utils::SIMD::filter_compare_int64(d, m, r, v, op, out);
  });
}

// A bitmap whose padding bits are set must behave exactly like one whose
// padding is clear: nothing past row_count may be read as a row.
TEST(SimdKernels, PaddingBitsPastRowCountAreIgnored) {
  for (size_t rows : {1U, 7U, 9U, 33U, 65U}) {
    const auto values = MakeValues<int32_t>(rows);
    auto clean = MakeMask(rows, 0x00, false);
    auto dirty = MakeMask(rows, 0x00, true);
    ASSERT_NE(clean, dirty);
    EXPECT_EQ(Utils::SIMD::count_non_null(clean.data(), rows), rows);
    EXPECT_EQ(Utils::SIMD::count_non_null(dirty.data(), rows), rows);
    EXPECT_EQ(Utils::SIMD::sum<int32_t>(values.data(), dirty.data(), rows),
              Utils::SIMD::sum<int32_t>(values.data(), clean.data(), rows));
  }
}

// ---------------------------------------------------------------------------
// Gathered filters: one cell pointer per row in, LSB-first bitmap out. The
// index-output filter runs on the same block kernels, so it is pinned here too.
// ---------------------------------------------------------------------------

bool BitAt(const uint8_t *bits, size_t i) { return ((bits[i >> 3] >> (i & 7)) & 1u) != 0; }

bool RefCompareSigned(int64_t lhs, int64_t rhs, CompareOp op) {
  switch (op) {
    case CompareOp::EQ:
      return lhs == rhs;
    case CompareOp::NE:
      return lhs != rhs;
    case CompareOp::LT:
      return lhs < rhs;
    case CompareOp::LE:
      return lhs <= rhs;
    case CompareOp::GT:
      return lhs > rhs;
    case CompareOp::GE:
      return lhs >= rhs;
  }
  return false;
}

bool RefCompareUnsigned(uint64_t lhs, uint64_t rhs, CompareOp op) {
  switch (op) {
    case CompareOp::EQ:
      return lhs == rhs;
    case CompareOp::NE:
      return lhs != rhs;
    case CompareOp::LT:
      return lhs < rhs;
    case CompareOp::LE:
      return lhs <= rhs;
    case CompareOp::GT:
      return lhs > rhs;
    case CompareOp::GE:
      return lhs >= rhs;
  }
  return false;
}

bool RefCompareDouble(double lhs, double rhs, CompareOp op) {
  switch (op) {
    case CompareOp::EQ:
      return lhs == rhs;
    case CompareOp::NE:
      return lhs != rhs;
    case CompareOp::LT:
      return lhs < rhs;
    case CompareOp::LE:
      return lhs <= rhs;
    case CompareOp::GT:
      return lhs > rhs;
    case CompareOp::GE:
      return lhs >= rhs;
  }
  return false;
}

template <typename T>
size_t CallBitmapCompare(const std::vector<const unsigned char *> &cells, size_t rows, T bound, CompareOp op,
                         bool is_unsigned, uint8_t *bits) {
  if constexpr (std::is_same_v<T, int32_t>)
    return Utils::SIMD::filter_compare_int32_bitmap(cells.data(), rows, bound, op, is_unsigned, bits);
  else
    return Utils::SIMD::filter_compare_int64_bitmap(cells.data(), rows, bound, op, is_unsigned, bits);
}

template <typename T>
size_t CallBitmapRange(const std::vector<const unsigned char *> &cells, size_t rows, T lower, T upper,
                       bool is_unsigned, bool negate, uint8_t *bits) {
  if constexpr (std::is_same_v<T, int32_t>)
    return Utils::SIMD::filter_range_int32_bitmap(cells.data(), rows, lower, upper, is_unsigned, negate, bits);
  else
    return Utils::SIMD::filter_range_int64_bitmap(cells.data(), rows, lower, upper, is_unsigned, negate, bits);
}

template <typename T>
void CheckGatheredCompare(const char *name, bool is_unsigned) {
  const CompareOp ops[] = {CompareOp::EQ, CompareOp::NE, CompareOp::LT,
                           CompareOp::LE, CompareOp::GT, CompareOp::GE};
  for (size_t rows : {size_t{1}, size_t{7}, size_t{64}, size_t{65}, size_t{173}}) {
    std::vector<T> raw(rows);
    std::vector<const unsigned char *> cells(rows, nullptr);
    for (size_t i = 0; i < rows; ++i) {
      if constexpr (std::is_same_v<T, int32_t>) {
        raw[i] = is_unsigned ? static_cast<T>(static_cast<uint32_t>(i) * 2654435761u)
                             : static_cast<T>(static_cast<int64_t>(i) * 1000003 - 50000000);
      } else {
        raw[i] = is_unsigned ? static_cast<T>(static_cast<uint64_t>(i) * 0x9E3779B97F4A7C15ull)
                             : static_cast<T>(static_cast<int64_t>(i) * 1000003 - 50000000);
      }
      if (i % 5 != 3) cells[i] = reinterpret_cast<const unsigned char *>(&raw[i]);
    }
    const T bound = raw[rows / 2];
    std::vector<uint8_t> bits(rows / 8 + 2, 0);
    for (CompareOp op : ops) {
      std::fill(bits.begin(), bits.end(), 0xA5);  // sentinel: bits outside the rows must survive
      const size_t matched = CallBitmapCompare<T>(cells, rows, bound, op, is_unsigned, bits.data());
      size_t expected_count = 0;
      for (size_t i = 0; i < rows; ++i) {
        const bool hit =
            cells[i] != nullptr &&
            (is_unsigned
                 ? RefCompareUnsigned(static_cast<uint64_t>(static_cast<std::make_unsigned_t<T>>(raw[i])),
                                      static_cast<uint64_t>(static_cast<std::make_unsigned_t<T>>(bound)), op)
                 : RefCompareSigned(raw[i], bound, op));
        EXPECT_EQ(BitAt(bits.data(), i), hit) << name << " rows=" << rows << " unsigned=" << is_unsigned
                                              << " op=" << int(op);
        expected_count += hit ? 1u : 0u;
      }
      EXPECT_EQ(matched, expected_count) << name << " count rows=" << rows << " op=" << int(op);
      EXPECT_EQ(bits[rows / 8 + 1], 0xA5) << name << " trailing byte rows=" << rows;
      if (rows % 8 != 0) {
        const uint8_t used = static_cast<uint8_t>((1u << (rows % 8)) - 1u);
        EXPECT_EQ(static_cast<uint8_t>(bits[rows / 8] & ~used), static_cast<uint8_t>(0xA5 & ~used))
            << name << " padding bits rows=" << rows;
      }
    }
  }
}

template <typename T>
void CheckGatheredRange(const char *name, bool is_unsigned, bool negate) {
  for (size_t rows : {size_t{1}, size_t{17}, size_t{64}, size_t{130}}) {
    std::vector<T> raw(rows);
    std::vector<const unsigned char *> cells(rows, nullptr);
    for (size_t i = 0; i < rows; ++i) {
      raw[i] = static_cast<T>(static_cast<int64_t>(i) * 37 - 2400);
      if (i % 4 != 2) cells[i] = reinterpret_cast<const unsigned char *>(&raw[i]);
    }
    const T lower = static_cast<T>(-500), upper = static_cast<T>(900);
    std::vector<uint8_t> bits(rows / 8 + 2, 0);
    const size_t matched = CallBitmapRange<T>(cells, rows, lower, upper, is_unsigned, negate, bits.data());
    size_t expected_count = 0;
    for (size_t i = 0; i < rows; ++i) {
      const bool inside =
          is_unsigned ? (RefCompareUnsigned(static_cast<uint64_t>(static_cast<std::make_unsigned_t<T>>(raw[i])),
                                            static_cast<uint64_t>(static_cast<std::make_unsigned_t<T>>(lower)),
                                            CompareOp::GE) &&
                         RefCompareUnsigned(static_cast<uint64_t>(static_cast<std::make_unsigned_t<T>>(raw[i])),
                                            static_cast<uint64_t>(static_cast<std::make_unsigned_t<T>>(upper)),
                                            CompareOp::LE))
                      : (raw[i] >= lower && raw[i] <= upper);
      const bool hit = cells[i] != nullptr && (negate ? !inside : inside);
      EXPECT_EQ(BitAt(bits.data(), i), hit)
          << name << " rows=" << rows << " unsigned=" << is_unsigned << " negate=" << negate;
      expected_count += hit ? 1u : 0u;
    }
    EXPECT_EQ(matched, expected_count) << name << " range count rows=" << rows;
  }
}

TEST(SimdGatheredFilters, IntegerCompareMatchesScalarReference) {
  for (bool is_unsigned : {false, true}) {
    CheckGatheredCompare<int32_t>("int32", is_unsigned);
    CheckGatheredCompare<int64_t>("int64", is_unsigned);
  }
}

TEST(SimdGatheredFilters, IntegerRangeMatchesScalarReference) {
  for (bool is_unsigned : {false, true}) {
    for (bool negate : {false, true}) {
      CheckGatheredRange<int32_t>("int32", is_unsigned, negate);
      CheckGatheredRange<int64_t>("int64", is_unsigned, negate);
    }
  }
}

TEST(SimdGatheredFilters, NullFilterMatchesScalarReference) {
  for (size_t rows : {size_t{1}, size_t{7}, size_t{64}, size_t{65}, size_t{173}}) {
    std::vector<int32_t> raw(rows);
    std::vector<const unsigned char *> cells(rows, nullptr);
    for (size_t i = 0; i < rows; ++i) {
      raw[i] = static_cast<int32_t>(i);
      if (i % 3 != 0) cells[i] = reinterpret_cast<const unsigned char *>(&raw[i]);
    }
    for (bool negate : {false, true}) {
      std::vector<uint8_t> bits(rows / 8 + 2, 0);
      const size_t matched = Utils::SIMD::filter_null_bitmap(cells.data(), rows, negate, bits.data());
      size_t expected_count = 0;
      for (size_t i = 0; i < rows; ++i) {
        const bool hit = (cells[i] == nullptr) != negate;
        EXPECT_EQ(BitAt(bits.data(), i), hit) << "rows=" << rows << " negate=" << negate;
        expected_count += hit ? 1u : 0u;
      }
      EXPECT_EQ(matched, expected_count) << "rows=" << rows << " negate=" << negate;
    }
  }
}

TEST(SimdGatheredFilters, DecodedDoubleFiltersMatchScalarReference) {
  constexpr size_t rows = 173;
  std::vector<double> wide(rows);
  std::vector<float> narrow(rows);
  std::vector<const unsigned char *> wide_cells(rows, nullptr), narrow_cells(rows, nullptr);
  for (size_t i = 0; i < rows; ++i) {
    wide[i] = static_cast<double>(i) * 0.25 - 12.5;
    narrow[i] = static_cast<float>(wide[i]);
    if (i % 6 != 4) {
      wide_cells[i] = reinterpret_cast<const unsigned char *>(&wide[i]);
      narrow_cells[i] = reinterpret_cast<const unsigned char *>(&narrow[i]);
    }
  }
  const auto decode_double = [](const unsigned char *p) {
    double v;
    std::memcpy(&v, p, sizeof(v));
    return v;
  };
  const auto decode_float = [](const unsigned char *p) {
    float v;
    std::memcpy(&v, p, sizeof(v));
    return static_cast<double>(v);
  };
  std::vector<uint8_t> bits(rows / 8 + 2, 0);
  for (CompareOp op : {CompareOp::EQ, CompareOp::NE, CompareOp::LT, CompareOp::LE, CompareOp::GT, CompareOp::GE}) {
    std::fill(bits.begin(), bits.end(), 0);
    Utils::SIMD::filter_compare_double_bitmap(wide_cells.data(), rows, 5.0, op, decode_double, bits.data());
    for (size_t i = 0; i < rows; ++i)
      EXPECT_EQ(BitAt(bits.data(), i), wide_cells[i] != nullptr && RefCompareDouble(wide[i], 5.0, op))
          << "double op=" << int(op);

    std::fill(bits.begin(), bits.end(), 0);
    Utils::SIMD::filter_compare_double_bitmap(narrow_cells.data(), rows, 5.0, op, decode_float, bits.data());
    for (size_t i = 0; i < rows; ++i)
      EXPECT_EQ(BitAt(bits.data(), i), narrow_cells[i] != nullptr && RefCompareDouble(narrow[i], 5.0, op))
          << "float op=" << int(op);
  }
  std::fill(bits.begin(), bits.end(), 0);
  Utils::SIMD::filter_range_double_bitmap(wide_cells.data(), rows, -2.0, 6.0, false, decode_double, bits.data());
  for (size_t i = 0; i < rows; ++i)
    EXPECT_EQ(BitAt(bits.data(), i), wide_cells[i] != nullptr && wide[i] >= -2.0 && wide[i] <= 6.0);
}

// The index-output filter and the bitmap filter are the same kernels, so they
// must agree row for row on the same data.
TEST(SimdGatheredFilters, IndexFilterMatchesBitmapFilter) {
  constexpr size_t rows = 173;
  std::vector<int32_t> raw(rows);
  std::vector<const unsigned char *> cells(rows, nullptr);
  std::vector<uint8_t> null_mask((rows + 7) / 8, 0);
  for (size_t i = 0; i < rows; ++i) {
    raw[i] = static_cast<int32_t>(i) - 80;
    if (i % 4 == 1) {
      null_mask[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
    } else {
      cells[i] = reinterpret_cast<const unsigned char *>(&raw[i]);
    }
  }
  for (CompareOp op : {CompareOp::EQ, CompareOp::NE, CompareOp::LT, CompareOp::LE, CompareOp::GT, CompareOp::GE}) {
    std::vector<uint8_t> bits(rows / 8 + 2, 0);
    Utils::SIMD::filter_compare_int32_bitmap(cells.data(), rows, -20, op, false, bits.data());
    std::vector<size_t> indices;
    Utils::SIMD::filter_compare_int32(raw.data(), null_mask.data(), rows, -20, op, indices);
    size_t seen = 0;
    for (size_t i = 0; i < rows; ++i) {
      if (!BitAt(bits.data(), i)) continue;
      ASSERT_LT(seen, indices.size());
      EXPECT_EQ(indices[seen], i) << "op=" << int(op);
      ++seen;
    }
    EXPECT_EQ(seen, indices.size()) << "op=" << int(op);
  }
}

TEST(SimdGatheredFilters, OrderedNaNComparisonsAgreeWithScalarAndSseBackends) {
  alignas(32) std::array<double, Utils::SIMD::detail::kCmpBlockRows> block{};
  block[0] = std::numeric_limits<double>::quiet_NaN();
  const Utils::SIMD::detail::CmpRequest unequal{CompareOp::NE, false, false};
  const Utils::SIMD::detail::CmpRequest outside{CompareOp::GE, true, true};
  EXPECT_EQ(0u, Utils::SIMD::detail::cmp_block_scalar<double>(block.data(), 0.0, 1.0, unequal));
  EXPECT_EQ(0u, Utils::SIMD::detail::cmp_block_scalar<double>(block.data(), 0.0, 1.0, outside));
#if defined(SHANNON_HAVE_SSE2)
  EXPECT_EQ(0u, Utils::SIMD::detail::cmp_block_sse_f64(block.data(), 0.0, 1.0, unequal));
  EXPECT_EQ(0u, Utils::SIMD::detail::cmp_block_sse_f64(block.data(), 0.0, 1.0, outside));
#endif
  const std::vector<double> values{std::numeric_limits<double>::quiet_NaN(), -1.0, -0.0, 0.0, 1.0,
                                   std::numeric_limits<double>::infinity()};
  const auto decode = [](const unsigned char *cell) {
    double value;
    std::memcpy(&value, cell, sizeof(value));
    return value;
  };
  for (size_t rows : {size_t{1}, size_t{3}, size_t{63}, size_t{64}, size_t{65}, size_t{129}}) {
    std::vector<const unsigned char *> cells(rows);
    for (size_t row = 0; row < rows; ++row)
      cells[row] = row % 11 == 10 ? nullptr : reinterpret_cast<const unsigned char *>(&values[row % values.size()]);
    std::vector<uint8_t> bits((rows + 7) / 8, 0);
    for (auto bounds : {std::pair{0.0, 1.0}, std::pair{1.0, 0.0},
                        std::pair{std::numeric_limits<double>::quiet_NaN(), 1.0},
                        std::pair{0.0, std::numeric_limits<double>::quiet_NaN()}}) {
      Utils::SIMD::filter_range_double_bitmap(cells.data(), rows, bounds.first, bounds.second, true, decode, bits.data());
      for (size_t row = 0; row < rows; ++row) {
        const double value = values[row % values.size()];
        EXPECT_EQ(cells[row] != nullptr && (value < bounds.first || value > bounds.second), BitAt(bits.data(), row));
      }
    }
    Utils::SIMD::filter_compare_double_bitmap(cells.data(), rows, 0.0, CompareOp::NE, decode, bits.data());
    for (size_t row = 0; row < rows; ++row) {
      const double value = values[row % values.size()];
      EXPECT_EQ(cells[row] != nullptr && !std::isnan(value) && value != 0.0, BitAt(bits.data(), row));
    }
  }
}

// ---------------------------------------------------------------------------
// Null-mask throughput: the dispatched kernel against the scalar fallback on
// the same data. Timings are printed, not asserted -- the assertion is that the
// fast path returns the reference result.
// ---------------------------------------------------------------------------

volatile uint64_t g_bench_sink = 0;

template <typename Fn>
double TimeNsPerRow(size_t rows, int reps, Fn &&fn) {
  for (int warmup = 0; warmup < 3; ++warmup) g_bench_sink += static_cast<uint64_t>(fn());
  double best = std::numeric_limits<double>::max();
  for (int r = 0; r < reps; ++r) {
    const auto start = std::chrono::steady_clock::now();
    g_bench_sink += static_cast<uint64_t>(fn());
    const auto stop = std::chrono::steady_clock::now();
    best = std::min(best, std::chrono::duration<double, std::nano>(stop - start).count());
  }
  return best / static_cast<double>(rows);
}

struct BenchPattern {
  const char *label;
  uint8_t bits;
  bool masked;
};

constexpr BenchPattern kBenchPatterns[] = {
    {"no nulls", 0x00, false}, {"half null", 0x55, true}, {"all null", 0xFF, true}};

template <typename T>
void BenchAggregates(const char *name, size_t rows, int reps) {
  const auto values = MakeValues<T>(rows);
  const T *data = values.data();
  for (const auto &pattern : kBenchPatterns) {
    const auto mask = MakeMask(rows, pattern.bits, false);
    const uint8_t *m = pattern.masked ? mask.data() : nullptr;

    const double scalar =
        TimeNsPerRow(rows, reps, [&] { return static_cast<double>(Utils::SIMD::sum_generic<T>(data, m, rows)); });
    const double sum = TimeNsPerRow(rows, reps, [&] { return static_cast<double>(Utils::SIMD::sum<T>(data, m, rows)); });
    const double min = TimeNsPerRow(rows, reps, [&] { return static_cast<double>(Utils::SIMD::min<T>(data, m, rows)); });
    const double max = TimeNsPerRow(rows, reps, [&] { return static_cast<double>(Utils::SIMD::max<T>(data, m, rows)); });

    EXPECT_TRUE(Same(Utils::SIMD::sum<T>(data, m, rows), RefSum(data, m, rows))) << name << " sum " << pattern.label;
    EXPECT_TRUE(Same(Utils::SIMD::min<T>(data, m, rows), RefMin(data, m, rows))) << name << " min " << pattern.label;
    EXPECT_TRUE(Same(Utils::SIMD::max<T>(data, m, rows), RefMax(data, m, rows))) << name << " max " << pattern.label;

    printf("  %-7s %-9s  sum scalar %6.3f vector %6.3f (x%5.2f)  min %6.3f  max %6.3f  ns/row\n", name,
           pattern.label, scalar, sum, scalar / sum, min, max);
  }
}

void BenchCountAndFilter(size_t rows, int reps) {
  const auto mask = MakeMask(rows, 0x55, true);
  const double count_scalar = TimeNsPerRow(
      rows, reps, [&] { return static_cast<double>(Utils::SIMD::count_non_null_generic(mask.data(), rows)); });
  const double count =
      TimeNsPerRow(rows, reps, [&] { return static_cast<double>(Utils::SIMD::count_non_null(mask.data(), rows)); });
  EXPECT_EQ(Utils::SIMD::count_non_null(mask.data(), rows), RefCountNonNull(mask.data(), rows));
  printf("  count_non_null half null  scalar %6.3f vector %6.3f (x%5.2f) ns/row\n", count_scalar, count,
         count_scalar / count);

  const auto i32 = MakeValues<int32_t>(rows);
  const auto i64 = MakeValues<int64_t>(rows);
  std::vector<size_t> out;
  out.reserve(rows);

  const double eq_scalar = TimeNsPerRow(rows, reps, [&] {
    out.clear();
    return static_cast<double>(Utils::SIMD::filter_generic<int32_t>(
        i32.data(), mask.data(), rows, [](int32_t v) { return v == 0; }, out));
  });
  const double eq_vector = TimeNsPerRow(rows, reps, [&] {
    out.clear();
    return static_cast<double>(Utils::SIMD::filter_eq_int32(i32.data(), mask.data(), rows, 0, out));
  });
  const double lt_scalar = TimeNsPerRow(rows, reps, [&] {
    out.clear();
    return static_cast<double>(Utils::SIMD::filter_generic<int32_t>(
        i32.data(), mask.data(), rows, [](int32_t v) { return v < 0; }, out));
  });
  const double lt32 = TimeNsPerRow(rows, reps, [&] {
    out.clear();
    return static_cast<double>(Utils::SIMD::filter_compare_int32(i32.data(), mask.data(), rows, 0, CompareOp::LT, out));
  });
  const double lt64 = TimeNsPerRow(rows, reps, [&] {
    out.clear();
    return static_cast<double>(Utils::SIMD::filter_compare_int64(i64.data(), mask.data(), rows, 0, CompareOp::LT, out));
  });
  printf("  filter     half null  eq scalar %6.3f vector %6.3f (x%5.2f)  lt32 %6.3f  lt64 %6.3f (scalar %6.3f) ns/row\n",
         eq_scalar, eq_vector, eq_scalar / eq_vector, lt32, lt64, lt_scalar);
}

TEST(Microbenchmarks, SimdNullMaskPath) {
  // One realistic IMCU scan batch; the numbers are per row.
  constexpr size_t rows = 8192;
  constexpr int reps = 25;
  printf("simd mask bench: rows=%zu reps=%d dispatch=%d\n", rows, reps, static_cast<int>(Utils::SIMD::simd_support()));
  BenchAggregates<float>("float", rows, reps);
  BenchAggregates<double>("double", rows, reps);
  BenchAggregates<int32_t>("int32", rows, reps);
  BenchAggregates<int64_t>("int64", rows, reps);
  BenchCountAndFilter(rows, reps);
}

}  // namespace
}  // namespace ShannonBase
