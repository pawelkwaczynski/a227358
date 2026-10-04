#!/usr/bin/env python3
"""Independently reconstruct the canonical multiset-B3 prefix catalog.

The B3 test below is definition-based: when y is appended to an increasing
set S, it explicitly constructs the new triple sums

    y + a + b  (a <= b in S),  2*y + a  (a in S),  and  3*y.

It deliberately does not invoke b3core.  Python integers are used only as a
compact set representation for the sums constructed from the definition; the
program has no dependency on b3core's fixed-width word arrays.  The traversal
and pruning order do match b3core's canonical shard catalog, because those
details are precisely what this program audits.
"""

from __future__ import annotations

import argparse
import bisect
import json
from collections.abc import Sequence


FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
MASK64 = (1 << 64) - 1
FNV_U64_FACTOR = pow(FNV_PRIME, 8, 1 << 64)
FNV_ZERO4_FACTOR = pow(FNV_PRIME, 4, 1 << 64)

# Canonical mode uses an inclusive incumbent bound by adding one here.
SLACK = 1


def parse_g(text: str) -> list[int]:
    """Parse the comma/space-separated lower bounds accepted by --g."""
    try:
        values = [int(item) for item in text.replace(",", " ").split()]
    except ValueError as exc:
        raise argparse.ArgumentTypeError("--g must contain integers") from exc
    if not values:
        raise argparse.ArgumentTypeError("--g must not be empty")
    return values


def extend_triple_sums(
    values: tuple[int, ...], pair_sums: int, triple_sums: int, y: int
) -> int | None:
    """Return the updated sum mask, or None if the B3 property would fail.

    The mask is only a compact representation of the three sum families.  In
    particular, shifting the pair-sum set realizes {y+a+b}; no b3core routine
    or fixed-width mask transition is used.
    """
    # Triples with exactly one occurrence of y: one bit y+a+b for every
    # explicitly maintained unordered pair sum a+b.
    introduced = pair_sums << y

    # Triples with exactly two occurrences of y.
    for a in values:
        introduced |= 1 << (2 * y + a)

    # The triple containing y three times.
    introduced |= 1 << (3 * y)

    # A smaller population means that two newly introduced triples collided.
    expected = len(values) * (len(values) + 1) // 2 + len(values) + 1
    if introduced.bit_count() != expected or introduced & triple_sums:
        return None
    return triple_sums | introduced


def is_valid_add(
    values: tuple[int, ...], pair_sums: int, triple_sums: int, y: int
) -> bool:
    """Early-exit form of the same three-family definition check."""
    # 3y must avoid old triples and y+(a+b).
    if triple_sums & (1 << (3 * y)):
        return False
    if pair_sums & (1 << (2 * y)):
        return False

    twice_y = 2 * y
    for a in values:
        # 2y+a must avoid both y+(b+c) and old triples.
        if pair_sums & (1 << (y + a)):
            return False
        if triple_sums & (1 << (twice_y + a)):
            return False

    # Every y+(a+b) must avoid every old triple.
    return not ((pair_sums << y) & triple_sums)


def extend(
    values: tuple[int, ...],
    pair_sums: int,
    triple_sums: int,
    y: int,
) -> tuple[int, int, bool] | None:
    """Append y using direct sums and return the new state plus pair-dup flag."""
    if not is_valid_add(values, pair_sums, triple_sums, y):
        return None

    next_triples = triple_sums | (pair_sums << y)
    twice_y = 2 * y
    for a in values:
        next_triples |= 1 << (twice_y + a)
    next_triples |= 1 << (3 * y)

    next_pairs = pair_sums
    pair_duplicate = False
    for total in (y + a for a in (*values, y)):
        bit = 1 << total
        if next_pairs & bit:
            pair_duplicate = True
        next_pairs |= bit
    return next_pairs, next_triples, pair_duplicate


def fnv_u64(hash_value: int, value: int) -> int:
    """Hash one uint64 as eight little-endian bytes, as b3core does."""
    for shift in range(0, 64, 8):
        hash_value ^= (value >> shift) & 0xFF
        hash_value = (hash_value * FNV_PRIME) & MASK64
    return hash_value


def fnv_u64_unrolled(hash_value: int, value: int) -> int:
    """The same operation without a Python loop (hot path for prefix IDs)."""
    hash_value = ((hash_value ^ (value & 0xFF)) * FNV_PRIME) & MASK64
    value >>= 8
    hash_value = ((hash_value ^ (value & 0xFF)) * FNV_PRIME) & MASK64
    value >>= 8
    hash_value = ((hash_value ^ (value & 0xFF)) * FNV_PRIME) & MASK64
    value >>= 8
    hash_value = ((hash_value ^ (value & 0xFF)) * FNV_PRIME) & MASK64
    value >>= 8
    hash_value = ((hash_value ^ (value & 0xFF)) * FNV_PRIME) & MASK64
    value >>= 8
    hash_value = ((hash_value ^ (value & 0xFF)) * FNV_PRIME) & MASK64
    value >>= 8
    hash_value = ((hash_value ^ (value & 0xFF)) * FNV_PRIME) & MASK64
    value >>= 8
    return ((hash_value ^ (value & 0xFF)) * FNV_PRIME) & MASK64


def make_small_u64_tables(max_value: int) -> list[tuple[int, ...]]:
    """Precompute exact eight-byte FNV transitions for small uint64 values.

    For a fixed eight-byte block the low-byte trajectory depends only on the
    input hash's low byte.  Thus T(h) = P**8*h + C[h & 255] (mod 2**64).
    This is merely a table-driven form of fnv_u64, verified in self-tests.
    """
    tables: list[tuple[int, ...]] = []
    for value in range(max_value + 1):
        tables.append(
            tuple(
                (
                    fnv_u64_unrolled(low_byte, value)
                    - FNV_U64_FACTOR * low_byte
                )
                & MASK64
                for low_byte in range(256)
            )
        )
    return tables


def fnv_catalog_id(hash_value: int, value: int) -> int:
    """Hash an ID, with a fast exact path for the catalog's 32-bit IDs."""
    if value >= 1 << 32:
        return fnv_u64_unrolled(hash_value, value)
    hash_value = ((hash_value ^ (value & 0xFF)) * FNV_PRIME) & MASK64
    value >>= 8
    hash_value = ((hash_value ^ (value & 0xFF)) * FNV_PRIME) & MASK64
    value >>= 8
    hash_value = ((hash_value ^ (value & 0xFF)) * FNV_PRIME) & MASK64
    value >>= 8
    hash_value = ((hash_value ^ value) * FNV_PRIME) & MASK64
    # Remaining four bytes are zero, hence four plain multiplications by P.
    return (FNV_ZERO4_FACTOR * hash_value) & MASK64


def fnv_prefix(hash_value: int, prefix_id: int, prefix: Sequence[int]) -> int:
    hash_value = fnv_u64(hash_value, prefix_id)
    hash_value = fnv_u64(hash_value, len(prefix))
    for value in prefix:
        hash_value = fnv_u64(hash_value, value)
    return hash_value


class CatalogAudit:
    """Canonical catalog traversal with definition-based B3 state updates."""

    def __init__(self, k: int, cap: int, g_values: Sequence[int], depth: int):
        self.k = k
        self.cap = cap
        self.g = [0, *g_values]
        self.target_depth = depth
        self.split_bound = cap + 1
        self.prefix_count = 0
        self.split_nodes = 0
        self.catalog_hash = FNV_OFFSET
        self.small_u64_tables = make_small_u64_tables(max(cap, k))

    def fnv_small_u64(self, hash_value: int, value: int) -> int:
        constants = self.small_u64_tables[value]
        return (
            FNV_U64_FACTOR * hash_value + constants[hash_value & 0xFF]
        ) & MASK64

    def accept(self, prefix: Sequence[int]) -> None:
        prefix_id = self.prefix_count
        self.prefix_count += 1
        hash_value = fnv_catalog_id(self.catalog_hash, prefix_id)
        hash_value = self.fnv_small_u64(hash_value, len(prefix))
        for value in prefix:
            hash_value = self.fnv_small_u64(hash_value, value)
        self.catalog_hash = hash_value

    def accept_suffixes(
        self,
        prefix: Sequence[int],
        candidates: Sequence[int],
        count: int,
    ) -> None:
        """Accept ``prefix + [y]`` for the first ``count`` candidates."""
        hash_value = self.catalog_hash
        prefix_id = self.prefix_count
        factor = FNV_U64_FACTOR
        mask = MASK64
        tables = self.small_u64_tables
        fixed_tables = (tables[len(prefix) + 1],) + tuple(
            tables[value] for value in prefix
        )
        hash_id = fnv_catalog_id

        for y in candidates[:count]:
            hash_value = hash_id(hash_value, prefix_id)
            for constants in fixed_tables:
                hash_value = (
                    factor * hash_value + constants[hash_value & 0xFF]
                ) & mask
            constants = tables[y]
            hash_value = (
                factor * hash_value + constants[hash_value & 0xFF]
            ) & mask
            prefix_id += 1

        self.prefix_count = prefix_id
        self.catalog_hash = hash_value

    def visit(
        self,
        depth: int,
        values: tuple[int, ...],
        candidates: tuple[int, ...],
        need: int,
        pair_sums: int,
        triple_sums: int,
        first_after_one: int,
        prefix: list[int],
    ) -> None:
        # The frontier is accepted before feasibility checks in gen_shard_tasks.
        if depth == self.target_depth:
            self.accept(prefix)
            return

        self.split_nodes += 1
        candidate_count = len(candidates)
        if candidate_count < need:
            return
        if need == 1:
            raise RuntimeError("prefix depth exceeds the searchable tree depth")

        # Hall-type span cuts, in exactly the order used by gen_shard_tasks.
        for position in range(1, need + 1):
            threshold = (
                self.split_bound - self.g[need - position + 1] + SLACK
            )
            if bisect.bisect_right(candidates, threshold) < position:
                return

        limit = self.split_bound - self.g[need] + SLACK
        if first_after_one:
            limit = min(
                limit, self.split_bound - first_after_one + SLACK
            )

        if depth + 1 == self.target_depth:
            accepted = min(
                bisect.bisect_right(candidates, limit),
                candidate_count - need + 1,
            )
            self.accept_suffixes(prefix, candidates, accepted)
            return

        for index, y in enumerate(candidates):
            if candidate_count - index < need:
                return
            if y > limit:
                return

            next_state = extend(values, pair_sums, triple_sums, y)
            if next_state is None:
                raise RuntimeError("catalog contains a candidate that is not B3")
            next_pairs, next_triples, pair_duplicate = next_state
            next_values = (*values, y)

            if pair_duplicate:
                next_candidates: tuple[int, ...] = ()
            else:
                next_candidates = tuple(
                    candidate
                    for candidate in candidates[index + 1 :]
                    if is_valid_add(
                        next_values, next_pairs, next_triples, candidate
                    )
                )

            prefix.append(y)
            self.visit(
                depth + 1,
                next_values,
                next_candidates,
                need - 1,
                next_pairs,
                next_triples,
                first_after_one or y,
                prefix,
            )
            prefix.pop()

    def run(self) -> dict[str, int | str]:
        root_state = extend((), 0, 0, 1)
        if root_state is None:  # Defensive: {1} is always a B3 set.
            raise RuntimeError("failed to construct the root set {1}")
        pair_sums, triple_sums, pair_duplicate = root_state
        if pair_duplicate:
            raise RuntimeError("unexpected duplicate pair sum at the root")

        values = (1,)
        candidates = tuple(
            y
            for y in range(2, self.cap + 1)
            if is_valid_add(values, pair_sums, triple_sums, y)
        )
        self.visit(
            depth=0,
            values=values,
            candidates=candidates,
            need=self.k - 1,
            pair_sums=pair_sums,
            triple_sums=triple_sums,
            first_after_one=0,
            prefix=[],
        )
        return {
            "prefix_count": self.prefix_count,
            "prefix_catalog_hash": f"fnv1a64:{self.catalog_hash:016x}",
            "split_nodes": self.split_nodes,
        }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--k", required=True, type=int)
    parser.add_argument("--cap", required=True, type=int)
    parser.add_argument("--g", required=True, type=parse_g)
    parser.add_argument("--depth", required=True, type=int)
    args = parser.parse_args()

    if args.k < 2:
        parser.error("--k must be at least 2")
    if args.cap < 1:
        parser.error("--cap must be positive")
    if not 0 <= args.depth <= args.k - 2:
        parser.error("--depth must be between 0 and k-2")
    if len(args.g) < args.k - 1:
        parser.error(f"--g must supply at least {args.k - 1} values")
    if any(value < 0 for value in args.g[: args.k - 1]):
        parser.error("the used --g values must be nonnegative")

    audit = CatalogAudit(args.k, args.cap, args.g[: args.k - 1], args.depth)
    print(json.dumps(audit.run(), separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
