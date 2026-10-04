# A227358(11) = 445

A227358(n) is the least a such that some n-element set of integers in [0, a] has all sums of three
(not necessarily distinct) elements pairwise distinct. Known terms a(1)..a(10) = 0, 1, 4, 11, 23, 45, 82,
129, 208, 309. This computation gives **a(11) = 445**.

Witness: {0, 2, 10, 17, 52, 108, 187, 323, 398, 434, 445} (and its mirror image). `verify_witness.py`
checks it from the definition in a few lines of standard-library Python.

## Method

`src/b3core.cpp` searches all 11-element sets normalised to smallest element 1 and largest element at most
`cap` (so span at most `cap - 1`), keeping the sums of three elements distinct as the set grows. It prunes
with the known values a(1)..a(10) (any i consecutive elements of a valid set span at least a(i)), a counting
bound over the remaining positions, and the reflection x -> max + 1 - x. The tree is cut at depth 3 into a
fixed catalog of prefixes; shard i of N processes the prefixes whose index is i mod N.
`src/merge_b3_shards.py` accepts a run only if every prefix of the catalog was processed exactly once and all
shards agree on configuration and catalog hash; `src/b3_catalog_audit.py` recomputes the catalog
independently, and `src/b3_audit.py` checks every shard against the expected campaign and binary.

## Runs (LUMI-C, October 2026, 4096 shards each)

| cap | meaning | result | tree nodes | core-hours (shards) |
|---|---|---|---|---|
| 440 | span <= 439 | no set | 7 846 706 699 510 | 785.5 |
| 446 | span <= 445 | best set has span 445 | 9 355 743 257 131 | 953.7 |

So no 11-element set of span 444 or less exists, and one of span 445 does. This agrees with J. Tromp's
2013 comment in the OEIS entry ("a(11) = 445 or a(11) < 440"), without depending on it.

Checks: for k = 8 and 9 the program reproduces the earlier node counts exactly (1 437 666 229 nodes for
k = 9, cap 208, on two different machines); with k = 10 it reproduces a(10) = 309 (cap 309: no set; cap 310:
a set of span 309); the summed node count does not depend on the number of worker processes; the prefix
catalog was recomputed by a separate program for both runs (3 393 908 and 3 654 793 prefixes).

This is an exhaustive search with these checks, not a formal certificate.

## Reproduce

    g++ -std=c++17 -O3 -fno-exceptions -pthread -o b3core-portable src/b3core.cpp
    ./b3core-portable --k 11 --cap 446 --g 1,2,5,12,24,46,83,130,209,310 --mode canonical \
        --variant multiset --threads 1 --prefix-depth 3 --shard 0/4096 --output shard-0.jsonl
    # ... shards 0..4095, then
    python3 src/merge_b3_shards.py shard-*.jsonl --expect-g 446 --output merged.json

`results/` holds both merged results, the witness check, the build manifests (compiler, flags, SHA-256 of
source and binary) and the shard assignment of the first run; `SHA256SUMS` covers every file here.

We acknowledge EuroHPC Joint Undertaking for awarding us access to LUMI at CSC, Finland.

## Data and citation

All shard records of both campaigns are archived on Zenodo, https://doi.org/10.5281/zenodo.23132453
(also the citable version of this repository). Code is MIT-licensed (`LICENSE`), the results under `results/` and
the Zenodo archives are CC BY 4.0 (`LICENSE-DATA.md`). See `CITATION.cff`.

Paths under `/project/project_465003389` and `/scratch/project_465003389` in `src/` are the defaults of the LUMI
project that ran the campaigns; `b3_task.sh` and `hq_campaign.sbatch` take `B3_BIN` and `HQ_DIR`, the other
scripts need these paths edited.
