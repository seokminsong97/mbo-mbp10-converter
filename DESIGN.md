# Design

## Goal and scope

Build a deterministic, offline converter from Databento MBO DBN files
(`.dbn` or `.dbn.zst`) to MBP-10 DBN files.

- C++ performs streaming decode, book reconstruction, and DBN encoding.
- Python compares decoded output with an official MBP-10 download made using
  the same dataset, symbols, and time range.
- The first correctness target is one week of historical `GLBX.MDP3` data.
  Other datasets must fail clearly until they have dataset-specific fixtures.
- Downloading data, live operation, and cross-publisher consolidation are out
  of scope for the first version.

## Data flow

`DBN reader -> event assembler -> order books -> MBP-10 projector -> DBN writer`

The reader validates that the input metadata declares the MBO schema and lets
the pinned official C++ SDK upgrade supported older DBN versions to DBNv3 in
memory. The writer copies request-range and symbology metadata, changes the
schema to MBP-10, clears the input record limit, and writes through a temporary
file before rename.

Book state is isolated by `(publisher_id, instrument_id)`. Each book keeps:

- `order_id -> {side, price, size}`
- bid price levels in descending order
- ask price levels in ascending order
- aggregate size and order count per level

`A`, `C`, `M`, and `R` mutate the book. `T`, `F`, and `N` do not; fills are
paired with book-changing records, and trades are still eligible for MBP-10
output. Cancels remove the reported quantity; modifies carry the new total
order size. A modify for an unknown order is treated as an add, matching the
official order-tracking example. Inconsistent IDs, sides, prices, sizes, or
aggregate overflow fail fast.

Normal book changes are coalesced per `(publisher_id, instrument_id)` event.
The last change that touched visible depth supplies the output event fields,
and the completed book at `F_LAST` supplies all ten levels. Transient states
inside a multi-record event are not published. Every Trade remains a separate
output record; Fill detail is omitted. If an event has trades and a book
update, the final book update carries `F_LAST`.

Historical MBO snapshots are a clear followed by zero or more adds, all marked
`F_SNAPSHOT`. They rebuild the full book but produce one `R` MBP-10 snapshot
at completion, and no record for an empty book. Only the best ten levels on
each side are projected; absent levels use `kUndefPrice` and zero size/count.

## CME normalization compatibility

Databento's `GLBX.MDP3` normalization cutover is scheduled for
2026-08-08 and is retroactive over historical data:

- Before the cutover, `F_LAST` is attached to the final MBO update for an
  instrument in an event.
- After the cutover, updates may arrive without `F_LAST`; a separate
  `action=N`, `side=N`, `F_LAST` record closes the event.

The event assembler keeps the last visible book update and pending trades
independently per book, and treats **any** `F_LAST` record as the boundary. A
standalone `N/F_LAST` record closes the event but never mutates the book or
becomes an MBP-10 action. `F_LAST` is moved to the final emitted record when
the boundary itself has no MBP-10 representation.

This behavior is content-driven. It must not branch on `ts_event`, the requested
historical date, the filename, or the download date. DBN wire-version handling
and CME normalization handling remain separate concerns. Unknown DBN versions,
actions, malformed event boundaries, incomplete EOF events, feed gaps, or
inconsistent order state fail fast with book and sequence context; there is no
silent repair mode.

## Performance model

Records are decoded and encoded as a stream; the full input and output are
never loaded into memory. Order lookup is average `O(1)`, price-level mutation
is `O(log L)`, and each MBP-10 projection visits at most ten levels per side.
Books are sharded by a packed publisher/instrument key. A bounded global output
queue preserves pending Trade ordering across interleaved instruments without
unbounded memory growth.

## Validation

Before 2026-08-08, preserve four matching fixtures for the same request:

1. production MBO and production MBP-10 (legacy normalization)
2. preview MBO and preview MBP-10 (new normalization)

The old historical normalization will not be reproducible after the retroactive
cutover. Market-data files stay outside Git; commit only a local fixture
manifest containing request parameters, library versions, sizes, and SHA-256
checksums.

The Python checker decodes the C++ result and its matching official MBP-10
oracle, then compares record count, order, headers, event fields, and all ten
bid/ask price, size, and count levels. DBN container/compression bytes are not
compared. Focused fixtures cover add, cancel, modify, trade/fill, clear,
snapshot, top-10 boundary changes, interleaved instruments, legacy inline
`F_LAST`, and the new standalone `N/F_LAST`.

Synthetic tests are necessary but not sufficient. The first validated release
is accepted only when both normalization fixture sets have zero
decoded-record mismatches and repeated runs produce identical decoded output.

## References

- [CME normalization cutover](https://databento.com/blog/cme-normalization-changes-2026-07)
- [MBO state management](https://databento.com/docs/examples/order-book/order-tracking)
- [MBO snapshots](https://databento.com/docs/standards-and-conventions/mbo-snapshot)
- [MBP-10 fields](https://databento.com/docs/schemas-and-data-formats/mbp-10)
- [Multi-level event coalescing](https://issues.databento.com/roadmap/events-with-multiple-levels-are-not-properly-coalesced-for-pre-mdp3-cme-mbp-1mbp-10-data)
- [DBN encoding and versioning](https://databento.com/docs/standards-and-conventions/databento-binary-encoding)
