#ifndef __SHANNONBASE_SQL_EXCHANGE_SORT_H__
#define __SHANNONBASE_SQL_EXCHANGE_SORT_H__

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

#include <iostream>
#include "sql/exchange.h"
#include "sql/filesort.h"
#include "sql/handler.h"
#include "sql/sql_binary_heap.h"
#include "sql/sql_class.h"
#include "sql/sql_sort.h"

struct TABLE;
class THD;
class Filesort;

#define MAX_RECORD_STORE 10
#define RECORD_BUFFER_SIZE 128

/** wrapper of message in MQ */
typedef struct mq_record_struct {
  uchar *m_data;       /** the message data in MQ */
  uint32 m_length;     /** the message length */
  uint32 m_buffer_len; /** the length of buffer used to cache this message */
  mq_record_struct() : m_data(nullptr), m_length(0), m_buffer_len(RECORD_BUFFER_SIZE) {}
} mq_record_st;

/** batch of records cached */
typedef struct mq_records_batch_struct {
  mq_record_st **records; /** the cached message from MQ, where records[i]
                              is the i-th worker's cached records */
  int n_total;            /** total number of records cached */
  int n_read;             /** number of records have been read */
  bool completed;         /** the status of worker's MQ completed = true indicates
                              that all messages have been read from MQ,
                              otherwise, completed = false */
} mq_records_batch_st;

class Exchange_sort : public Exchange {
  public:
  Exchange_sort()
      : Exchange(),
        m_sort_param(nullptr),
        m_file(nullptr),
        m_min_records(nullptr),
        m_record_groups(nullptr),
        m_heap(nullptr),
        m_sort(nullptr),
        m_init_heap(false),
        m_tmp_key(nullptr) {}
  
  Exchange_sort(THD *thd, TABLE *table, Filesort *sort, handler *file, uint32 workers, uint ref_len,
                bool stab_output = false)
      : Exchange(thd, table, workers, ref_len, stab_output),
        m_sort_param(nullptr),
        m_file(file),
        m_min_records(nullptr),
        m_record_groups(nullptr),
        m_heap(nullptr),
        m_sort(sort),
        m_init_heap(false),
        m_tmp_key(nullptr) {}

  virtual ~Exchange_sort() override {}
 public:
  /** init members */
  bool init() override;
  /** read message from MQ and fill it into table->record[0] */
  bool read_mq_record() override;
  /** cleanup */
  void cleanup() override;

  /** get the k-th record in m_min_records */
  inline mq_record_st *get_record(int k) {
    assert(0 <= k && k < lanuch_workers());
    mq_record_st *record = m_min_records[k];
    return record;
  }

  inline handler *get_file() { return m_file; }

  inline const Filesort *get_filesort() { return m_sort; }

  inline uchar *get_row_id(int i) {
    assert(0 <= i && i < 2);
    return row_id[i];
  }

  inline uchar *get_key(int i) {
    assert(0 <= i && i < 2);
    return keys[i];
  }

  inline const uchar *get_tmp_key() { return m_tmp_key; }

  inline Sort_param *get_sort_param() { return m_sort_param; }

  EXCHANGE_TYPE get_exchange_type() override { return EXCHANGE_SORT; }

  Sort_param *m_sort_param; /** sort param */
  uchar *row_id[2]{NULL};   /** row_id (or PK) used for stable output */
  handler *m_file;          /** innodb handler */
  uchar *keys[2]{NULL};     /** compared-keys of two nodes in heap*/
 private:
  /** alloc space for sort */
  bool alloc();
  /** build the binary heap */
  void build_heap();
  /** get minimum record */
  mq_record_st *get_min_record();
  /** read one message from MQ[id] and then copy it
      to m_record_groups[id].records[i] */
  bool load_group_record(int id, int i, bool *completed, bool nowait);
  /** fetch the current minimum record of the id-th records group
      and store it in m_min_records */
  bool read_group(int id, bool nowait);
  /** try to load a batch of messages in no-blocking mode */
  void load_group_records(int id);
  /** store message from table->record as a mq_record */
  bool store_mq_record(mq_record_st *rec, uchar *data, uint32 msg_len);

  mq_record_st **m_min_records;         /** array of minimum records */
  mq_records_batch_st *m_record_groups; /** array of minimum recors of each worker's group */
  binary_heap *m_heap;                  /** binary heap used for merge sort */
  Filesort *m_sort;                     /** sort structure */
  bool m_init_heap;                     /** indicates whether init the heap */
  uchar *m_tmp_key;                     /** tmp key for comparing */
};

/** compare two nodes in heap */
extern bool heap_compare_records(int a, int b, void *arg);

#endif  // #define __SHANNONBASE_SQL_EXCHANGE_SORT_H__