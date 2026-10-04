"""Audit of a b3core shard campaign against the configuration it is supposed to prove.

    python3 b3_audit.py --k 11 --cap 440 --g 1,2,5,12,24,46,83,130,209,310 --depth 3 --N 32768 \
        --bin-sha <sha256 from MANIFEST.txt> [--catalog-count C --catalog-hash H] DIR [DIR ...]

merge_b3_shards.py checks that the shards agree with each other; this script also checks them against
the expected campaign: every shard index exactly once across all DIRs (a rerun of missing shards goes
to a new DIR), exit code 0 and the expected binary for every shard, the exact k, cap, g, variant, mode,
prefix depth and shard count, and optionally the prefix catalog computed by an independent program.
While shards are missing it reports progress and writes missing_<N>.txt; once all are present it runs
the merger and prints its verdict. Exit code 0 only when the merge succeeds with status "none".
"""
import argparse
import json
import pathlib
import statistics
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
import merge_b3_shards  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="+", type=pathlib.Path)
    ap.add_argument("--k", type=int, required=True)
    ap.add_argument("--cap", type=int, required=True)
    ap.add_argument("--g", required=True)
    ap.add_argument("--depth", type=int, required=True)
    ap.add_argument("--N", type=int, required=True)
    ap.add_argument("--bin-sha", required=True)
    ap.add_argument("--catalog-count", type=int)
    ap.add_argument("--catalog-hash")
    args = ap.parse_args()
    g = [int(x) for x in args.g.split(",")]
    expected = {"k": args.k, "cap": args.cap, "g": g, "variant": "multiset", "mode": "canonical",
                "prefix_depth": args.depth, "shard_count": args.N}

    problems, records, found_by_index, seconds = [], [], {}, []
    for d in args.dirs:
        for path in sorted(d.glob("shard-*.jsonl")):
            meta_path = path.with_name(path.name.replace(".jsonl", ".meta.json"))
            try:
                rec = merge_b3_shards.read_jsonl([path])[0]
                meta = json.loads(meta_path.read_text())
            except (OSError, ValueError, IndexError) as exc:
                problems.append(f"{path}: unreadable record or meta ({exc})")
                continue
            idx = rec.get("shard_index")
            bad = [f for f, v in expected.items() if rec.get(f) != v]
            if meta.get("rc") != 0:
                bad.append("rc")
            if meta.get("bin_sha256") != args.bin_sha:
                bad.append("binary")
            if meta.get("shard") != idx:
                bad.append("meta shard index")
            if bad:
                problems.append(f"{path}: differs from the expected campaign in {bad}")
                continue
            if idx in found_by_index:
                problems.append(f"shard {idx}: duplicate ({found_by_index[idx]} and {path})")
                continue
            found_by_index[idx] = path
            records.append(rec)
            seconds.append(rec["seconds"])

    if args.catalog_count is not None and records:
        r0 = records[0]
        if r0["prefix_count"] != args.catalog_count or str(r0["prefix_catalog_hash"]) != str(args.catalog_hash):
            problems.append(f"prefix catalog differs from the independent one: "
                            f"{r0['prefix_count']}/{r0['prefix_catalog_hash']} vs {args.catalog_count}/{args.catalog_hash}")

    missing = [i for i in range(args.N) if i not in found_by_index]
    found = [r["set"] for r in records if r["status"] == "found"]
    print(f"k={args.k} cap={args.cap} depth={args.depth} N={args.N}: shards present {len(records)}, "
          f"missing {len(missing)}, found-witness shards {len(found)}")
    if seconds:
        print(f"shard seconds: sum={sum(seconds):.0f} ({sum(seconds) / 3600:.1f} core-h) "
              f"mean={statistics.mean(seconds):.1f} max={max(seconds):.1f} "
              f"p99={sorted(seconds)[int(0.99 * (len(seconds) - 1))]:.1f}")
    if found:
        print("WITNESS", min(found, key=lambda s: (s[-1], s)))
    for p in problems[:25]:
        print("PROBLEM", p)
    if missing:
        pathlib.Path(f"missing_{args.N}.txt").write_text("".join(f"{i}\n" for i in missing))
        print("VERDICT INCOMPLETE (missing shards listed in", f"missing_{args.N}.txt)")
        return 1
    if problems:
        print("VERDICT INVALID")
        return 1
    try:
        merged = merge_b3_shards.merge(records, None, None)
    except ValueError as exc:
        print("VERDICT MERGE FAILED:", exc)
        return 1
    print(json.dumps({k: merged[k] for k in ("coverage", "status", "set", "nodes", "prefix_count",
                                             "core_seconds", "max_shard_seconds")}))
    print("VERDICT", "NONE (no set within cap)" if merged["status"] == "none" else "FOUND")
    return 0 if merged["status"] == "none" else 2


if __name__ == "__main__":
    sys.exit(main())
