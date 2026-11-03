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


Data processing Unit
====================

Hardware offload for the Rapid Engine execution layer. Design tracked in
issue #1004.

STATUS: skeleton. Only the interface, the dispatcher and a no-device host
backend exist. No kernel is offloaded and no behaviour changes yet.

Contents
--------
  backend.h            DeviceBackend: capabilities, capability probe, execute,
                       device memory, copy in/out, synchronise.
  kernel.h             Kernel vocabulary: KernelId, ElementType, ColumnView,
                       SelectionView, KernelRequest, KernelResult.
  dispatch.h/.cpp      DeviceDispatcher singleton: backend registry, policy,
                       row threshold, counters.
  host/host_backend.*  The no-device backend. Registered in every build and it
                       never claims a request, so UNCLAIMED always means "run
                       the existing host path".


1. Where offload attaches
-------------------------
The engine is already columnar and vectorized, so nothing under sql/iterators/
(MySQL upstream) has to change. Two places are enough:

  ColumnChunkOper::Sum / Min / Max / CountNonNull / Filter
      executor/iterators/iterator.h  (the body of each function)
  VectorizedExpression::Evaluate
      executor/iterators/expression_iterator.h

Both are header templates, so each call site needs a non-template trampoline
(for example DispatchSum(type, chunk, rows)) to keep backend translation units
out of the headers. With no backend claiming, the trampoline returns UNCLAIMED
and the current code runs unchanged.

Boundaries that must be respected:

  ColumnChunk                executor/iterators/iterator.h
                             the only data carrier; [data][null_bitmap]
  BatchReadable              executor/iterators/iterator.h
                             ReadBatch / ReadSelectedBatch delivery
  Morsel / MorselSource      executor/executor.h
                             physical splitting; maps onto blocks/grids
  ResMgmt::MemoryBudget      resource_management/res_mgmt.h
                             every byte must be charged here
  BatchHardware              executor/iterators/iterator.h
                             the natural place for device traits


2. Data contract
----------------
A KernelRequest carries ColumnViews, not ColumnChunks, so backend headers stay
free of sql/field.h and can be included from a .cu or a SYCL source.

  - data holds rows * stride bytes; stride must equal ElementSize(type).
  - null_mask is a bit array where a SET bit means NULL, the same convention as
    Utils::SIMD. A null null_mask means the column has no NULLs.
  - selection is a sparse list of row indexes; empty means the rows are dense.
    With a sparse selection the work size is selection.count, not the column
    length.
  - Pointers are host memory owned by the caller for the duration of the call.
    A backend may copy them out; it must never retain them.
  - The output buffer is host memory owned by the caller as well. A backend
    fills it completely or returns UNCLAIMED without touching it. Partial
    writes are a contract violation.
  - ColumnChunk is single-consumer and non-atomic. A device owns its own chunks;
    merging happens at an operator boundary, never inside a kernel.
  - Chunks are bound to Field/TABLE lifetime. Device buffers are leased per call
    and must not outlive the statement.


3. Dispatch contract
--------------------
Backends are tried in registration order; the first one that Supports() wins.
The host backend is always last and never claims.

  UNCLAIMED  run the host path. Nothing was written.
  OK         the result is complete.
  ERROR      a claimed kernel failed. The statement fails; it is NOT retried on
             the host, because a partially written buffer cannot be told apart
             from a real answer.

Supports() must be a pure, side-effect-free capability question, because the
optimizer calls it at plan time. A plan that selects a device operator and is
then refused at run time aborts the query, it does not fall back. The pattern to
copy is CanUseHashAggregate() / VectorizableAggregateValueExpr(): the executor
owns the gate and the planner asks.

Policy (dispatch.h DispatchPolicy, to become rapid_dpu_* system variables):

  enabled   false by default. A device is opted into, never picked up silently.
  min_rows  4096 by default. A smaller batch costs more in the copy round trip
            than the kernel saves, so it is refused with refused_small.

Counters (DispatchCounters) separate requests, claims, the three refusal
reasons, executed and failed. They are software counters; hardware PMCs stay in
the perf harness (see executor/iterators/iterator.h VectorizedOperatorStats).


4. What can be offloaded
------------------------
Tier 1, recommended first. One representation, no branch divergence, no pointer
chasing; a single dispatch point covers the whole cost.

  scan projection        ha_rapid::rnd_next_batch -> ReadBatch    bandwidth
  predicate filter       VectorizedFilterIterator + ColumnChunkOper::Filter
  exact SUM/AVG/COUNT    aggregate_iterator ProcessVectorizedAggregates
  expression tree        VectorizedExpression (fixed-point int128 IR)
  VECTOR distance        not implemented yet; only the type exists
                         (imcs/row0row.cpp, MYSQL_TYPE_VECTOR)

Tier 2, feasible but the host/device boundary becomes much wider.

  hash aggregate build   BuildHashGroupsBatch; group-key ordering matters
  hash join build/probe  fanout ordinals define output order
  window aggregates      window_iterator; prefix/segment ranges

Tier 3, do not offload.

  sort / top-N           keys come from Field::make_sort_key (charset and
                         collation dependent) and spill to disk
  LIMIT / MATERIALIZE / NESTED LOOP / STREAM / record
                         control flow, not kernel shaped


5. Constraints that are correctness, not performance
-----------------------------------------------------
  1. Device memory must register as its own pool and participate in
     MemoryBudget / Evictable. Allocating behind the governor bypasses the query
     limit and eviction arbitration.
  2. SQL semantics stay on the host: three-valued NULL logic, my_decimal
     exactness, collation and charset, Field error codes. A kernel is
     speculative: claim, or refuse, never partially write.
  3. Determinism: Morsel ordinals define reproducible parallel output order, and
     float reduction is order dependent. Fix the reduction tree, or the TPC-H
     digest comparison drifts.
  4. A backend must not read IMCUs. Scans consume the RapidCursor pinned
     snapshot under the mutation lock, and CU payloads may be compressed
     (LZ4 / ZSTD / ZLIB); decompression stays on the host.
  5. devirtualisation / eviction: a claimed kernel runs inside the operator's
     existing reservation. It does not acquire a new grant and does not evict
     another thread's live operators.


6. Adding a backend
-------------------
  1. Add dpu/<kind>/<kind>_backend.{h,cpp} implementing DeviceBackend.
  2. Keep it out of the default build: guard the sources in CMakeLists.txt with
     the corresponding WITH_RAPID_DPU_* option.
  3. Register it once at engine startup through
     DeviceDispatcher::Instance().RegisterBackend(). Registration order is
     priority order; the host backend is always kept last.
  4. Implement Supports() for exactly the element types and shapes Execute()
     honours. Do not claim a shape that then has to be rejected at run time.
  5. Add a gunit case next to unittest/gunit/secondary_engine/ that drives
     DispatchPolicy through the dispatcher and compares the kernel against the
     host result; MTR covers the SQL-level parity.

One backend per computation model. CUDA/HIP/SYCL launch per kernel and attach to
ColumnChunkOper directly. A TPU or NPU has no per-kernel model, so it needs an
exported graph boundary (XLA / StableHLO / PJRT) instead: the vectorized
expression program plus the aggregate reduction, submitted as one unit.


7. Roadmap (issue #1004)
------------------------
  P0  this skeleton: interface, dispatcher, host backend, counters.
      Gate: identical results and unchanged behaviour with no device; MTR green.
  P1  Filter / Project / Sum kernels with the row threshold.
      Gate: assert_rapid_matches_innodb digest parity.
  P2  aggregate reduction offload with spill fallback, device-level morsels
      (BatchHardware gains SM count and HBM bandwidth, reusing
      SelectBatchRows()).
  P3  VECTOR distance kernels wired into ml/ retrieval; this is where TPU/NPU
      becomes relevant and where the graph boundary is required.

Benchmarks before any default changes: tools/tpch/compare_tpch.py
--kernel-probes (Q100-Q111).
