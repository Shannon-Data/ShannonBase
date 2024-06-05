#ifndef __SHANNONBASE_SQL_EXCHANGE_NOSORT_H__
#define __SHANNONBASE_SQL_EXCHANGE_NOSORT_H__

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

#include "exchange.h"

class Exchange_nosort : public Exchange {
 public:
  Exchange_nosort() : Exchange(), m_next_queue(0), m_active_readers(0) {}

  Exchange_nosort(THD *thd, TABLE *table, int workers, int ref_length, bool stab_output)
      : Exchange(thd, table, workers, ref_length, stab_output), m_next_queue(0), m_active_readers(workers) {}

  virtual ~Exchange_nosort() override {}

  /** read/convert one message from mq to table->record[0] */
  bool read_mq_record() override;

  inline EXCHANGE_TYPE get_exchange_type() override { return EXCHANGE_NOSORT; }

 private:
  /** read one message from MQ[next_queue] */
  bool get_next(void **datap, uint32 *len, bool *done);
  /** read one message from MQ in a round-robin manner */
  bool read_next(void **datap, uint32 *len);

  int m_next_queue;     /** the next read queue */
  int m_active_readers; /** number of left queues which is sending or
                            receiving message */
};

#endif  // __SHANNONBASE_SQL_EXCHANGE_NOSORT_H__
