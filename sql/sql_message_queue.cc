/* Copyright (c) 2007, 2020, Oracle and/or its affiliates. All rights reserved.

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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates. */

#include "sql_message_queue.h"
// #include <arm_neon.h>

#ifndef NDEBUG
bool dbug_pq_worker_stall = 0;
#endif

/**
 * send data of nbytes to MQ
 * @nbytes
 * @data
 * @written: number of bytes that have written into MQ
 * @nowait: the mode of write data, i.e., blocking/non-blocking mode
 *
 * @return the send status
 */
MQ_RESULT MQueue_handle::send_bytes(uint32 nbytes, const void *data, uint32 *written, bool nowait) {
  uint32 used;
  uint32 ringsize = m_queue->m_ring_size;
  uint32 sent = 0;
  uint64 available = 0;

  /** only worker thread can send data to message queue */
  THD *thd = current_thd;
  assert(!m_queue->m_sender_event->PQ_caller || thd == m_queue->m_sender_event->PQ_caller);

  uint64 rb, wb;
  while (!thd->is_pq_error() && (sent < nbytes)) {
    /** atomically obtain the read position */
    rb = atomic_read_u64(&m_queue->m_bytes_read);
    /** atomically obtain the write position */
    wb = atomic_read_u64(&m_queue->m_bytes_written);
    assert(wb >= rb);

    used = wb - rb;
    assert(used <= ringsize);
    // uint32x2_t v_a = {ringsize, nbytes};
    // uint32x2_t v_b = {used, sent};
    // uint64x2_t v = vsubl_u32(v_a, v_b);
    // uint64 m_a = vgetq_lane_u64(v, 0);
    // uint64 m_b = vgetq_lane_u64(v, 1);

    uint64 m_a = (uint64)(ringsize - used);
    uint64 m_b = (uint64)(nbytes - sent);
    available = std::min(m_a, m_b);

    compiler_barrier();
    if (m_queue->detached == MQ_HAVE_DETACHED) {
      *written = sent;
      return MQ_DETACHED;
    }

    if (m_queue->detached == MQ_TMP_DETACHED) {
      *written = sent;
      return MQ_SUCCESS;
    }

    /**
     * Considering the case that available = 0: sender first notifies receiver
     * to receive data from MQ. If nowait = false, then sender enters into a
     * blocking status until receiver has received data from MQ;  otherwise,
     * sender directly returns.
     */
    if (available == 0) {
      /** notify receiver to receive data from MQ */
      end_wait(m_queue->m_receiver_event);
      /** blocking mode by default, i.e, nowait = false */
      if (nowait) {
        *written = sent;
        return MQ_WOULD_BLOCK;
      }
      /** sender enters into the blocking wait status */
      set_wait(m_queue->m_sender_event);
      /** reset the wait status */
      reset_wait(m_queue->m_sender_event);
    } else {
      uint32 offset;
      uint32 sent_once;

      /** compute the real write position in ring array */
      offset = MOD(wb, ringsize);
      sent_once = std::min((uint32)available, ringsize - offset);

      /** this barrier ensures that memcpy() is finished before end_wait() */
      memory_barrier();
      memcpy(&m_queue->m_buffer[offset], reinterpret_cast<char *>(const_cast<void *>(data)) + sent, sent_once);
      sent += sent_once;

      /** atomically update the write position */
      atomic_inc_bytes_written(sent_once);
      /** notify receiver to receive data */
      end_wait(m_queue->m_receiver_event);
    }
#ifndef NDEBUG
    if (dbug_pq_worker_stall) {
      sleep(1);  // 1 second
    }
#endif
  }

  if (thd->is_pq_error()) {
    set_datched_status(MQ_HAVE_DETACHED);
    return MQ_DETACHED;
  }

  assert(sent == nbytes);
  *written = sent;
  return MQ_SUCCESS;
}

/**
 * sending data to message queue is divided into two steps:
 *  (s1) send the message length
 *  (s2) send the whole message
 *
 * Note that: the sending process is in a blocking mode.
 * @data: the message data
 * @len: the message length
 * @nowait: the sending status
 */
MQ_RESULT MQueue_handle::send(const void *data, uint32 len, bool nowait) {
  MQ_RESULT res;
  uint32 nbytes = len;
  uint32 written;

  /** (1) write the message length into MQ */
  res = send_bytes(WORD_LENGTH, (char *)&nbytes, &written, nowait);
  if (res != MQ_SUCCESS) {
    assert(res == MQ_DETACHED);
    return res;
  }
  assert((!written && m_queue->detached == MQ_TMP_DETACHED) || written == WORD_LENGTH);

  /** (2) write the message data into MQ */
  res = send_bytes(nbytes, data, &written, nowait);
  if (res != MQ_SUCCESS) {
    assert(res == MQ_DETACHED);
    return res;
  }
  assert((!written && m_queue->detached == MQ_TMP_DETACHED) || written == nbytes);
  return MQ_SUCCESS;
}

/** sending message to MQ in a Field_raw_data manner */
MQ_RESULT MQueue_handle::send(Field_raw_data *fm) {
  MQ_RESULT res = MQ_SUCCESS;
  uint32 written;

  /** (s1) sending the variable-field's length_bytes */
  if (fm->m_var_len) {
    res = send_bytes(1, (void *)&fm->m_var_len, &written);
    assert((res == MQ_SUCCESS && written == 1) || res == MQ_DETACHED ||
           (!written && m_queue->detached == MQ_TMP_DETACHED));
  }

  /** (s2) sending the data of field->ptr */
  if (MQ_SUCCESS == res) {
    res = send_bytes(fm->m_len, fm->m_ptr, &written);
    assert((res == MQ_SUCCESS && written == fm->m_len) || res == MQ_DETACHED ||
           (!written && m_queue->detached == MQ_TMP_DETACHED));
  }
  return res;
}

/**
 * receive data of bytes_needed from MQ
 * @bytes_needed: number of bytes needed from the read position
 * @nbytesp: the acutal bytes read
 * @datap: the data read
 * @nowait: reading mode, nowait=true by default
 * @return
 */
MQ_RESULT MQueue_handle::receive_bytes(uint32 bytes_needed, uint32 *nbytesp, void *datap, bool nowait) {
  uint64 rb, wb;
  uint32 used, offset;

  *nbytesp = 0;
  uint32 ringsize = m_queue->m_ring_size;
  THD *thd = current_thd;
  assert(!m_queue->m_receiver_event->PQ_caller || thd == m_queue->m_receiver_event->PQ_caller);
  while (!thd->is_killed() && !thd->pq_error) {
    rb = atomic_read_u64(&m_queue->m_bytes_read) + m_consume_pending;
    wb = atomic_read_u64(&m_queue->m_bytes_written);
    assert(wb >= rb);

    used = wb - rb;
    assert(used <= ringsize);
    offset = MOD(rb, ringsize);

    /** we have enough space and then directly read bytes_needed data into datap
     */
    if (used >= bytes_needed) {
      /** (s1) read data located in [offset, ..., ringsize] */
      if (offset + bytes_needed <= ringsize) {
        memcpy(datap, &m_queue->m_buffer[offset], bytes_needed);
      } else {
        /** (s2) read data located in [offset, ringsize], [0, bytes_needed -
         * (ringsize - offset))] */
        int part_1 = ringsize - offset;
        int part_2 = bytes_needed - part_1;

        memcpy(datap, &m_queue->m_buffer[offset], part_1);
        memcpy((char *)datap + part_1, &m_queue->m_buffer[0], part_2);
      }

      *nbytesp = bytes_needed;
      memory_barrier();

      /** notify sender that there is available space in MQ */
      end_wait(m_queue->m_sender_event);
      return MQ_SUCCESS;
    }

    /**
     * there are not enough data for receiver, and receiver only
     * receives the data located into [offset, ringsize].
     */
    if (offset + used >= ringsize) {
      memcpy(datap, &m_queue->m_buffer[offset], ringsize - offset);
      *nbytesp = ringsize - offset;

      memory_barrier();
      end_wait(m_queue->m_sender_event);
      return MQ_SUCCESS;
    }

    /**
     * if m_queue is detached and there are still data in m_queue,
     * receiver can receive data until it reads all data.
     */
    if (m_queue->detached == MQ_HAVE_DETACHED) {
      read_barrier();
      if (wb != atomic_read_u64(&m_queue->m_bytes_written)) {
        continue;
      }
      return MQ_DETACHED;
    }

    /**
     * updating the read position, note that
     * {
     *    atomic_inc_bytes_read(m_consume_pending);
     *    m_consume_pending = 0;
     * }
     * should be a group of atomic operation.
     */
    if (m_consume_pending > 0) {
      offset = m_consume_pending;
      m_consume_pending = 0;

      /** ensure that: consume_pending has written into memory */
      memory_barrier();
      atomic_inc_bytes_read(offset);
    }

    /** the blocking-read mode */
    if (nowait) {
      return MQ_WOULD_BLOCK;
    }

    set_wait(m_queue->m_receiver_event);
    reset_wait(m_queue->m_receiver_event);
  }
  return MQ_DETACHED;
}

/**
 * receive message  from MQ
 * @datap
 * @nbytesp
 * @nowait
 */
MQ_RESULT MQueue_handle::receive(void **datap, uint32 *nbytesp, bool nowait) {
  MQ_RESULT res;
  uint32 nbytes, offset;
  uint32 rb = 0;
  /**
   * only when m_consume_pending is greater than 1/4 * m_ring_size, we update
   * the read position m_read_bytes using m_consume_pending; otherwise, the
   * number of read bytes is firstly accumulated to m_consume_pending and then
   * using (m_read_bytes + m_consume_pending) as the real read position in ring
   * array.
   *
   */
  if (m_consume_pending > m_queue->m_ring_size / 4) {
    offset = m_consume_pending;
    m_consume_pending = 0;

    memory_barrier();
    atomic_inc_bytes_read(offset);
  }

  /**
   * for the receive process:
   *   (1) first, we read the message length from MQ;
   *   (2) then, we read the message data.
   * As in the non-blocking mode, we read bytes from MQ as many as possible
   * when there is available data in MQ. Thus, we should remember the read
   * info. of last receive process.
   *
   */

  /** (1) read the message length */
  while (!m_length_word_complete) {
    assert(m_partial_bytes < WORD_LENGTH);
    res = receive_bytes(WORD_LENGTH - m_partial_bytes, &rb, &m_buffer[m_partial_bytes], nowait);
    if (res != MQ_SUCCESS) {
      return res;
    }

    // uint32x2_t v_a = {m_partial_bytes, m_consume_pending};
    // uint32x2_t v_b = {rb, rb};

    // v_a = vadd_u32(v_a, v_b);
    // m_partial_bytes = vget_lane_u32(v_a, 0);
    // m_consume_pending = vget_lane_u32(v_a, 1);

    m_partial_bytes += rb;
    m_consume_pending += rb;

    if (m_partial_bytes >= WORD_LENGTH) {
      assert(m_partial_bytes == WORD_LENGTH);
      m_expected_bytes = *(uint32 *)m_buffer;
      m_length_word_complete = true;
      m_partial_bytes = 0;
    }
  }
  nbytes = m_expected_bytes;

  /** re-allocing local buffer when m_buffer_len is smaller than nbytes */
  if (m_buffer_len < nbytes || DBUG_EVALUATE_IF("pq_mq_error3", true, false)) {
    while (m_buffer_len < nbytes) {
      m_buffer_len *= 2;
    }
    if (m_buffer) {
      destroy(m_buffer);
    }
    THD *thd = current_thd;
    assert(!m_queue->m_receiver_event->PQ_caller || thd == m_queue->m_receiver_event->PQ_caller);
    m_buffer = new (thd->pq_mem_root) char[m_buffer_len];
    /**  if m_buffer allocates fail, then directly return my_error */
    if (m_buffer == nullptr || DBUG_EVALUATE_IF("pq_mq_error3", true, false)) {
      my_error(ER_STD_BAD_ALLOC_ERROR, MYF(0), "", "(MQ::receive)");
      return MQ_DETACHED;
    }
  }

  /** (2) read data of nbytes **/
  for (;;) {
    size_t still_needed;
    assert(m_partial_bytes <= nbytes);

    still_needed = nbytes - m_partial_bytes;
    res = receive_bytes(still_needed, &rb, &m_buffer[m_partial_bytes], nowait);
    if (res != MQ_SUCCESS) {
      return res;
    }
    // uint32x2_t v_a = {m_partial_bytes, m_consume_pending};
    // uint32x2_t v_b = {rb, rb};
    // v_a = vadd_u32(v_a, v_b);
    // m_partial_bytes = vget_lane_u32(v_a, 0);
    // m_consume_pending = vget_lane_u32(v_a, 1);

    m_partial_bytes += rb;
    m_consume_pending += rb;
    if (m_partial_bytes >= nbytes) {
      break;
    }
  }

  /** reset for next read */
  m_length_word_complete = false;
  m_partial_bytes = 0;

  *nbytesp = nbytes;
  *datap = m_buffer;
  return MQ_SUCCESS;
}
