"""Run mutmut over aember_fleet/ and write a report to OUT
(aember-fleet-server.json and .md): per module how many mutants the tests
killed, how many survived, and how many no test reached at all.

Statuses come from `mutmut results --all true` (`<module>.<function>__mutmut_<n>:
<status>`). Killed and timeout count as killed; the score is killed / all.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

NAME = "aember-fleet-server"
KILLED = {"killed", "timeout"}


def parse(lines: list[str]) -> dict[str, Counter]:
    """{module: Counter(status)} from `mutmut results --all true` lines."""
    per_module: dict[str, Counter] = defaultdict(Counter)
    for line in lines:
        name, sep, status = line.strip().rpartition(": ")
        if not sep or "__mutmut_" not in name:
            continue
        module = name.split(".")[1] if name.count(".") >= 2 else name
        per_module[module][status.strip()] += 1
    return dict(per_module)


def score(c: Counter) -> float | None:
    total = sum(c.values())
    if not total:
        return None
    return round(100.0 * sum(c[s] for s in KILLED) / total, 1)


def markdown(per_module: dict[str, Counter], survivors: list[str]) -> str:
    everything: Counter = sum(per_module.values(), Counter())
    lines = [
        f"### {NAME}: {score(everything)}% "
        f"({sum(everything.values())} mutants, {everything['survived']} survived, "
        f"{everything['no tests']} not reached by any test)",
        "",
        "| Module | Score | Mutants | Survived | No tests |",
        "|---|---:|---:|---:|---:|",
    ]
    for module, c in sorted(per_module.items()):
        lines.append(
            f"| {module} | {score(c)}% | {sum(c.values())} | {c['survived']} "
            f"| {c['no tests']} |"
        )
    if survivors:
        lines += ["", "Survivors (`mutmut show <name>` for the diff):", ""]
        lines += [f"- `{s}`" for s in survivors]
    return "\n".join(lines) + "\n"


def main(out: Path) -> int:
    jobs = str(max(1, (os.cpu_count() or 2) - 1))
    subprocess.run(["mutmut", "run", "--max-children", jobs], check=True,
                   stdout=subprocess.DEVNULL)
    res = subprocess.run(
        ["mutmut", "results", "--all", "true"],
        check=True,
        capture_output=True,
        text=True,
    )
    lines = res.stdout.splitlines()
    per_module = parse(lines)
    survivors = sorted(
        line.rpartition(": ")[0].strip()
        for line in lines
        if line.strip().endswith(": survived")
    )
    out.mkdir(parents=True, exist_ok=True)
    everything: Counter = sum(per_module.values(), Counter())
    report = {
        "unit": NAME,
        "score": score(everything),
        "mutants": sum(everything.values()),
        "modules": {m: dict(c) for m, c in per_module.items()},
        "survived": survivors,
    }
    (out / f"{NAME}.json").write_text(json.dumps(report, indent=2) + "\n")
    text = markdown(per_module, survivors)
    (out / f"{NAME}.md").write_text(text)
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(Path(sys.argv[1] if len(sys.argv) > 1 else "out")))
