#!/usr/bin/env python3
"""Merge and independently validate a complete family of b3indep shards."""

from __future__ import annotations

import argparse
import itertools
import json
import pathlib
import sys
from typing import Any


class MergeError(ValueError):
    pass


def require_int(obj: dict[str, Any], key: str, *, minimum: int | None = None) -> int:
    value = obj.get(key)
    if isinstance(value, bool) or not isinstance(value, int):
        raise MergeError(f"{key!r} must be an integer")
    if minimum is not None and value < minimum:
        raise MergeError(f"{key!r} must be >= {minimum}")
    return value


def load_one(path: pathlib.Path) -> dict[str, Any]:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise MergeError(f"cannot read {path}: {exc}") from exc
    lines = [line for line in text.splitlines() if line.strip()]
    if len(lines) != 1:
        raise MergeError(f"{path}: expected exactly one nonempty JSON line")
    try:
        data = json.loads(lines[0])
    except json.JSONDecodeError as exc:
        raise MergeError(f"{path}: invalid JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise MergeError(f"{path}: top-level JSON value must be an object")
    data["__path"] = str(path)
    return data


def is_b3_by_definition(values: list[int]) -> bool:
    sums = [sum(triple) for triple in itertools.combinations_with_replacement(values, 3)]
    return len(sums) == len(set(sums))


def validate_printed_solutions(shard: dict[str, Any]) -> None:
    path = shard["__path"]
    k = require_int(shard, "k", minimum=1)
    maxel = require_int(shard, "maxel", minimum=-1)
    solutions = require_int(shard, "solutions", minimum=0)
    at_min = require_int(shard, "solutions_at_min", minimum=0)
    max_print = require_int(shard, "max_print", minimum=0)
    min_span = shard.get("min_span")
    printed = shard.get("first_solutions")

    if not isinstance(printed, list):
        raise MergeError(f"{path}: first_solutions must be a list")
    if solutions == 0:
        if min_span is not None or at_min != 0 or printed:
            raise MergeError(f"{path}: inconsistent empty-solution summary")
        return
    if isinstance(min_span, bool) or not isinstance(min_span, int) or min_span < 0:
        raise MergeError(f"{path}: nonempty shard needs a nonnegative integer min_span")
    if not (1 <= at_min <= solutions):
        raise MergeError(f"{path}: invalid solutions_at_min")
    if len(printed) != min(max_print, at_min):
        raise MergeError(f"{path}: wrong number of first_solutions")

    previous: tuple[int, ...] | None = None
    for number, candidate in enumerate(printed):
        label = f"{path}: first_solutions[{number}]"
        if not isinstance(candidate, list) or len(candidate) != k:
            raise MergeError(f"{label}: expected a list of exactly {k} integers")
        if any(isinstance(x, bool) or not isinstance(x, int) for x in candidate):
            raise MergeError(f"{label}: every element must be an integer")
        if candidate[0] != 0 or any(a >= b for a, b in zip(candidate, candidate[1:])):
            raise MergeError(f"{label}: set must start at 0 and be strictly increasing")
        if candidate[-1] > maxel:
            raise MergeError(f"{label}: final element exceeds maxel")
        if candidate[-1] - candidate[0] != min_span:
            raise MergeError(f"{label}: span disagrees with min_span")
        # This is deliberately definition-level and independent of the C bitset update.
        if not is_b3_by_definition(candidate):
            raise MergeError(f"{label}: not B3 by combinations_with_replacement")
        as_tuple = tuple(candidate)
        if previous is not None and as_tuple <= previous:
            raise MergeError(f"{path}: first_solutions is not strictly lexicographically sorted")
        previous = as_tuple


def expected_prefixes(catalog_count: int, shard_index: int, nshards: int) -> int:
    if shard_index >= catalog_count:
        return 0
    return (catalog_count - 1 - shard_index) // nshards + 1


def validate_configuration(shard: dict[str, Any]) -> None:
    path = shard["__path"]
    k = require_int(shard, "k", minimum=1)
    require_int(shard, "maxel", minimum=-1)
    depth = require_int(shard, "depth", minimum=0)
    if depth > k - 1:
        raise MergeError(f"{path}: depth must not exceed k-1")
    require_int(shard, "N", minimum=1)
    require_int(shard, "max_print", minimum=0)
    bounds = shard.get("bounds")
    if bounds is not None:
        if not isinstance(bounds, list):
            raise MergeError(f"{path}: bounds must be null or a list")
        if len(bounds) < k - 1:
            raise MergeError(f"{path}: bounds must contain at least k-1 values")
        if not bounds or bounds[0] != 0:
            raise MergeError(f"{path}: bounds must start with a(1)=0")
        if any(isinstance(x, bool) or not isinstance(x, int) or x < 0 for x in bounds):
            raise MergeError(f"{path}: bounds must contain nonnegative integers")
        if any(a > b for a, b in zip(bounds, bounds[1:])):
            raise MergeError(f"{path}: bounds must be nondecreasing")


def merge(paths: list[pathlib.Path]) -> dict[str, Any]:
    if not paths:
        raise MergeError("no shard files supplied")
    shards = [load_one(path) for path in paths]

    first = shards[0]
    validate_configuration(first)
    nshards = require_int(first, "N", minimum=1)
    if len(shards) != nshards:
        raise MergeError(f"expected exactly N={nshards} files, got {len(shards)}")

    config_keys = (
        "k",
        "maxel",
        "bounds",
        "depth",
        "N",
        "max_print",
        "symmetry_pruning",
        "catalog_count",
        "catalog_hash",
    )
    reference = {key: first.get(key) for key in config_keys}
    if reference["symmetry_pruning"] is not False:
        raise MergeError("symmetry_pruning must be false")
    catalog_count = require_int(first, "catalog_count", minimum=0)
    catalog_hash = first.get("catalog_hash")
    if not isinstance(catalog_hash, str) or len(catalog_hash) != 18 or not catalog_hash.startswith("0x"):
        raise MergeError("catalog_hash must be a 0x-prefixed 16-digit string")
    try:
        int(catalog_hash[2:], 16)
    except ValueError as exc:
        raise MergeError("catalog_hash is not hexadecimal") from exc

    by_index: dict[int, dict[str, Any]] = {}
    for shard in shards:
        path = shard["__path"]
        validate_configuration(shard)
        for key in config_keys:
            if shard.get(key) != reference[key]:
                raise MergeError(f"{path}: configuration mismatch in {key!r}")
        if shard.get("complete") is not True:
            raise MergeError(f"{path}: shard is not complete")
        index = require_int(shard, "shard", minimum=0)
        if index >= nshards:
            raise MergeError(f"{path}: shard index is outside 0..N-1")
        if index in by_index:
            raise MergeError(f"duplicate shard index {index}")
        done = require_int(shard, "prefixes_done", minimum=0)
        wanted = expected_prefixes(catalog_count, index, nshards)
        if done != wanted:
            raise MergeError(f"{path}: prefixes_done={done}, expected {wanted}")
        require_int(shard, "nodes", minimum=0)
        seconds = shard.get("seconds")
        if isinstance(seconds, bool) or not isinstance(seconds, (int, float)) or seconds < 0:
            raise MergeError(f"{path}: invalid seconds")
        validate_printed_solutions(shard)
        by_index[index] = shard

    missing = sorted(set(range(nshards)) - set(by_index))
    if missing:
        raise MergeError(f"missing shard indices: {missing}")

    ordered = [by_index[i] for i in range(nshards)]
    total_nodes = sum(shard["nodes"] for shard in ordered)
    total_solutions = sum(shard["solutions"] for shard in ordered)
    nonempty_spans = [shard["min_span"] for shard in ordered if shard["min_span"] is not None]
    global_min = min(nonempty_spans) if nonempty_spans else None
    global_at_min = sum(
        shard["solutions_at_min"]
        for shard in ordered
        if shard["min_span"] == global_min and global_min is not None
    )
    max_print = first["max_print"]
    candidates = sorted(
        tuple(solution)
        for shard in ordered
        if shard["min_span"] == global_min and global_min is not None
        for solution in shard["first_solutions"]
    )
    if len(candidates) != len(set(candidates)):
        raise MergeError("the same printed solution appears in more than one shard")
    global_first = [list(solution) for solution in candidates[:max_print]]
    if len(global_first) != min(max_print, global_at_min):
        raise MergeError("reported shard prefixes are insufficient for global first_solutions")

    return {
        "verdict": "OK",
        "k": first["k"],
        "maxel": first["maxel"],
        "bounds": first["bounds"],
        "depth": first["depth"],
        "N": nshards,
        "max_print": max_print,
        "symmetry_pruning": False,
        "catalog_count": catalog_count,
        "catalog_hash": catalog_hash,
        "prefixes_done": sum(shard["prefixes_done"] for shard in ordered),
        "nodes": total_nodes,
        "solutions": total_solutions,
        "min_span": global_min,
        "solutions_at_min": global_at_min,
        "first_solutions": global_first,
        "complete": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Merge and independently validate b3indep JSON shard files."
    )
    parser.add_argument("files", nargs="+", type=pathlib.Path)
    args = parser.parse_args()
    try:
        verdict = merge(args.files)
    except MergeError as exc:
        print(json.dumps({"verdict": "REJECT", "error": str(exc)}, ensure_ascii=False))
        return 1
    print(json.dumps(verdict, separators=(",", ":"), ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
