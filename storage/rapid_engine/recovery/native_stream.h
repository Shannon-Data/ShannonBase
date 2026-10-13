// Copyright (c) 2026, Shannon Data AI and/or its affiliates.
// SPDX-License-Identifier: GPL-2.0-only
#ifndef SHANNONBASE_NATIVE_RECOVERY_STREAM_H
#define SHANNONBASE_NATIVE_RECOVERY_STREAM_H
#include <algorithm>
#include <cstdint>
#include <streambuf>

namespace ShannonBase::Recovery {
/** Read one length-delimited native column directly from the source stream.
 * A malformed column cannot consume the next column's header or payload.
 * No whole-column staging buffer is allocated here; the CU owns decoding.
 */
class NativeColumnStream final : public std::streambuf {
 public:
  NativeColumnStream(std::streambuf *source, uint64_t length) : m_source(source), m_remaining(length) {}
  uint64_t remaining() const { return m_remaining; }

 protected:
  std::streamsize xsgetn(char *destination, std::streamsize count) override {
    if (count <= 0 || !m_source) return 0;
    const auto bounded = static_cast<std::streamsize>(std::min<uint64_t>(m_remaining, count));
    const auto read = m_source->sgetn(destination, bounded);
    m_remaining -= static_cast<uint64_t>(read);
    return read;
  }
  int_type underflow() override { return m_remaining && m_source ? m_source->sgetc() : traits_type::eof(); }
  int_type uflow() override {
    const auto value = underflow();
    if (!traits_type::eq_int_type(value, traits_type::eof())) {
      m_source->sbumpc();
      --m_remaining;
    }
    return value;
  }

 private:
  std::streambuf *m_source;
  uint64_t m_remaining;
};
}  // namespace ShannonBase::Recovery
#endif
