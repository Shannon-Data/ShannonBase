/*
  Copyright (c) 2022, 2023, Oracle and/or its affiliates.

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
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "classic_frame.h"

#include "classic_connection_base.h"
#include "classic_session_tracker.h"
#include "mysql/harness/tls_error.h"

static bool has_frame_header(ClassicProtocolState *src_protocol) {
  return src_protocol->current_frame().has_value();
}

static bool has_msg_type(ClassicProtocolState *src_protocol) {
  return src_protocol->current_msg_type().has_value();
}

/**
 * ensure message has a frame-header and msg-type.
 *
 * @retval true if src-protocol's recv-buffer has frame-header and msg-type.
 */
stdx::expected<void, std::error_code> ClassicFrame::ensure_has_msg_prefix(
    Channel *src_channel, ClassicProtocolState *src_protocol) {
  if (has_frame_header(src_protocol) && has_msg_type(src_protocol)) return {};

  if (!has_frame_header(src_protocol)) {
    auto decode_frame_res = ensure_frame_header(src_channel, src_protocol);
    if (!decode_frame_res) {
      return decode_frame_res.get_unexpected();
    }
  }

  if (!has_msg_type(src_protocol)) {
    auto &current_frame = src_protocol->current_frame().value();

    if (current_frame.frame_size_ < 5) {
      // expected a frame with at least one msg-type-byte
      return stdx::make_unexpected(make_error_code(std::errc::bad_message));
    }

    if (current_frame.forwarded_frame_size_ >= 4) {
      return stdx::make_unexpected(make_error_code(std::errc::bad_message));
    }

    const size_t msg_type_pos = 4 - current_frame.forwarded_frame_size_;

    auto &recv_buf = src_channel->recv_plain_view();
    if (msg_type_pos >= recv_buf.size()) {
      // read some more data.
      auto read_res = src_channel->read_to_plain(1);
      if (!read_res) return read_res.get_unexpected();

      if (msg_type_pos >= recv_buf.size()) {
        return stdx::make_unexpected(make_error_code(TlsErrc::kWantRead));
      }
    }

    src_protocol->current_msg_type() = recv_buf[msg_type_pos];
  }

#if defined(DEBUG_IO)
  std::cerr << __LINE__ << ": "
            << "seq-id: " << (int)src_protocol->current_frame()->seq_id_
            << ", frame-size: "
            << (int)src_protocol->current_frame()->frame_size_
            << ", msg-type: " << (int)src_protocol->current_msg_type().value()
            << "\n";
#endif

  return {};
}

static stdx::expected<std::pair<size_t, ClassicProtocolState::FrameInfo>,
                      std::error_code>
decode_frame_header(const net::const_buffer &recv_buf) {
  const auto decode_res =
      classic_protocol::decode<classic_protocol::frame::Header>(
          net::buffer(recv_buf), 0);
  if (!decode_res) {
    const auto ec = decode_res.error();

    if (ec == classic_protocol::codec_errc::not_enough_input) {
      return stdx::make_unexpected(make_error_code(TlsErrc::kWantRead));
    }
    return decode_res.get_unexpected();
  }

  const auto frame_header_res = decode_res.value();
  const auto header_size = frame_header_res.first;
  const auto seq_id = frame_header_res.second.seq_id();
  const auto payload_size = frame_header_res.second.payload_size();

  const auto frame_size = header_size + payload_size;

  return {std::in_place, header_size,
          ClassicProtocolState::FrameInfo{seq_id, frame_size, 0u}};
}

/**
 * ensure current_frame() has a current frame-info.
 *
 * @post after success returned, src_protocol->current_frame() has a frame
 * decoded.
 */
stdx::expected<void, std::error_code> ClassicFrame::ensure_frame_header(
    Channel *src_channel, ClassicProtocolState *src_protocol) {
  auto &recv_buf = src_channel->recv_plain_view();

  const size_t min_size{4};
  const auto cur_size = recv_buf.size();
  if (cur_size < min_size) {
    // read the rest of the header.
    auto read_res = src_channel->read_to_plain(min_size - cur_size);
    if (!read_res) return read_res.get_unexpected();

    if (recv_buf.size() < min_size) {
      return stdx::make_unexpected(make_error_code(TlsErrc::kWantRead));
    }
  }

  const auto decode_frame_res = decode_frame_header(net::buffer(recv_buf));
  if (!decode_frame_res) return decode_frame_res.get_unexpected();

  src_protocol->current_frame() = decode_frame_res->second;

  return {};
}

[[nodiscard]] stdx::expected<void, std::error_code>
ClassicFrame::ensure_has_full_frame(Channel *src_channel,
                                    ClassicProtocolState *src_protocol) {
  harness_assert(src_protocol->current_frame());

  auto &current_frame = src_protocol->current_frame().value();
  auto &recv_buf = src_channel->recv_plain_view();

  const auto min_size = current_frame.frame_size_;
  const auto cur_size = recv_buf.size();
  if (cur_size >= min_size) return {};

  auto read_res = src_channel->read_to_plain(min_size - cur_size);
  if (!read_res) return read_res.get_unexpected();

  if (recv_buf.size() >= min_size) return {};

  return stdx::make_unexpected(make_error_code(TlsErrc::kWantRead));
}

[[nodiscard]] stdx::expected<void, std::error_code>
ClassicFrame::recv_frame_sequence(Channel *src_channel,
                                  ClassicProtocolState *src_protocol) {
  auto &recv_buf = src_channel->recv_plain_view();

  bool expect_header{true};  // toggle between header and payload
  const size_t hdr_size{4};
  size_t expected_size{hdr_size};
  bool is_multi_frame{true};
  uint8_t seq_id{};

  src_protocol->current_frame().reset();

  for (;;) {
    // fill the recv-buf with the expected bytes.
    if (recv_buf.size() < expected_size) {
      auto read_res =
          src_channel->read_to_plain(expected_size - recv_buf.size());
      if (!read_res) return read_res.get_unexpected();

      if (recv_buf.size() < expected_size) {
        return stdx::make_unexpected(make_error_code(TlsErrc::kWantRead));
      }
    }

    if (expect_header) {
      const auto hdr_res =
          classic_protocol::decode<classic_protocol::frame::Header>(
              net::buffer(recv_buf) + (expected_size - hdr_size), 0);
      if (!hdr_res) return hdr_res.get_unexpected();

      auto hdr = *hdr_res;
      seq_id = hdr.second.seq_id();
      auto payload_size = hdr.second.payload_size();

      // expected to read the payload next
      expected_size += payload_size;

      if (!src_protocol->current_frame()) {
        // remember the first frame.
        src_protocol->current_frame() =
            ClassicProtocolState::FrameInfo{seq_id, expected_size, 0};
      }

      expect_header = false;

      // remember if there is another frame after this one.
      is_multi_frame = (payload_size == 0xffffff);
    } else {
      // payload.
      if (is_multi_frame) {
        // as the last frame was 0xffffff, expected a header again.
        expect_header = true;

        expected_size += hdr_size;
      } else {
        src_protocol->seq_id(seq_id);
        return {};
      }
    }
  }
}

template <class Msg>
inline void trace_set_attributes_impl(TraceEvent *ev,
                                      ClassicProtocolState *src_protocol,
                                      const Msg &msg) {
  if (ev == nullptr) return;

  if (msg.last_insert_id() != 0) {
    ev->attrs.emplace_back("mysql.response.last_insert_id",
                           static_cast<int64_t>(msg.last_insert_id()));
  }
  if (msg.warning_count() != 0) {
    ev->attrs.emplace_back("mysql.response.warning_count",
                           static_cast<int64_t>(msg.warning_count()));
  }
  if (msg.affected_rows() != 0) {
    ev->attrs.emplace_back("mysql.response.affected_rows",
                           static_cast<int64_t>(msg.affected_rows()));
  }

  if (!msg.session_changes().empty()) {
    auto sess_tracker_res =
        session_trackers_to_string(net::buffer(msg.session_changes()),
                                   src_protocol->shared_capabilities());

    if (sess_tracker_res) {
      for (auto kv : *sess_tracker_res) {
        if (kv.first == "schema") {
          // use a common name of schema from OTEL.
          ev->attrs.emplace_back("db.name", kv.second);
        } else {
          ev->attrs.emplace_back("mysql.session." + kv.first, kv.second);
        }
      }
    }
  }
}

void ClassicFrame::trace_set_attributes(
    TraceEvent *ev, ClassicProtocolState *src_protocol,
    const classic_protocol::borrowed::message::server::Ok &msg) {
  trace_set_attributes_impl(ev, src_protocol, msg);
}

void ClassicFrame::trace_set_attributes(
    TraceEvent *ev, ClassicProtocolState *src_protocol,
    const classic_protocol::borrowed::message::server::Eof &msg) {
  trace_set_attributes_impl(ev, src_protocol, msg);
}

void ClassicFrame::trace_set_attributes(
    TraceEvent *ev, ClassicProtocolState * /* src_protocol */,
    const classic_protocol::borrowed::message::server::Error &msg) {
  if (ev == nullptr) return;

  ev->attrs.emplace_back("mysql.error_code",
                         static_cast<int64_t>(msg.error_code()));

  ev->attrs.emplace_back("mysql.error_message", std::string(msg.message()));
}
