import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from typing import Optional


@dataclass
class Case:
    name: str
    n: int
    k: int
    j: int
    s: int
    t: float


DEFAULT_CASES = [
    Case("G1_sj_small", 9, 4, 3, 3, 8.0),
    Case("G2_sj_ref", 10, 5, 3, 3, 8.0),
    Case("G3_mid", 12, 5, 4, 2, 8.0),
    Case("G4_mid", 11, 5, 4, 3, 8.0),
    Case("G5_mid", 12, 6, 4, 2, 8.0),
    Case("G6_sj_larger", 12, 6, 4, 4, 8.0),
]


def run_one(case: Case, cwd: str) -> dict:
    py = (
        "import backend.cover_core.cover_core_ext as cc;"
        f"pool=list(range(1,{case.n + 1}));"
        f"r,a=cc.backtracking_selection(pool,{case.k},{case.j},{case.s},{case.t});"
        "print(f'__RESULT__ size={len(r)} aborted={a}')"
    )
    cp = subprocess.run(
        [sys.executable, "-c", py],
        cwd=cwd,
        text=True,
        capture_output=True,
        check=False,
    )
    out = (cp.stdout or "") + "\n" + (cp.stderr or "")
    return parse_output(case, out, cp.returncode)


def _f(pattern: str, text: str, cast=float) -> Optional[float]:
    m = re.search(pattern, text)
    if not m:
        return None
    return cast(m.group(1))


def parse_output(case: Case, out: str, returncode: int) -> dict:
    lb = _f(r"Schonheim lower bound =\s*(\d+)", out, int)
    seed = _f(r"seed/LB ratio = [\d.]+ \((\d+)/\d+\)", out, int)
    result_size = _f(r"__RESULT__ size=(\d+)", out, int)
    aborted_raw = re.search(r"__RESULT__ size=\d+ aborted=(True|False)", out)
    aborted = None if not aborted_raw else (aborted_raw.group(1) == "True")

    build_ms = _f(r"build=([\d.]+)ms", out, float)
    greedy_ms = _f(r"greedy=([\d.]+)ms", out, float)
    dfs_ms = _f(r"dfs=([\d.]+)ms", out, float)
    ils_ms = _f(r"ils=([\d.]+)ms", out, float)
    total_ms = _f(r"total=([\d.]+)ms", out, float)
    targets = _f(r"targets=(\d+)", out, int)
    cands = _f(r"cands=(\d+)", out, int)
    nodes = _f(r"dfs_nodes=(\d+)", out, int)
    if total_ms is None:
        parts = [v for v in (build_ms, greedy_ms, dfs_ms, ils_ms) if v is not None]
        if parts:
            total_ms = float(sum(parts))

    return {
        "name": case.name,
        "n": case.n,
        "k": case.k,
        "j": case.j,
        "s": case.s,
        "time_limit": case.t,
        "lb": lb,
        "seed": seed,
        "result": result_size,
        "gap_to_lb": None if (lb is None or result_size is None) else (result_size - lb),
        "aborted": aborted,
        "build_ms": build_ms,
        "greedy_ms": greedy_ms,
        "dfs_ms": dfs_ms,
        "ils_ms": ils_ms,
        "total_ms": total_ms,
        "targets": targets,
        "cands": cands,
        "dfs_nodes": nodes,
        "ok": returncode == 0 and result_size is not None,
    }


def print_table(rows: list[dict]) -> None:
    header = (
        "| case | n,k,j,s | LB | seed | final | gap | aborted | "
        "build | greedy | dfs | ils | total | nodes |"
    )
    sep = "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
    print(header)
    print(sep)
    for r in rows:
        nkjs = f"{r['n']},{r['k']},{r['j']},{r['s']}"
        print(
            f"| {r['name']} | {nkjs} | {r['lb']} | {r['seed']} | {r['result']} | "
            f"{r['gap_to_lb']} | {r['aborted']} | {r['build_ms']} | {r['greedy_ms']} | "
            f"{r['dfs_ms']} | {r['ils_ms']} | {r['total_ms']} | {r['dfs_nodes']} |"
        )


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--json", action="store_true", help="print JSON instead of markdown table")
    p.add_argument("--case", action="append", default=[], help="run only specific case name(s)")
    args = p.parse_args()

    root = "."
    cases = DEFAULT_CASES
    if args.case:
        wanted = set(args.case)
        cases = [c for c in cases if c.name in wanted]

    rows = [run_one(c, root) for c in cases]
    if args.json:
        print(json.dumps(rows, indent=2, ensure_ascii=False))
    else:
        print_table(rows)


if __name__ == "__main__":
    main()
