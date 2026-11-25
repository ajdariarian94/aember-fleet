"""tools/mutation.py: turning `mutmut results` into the report."""

import importlib.util
from collections import Counter
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "mutation", Path(__file__).resolve().parents[1] / "tools" / "mutation.py"
)
mutation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mutation)


def test_counts_per_module_and_score():
    lines = [
        "    aember_fleet.store.x_add__mutmut_1: killed",
        "    aember_fleet.store.x_add__mutmut_2: survived",
        "    aember_fleet.cli.x_main__mutmut_3: no tests",
        "    aember_fleet.cli.x_main__mutmut_4: timeout",
        "some other output",
    ]
    per_module = mutation.parse(lines)
    assert per_module == {
        "store": Counter({"killed": 1, "survived": 1}),
        "cli": Counter({"no tests": 1, "timeout": 1}),
    }
    assert mutation.score(per_module["store"]) == 50.0
    assert mutation.score(per_module["cli"]) == 50.0  # a timeout counts as killed
    assert mutation.score(Counter()) is None
    text = mutation.markdown(per_module, ["aember_fleet.store.x_add__mutmut_2"])
    assert "aember-fleet-server: 50.0% (4 mutants, 1 survived, 1 not reached" in text
    assert "| store | 50.0% | 2 | 1 | 0 |" in text
    assert "- `aember_fleet.store.x_add__mutmut_2`" in text
