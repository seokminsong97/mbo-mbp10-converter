#!/usr/bin/env python3
"""Stream-compare two decoded DBN MBP-10 files record by record."""

from __future__ import annotations

import argparse
import itertools
import sys
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import databento as db


SCALAR_FIELDS = (
    "rtype",
    "publisher_id",
    "instrument_id",
    "ts_event",
    "price",
    "size",
    "action",
    "side",
    "flags",
    "depth",
    "ts_recv",
    "ts_in_delta",
    "sequence",
)
LEVEL_FIELDS = ("bid_px", "ask_px", "bid_sz", "ask_sz", "bid_ct", "ask_ct")
MISSING = object()


def normalized(value: Any) -> Any:
    """Normalize IntEnum-like extension values without touching strings."""
    if isinstance(value, str):
        return value
    try:
        return int(value)
    except (TypeError, ValueError):
        return value


def record_differences(expected: Any, actual: Any) -> Iterator[str]:
    for field in SCALAR_FIELDS:
        expected_value = normalized(getattr(expected, field))
        actual_value = normalized(getattr(actual, field))
        if expected_value != actual_value:
            yield f"{field}: expected={expected_value!r}, actual={actual_value!r}"

    expected_levels = expected.levels
    actual_levels = actual.levels
    if len(expected_levels) != 10 or len(actual_levels) != 10:
        yield (
            "levels length: "
            f"expected={len(expected_levels)}, actual={len(actual_levels)}"
        )
        return

    for depth, (expected_level, actual_level) in enumerate(
        zip(expected_levels, actual_levels, strict=True)
    ):
        for field in LEVEL_FIELDS:
            expected_value = getattr(expected_level, field)
            actual_value = getattr(actual_level, field)
            if expected_value != actual_value:
                yield (
                    f"levels[{depth}].{field}: "
                    f"expected={expected_value!r}, actual={actual_value!r}"
                )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Compare an official MBP-10 DBN oracle with converter output. "
            "Compression/container bytes are intentionally ignored."
        )
    )
    parser.add_argument("expected", type=Path, help="official MBP-10 DBN file")
    parser.add_argument("actual", type=Path, help="converter output DBN file")
    parser.add_argument(
        "--max-differences",
        type=int,
        default=20,
        help="stop after this many differing records (default: 20)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.max_differences <= 0:
        raise SystemExit("--max-differences must be positive")

    expected_store = db.DBNStore.from_file(args.expected)
    actual_store = db.DBNStore.from_file(args.actual)

    metadata_errors: list[str] = []
    if str(expected_store.dataset) != str(actual_store.dataset):
        metadata_errors.append(
            f"dataset: expected={expected_store.dataset}, actual={actual_store.dataset}"
        )
    if str(expected_store.schema) != str(actual_store.schema):
        metadata_errors.append(
            f"schema: expected={expected_store.schema}, actual={actual_store.schema}"
        )
    if metadata_errors:
        for error in metadata_errors:
            print(f"metadata mismatch: {error}", file=sys.stderr)
        return 1

    differing_records = 0
    compared_records = 0
    for index, pair in enumerate(
        itertools.zip_longest(expected_store, actual_store, fillvalue=MISSING)
    ):
        expected, actual = pair
        if expected is MISSING:
            print(
                f"record {index}: expected EOF, actual has another record",
                file=sys.stderr,
            )
            differing_records += 1
        elif actual is MISSING:
            print(
                f"record {index}: actual EOF, expected has another record",
                file=sys.stderr,
            )
            differing_records += 1
        else:
            compared_records += 1
            differences = list(record_differences(expected, actual))
            if differences:
                differing_records += 1
                print(f"record {index} differs:", file=sys.stderr)
                for difference in differences:
                    print(f"  {difference}", file=sys.stderr)

        if differing_records >= args.max_differences:
            print(
                f"stopped after {differing_records} differing records",
                file=sys.stderr,
            )
            return 1

    if differing_records:
        print(
            f"FAIL: {differing_records} differing records "
            f"({compared_records} paired records checked)",
            file=sys.stderr,
        )
        return 1

    print(f"OK: {compared_records} MBP-10 records match exactly")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
