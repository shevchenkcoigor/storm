#!/usr/bin/env python3
"""Assert every storm-ci image digest pin in the workflows agrees (#530, #708).

The storm-ci image is pinned by digest in more than one workflow (ci.yml,
clang-tidy-sweep.yml, bench-ratio-spike.yml). A job's `container.image` is
resolved before any step runs, so it cannot read `env:` or a shared file — the
pins have to be literal in each file. Deduplicating them would mean a setup job on
every run or moving the pin into repo variables, which takes it out of git and
defeats the point of pinning. Instead, assert they agree, so drift fails loudly
rather than silently running two workflows on two different clang-p2996 builds
(as in #498).

Formerly inline bash in ci.yml's `digest-guard` job; now a pre-commit guard that
CI runs through `pre_commit_guards.py --all`. Stdlib only, no syntax newer than
Python 3.7, so macOS system python3 runs it.

    scripts/check_ci_digests.py [workflows-dir]   (default: .github/workflows)

Exit: 0 = at least two pins and all agree, 1 = drift, too few pins, or no dir.
"""
import pathlib
import re
import sys
from typing import List, NamedTuple

DEFAULT_WORKFLOWS_DIR = pathlib.Path(__file__).resolve().parent.parent / ".github" / "workflows"
PIN = re.compile(r"storm-ci@sha256:[0-9a-f]{64}")
# Fewer than this means the image reference was renamed or restructured, and an
# "all agree" over one pin (or none) would pass while checking nothing.
MIN_PINS = 2


class Pin(NamedTuple):
    location: str  # "<file>:<line>"
    digest: str


def pins_in(workflows_dir):
    pins = []
    for path in sorted(workflows_dir.iterdir()):
        if path.suffix not in (".yml", ".yaml"):
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            pins.extend(Pin("%s:%d" % (path.name, number), m.group(0)) for m in PIN.finditer(line))
    return pins


def problems(pins: List[Pin]) -> List[str]:
    if len(pins) < MIN_PINS:
        return ["expected at least %d storm-ci digest pins, found %d — the image reference may have "
                "been renamed or restructured; update scripts/check_ci_digests.py" % (MIN_PINS, len(pins))]
    if len({pin.digest for pin in pins}) != 1:
        listing = "\n".join("  %s  %s" % (pin.location, pin.digest) for pin in pins)
        return ["storm-ci digest drift between workflows — pin the same digest everywhere:\n" + listing]
    return []


def main(argv):
    workflows_dir = pathlib.Path(argv[1]) if len(argv) > 1 else DEFAULT_WORKFLOWS_DIR
    if not workflows_dir.is_dir():
        print("check-ci-digests: no such directory: %s" % workflows_dir, file=sys.stderr)
        return 1
    pins = pins_in(workflows_dir)
    found = problems(pins)
    for problem in found:
        print("::error::%s" % problem, file=sys.stderr)
    if found:
        return 1
    print("OK: %d storm-ci pins agree — %s" % (len(pins), pins[0].digest))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
