#!/usr/bin/env python3
"""Validate and deterministically merge b3core JSONL shard results."""

from __future__ import annotations

import argparse
import itertools
import json
import pathlib
import sys


FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
MASK64 = (1 << 64) - 1


def fnv_bytes(data: bytes) -> int:
    value = FNV_OFFSET
    for byte in data:
        value ^= byte
        value = (value * FNV_PRIME) & MASK64
    return value


def fnv_u64(value: int, item: int) -> int:
    for byte in int(item).to_bytes(8, "little", signed=False):
        value ^= byte
        value = (value * FNV_PRIME) & MASK64
    return value


def prefix_hash(items: list[tuple[int, list[int]]]) -> str:
    value = FNV_OFFSET
    for prefix_id, prefix in items:
        value = fnv_u64(value, prefix_id)
        value = fnv_u64(value, len(prefix))
        for element in prefix:
            value = fnv_u64(value, element)
    return f"fnv1a64:{value:016x}"


def config_hash(record: dict) -> str:
    g_csv = ",".join(map(str, record["g"]))
    seed_csv = ",".join(map(str, record["seed"]))
    config = (
        f"b3core-shard-v1;k={record['k']};cap={record['cap']}"
        f";mode={record['mode']};variant={record['variant']};g={g_csv}"
        f";seed={seed_csv};prefix_depth={record['prefix_depth']}"
        f";shard_count={record['shard_count']}"
    )
    return f"fnv1a64:{fnv_bytes(config.encode('ascii')):016x}"


def verify_witness(values: list[int], k: int, cap: int, variant: str) -> None:
    if len(values) != k or values != sorted(set(values)) or values[0] != 1 or values[-1] > cap:
        raise ValueError(f"malformed witness: {values}")
    triples = (
        itertools.combinations_with_replacement(values, 3)
        if variant == "multiset"
        else itertools.combinations(values, 3)
    )
    seen: set[int] = set()
    for triple in triples:
        total = sum(triple)
        if total in seen:
            raise ValueError(f"invalid witness, repeated triple sum {total}: {values}")
        seen.add(total)


def read_jsonl(paths: list[pathlib.Path]) -> list[dict]:
    records: list[dict] = []
    for path in paths:
        with path.open(encoding="utf-8") as source:
            for line_number, line in enumerate(source, 1):
                if not line.strip():
                    continue
                try:
                    record = json.loads(line)
                except json.JSONDecodeError as exc:
                    raise ValueError(f"{path}:{line_number}: invalid JSON: {exc}") from exc
                record["_source"] = f"{path}:{line_number}"
                records.append(record)
    if not records:
        raise ValueError("no shard records found")
    return records


def normalized_result(status: str, witness: list[int]) -> str:
    return json.dumps({"set": witness, "status": status}, sort_keys=True, separators=(",", ":"))


def merge(records: list[dict], baseline: dict | None, expected_g: int | None) -> dict:
    required = {
        "schema", "config_hash", "k", "cap", "mode", "variant", "g", "seed",
        "shard_index", "shard_count", "assignment", "prefix_depth", "prefix_count",
        "prefix_catalog_hash", "processed_prefix_hash", "prefix_ids", "prefixes",
        "threads", "tasks", "nodes", "dfs_nodes", "split_nodes",
        "attributed_split_nodes", "status", "complete", "seconds", "set",
    }
    for record in records:
        missing = sorted(required - record.keys())
        if missing:
            raise ValueError(f"{record['_source']}: missing fields: {', '.join(missing)}")
        if record["schema"] != "b3core-shard-v1":
            raise ValueError(f"{record['_source']}: unsupported schema {record['schema']!r}")
        if record["config_hash"] != config_hash(record):
            raise ValueError(f"{record['_source']}: configuration hash mismatch")
        if not record["complete"] or record["status"] not in {"none", "found"}:
            raise ValueError(f"{record['_source']}: incomplete shard ({record['status']})")
        if record["mode"] != "canonical" or record["threads"] != 1:
            raise ValueError(f"{record['_source']}: shards must be canonical and single-threaded")
        if record["assignment"] != "prefix_id_mod_shard_count":
            raise ValueError(f"{record['_source']}: unsupported assignment")

    common_fields = (
        "config_hash", "k", "cap", "mode", "variant", "g", "seed", "shard_count",
        "prefix_depth", "prefix_count", "prefix_catalog_hash", "split_nodes",
    )
    first = records[0]
    for record in records[1:]:
        for field in common_fields:
            if record[field] != first[field]:
                raise ValueError(f"{record['_source']}: {field} differs across shards")

    shard_count = first["shard_count"]
    if len(records) != shard_count:
        raise ValueError(f"expected {shard_count} shard records, got {len(records)}")
    by_index: dict[int, dict] = {}
    for record in records:
        index = record["shard_index"]
        if not isinstance(index, int) or not 0 <= index < shard_count:
            raise ValueError(f"{record['_source']}: invalid shard index {index!r}")
        if index in by_index:
            raise ValueError(f"duplicate shard index {index}")
        by_index[index] = record
    if set(by_index) != set(range(shard_count)):
        raise ValueError("shard indices do not cover 0..N-1 exactly once")

    prefix_count = first["prefix_count"]
    all_prefixes: dict[int, list[int]] = {}
    seen_values: set[tuple[int, ...]] = set()
    for index, record in sorted(by_index.items()):
        ids = record["prefix_ids"]
        prefixes = record["prefixes"]
        expected_ids = list(range(index, prefix_count, shard_count))
        if ids != expected_ids:
            raise ValueError(f"shard {index}: prefix IDs do not match modulo assignment")
        if len(ids) != len(prefixes) or record["tasks"] != len(ids):
            raise ValueError(f"shard {index}: task/prefix length mismatch")
        pairs = list(zip(ids, prefixes, strict=True))
        if prefix_hash(pairs) != record["processed_prefix_hash"]:
            raise ValueError(f"shard {index}: processed prefix hash mismatch")
        if record["nodes"] != record["dfs_nodes"] + record["attributed_split_nodes"]:
            raise ValueError(f"shard {index}: inconsistent node accounting")
        expected_split = record["split_nodes"] if index == 0 else 0
        if record["attributed_split_nodes"] != expected_split:
            raise ValueError(f"shard {index}: split nodes attributed incorrectly")
        for prefix_id, prefix in pairs:
            if len(prefix) != first["prefix_depth"]:
                raise ValueError(f"shard {index}: prefix {prefix_id} has wrong depth")
            values = tuple(prefix)
            if values in seen_values:
                raise ValueError(f"duplicate semantic prefix {values}")
            seen_values.add(values)
            all_prefixes[prefix_id] = prefix

    if set(all_prefixes) != set(range(prefix_count)):
        raise ValueError("prefix IDs do not cover the full catalog exactly once")
    catalog = [(prefix_id, all_prefixes[prefix_id]) for prefix_id in range(prefix_count)]
    if prefix_hash(catalog) != first["prefix_catalog_hash"]:
        raise ValueError("reconstructed prefix catalog hash mismatch")

    witnesses: list[list[int]] = []
    for record in records:
        if record["status"] == "found":
            verify_witness(record["set"], first["k"], first["cap"], first["variant"])
            witnesses.append(record["set"])
        elif record["set"]:
            raise ValueError(f"{record['_source']}: none shard contains a witness")
    witness = min(witnesses, key=lambda values: (values[-1], values)) if witnesses else []
    status = "found" if witness else "none"
    result_bytes = normalized_result(status, witness)

    nodes = sum(record["nodes"] for record in records)
    baseline_match = None
    baseline_node_match = None
    if baseline is not None:
        baseline_bytes = normalized_result(baseline["status"], baseline.get("set", []))
        baseline_match = result_bytes == baseline_bytes
        if not baseline_match:
            raise ValueError(f"merged result differs from baseline: {result_bytes} != {baseline_bytes}")
        baseline_node_match = nodes == baseline["nodes"]
        if status == "none" and not baseline_node_match:
            raise ValueError(
                f"none-result node count differs from baseline: {nodes} != {baseline['nodes']}"
            )

    if expected_g is not None and (status != "found" or witness[-1] != expected_g):
        raise ValueError(f"merged optimum is not expected g={expected_g}: {result_bytes}")

    return {
        "schema": "b3core-merge-v1",
        "config_hash": first["config_hash"],
        "k": first["k"],
        "cap": first["cap"],
        "mode": first["mode"],
        "variant": first["variant"],
        "shard_count": shard_count,
        "prefix_depth": first["prefix_depth"],
        "prefix_count": prefix_count,
        "coverage": "every_prefix_exactly_once",
        "status": status,
        "set": witness,
        "result_bytes": result_bytes,
        "nodes": nodes,
        "dfs_nodes": sum(record["dfs_nodes"] for record in records),
        "split_nodes": first["split_nodes"],
        "core_seconds": round(sum(record["seconds"] for record in records), 3),
        "max_shard_seconds": round(max(record["seconds"] for record in records), 3),
        "baseline_result_match": baseline_match,
        "baseline_node_match": baseline_node_match,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", nargs="+", type=pathlib.Path)
    parser.add_argument("--baseline", type=pathlib.Path)
    parser.add_argument("--expect-g", type=int)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    try:
        records = read_jsonl(args.results)
        baseline = None
        if args.baseline:
            baseline_records = read_jsonl([args.baseline])
            if len(baseline_records) != 1:
                raise ValueError("baseline must contain exactly one JSON object")
            baseline = baseline_records[0]
        result = merge(records, baseline, args.expect_g)
    except (OSError, KeyError, TypeError, ValueError) as exc:
        print(f"merge error: {exc}", file=sys.stderr)
        return 2

    payload = json.dumps(result, sort_keys=True, separators=(",", ":")) + "\n"
    if args.output:
        temporary = args.output.with_suffix(args.output.suffix + ".tmp")
        temporary.write_text(payload, encoding="utf-8")
        temporary.replace(args.output)
    sys.stdout.write(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
