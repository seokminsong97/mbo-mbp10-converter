# mbo-mbp10-converter

A deterministic, offline MBO-to-MBP-10 reconstruction engine for Databento
Binary Encoding (DBN) files.

- C++17 performs streaming DBN I/O, order-book reconstruction, event
  coalescing, and MBP-10 encoding.
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

The converter builds and passes focused unit tests plus raw/Zstandard DBN
round-trip tests. It handles the daily MBO snapshot as one coalesced MBP
snapshot and emits at most one quote update per completed instrument event,
while retaining every Trade record.

Exact parity with Databento's normalization is **not yet certified**. Before
production use, compare against official MBO and MBP-10 files requested with
the same dataset, symbols, and time range. The Python checker is provided for
that validation.

## Build

Requirements:

- CMake 3.24 or newer
- a C++17 compiler
- OpenSSL 3 and Zstandard
- network access during the first configure, unless `databento-cpp` and its
  dependencies are already installed

The build pins the official C++ SDK to `v0.62.1`.

On macOS:

```sh
brew install cmake openssl@3 zstd
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

On Ubuntu, install `cmake`, `build-essential`, `libssl-dev`, and `libzstd-dev`,
then run the same CMake build without `OPENSSL_ROOT_DIR`.

## Convert

```sh
./build/mbo-mbp10 input.dbn.zst output.dbn.zst
```

Output compression is selected by the filename:

- `.dbn`: uncompressed DBN
- `.dbn.zst`: Zstandard-compressed DBN

Safe defaults require MBO metadata, `GLBX.MDP3`, `ts_out=false`, a valid
initial `R` snapshot/clear for each book, complete `F_LAST` event boundaries,
and no `MAYBE_BAD_BOOK` records. Run `mbo-mbp10 --help` for explicit override
options. Existing output is preserved unless `--force` is supplied, and the
writer uses a temporary file before commit.

For historical conversion, request data from `00:00:00 UTC` so the daily MBO
snapshot is present. An arbitrary intraday slice generally cannot reconstruct
the orders that were already resting before the slice began.

## Compare with the official MBP-10 download

```sh
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements-dev.txt
python tools/compare_dbn.py official.mbp-10.dbn.zst converted.dbn.zst
```

The checker streams both files and compares record order, headers, event
fields, flags, and all ten bid/ask price, size, and count levels. DBN container
bytes and compression frames are intentionally not compared.

See [DESIGN.md](DESIGN.md) for the event and order-book rules.

## Important notice

This is an independent, unofficial open-source project. It is not affiliated
with, endorsed by, sponsored by, approved by, or supported by Databento, Inc.
The Databento name and related marks belong to their respective owners and are
used here only to describe format compatibility. Do not contact Databento for
support for this project.

This repository licenses source code only. It does not grant any license or
other rights to market data, DBN files obtained from a data provider, or data
owned by an exchange or another third party. Conversion into MBP-10 does not
remove restrictions from the source data or create redistribution rights.
Users are solely responsible for obtaining valid data access and complying
with all applicable provider terms, exchange licenses, laws, and regulations.
Do not commit or redistribute API keys or downloaded/derived market data unless
you are expressly authorized to do so.

The software and its output are provided **"AS IS"**, without warranties of
accuracy, completeness, merchantability, fitness for a particular purpose, or
non-infringement. Verify converted data independently before relying on it.
Neither this project nor its contributors provide investment, trading, legal,
or compliance advice, and they are not liable for trading losses, data loss, or
other damages arising from use of the software, to the extent permitted by
applicable law.

The source code is available under the [Apache License 2.0](LICENSE). If this
notice conflicts with the license as to the software, the license controls.
