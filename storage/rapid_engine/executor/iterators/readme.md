# Design: `ReadBatch` Interface for True Vectorized Aggregation in ShannonBase Rapid Engine

**Status:** Done
**Scope:** `storage/rapid_engine/executor/iterators/`  
**Constraint:** Zero modification to MySQL upstream iterator code (`sql/iterators/`)

---

## 1. Problem Statement

`VectorizedAggregateIterator` currently pulls rows from its source via the standard
`RowIterator::Read()` interface — one row per virtual call. Even though SIMD is applied
to the accumulated data, the ingestion path is still fundamentally row-at-a-time:

```
for each row:
    m_source->Read()          // virtual dispatch, one row
    AppendCurrentRowToChunks() // copy field → ColumnChunk
ProcessVectorizedAggregates()  // SIMD sum over chunks
```

This means:

- One virtual function call per row (cache-unfriendly indirect branch).
- Data is unpacked from columnar storage into `table->field` (row format) by
  `VectorizedTableScanIterator::PopulateCurrentRow()`, then immediately re-packed
  back into `ColumnChunk` by the aggregate iterator — a round-trip copy that serves
  no purpose.
- The SIMD acceleration applies only to the final reduction step, not to the dominant
  data-movement cost.

True vectorization requires that the aggregate iterator consumes data in column-chunk
form directly, without the intermediate row-format layer.

---

## 2. Design Goals

| Goal | Description |
|------|-------------|
| **G1** | No changes to any file under `sql/iterators/` or `sql/` (MySQL upstream). |
| **G2** | `ReadBatch` is an opt-in extension; all existing iterators continue to work unchanged via the existing `Read()` fallback. |
| **G3** | Data flows directly from `ha_rapid::rnd_next_batch()` into `ColumnChunk` — zero intermediate row-format copies for the vectorized path. |
| **G4** | Correct GROUP BY semantics: group boundary detection must still work even when rows arrive in batches. |
| **G5** | The interface must be implementable by `VectorizedTableScanIterator` without exposing internal storage details to the base class. |

---

## 3. Architectural Overview

```
┌─────────────────────────────────────────────┐
│         VectorizedAggregateIterator          │
│                                              │
│  Init(): probe source for ReadBatch support  │
│  Read(): calls ProcessCurrentGroup()         │
│    ├─ if source supports ReadBatch           │
│    │    └─ True vectorized path              │
│    │         m_source->ReadBatch(chunks, N)  │
│    │         ProcessVectorizedAggregates()   │ ← SIMD
│    └─ else                                   │
│         Traditional row-by-row path          │
│         m_source->Read() per row             │
└──────────────────┬──────────────────────────┘
                   │ ReadBatch()
┌──────────────────▼──────────────────────────┐
│       VectorizedTableScanIterator            │
│                                              │
│  ReadBatch(): delegates to rnd_next_batch()  │
│  Lookahead buffer for batch-tail pushback    │
└──────────────────┬──────────────────────────┘
                   │ rnd_next_batch()
┌──────────────────▼──────────────────────────┐
│              ha_rapid                        │
│  (already implemented, no changes needed)    │
└─────────────────────────────────────────────┘
```

The `RowIterator` base class gains one new virtual method with a default implementation
that returns `HA_ERR_UNSUPPORTED`. This is the only addition to the shared iterator
infrastructure, and it is purely additive — no existing behavior changes.

---

## 4. Interface Definition

### 4.1 Extension to `RowIterator` (Rapid-side base, not MySQL's)

To strictly satisfy G1, `ReadBatch` is **not** added to `sql/iterators/row_iterator.h`.
Instead, it is declared on a Rapid-internal mixin:

```cpp
// storage/rapid_engine/executor/iterators/iterator.h  (already Rapid-owned)

namespace ShannonBase::Executor {

/**
 * Optional batch-pull interface for Rapid iterators.
 * Iterators that can deliver data in columnar batches implement this.
 * Callers must dynamic_cast or use a capability-probe pattern (see §5.1).
 */
class BatchReadable {
 public:
  virtual ~BatchReadable() = default;

  /**
   * Fill `col_chunks` with up to `capacity` rows.
   *
   * Preconditions:
   *   - col_chunks is sized and allocated by the caller (see §5.2).
   *   - Each ColumnChunk in col_chunks has been cleared before the call.
   *
   * @param col_chunks  Column chunks indexed by field index (same layout as
   *                    VectorizedTableScanIterator::m_col_chunks).
   * @param capacity    Maximum rows to read.
   * @param rows_read   [out] Number of rows actually written into col_chunks.
   *
   * @return  0                    success, rows_read > 0
   *          HA_ERR_END_OF_FILE   EOF; rows_read may be > 0 (last partial batch)
   *          other                error code
   */
  virtual int ReadBatch(std::vector<ColumnChunk> &col_chunks,
                        size_t capacity,
                        size_t &rows_read) = 0;

  /**
   * Push back rows [from_row, total_rows) of a batch that the caller has
   * already received but cannot process in the current aggregation group.
   * The next ReadBatch() call will re-deliver these rows first.
   *
   * Used by VectorizedAggregateIterator when a GROUP BY boundary is detected
   * inside a batch (see §6.2).
   */
  virtual void PushbackBatchTail(const std::vector<ColumnChunk> &chunks,
                                 size_t from_row,
                                 size_t total_rows) = 0;
};

} // namespace ShannonBase::Executor
```

Using a separate mixin rather than polluting `RowIterator` keeps the Rapid extension
entirely within the `storage/rapid_engine/` tree.

### 4.2 `VectorizedTableScanIterator` implements `BatchReadable`

```cpp
class VectorizedTableScanIterator final
    : public TableRowIterator,        // MySQL base — unchanged
      public BatchReadable {          // Rapid extension — new

  int ReadBatch(std::vector<ColumnChunk> &col_chunks,
                size_t capacity, size_t &rows_read) override;

  void PushbackBatchTail(const std::vector<ColumnChunk> &chunks,
                         size_t from_row, size_t total_rows) override;

 private:
  // Lookahead buffer (see §6.2)
  std::vector<ColumnChunk> m_lookahead_chunks;
  size_t m_lookahead_start{0};
  size_t m_lookahead_count{0};
};
```

---

## 5. Capability Probe and Chunk Layout Contract

### 5.1 Capability probe in `VectorizedAggregateIterator::Init()`

```cpp
bool VectorizedAggregateIterator::Init() {
  // ... existing init ...
  if (m_source->Init()) return true;

  // Probe: does the source support batch pull?
  m_batch_source = dynamic_cast<BatchReadable *>(m_source.get());
  m_source_supports_batch = (m_batch_source != nullptr);

  // ... rest of init ...
}
```

`dynamic_cast` is safe here because it executes once at query-init time, not per row.

### 5.2 Chunk layout contract

`VectorizedAggregateIterator` allocates and owns the `ColumnChunk` vector that is
passed into `ReadBatch`. The layout must match what `ha_rapid::rnd_next_batch()`
expects:

- Vector is sized to `table->s->fields`.
- Each slot corresponding to an active (non-`NOT_SECONDARY_FLAG`) field is
  pre-allocated with sufficient capacity (`opt_batch_size` rows).
- Inactive slots hold a default-constructed (null) `ColumnChunk`.

This is identical to `VectorizedTableScanIterator::m_col_chunks` layout, so the
aggregate iterator can reuse `PreallocateColumnChunks()` logic (extracted to a shared
utility if needed).

---

## 6. Data Flow for the True Vectorized Path

### 6.1 Global aggregate (no GROUP BY) — simplest case

```
SELECT SUM(S_ACCTBAL) FROM SUPPLIER;
```

```
Init():
  m_source->Init()
  m_batch_source = dynamic_cast<BatchReadable*>(m_source.get())  // non-null

Read() [called once by executor]:
  reset_and_add() on sum_funcs          // clear accumulator

  loop:
    m_batch_source->ReadBatch(chunks, capacity, rows_read)
    ProcessVectorizedAggregates()       // SIMD Sum over chunks[field_idx]
    if EOF: break

  return 0  // emit single result row
```

No `table->field` writes at all. Data path: `ha_rapid` → `ColumnChunk` → SIMD reduction.

### 6.2 Grouped aggregate (with GROUP BY) — boundary handling

When GROUP BY is present, a batch may contain rows from two different groups.
The aggregate iterator must detect the boundary and split the batch.

**Problem:** Group key comparison relies on `update_item_cache_if_changed(m_join->group_fields)`,
which reads from `table->field` (row format). The batch delivers data in columnar format.

**Solution — Hybrid approach:**

Group key fields and aggregate fields are separated into two sets:

- **Aggregate fields**: stay in `ColumnChunk`, processed by SIMD.
- **Group key fields**: written back to `table->field` row-by-row *only* for boundary
  detection. Since group key cardinality is typically very low (one boundary per group),
  this write-back cost is negligible.

```
loop over rows in batch [0 .. rows_read):
    RestoreGroupKeyFields(row_idx)          // write only key fields to table->field
    first_changed = update_item_cache_if_changed(group_fields)
    if first_changed >= 0:
        boundary = row_idx
        break

// SIMD-accumulate rows [0, boundary)
current_batch.row_count = boundary
ProcessVectorizedAggregates()

if boundary < rows_read:
    // rows [boundary, rows_read) belong to the next group
    m_batch_source->PushbackBatchTail(chunks, boundary, rows_read)
    StoreFromTableBuffers(...)             // save next-group first row
    m_state = LAST_ROW_STARTED_NEW_GROUP
    return 0
```

`RestoreGroupKeyFields(row_idx)` writes only the fields participating in GROUP BY
from the chunk back to `table->field`. This requires the chunk layout to include
group key fields, or a separate small chunk set for keys.

### 6.3 `PushbackBatchTail` implementation

```cpp
void VectorizedTableScanIterator::PushbackBatchTail(
    const std::vector<ColumnChunk> &src_chunks,
    size_t from_row, size_t total_rows) {

  size_t tail_len = total_rows - from_row;
  EnsureLookaheadCapacity(tail_len);

  for (size_t i = 0; i < src_chunks.size(); ++i) {
    if (!src_chunks[i].is_valid()) continue;
    CopyChunkRows(src_chunks[i], from_row,
                  m_lookahead_chunks[i], 0, tail_len);
  }
  m_lookahead_start = 0;
  m_lookahead_count = tail_len;
}
```

Subsequent `ReadBatch()` calls drain the lookahead buffer first before issuing a new
`rnd_next_batch()` to the handler.

---

## 7. Fallback Guarantee

The traditional row-by-row path is preserved in full and is selected when:

- The source iterator does not implement `BatchReadable` (e.g., a join node, subquery,
  or any upstream node that is not `VectorizedTableScanIterator`).
- `m_rollup` is true (rollup interleaves output rows within a group; batching complicates
  this without meaningful gain).
- The aggregate contains non-vectorizable functions (e.g., `GROUP_CONCAT`, `JSON_ARRAYAGG`).

```cpp
bool use_batch_path = m_source_supports_batch
                   && m_vectorizer.can_vectorize_curr_grp
                   && !m_rollup
                   && m_vectorization_enabled;
```

---

## 8. Files Changed

| File | Change | MySQL upstream? |
|------|--------|----------------|
| `storage/rapid_engine/executor/iterators/iterator.h` | Add `BatchReadable` mixin | No |
| `storage/rapid_engine/executor/iterators/table_scan_iterator.h` | Inherit `BatchReadable`; add lookahead members | No |
| `storage/rapid_engine/executor/iterators/table_scan_iterator.cpp` | Implement `ReadBatch`, `PushbackBatchTail` | No |
| `storage/rapid_engine/executor/iterators/aggregate_iterator.h` | Add `m_batch_source`, `m_source_supports_batch` | No |
| `storage/rapid_engine/executor/iterators/aggregate_iterator.cpp` | Probe in `Init()`; batch path in `ProcessCurrentGroup` | No |

**Zero files under `sql/` are modified.**

---

## 9. Performance Model

For a table of N rows with a single `SUM` aggregate and no GROUP BY:

| Path | Virtual calls | `table->field` writes | SIMD ops |
|------|--------------|----------------------|----------|
| Current (row-by-row) | N | N (PopulateCurrentRow) | 1 reduction over N elements |
| Proposed (batch) | ⌈N/B⌉ where B=batch size | 0 | ⌈N/B⌉ reductions over B elements each |

For B = 4096 and N = 10M rows, virtual call overhead drops from 10M to ~2500 calls.
The `table->field` write-back is eliminated entirely for pure aggregate queries.

---

## 10. Open Questions

1. **Group key chunk allocation**: Should group key fields share the same `ColumnChunk`
   vector as aggregate fields, or be maintained in a separate smaller vector? A unified
   vector simplifies `ReadBatch` but increases memory for keys that are never SIMD-processed.

2. **Multi-source support**: If the source is a `HashJoin` or `NestLoopJoin` node,
   `BatchReadable` is not implemented. A future extension could add `ReadBatch` to join
   nodes that themselves receive batch input from both sides.

3. **Batch size for GROUP BY**: When groups are small (e.g., high-cardinality GROUP BY),
   batches will frequently be split at boundaries and most of `PushbackBatchTail` will
   copy near-empty tails. A heuristic to downgrade to row-by-row when average group size
   falls below a threshold (e.g., < 8 rows) would avoid the overhead.

4. **Hash Join**
    hash_join_iterator can use this design doc to impl its iterator.