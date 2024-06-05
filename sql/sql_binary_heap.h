#ifndef __SHANNONBASE_SQL_BINARY_HEAP_H__
#define __SHANNONBASE_SQL_BINARY_HEAP_H__

/* Copyright (c) 2020, Oracle and/or its affiliates. All Rights Reserved.
   Copyright (c) 2022, Huawei Technologies Co., Ltd.

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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.   */

#include "sql/sql_class.h"

// compare based on the sort_key
typedef bool (*binaryheap_comparator)(int a, int b, void *arg);
class binary_heap {
 public:
  binary_heap(int element_size, void *arg, binaryheap_comparator cmp, THD *thd)
      : m_queue(nullptr), m_capacity(element_size), m_size(0), m_compare(cmp), m_arg(arg), m_thd(thd) {}
  ~binary_heap() {}
  /* @retval: false of success, and true otherwise. */
  bool init_binary_heap() {
    if (m_capacity <= 0) {
      return true;
    }
    m_queue = new (m_thd->pq_mem_root) int[m_capacity + 1];
    if (!m_queue || DBUG_EVALUATE_IF("pq_msort_error9", true, false)) {
      my_error(ER_STD_BAD_ALLOC_ERROR, MYF(0), "", "(PQ::init)");
      return true;
    }
    return false;
  }

  void reset() { m_size = 0; }

  void add_unorderd(int element) {
    if (m_size >= m_capacity || DBUG_EVALUATE_IF("pq_msort_error8", true, false)) {
      my_error(ER_STD_BAD_ALLOC_ERROR, MYF(0), "", "(PQ::add_unorderd)");
      return;
    }
    m_queue[m_size++] = element;
  }

  void build() {
    if (m_size <= 1) {
      return;
    }
    for (int i = parent(m_size - 1); i >= 0; i--) {
      sift_down(i);
    }
  }

  int first() {
    assert(!empty());
    return m_queue[0];
  }

  int remove_first() {
    assert(!empty());
    if (m_size == 1) {
      m_size--;
      return m_queue[0];
    }

    swap_node(0, m_size - 1);
    m_size--;
    sift_down(0);

    return m_queue[m_size];
  }

  void replace_first(int element) {
    assert(!empty());
    m_queue[0] = element;
    if (m_size > 1) {
      sift_down(0);
    }
  }

  bool empty() { return m_size == 0; }

  void cleanup() {
    if (m_queue) {
      destroy(m_queue);
    }
  }

 private:
  /* return the index ((i - 1) / 2) of the parent node of node i */
  int parent(unsigned int i) {
    assert(i != 0);
    return (--i) >> 1;
  }

  /* return the index (2 * i + 1) of the left child of node i */
  int left(unsigned int i) { return (i << 1) | 1; }

  /* return the index (2 * i + 2) of the right child of node  */
  int right(unsigned int i) { return (++i) << 1; }

  uint size() { return m_size; }

  void add(int element) {
    if (m_size >= m_capacity) {
      my_error(ER_STD_BAD_ALLOC_ERROR, MYF(0), "out of binary heap space");
      return;
    }
    m_queue[m_size++] = element;
    sift_up(m_size - 1);
  }

  void swap_node(int a, int b) {
    int temp;
    temp = m_queue[a];
    m_queue[a] = m_queue[b];
    m_queue[b] = temp;
  }

  void sift_down(int node_off) {
    while (true) {
      int left_off = left(node_off);
      int right_off = right(node_off);
      int swap_off = 0;

      if (left_off < m_size && m_compare(m_queue[left_off], m_queue[node_off], m_arg)) {
        swap_off = left_off;
      }

      if (right_off < m_size && m_compare(m_queue[right_off], m_queue[node_off], m_arg)) {
        if (!swap_off || m_compare(m_queue[right_off], m_queue[left_off], m_arg)) {
          swap_off = right_off;
        }
      }

      if (!swap_off) {
        break;
      }

      swap_node(swap_off, node_off);
      node_off = swap_off;
    }
  }

  void sift_up(int node_off) {
    bool cmp = false;
    int parent_off;
    while (node_off != 0) {
      parent_off = parent(node_off);
      cmp = m_compare(m_queue[parent_off], m_queue[node_off], m_arg);
      if (cmp) {
        break;
      }

      swap_node(node_off, parent_off);
      node_off = parent_off;
    }
  }

  int *m_queue;
  int m_capacity;
  int m_size;
  binaryheap_comparator m_compare;
  void *m_arg;
  THD *m_thd;
};
#endif  // __SHANNONBASE_SQL_BINARY_HEAP_H__