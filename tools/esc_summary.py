#!/usr/bin/env python3
"""Summarise an esctest2 log (--logfile) by test class.

    python3 esc_summary.py esctest.log [--failures] [--compare older.log]

esctest prints "Run test: Class.test_name" and then either "Passed.",
"*** TEST ... FAILED:" with a traceback, or an "EXPECTED FAILURE" block
for tests needing options or knowingly failing on the selected terminal.
This tallies outcomes per class, and with --failures lists each failing
test with the line that raised (usually the Assert...), which is what
groups failures into fixable clusters. --compare lists tests whose
outcome changed between two runs.
"""

from __future__ import annotations

import argparse
import re
from collections import Counter, defaultdict
from pathlib import Path

RUN = re.compile(r"^Run test: (\S+)")


def parse(path: Path) -> dict[str, tuple[str, str]]:
    """test name -> (outcome, detail line)."""
    results: dict[str, tuple[str, str]] = {}
    current = None
    detail = ""
    outcome = None
    for line in path.read_text(errors="replace").splitlines():
        match = RUN.match(line)
        if match:
            if current:
                results[current] = (outcome or "unknown", detail)
            current, outcome, detail = match.group(1), None, ""
            continue
        if current is None:
            continue
        if line.startswith("Passed."):
            outcome = "pass"
        elif "FAILED:" in line and line.startswith("*** TEST"):
            outcome = "fail"
        elif line.startswith("Skipped because") and outcome is None:
            outcome = "skipped"
        elif (line.startswith("EXPECTED FAILURE") or
              line.startswith("Fails as expected")) and outcome is None:
            outcome = "expected-fail"
        stripped = line.strip()
        if outcome == "fail" and (stripped.startswith("Assert") or
                                  "Exception" in stripped or "Error" in stripped):
            detail = stripped
    if current:
        results[current] = (outcome or "unknown", detail)
    return results


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--failures", action="store_true")
    parser.add_argument("--compare", type=Path)
    args = parser.parse_args()

    results = parse(args.log)
    totals = Counter(outcome for outcome, _ in results.values())
    print(f"{len(results)} tests: " + ", ".join(f"{k} {v}" for k, v in totals.most_common()))

    by_class: dict[str, Counter] = defaultdict(Counter)
    for name, (outcome, _) in results.items():
        by_class[name.split(".")[0]][outcome] += 1
    for cls in sorted(by_class, key=lambda c: -by_class[c]["fail"]):
        counts = by_class[cls]
        if counts["fail"]:
            total = sum(counts.values())
            print(f"  {cls:28} fail {counts['fail']:3} / {total:3}")

    if args.failures:
        for name, (outcome, detail) in sorted(results.items()):
            if outcome == "fail":
                print(f"{name}: {detail}")

    if args.compare:
        before = parse(args.compare)
        for name in sorted(set(results) | set(before)):
            old = before.get(name, ("-", ""))[0]
            new = results.get(name, ("-", ""))[0]
            if old != new:
                print(f"changed {name}: {old} -> {new}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
