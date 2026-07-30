#!/usr/bin/env python3
"""Stream-compare an MBP-10 Parquet file with an official DBN oracle."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Iterator, Optional

import databento as db
import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq


BATCH_SIZE = 2**16
BASE_COLUMNS = (
    "ts_recv",
    "ts_event",
    "rtype",
    "publisher_id",
    "instrument_id",
    "action",
    "side",
    "depth",
    "price",
    "size",
    "flags",
    "ts_in_delta",
    "sequence",
)
LEVEL_FIELDS = ("bid_px", "ask_px", "bid_sz", "ask_sz", "bid_ct", "ask_ct")
EXPECTED_COLUMNS = BASE_COLUMNS + tuple(
    f"{field}_{depth:02d}"
    for depth in range(10)
    for field in LEVEL_FIELDS
)
EXPECTED_TYPES = (
    pa.timestamp("ns", tz="UTC"),
    pa.timestamp("ns", tz="UTC"),
    pa.uint8(),
    pa.uint16(),
    pa.uint32(),
    pa.string(),
    pa.string(),
    pa.uint8(),
    pa.int64(),
    pa.uint32(),
    pa.uint8(),
    pa.int32(),
    pa.uint32(),
) + tuple(
    field_type
    for _ in range(10)
    for field_type in (
        pa.int64(),
        pa.int64(),
        pa.uint32(),
        pa.uint32(),
        pa.uint32(),
        pa.uint32(),
    )
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Compare an official MBP-10 DBN oracle with fixed-point Parquet "
            "output, including all scalar fields and ten book levels."
        )
    )
    parser.add_argument("expected", type=Path, help="official MBP-10 DBN file")
    parser.add_argument("actual", type=Path, help="converter Parquet file")
    return parser.parse_args()


def official_batches(
    path: Path, schema: pa.Schema
) -> Iterator[pa.RecordBatch]:
    store = db.DBNStore.from_file(path)
    frames = store.to_df(
        price_type="fixed",
        pretty_ts=False,
        map_symbols=False,
        count=BATCH_SIZE,
    )
    for frame in frames:
        frame = frame.reset_index()
        frame["ts_recv"] = pd.to_datetime(frame["ts_recv"], unit="ns", utc=True)
        frame["ts_event"] = pd.to_datetime(
            frame["ts_event"], unit="ns", utc=True
        )
        table = pa.Table.from_pandas(
            frame, schema=schema, preserve_index=False
        )
        yield from table.to_batches()


def first_difference(
    expected: pa.RecordBatch, actual: pa.RecordBatch
) -> Optional[tuple[int, str, Any, Any]]:
    for column_index, name in enumerate(EXPECTED_COLUMNS):
        expected_column = expected.column(column_index)
        actual_column = actual.column(column_index)
        if expected_column.equals(actual_column):
            continue
        for row in range(expected.num_rows):
            expected_value = expected_column[row].as_py()
            actual_value = actual_column[row].as_py()
            if expected_value != actual_value:
                return row, name, expected_value, actual_value
    return None


def validate_schema(parquet_file: pq.ParquetFile) -> dict[str, str]:
    schema = parquet_file.schema_arrow
    if tuple(schema.names) != EXPECTED_COLUMNS:
        raise ValueError(
            "unexpected Parquet columns:\n"
            f"expected={EXPECTED_COLUMNS!r}\nactual={tuple(schema.names)!r}"
        )
    if len(schema.names) != 73:
        raise ValueError(f"expected 73 columns, found {len(schema.names)}")
    for name, expected_type in zip(EXPECTED_COLUMNS, EXPECTED_TYPES):
        field = schema.field(name)
        if field.type != expected_type:
            raise ValueError(
                f"{name}: expected type {expected_type}, found {field.type}"
            )
        if field.nullable:
            raise ValueError(f"{name}: expected a non-nullable field")

    metadata = {
        key.decode(): value.decode()
        for key, value in (schema.metadata or {}).items()
    }
    if metadata.get("dbn.schema") != "mbp-10":
        raise ValueError("Parquet metadata does not identify schema mbp-10")
    if metadata.get("mbo_mbp10.price_encoding") != "fixed":
        raise ValueError("Parquet metadata does not identify fixed prices")
    json.loads(metadata["dbn.metadata"])
    return metadata


def normalized_mappings(mappings: dict[str, Any]) -> dict[str, Any]:
    return {
        raw_symbol: [
            {
                "start": interval["start_date"].isoformat(),
                "end": interval["end_date"].isoformat(),
                "symbol": interval["symbol"],
            }
            for interval in intervals
        ]
        for raw_symbol, intervals in mappings.items()
    }


def metadata_differences(
    expected: Any, encoded_actual: str
) -> Iterator[str]:
    actual = json.loads(encoded_actual)
    actual_mappings = {
        mapping["raw_symbol"]: mapping["intervals"]
        for mapping in actual["mappings"]
    }
    expected_values = {
        "dataset": expected.dataset,
        "schema": str(expected.schema),
        "start": expected.start,
        "end": expected.end,
        "limit": expected.limit or 0,
        "stype_in": (
            str(expected.stype_in) if expected.stype_in is not None else None
        ),
        "stype_out": str(expected.stype_out),
        "ts_out": expected.ts_out,
        "symbols": expected.symbols,
        "partial": expected.partial,
        "not_found": expected.not_found,
        "mappings": normalized_mappings(expected.mappings),
    }
    actual_values = {
        key: actual[key]
        for key in expected_values
        if key != "mappings"
    }
    actual_values["mappings"] = actual_mappings
    for key, expected_value in expected_values.items():
        if actual_values[key] != expected_value:
            yield (
                f"{key}: expected={expected_value!r}, "
                f"actual={actual_values[key]!r}"
            )


def main() -> int:
    args = parse_args()
    parquet_file = pq.ParquetFile(args.actual)
    metadata = validate_schema(parquet_file)
    expected_store = db.DBNStore.from_file(args.expected)
    metadata_errors = list(
        metadata_differences(
            expected_store.metadata, metadata["dbn.metadata"]
        )
    )
    if metadata_errors:
        for error in metadata_errors:
            print(f"metadata mismatch: {error}", file=sys.stderr)
        return 1

    expected_iter = iter(official_batches(args.expected, parquet_file.schema_arrow))
    actual_iter = iter(parquet_file.iter_batches(batch_size=BATCH_SIZE))
    expected_batch = next(expected_iter, None)
    actual_batch = next(actual_iter, None)
    expected_offset = 0
    actual_offset = 0
    compared_rows = 0

    while expected_batch is not None and actual_batch is not None:
        row_count = min(
            expected_batch.num_rows - expected_offset,
            actual_batch.num_rows - actual_offset,
        )
        expected_slice = expected_batch.slice(expected_offset, row_count)
        actual_slice = actual_batch.slice(actual_offset, row_count)
        difference = first_difference(expected_slice, actual_slice)
        if difference is not None:
            row, column, expected_value, actual_value = difference
            print(
                f"row {compared_rows + row}, column {column}: "
                f"expected={expected_value!r}, actual={actual_value!r}",
                file=sys.stderr,
            )
            return 1

        compared_rows += row_count
        expected_offset += row_count
        actual_offset += row_count
        if expected_offset == expected_batch.num_rows:
            expected_batch = next(expected_iter, None)
            expected_offset = 0
        if actual_offset == actual_batch.num_rows:
            actual_batch = next(actual_iter, None)
            actual_offset = 0

    if expected_batch is not None:
        print(
            f"row {compared_rows}: actual Parquet reached EOF first",
            file=sys.stderr,
        )
        return 1
    if actual_batch is not None:
        print(
            f"row {compared_rows}: official DBN reached EOF first",
            file=sys.stderr,
        )
        return 1

    print(
        f"OK: {compared_rows} MBP-10 rows match exactly across 73 columns"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
