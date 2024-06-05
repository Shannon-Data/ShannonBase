#ifndef __SHANNONBASE_SQL_MESSAGE_QUEUE_PQ__
#define __SHANNONBASE_SQL_MESSAGE_QUEUE_PQ__

/* Copyright (c) 2007, 2020, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2021, Huawei Technologies Co., Ltd.

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

#include <sys/time.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include "log.h"
#include "my_dbug.h"
#include "my_sys.h"
#include "sql_base.h"
#include "sql_class.h"
#include "sql_error.h"
#include "sql_parallel_global.h"

#define MQ_BUFFER_SIZE 1024
#define RING_SIZE 1048576

// one message cannot exceed 2^32 bytes
const size_t WORD_LENGTH = sizeof(uint32);
enum MQ_RESULT {
  MQ_SUCCESS,     /** Sent or received a message */
  MQ_WOULD_BLOCK, /** Not completed; retry later */
  MQ_DETACHED     /** worker has detached queue */
};

enum MQ_DETACHED_STATUS {
  MQ_NOT_DETACHED = 0, /** not detached from MQ */
  MQ_HAVE_DETACHED,    /** have detached from MQ */
  MQ_TMP_DETACHED      /** tmp detached from MQ, and reset MQ_NOT_DETACHED later */
};

enum MESSAGE_TYPE { EMPTY_MSG, ERROR_MSG };

/**
 * field raw data, used to store in MQ
 *
 * */
struct Field_raw_data {
  uchar *m_ptr;    /** point to raw data buffer */
  uint32 m_len;    /** length of message */
  uchar m_var_len; /** length of varstring */

  /**
   * m_need_send = true indicates message should be sent to MQ;
   * otherwise, m_need_send = false.
   */
  bool m_need_send;
  Field_raw_data() {
    m_ptr = nullptr;
    m_len = 0;
    m_var_len = 0;
    m_need_send = true;
  }
};

const uint RING_SIZE_MOD = RING_SIZE - 1;
#define MOD(x, y) ((x)&RING_SIZE_MOD)

/** lock-free operation */
#define write_barrier() __atomic_thread_fence(__ATOMIC_RELEASE)
#define read_barrier() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define compiler_barrier() __asm__ __volatile__("" ::: "memory")
#define memory_barrier() __sync_synchronize()

/** atomic CAS operation */
static inline bool atomic_compare_exchange_u64(volatile uint64 *ptr, uint64 *expected, uint64 newval) {
  bool ret;
  uint64 current;
  current = __sync_val_compare_and_swap(ptr, *expected, newval);
  ret = current == *expected;
  *expected = current;
  return ret;
}

/** atomic read uint64 value */
static inline uint64 atomic_read_u64(volatile uint64 *ptr) {
  uint64 old = 0;
  atomic_compare_exchange_u64(ptr, &old, 0);
  return old;
}

/** atomic write uint64 value */
static inline uint64 atomic_write_u64(volatile uint64 *ptr, uint64 val) {
  uint64 old = *ptr;
  while (!atomic_compare_exchange_u64(ptr, &old, val))
    ;
  return old;
}

class MQ_event {
 public:
  THD *PQ_caller;
  std::atomic<bool> latch;

 public:
  MQ_event(THD *thd) : PQ_caller(thd), latch(false) {}
  MQ_event() {
    PQ_caller = nullptr;
    latch = false;
  }
  ~MQ_event() {}

  /** set waiting status */
  void set_latch() { latch.store(true); }

  /**
   * Check whether the partner has set latch's status in a loop manner until
   * reaching the maximal timeout time (100ms, by default).
   */
  void wait_latch(uint64 wait_max_time = 100) {
    struct timespec start_ts;
    struct timespec end_ts;
    ulong difftime;

    set_timespec(&start_ts, 0);
    latch.load();
    THD *thd = PQ_caller ? PQ_caller : current_thd;
    while (!latch && (thd && !thd->is_killed())) {
      set_timespec(&end_ts, 0);
      difftime = (end_ts.tv_sec - start_ts.tv_sec) * TIME_THOUSAND + (end_ts.tv_nsec - start_ts.tv_nsec) / TIME_MILLION;

      if (difftime >= wait_max_time) break;
      latch.load();
    }
  }

  /** reset waiting status */
  void reset_latch() { latch.store(false); }
};

class MQueue {
 public:
  MQ_event *m_sender_event;    /** sender's event */
  MQ_event *m_receiver_event;  /** receiver's event */
  uint64 m_bytes_written;      /** number of written bytes */
  uint64 m_bytes_read;         /** number of read bytes */
  char *m_buffer;              /** ring array that stores the message */
  uint32 m_ring_size;          /** the size of ring array */
  MQ_DETACHED_STATUS detached; /** the status of message queue */

 public:
  MQueue()
      : m_sender_event(nullptr),
        m_receiver_event(nullptr),
        m_bytes_written(0),
        m_bytes_read(0),
        m_buffer(nullptr),
        m_ring_size(0),
        detached(MQ_NOT_DETACHED) {}

  MQueue(MQ_event *sender_event, MQ_event *receiver_event, char *ring, size_t ring_size)
      : m_sender_event(sender_event),
        m_receiver_event(receiver_event),
        m_bytes_written(0),
        m_bytes_read(0),
        m_buffer(ring),
        m_ring_size(ring_size),
        detached(MQ_NOT_DETACHED) {}
  ~MQueue() {}
};

class MQueue_handle {
 public:
  MQueue *m_queue; /** message queue */

 private:
  char *m_buffer;              /** local buffer for cache reading */
  uint32 m_buffer_len;         /** the length of local buffer */
  uint32 m_consume_pending;    /** accumulated read bytes for updating read number
                                   of bytes in batch manner */
  uint32 m_partial_bytes;      /** partial bytes that has been read in one message */
  uint32 m_expected_bytes;     /** expected bytes for one complete message */
  bool m_length_word_complete; /** indicates whether message's length has been
                                  completely read */

 public:
 public:
  MQueue_handle()
      : m_queue(nullptr),
        m_buffer(nullptr),
        m_buffer_len(0),
        m_consume_pending(0),
        m_partial_bytes(0),
        m_expected_bytes(0),
        m_length_word_complete(false) {}

  MQueue_handle(MQueue *queue, size_t buffer_len)
      : m_queue(queue),
        m_buffer(nullptr),
        m_buffer_len(buffer_len),
        m_consume_pending(0),
        m_partial_bytes(0),
        m_expected_bytes(0),
        m_length_word_complete(false) {}

  ~MQueue_handle() {}

 public:
  MQ_RESULT send(Field_raw_data *fm);
  MQ_RESULT send(const void *datap, uint32 len, bool nowait = false);
  MQ_RESULT receive(void **datap, uint32 *, bool nowait = true);

  inline MQueue *get_mqueue() { return m_queue; }
  inline MQ_event *get_receiver() { return m_queue->m_receiver_event; }

  inline MQ_event *get_sender() { return m_queue->m_sender_event; }
  /** set detached status */
  inline void set_datched_status(MQ_DETACHED_STATUS status) {
    if (m_queue) m_queue->detached = status;
  }

  inline bool send_exception_msg(MESSAGE_TYPE msg) {
    MQ_RESULT result = send((void *)&msg, 1);
    if ((result != MQ_SUCCESS && result != MQ_DETACHED) || DBUG_EVALUATE_IF("pq_mq_error6", true, false)) {
      /*
       * Note that: if we can not send error msg to MQ, then the only
       * solution is to generate my_error info.
       */
      my_error(ER_PARALLEL_EXEC_ERROR, MYF(0));
      return true;
    }
    return false;
  }

  /**
   * init the handle structure
   * @retval:
   *   false for success, and otherwise true.
   */
  bool init_mqueue_handle(THD *thd) {
    if (!m_buffer_len) return true;
    m_buffer = new (thd->pq_mem_root) char[m_buffer_len];
    if (!m_buffer) return true;

    return false;
  }

  /** cleanup the allocated buffer */
  void cleanup() {
    destroy(m_buffer);
    if (m_queue) {
      destroy(m_queue->m_sender_event);
      destroy(m_queue->m_buffer);
    }
    destroy(m_queue);
  }

 private:
  /**increase #read bytes in atomic manner */
  void atomic_inc_bytes_read(int n) {
    read_barrier();
    atomic_write_u64(&m_queue->m_bytes_read, atomic_read_u64(&m_queue->m_bytes_read) + n);
  }

  /** increase #written bytes in atomic manner */
  void atomic_inc_bytes_written(int n) {
    write_barrier();
    atomic_write_u64(&m_queue->m_bytes_written, atomic_read_u64(&m_queue->m_bytes_written) + n);
  }

  /** let event into wait status */
  void set_wait(MQ_event *event) {
    assert(event != nullptr);
    event->wait_latch();
  }

  /** end event's wait status */
  void end_wait(MQ_event *event) {
    assert(event != nullptr);
    event->set_latch();
  }

  /** reset event's wait status */
  void reset_wait(MQ_event *event) {
    assert(event != nullptr);
    event->reset_latch();
  }

  MQ_RESULT send_bytes(uint32 nbytes, const void *data, uint32 *written, bool nowait = false);
  MQ_RESULT receive_bytes(uint32 bytes_needed, uint32 *nbytesp, void *datap, bool nowait = true);
};

#endif  //__SHANNONBASE_SQL_MESSAGE_QUEUE_PQ__
