/* Copyright (c) 2020, Oracle and/or its affiliates. All Rights Reserved.
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

#include "sql_exchange_nosort.h"

/**
 * read one message from MQ[next_queue]
 *
 * @retval: true for success, and otherwise false
 */
bool Exchange_nosort::get_next(void **datap, uint32 *m_len, bool *done) {
  MQ_RESULT result;
  if (done != nullptr) {
    *done = false;
  }
  MQueue_handle *reader = get_mq_handle(m_next_queue);
  result = reader->receive(datap, m_len);
  if (result == MQ_DETACHED) {
    if (done != nullptr) {
      *done = true;
    }
    return false;
  }
  if (result == MQ_WOULD_BLOCK) {
    return false;
  }

  return true;
}

/**
 * read one message from MQ in a round-robin method
 * @datap: the message data
 * @m_len: the message length
 *
 * @retval: true for success, and otherwise false
 */
bool Exchange_nosort::read_next(void **datap, uint32 *m_len) {
  bool readerdone = false;
  int nvisited = 0;
  bool read_result = false;
  THD *thd = get_thd();

  /** round-robin method to acquire the data */
  while (!thd->is_killed() && !thd->pq_error) {
    read_result = get_next(datap, m_len, &readerdone);
    /** detached and its content is also read done */
    if (readerdone) {
      assert(false == read_result);
      m_active_readers--;
      /** read done for all queues */
      if (m_active_readers == 0) {
        return false;
      }
      mqueue_mmove(m_next_queue, m_active_readers);
      if (m_next_queue >= m_active_readers) {
        m_next_queue = 0;
      }
      continue;
    }

    /** data has successfully read into datap */
    if (read_result) {
      return true;
    }
    /** move to next worker */
    m_next_queue++;
    if (m_next_queue >= m_active_readers) {
      m_next_queue = 0;
    }
    nvisited++;
    /** In a round-robin, we cannot read one message from MQ */
    if (nvisited >= m_active_readers) {
      /**
       * this barrier ensures that the receiver first enters into a
       * waiting status and then is waked by one sender.
       */
      memory_barrier();
      MQ_event *receiver = get_mq_handle(0)->get_receiver();
      if (receiver) {
        receiver->wait_latch();
        receiver->reset_latch();
      }
      nvisited = 0;
    }
  }

  return false;
}

/**
 * read one message from MQ and fill it to table->record[0]
 *
 * @retval: true for success, and otherwise false
 */
bool Exchange_nosort::read_mq_record() {
  assert(!is_stable() && get_exchange_type() == EXCHANGE_NOSORT);
  bool result = false;
  uchar *data = nullptr;
  uint32 msg_len = 0;

  /** read a message from MQ's local buffer */
  result = read_next((void **)&data, &msg_len);
  return (result && convert_mq_data_to_record(data, msg_len));
}