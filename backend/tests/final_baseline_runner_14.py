import json
import os
import sys
import time
from itertools import combinations

import importlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

cc = None
for mod_name in ("backend.cover_core.cover_core_ext", "cover_core_ext"):
    try:
        mod = importlib.import_module(mod_name)
        if hasattr(mod, "backtracking_selection"):
            cc = mod
            break
    except Exception:
        continue
if cc is None:
    raise ImportError("cannot import cover_core extension module")


def verify_cover(pool, k, j, s, result):
    for jsub in combinations(pool, j):
        jset = set(jsub)
        if not any(len(jset & set(b)) >= s for b in result):
            return False
    return True


def main():
    cases = [
        (1, list(range(1, 13)), 6, 5, 5, 132),
        (2, list(range(1, 14)), 6, 5, 5, 245),
        (3, list(range(1, 15)), 6, 4, 4, 99),
        (4, list(range(1, 16)), 6, 4, 4, 130),
        (5, list(range(1, 19)), 6, 4, 4, 258),
        (6, list(range(1, 16)), 6, 6, 5, 190),
        (7, list(range(1, 17)), 6, 6, 5, 280),
        (8, list(range(1, 15)), 6, 5, 4, 40),
        (9, list(range(1, 17)), 6, 5, 4, 65),
        (10, list(range(1, 18)), 6, 5, 4, 88),
        (11, list(range(1, 21)), 6, 5, 4, 216),
        (12, list(range(1, 16)), 6, 6, 4, 22),
        (13, list(range(1, 19)), 6, 6, 4, 42),
        (14, list(range(1, 21)), 6, 6, 4, 100),
    ]

    results = []
    for cid, pool, k, j, s, target in cases:
        t = time.time()
        try:
            r, a = cc.backtracking_selection(pool, k, j, s, 30.0)
            elapsed = time.time() - t
            verified = verify_cover(pool, k, j, s, r) if len(r) > 0 else False
            diff = len(r) - target
            row = {
                "case": cid,
                "n": len(pool),
                "k": k,
                "j": j,
                "s": s,
                "target": target,
                "size": len(r),
                "diff": diff,
                "verified": verified,
                "aborted": a,
                "elapsed_s": round(elapsed, 1),
            }
            results.append(row)
            print(
                f"case {cid:02d}: n={len(pool):2d} target={target:4d} size={len(r):4d} "
                f"diff={diff:+4d} verified={verified} elapsed={elapsed:.1f}s"
            )
        except Exception as e:
            elapsed = time.time() - t
            row = {
                "case": cid,
                "n": len(pool),
                "k": k,
                "j": j,
                "s": s,
                "target": target,
                "size": 0,
                "diff": None,
                "verified": False,
                "aborted": True,
                "elapsed_s": round(elapsed, 1),
                "error": str(e),
            }
            results.append(row)
            print(f"case {cid:02d}: ERROR {e}")

    out_path = os.path.join(ROOT, "results", "final_baseline_v2.json")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(results, f, ensure_ascii=False, indent=2)

    verified_n = sum(1 for r in results if r["verified"])
    within_10 = sum(
        1 for r in results if r["verified"] and r["diff"] is not None and r["diff"] <= 10
    )
    total_t = sum(r["elapsed_s"] for r in results)
    print(f"\nverified: {verified_n}/14")
    print(f"diff<=10: {within_10}/14")
    print(f"total elapsed: {total_t:.1f}s")


if __name__ == "__main__":
    main()
