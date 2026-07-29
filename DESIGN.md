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

The reader validates that the input metadata declares the MBO schema, selects
the DBN wire decoder from `metadata.version`, and upgrades supported older DBN
versions to one pinned internal representation. The writer copies the request
range and symbology metadata, changes the schema to MBP-10, and writes through
a temporary file before an atomic rename.

Book state is isolated by `(publisher_id, instrument_id)`. Each book keeps:

- `order_id -> {side, price, size}`
- bid price levels in descending order
- ask price levels in ascending order
- aggregate size and order count per level

`A`, `C`, `M`, and `R` mutate the book. `T`, `F`, and `N` do not; fills are
paired with book-changing records, and trades are still eligible for MBP-10
output. Snapshots and clears rebuild state before normal events continue.
Only the best ten levels on each side are projected, using DBN sentinel values
for empty levels.

## CME normalization compatibility

Databento's `GLBX.MDP3` normalization cutover is scheduled for
2026-08-08 and is retroactive over historical data:

- Before the cutover, `F_LAST` is attached to the final MBO update for an
  instrument in an event.
- After the cutover, updates may arrive without `F_LAST`; a separate
  `action=N`, `side=N`, `F_LAST` record closes the event.

The event assembler therefore buffers pending records independently per book
and treats **any** `F_LAST` record as the boundary. A standalone `N/F_LAST`
record closes the event but never mutates the book or becomes an MBP-10 action.
The projector receives the complete semantic event, so it cannot lose the last
real update when the boundary is a separate record.

This behavior is content-driven. It must not branch on `ts_event`, the requested
historical date, the filename, or the download date. DBN wire-version handling
and CME normalization handling remain separate concerns. Unknown DBN versions,
actions, malformed event boundaries, or inconsistent order state fail fast
with the input record index and book key; there is no silent repair mode.

## Validation

Before 2026-08-08, preserve four matching fixtures for the same request:

1. production MBO and production MBP-10 (legacy normalization)
2. preview MBO and preview MBP-10 (new normalization)

The old historical normalization will not be reproducible after the retroactive
cutover. Market-data files stay outside Git; commit only a local fixture
manifest containing request parameters, library versions, sizes, and SHA-256
checksums.

Python/pytest decodes the C++ result and its matching official MBP-10 oracle,
then compares record count, order, headers, event fields, and all ten
bid/ask price, size, and count levels. DBN container/compression bytes are not
compared. Focused fixtures cover add, cancel, modify, trade/fill, clear,
snapshot, top-10 boundary changes, interleaved instruments, legacy inline
`F_LAST`, and the new standalone `N/F_LAST`.

The first version is accepted only when both normalization fixture sets have
zero decoded-record mismatches and repeated runs produce identical output.

## References

- [CME normalization cutover](https://databento.com/blog/cme-normalization-changes-2026-07)
- [MBO state management](https://databento.com/docs/examples/order-book/order-tracking)
- [MBP-10 fields](https://databento.com/docs/schemas-and-data-formats/mbp-10)
- [DBN encoding and versioning](https://databento.com/docs/standards-and-conventions/databento-binary-encoding)
