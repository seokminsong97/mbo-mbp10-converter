# mbo-mbp10-converter

A deterministic, offline MBO-to-MBP-10 reconstruction engine for Databento
Binary Encoding (DBN) files.

- C++17 performs streaming DBN I/O, order-book reconstruction, event
  coalescing, and MBP-10 DBN or Parquet encoding.
- Python independently compares decoded output with an official MBP-10
  download.
- The current correctness scope is `GLBX.MDP3`.
- Both the legacy inline `F_LAST` form and the standalone `N/F_LAST` form
  scheduled for production on 2026-08-08 are handled from record content, not
  dates or filenames.

The project uses the official `databento-cpp` SDK for DBN decoding and
encoding. The MBO-to-MBP-10 conversion algorithm itself is an independent,
unofficial implementation; Databento does not publish or certify this
converter.

## Status

The converter builds and passes focused unit tests plus raw/Zstandard DBN and
native Parquet round-trip tests. It handles the daily MBO snapshot as one
coalesced MBP snapshot and emits at most one quote update per completed
instrument event, while retaining every Trade record.

The current logic has zero decoded-record mismatches across the local
seven-file `6E.FUT` fixture covering 2026-06-01 through 2026-06-08:
13,666,212 derived MBP-10 records match the corresponding official files in
record order, headers, event fields, flags, and all ten levels. The native
Parquet output also matches all 13,666,212 official records across its complete
73-column schema, and its request-range and symbology metadata match modulo the
SDK's DBN version upgrade. Broader symbol coverage and the post-cutover
standalone-`N/F_LAST` normalization are **not yet certified**. Before production
use, compare against official MBO and MBP-10 files requested with the same
dataset, symbols, and time range.

## Build

Requirements:

- CMake 3.24 or newer
- a C++17 compiler
- OpenSSL 3 and Zstandard
- Apache Arrow C++ with Parquet support
- network access during the first configure, unless `databento-cpp` and its
  dependencies are already installed

The build pins the official C++ SDK to `v0.62.1`.

On macOS:

```sh
brew install cmake openssl@3 zstd apache-arrow
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

On Ubuntu, install `cmake`, `build-essential`, `libssl-dev`, `libzstd-dev`,
and an Apache Arrow C++ distribution with Parquet. Then run the same CMake
build without `OPENSSL_ROOT_DIR`.

If Arrow came from a PyArrow wheel or a distribution without CMake package
files, point CMake at the directory containing its `include`, `libarrow`, and
`libparquet` files:

```sh
cmake -S . -B build \
  -DMBO_MBP10_ARROW_ROOT=/path/to/site-packages/pyarrow
```

## Convert

```sh
./build/mbo-mbp10 input.dbn.zst output.dbn.zst
./build/mbo-mbp10 input.dbn.zst output.parquet
```

Output format and compression are selected by the filename:

- `.dbn`: uncompressed DBN
- `.dbn.zst`: Zstandard-compressed DBN
- `.parquet`: Zstandard-compressed Parquet

Safe defaults require MBO metadata, `GLBX.MDP3`, `ts_out=false`, a valid
initial `R` snapshot/clear for each book, complete `F_LAST` event boundaries,
and no `MAYBE_BAD_BOOK` records. Before decoding, the converter streams through
the complete input and validates the raw DBN framing or every Zstandard frame,
the declared metadata length, and every DBN record length. Truncated metadata,
partial records, corrupt compression frames, and trailing malformed data fail
before an output file is created. Run `mbo-mbp10 --help` for explicit override
options. Existing output is preserved unless `--force` is supplied, and the
writer uses a temporary file before commit.

For production archives, also verify each downloaded file against the
provider's manifest SHA-256 before conversion. Framing validation proves that
the file is structurally complete; the independent manifest hash proves that
the bytes are the intended download.

For historical conversion, request data from `00:00:00 UTC` so the daily MBO
snapshot is present. An arbitrary intraday slice generally cannot reconstruct
the orders that were already resting before the slice began.

## Parquet representation

Parquet output uses the standard flattened MBP-10 column names: 13 scalar
columns followed by six fields for each of ten levels, for 73 columns total.
`ts_recv` and `ts_event` are UTC nanosecond timestamps. `price`, `bid_px_00`
through `bid_px_09`, and `ask_px_00` through `ask_px_09` remain signed
fixed-point `int64` values where one unit is `1e-9`; they are never converted
to floating point. Unsigned DBN fields retain unsigned Arrow types, `action`
and `side` are one-character strings, and missing levels retain the exact DBN
`kUndefPrice` sentinel plus zero size/count. The writer uses bounded 65,536-row
groups and embeds the converted DBN request-range and symbology metadata under
the `dbn.metadata` schema key.

MBO-to-MBP-10 conversion is inherently an aggregation and therefore cannot
preserve MBO-only information such as individual `order_id` values or every
order-level `A/C/M/F` message. The preservation guarantee is that every field
of every *derived MBP-10 record* is written to Parquet exactly, in order,
without price, timestamp, integer, flag, or level loss.

## Compare with the official MBP-10 download

```sh
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements-dev.txt
python tools/compare_dbn.py official.mbp-10.dbn.zst converted.dbn.zst
python tools/compare_parquet.py official.mbp-10.dbn.zst converted.parquet
```

The checkers stream the files and compare record order, headers, event fields,
flags, and all ten bid/ask price, size, and count levels. The Parquet checker
also validates its 73-column fixed-point schema and embedded metadata. DBN
container bytes and compression frames are intentionally not compared.

See [DESIGN.md](DESIGN.md) for the event and order-book rules.

## Notice

This is an independent, unofficial project and is not affiliated with or
endorsed by Databento, Inc. Databento and related marks belong to their
respective owners and are used only to identify compatible formats.

This repository contains and licenses software, not market data. Users are
responsible for ensuring that their access, use, and redistribution of data
comply with applicable provider and exchange terms. The software is provided
under the [Apache License 2.0](LICENSE), without warranties; validate converted
output before relying on it.
