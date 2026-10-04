"""Check the A227358(11) witness from the definition: 11 integers in [0, 445] whose sums of three
(not necessarily distinct) elements are pairwise distinct. Standard library only."""
from itertools import combinations_with_replacement

W = [0, 2, 10, 17, 52, 108, 187, 323, 398, 434, 445]
sums = [a + b + c for a, b, c in combinations_with_replacement(W, 3)]
assert len(W) == 11 and len(set(W)) == 11 and min(W) == 0 and max(W) == 445
assert len(sums) == 286 and len(set(sums)) == 286, "two triples share a sum"
print("OK: 11 elements, span 445, all 286 triple sums distinct")
