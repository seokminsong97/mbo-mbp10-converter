# Design

## Goal and scope

Build a deterministic, offline converter from Databento MBO DBN files
(`.dbn` or `.dbn.zst`) to MBP-10 DBN or Parquet files.

- C++ performs streaming decode, book reconstruction, and DBN/Parquet
  encoding.
- Python compares decoded output with an official MBP-10 download made using
  the same dataset, symbols, and time range.
- The first correctness target is one week of historical `GLBX.MDP3` data.
  Other datasets must fail clearly until they have dataset-specific fixtures.
- Downloading data, live operation, and cross-publisher consolidation are out
  of scope for the first version.

## Data flow

`DBN reader -> event assembler -> order books -> MBP-10 projector -> DBN or Parquet writer`

The reader first performs a bounded-memory pass over the entire input. For raw
DBN it validates the prelude, declared metadata extent, and every encoded
record length. For compressed DBN it additionally validates every concatenated
Zstandard frame and requires the final frame to complete. This rejects
truncated metadata or records, corrupt frames, and trailing malformed bytes
that a permissive streaming decoder could otherwise treat as end-of-file. The
reader then validates that the metadata declares the MBO schema and lets the
pinned official C++ SDK upgrade supported older DBN versions to DBNv3 in
memory. The writer copies request-range and symbology metadata, changes the
schema to MBP-10, clears the input record limit, and writes through a temporary
file before rename.

The Parquet branch receives the same `Mbp10Msg` objects as the DBN encoder. It
flattens every MBP-10 record into 73 non-null columns: 13 scalar fields and six
price/size/count fields at each of ten depths. Nanosecond timestamps remain
nanosecond timestamps, all prices remain fixed-point `int64`, and unsigned
integer widths are retained. Undefined prices remain `kUndefPrice`, not null.
Zstandard-compressed row groups are bounded at 65,536 records. Converted DBN
metadata, including request range, symbols, and mapping intervals, is embedded
as Parquet schema metadata.

MBP-10 is an aggregate projection and cannot retain MBO-only order identity or
each source order-level mutation. "Lossless Parquet" means lossless with
respect to the complete derived MBP-10 record stream, not reversible back to
the source MBO stream.

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
The final `A`, `C`, or `M` supplies the output price, size, action, and
timestamps even when that change is below level 10. The shallowest visible
depth touched supplies `depth`; `side` identifies the side that touched that
depth, or `N` when both sides did. A Trade after earlier unclosed book changes
starts a fresh quote-impact group for subsequent changes. The completed book
at `F_LAST` supplies all ten levels, so transient states are not published.
Every Trade remains a separate output record and Fill detail is omitted.

Historical MBO snapshots are a clear followed by zero or more adds, all marked
`F_SNAPSHOT`. They rebuild the full book but produce one MBP-10 snapshot at
completion, sourced from the final add with `side=N`; an empty book produces no
record. Only the best ten levels on each side are projected; absent levels use
`kUndefPrice` and zero size/count.

GLBX can also emit an unflagged, sequence-zero empty `R` reset. It participates
in the first completed event for that instrument. If no record follows it, it
is an empty initialization marker and is safely suppressed at EOF.

## CME normalization compatibility

Databento's `GLBX.MDP3` normalization cutover is scheduled for
2026-08-08 and is retroactive over historical data:

- Before the cutover, `F_LAST` is attached to the final MBO update for an
  instrument in an event.
- After the cutover, updates may arrive without `F_LAST`; a separate
  `action=N`, `side=N`, `F_LAST` record closes the event.

The event assembler keeps the final book-changing update, shallowest affected
depth/side, and pending trades independently per book, and treats **any**
`F_LAST` record as the boundary. A standalone `N/F_LAST` record closes the
event but never mutates the book or becomes an MBP-10 action. A generated quote
carries `F_LAST`; an invisible book boundary does not promote `F_LAST` onto an
earlier Trade when no quote is emitted.

This behavior is content-driven. It must not branch on `ts_event`, the requested
historical date, the filename, or the download date. DBN wire-version handling
and CME normalization handling remain separate concerns. Unknown DBN versions,
actions, malformed event boundaries, incomplete EOF events, feed gaps, or
inconsistent order state fail fast with book and sequence context; there is no
silent repair mode.

## Performance model

Records are decoded and encoded as a stream; the full input and output are
never loaded into memory. The Parquet writer buffers at most one configured row
group of completed MBP-10 records. Order lookup is average `O(1)`, price-level
mutation is `O(log L)`, and each MBP-10 projection visits at most ten levels
per side. Books are sharded by a packed publisher/instrument key. A bounded
global output queue preserves pending Trade ordering across interleaved
instruments without unbounded memory growth.

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
compared. The Parquet checker additionally compares all 73 columns with
fixed-point prices and validates embedded metadata. Focused fixtures cover add,
cancel, modify, trade/fill, clear, snapshot, top-10 boundary changes,
interleaved instruments, legacy inline `F_LAST`, the new standalone
`N/F_LAST`, and native Parquet serialization.

Synthetic tests are necessary but not sufficient. The first validated release
is accepted only when both normalization fixture sets have zero
decoded-record mismatches and repeated runs produce identical decoded output.
Downloaded fixture and production inputs must also match their provider
manifest sizes and SHA-256 hashes; structural DBN/Zstandard validation is not
a substitute for source authenticity.

## References

- [CME normalization cutover](https://databento.com/blog/cme-normalization-changes-2026-07)
- [MBO state management](https://databento.com/docs/examples/order-book/order-tracking)
- [MBO snapshots](https://databento.com/docs/standards-and-conventions/mbo-snapshot)
- [MBP-10 fields](https://databento.com/docs/schemas-and-data-formats/mbp-10)
- [Multi-level event coalescing](https://issues.databento.com/roadmap/events-with-multiple-levels-are-not-properly-coalesced-for-pre-mdp3-cme-mbp-1mbp-10-data)
- [DBN encoding and versioning](https://databento.com/docs/standards-and-conventions/databento-binary-encoding)
