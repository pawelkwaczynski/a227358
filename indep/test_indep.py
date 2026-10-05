#!/usr/bin/env python3
"""Short, reproducible independent tests for b3indep (Python stdlib only)."""

from __future__ import annotations

import itertools
import json
import pathlib
import shutil
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parent
SOURCE = ROOT / "b3indep.c"
BINARY = ROOT / "b3indep"
FAULT_BINARY = ROOT / "b3indep_fault"
MERGER = ROOT / "merge_indep.py"
KNOWN = [0, 1, 4, 11, 23, 45, 82, 129]


def compile_binary(output: pathlib.Path, *extra: str) -> None:
    subprocess.run(
        [
            "cc",
            "-O3",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-pedantic",
            *extra,
            str(SOURCE),
            "-o",
            str(output),
        ],
        cwd=ROOT,
        check=True,
    )


def run_enum(
    binary: pathlib.Path,
    k: int,
    maxel: int,
    *,
    bounds: list[int] | None = None,
    depth: int | None = None,
    shard: str | None = None,
    max_print: int = 100,
) -> dict:
    command = [str(binary), "--k", str(k), "--maxel", str(maxel), "--max-print", str(max_print)]
    if bounds is not None:
        command += ["--bounds", ",".join(map(str, bounds))]
    if depth is not None or shard is not None:
        assert depth is not None and shard is not None
        command += ["--depth", str(depth), "--shard", shard]
    completed = subprocess.run(command, cwd=ROOT, check=True, text=True, capture_output=True)
    lines = [line for line in completed.stdout.splitlines() if line.strip()]
    assert len(lines) == 1, completed.stdout
    result = json.loads(lines[0])
    assert result["complete"] is True
    return result


def b3_by_definition(values: tuple[int, ...]) -> bool:
    sums = [sum(triple) for triple in itertools.combinations_with_replacement(values, 3)]
    return len(sums) == len(set(sums))


def brute(k: int, maxel: int) -> list[list[int]]:
    if maxel < 0:
        return []
    return [
        list((0, *tail))
        for tail in itertools.combinations(range(1, maxel + 1), k - 1)
        if b3_by_definition((0, *tail))
    ]


def test_bruteforce() -> None:
    summaries: list[str] = []
    for k, maxel in enumerate(KNOWN[:5], start=1):
        expected = brute(k, maxel)
        actual = run_enum(BINARY, k, maxel, max_print=len(expected) + 1)
        assert actual["solutions"] == len(expected)
        assert actual["first_solutions"] == expected
        assert actual["solutions_at_min"] == len(expected)
        summaries.append(f"k={k},M={maxel}: {len(expected)}")

    # Also compare the full count and the complete minimum-span list away from
    # the optimum, where not every enumerated set is printed by design.
    expected = brute(5, 30)
    min_span = min(values[-1] for values in expected)
    expected_min = [values for values in expected if values[-1] == min_span]
    actual = run_enum(BINARY, 5, 30, max_print=len(expected_min) + 1)
    assert actual["solutions"] == len(expected)
    assert actual["min_span"] == min_span
    assert actual["solutions_at_min"] == len(expected_min)
    assert actual["first_solutions"] == expected_min
    summaries.append(f"k=5,M=30: {len(expected)} total, {len(expected_min)} at min")
    print("[OK] brute force Python == C: " + "; ".join(summaries))


def test_bounds(binary: pathlib.Path = BINARY) -> list[int]:
    # A bound enters the next test only after its own positive/negative pair
    # has been established.  Thus the suffix bounds are accumulated from prior
    # verified results rather than assumed en bloc.
    verified: list[int] = []
    for k, target in enumerate(KNOWN, start=1):
        usable_bounds = verified if verified else [0]
        positive = run_enum(binary, k, target, bounds=usable_bounds, max_print=3)
        negative = run_enum(binary, k, target - 1, bounds=usable_bounds, max_print=3)
        assert positive["solutions"] > 0, f"k={k}, M={target}: expected a solution"
        assert negative["solutions"] == 0, f"k={k}, M={target - 1}: expected no solution"
        verified.append(target)
    print("[OK] bounds: independently accumulated a(1..8) = " + ",".join(map(str, verified)))
    return verified


def run_family(nshards: int, bounds: list[int]) -> list[dict]:
    return [
        run_enum(
            BINARY,
            7,
            82,
            bounds=bounds[:6],
            depth=3,
            shard=f"{index}/{nshards}",
            max_print=20,
        )
        for index in range(nshards)
    ]


def test_shards(bounds: list[int]) -> None:
    families = {n: run_family(n, bounds) for n in (1, 7, 16)}
    aggregate: dict[int, tuple[int, int]] = {}
    reference_catalog = None
    for nshards, shards in families.items():
        catalogs = {(item["catalog_count"], item["catalog_hash"]) for item in shards}
        assert len(catalogs) == 1
        catalog = next(iter(catalogs))
        if reference_catalog is None:
            reference_catalog = catalog
        assert catalog == reference_catalog
        assert sum(item["prefixes_done"] for item in shards) == catalog[0]
        aggregate[nshards] = (
            sum(item["nodes"] for item in shards),
            sum(item["solutions"] for item in shards),
        )
    assert len(set(aggregate.values())) == 1, aggregate

    # Exercise the standalone merger on the seven-shard family.
    temp_dir = pathlib.Path(tempfile.mkdtemp(prefix="indep-shards-", dir=ROOT))
    try:
        paths = []
        for item in families[7]:
            path = temp_dir / f"shard-{item['shard']:02d}.json"
            path.write_text(json.dumps(item, separators=(",", ":")) + "\n", encoding="utf-8")
            paths.append(path)
        merged = subprocess.run(
            ["python3", str(MERGER), *map(str, paths)],
            cwd=ROOT,
            check=True,
            text=True,
            capture_output=True,
        )
        verdict = json.loads(merged.stdout)
        assert verdict["verdict"] == "OK"
        assert (verdict["nodes"], verdict["solutions"]) == aggregate[7]

        # Now corrupt one printed set while keeping its shape, ordering and
        # span plausible.  The merger must reject it specifically by the
        # definition-level combinations_with_replacement check.
        victim = next(item for item in families[7] if item["first_solutions"])
        victim_path = temp_dir / f"shard-{victim['shard']:02d}.json"
        corrupted = json.loads(victim_path.read_text(encoding="utf-8"))
        corrupted["first_solutions"][0] = [0, 1, 2, 3, 4, 5, corrupted["min_span"]]
        victim_path.write_text(
            json.dumps(corrupted, separators=(",", ":")) + "\n", encoding="utf-8"
        )
        rejected = subprocess.run(
            ["python3", str(MERGER), *map(str, paths)],
            cwd=ROOT,
            check=False,
            text=True,
            capture_output=True,
        )
        rejection = json.loads(rejected.stdout)
        assert rejected.returncode == 1
        assert rejection["verdict"] == "REJECT"
        assert "not B3" in rejection["error"]
    finally:
        shutil.rmtree(temp_dir)

    nodes, solutions = aggregate[1]
    print(
        f"[OK] shards N=1,7,16: nodes={nodes}, solutions={solutions}, "
        f"catalog={reference_catalog[0]} {reference_catalog[1]}"
    )
    print("[OK] merger rejects a deliberately corrupted printed set")


def test_fault_injection() -> None:
    try:
        compile_binary(FAULT_BINARY, "-DB3_STRICT_BOUND_BUG")
        broken = run_enum(FAULT_BINARY, 2, 1, bounds=[0], max_print=3)
        detected = broken["solutions"] == 0
        assert detected, "the deliberately strict bound was not detected"
        correct = run_enum(BINARY, 2, 1, bounds=[0], max_print=3)
        assert correct["solutions"] > 0
    finally:
        FAULT_BINARY.unlink(missing_ok=True)
    print("[OK] mutation test: strict '>' loses {0,1}; regular build restores it")


def benchmark(bounds: list[int]) -> None:
    result = run_enum(
        BINARY,
        8,
        129,
        bounds=bounds[:7],
        depth=3,
        shard="0/1",
        max_print=0,
    )
    rate = result["nodes"] / result["seconds"] if result["seconds"] else float("inf")
    print(
        f"[BENCH] k=8,M=129,D=3: nodes={result['nodes']}, "
        f"seconds={result['seconds']:.6f}, nodes/s={rate:.0f}"
    )


def main() -> None:
    compile_binary(BINARY)
    test_bruteforce()
    bounds = test_bounds()
    test_shards(bounds)
    test_fault_injection()
    benchmark(bounds)
    print("ALL TESTS PASSED")


if __name__ == "__main__":
    main()
