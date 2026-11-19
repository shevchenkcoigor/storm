#!/usr/bin/env python3
"""Check that the vendored CPM.cmake is consistent with what cmake/cpm.cmake records (#851).

cmake/CPM_<version>.cmake is the upstream release asset, byte for byte, so configuring never
fetches the bootstrap. cmake/cpm.cmake records the version (CPM_DOWNLOAD_VERSION) and the SHA256
of the file (CPM_SHA256). Fails when there is not exactly one cmake/CPM_*.cmake, when its name
disagrees with CPM_DOWNLOAD_VERSION, or when its bytes disagree with CPM_SHA256. To bump the
version, run scripts/update_cpm.py. Stdlib only, no syntax newer than Python 3.7.

    scripts/ci/check_cpm_vendored.py [--root DIR]

Exit: 0 = consistent, 1 = a finding was printed.
"""
import argparse
import hashlib
import pathlib
import re
import sys

DEFAULT_ROOT = pathlib.Path(__file__).resolve().parents[2]
VENDORED_GLOB = "CPM_*.cmake"


def recorded_value(text, name):
    found = re.search(r"^\s*set\(\s*%s\s+([^\s)]+)\s*\)" % name, text, re.MULTILINE)
    return found.group(1) if found else None


def findings(root):
    cmake_dir = root / "cmake"
    bootstrap = cmake_dir / "cpm.cmake"
    if not bootstrap.is_file():
        return ["cmake/cpm.cmake is missing"]
    text = bootstrap.read_text(encoding="utf-8")
    version = recorded_value(text, "CPM_DOWNLOAD_VERSION")
    digest = recorded_value(text, "CPM_SHA256")

    problems = []
    if version is None:
        problems.append("cmake/cpm.cmake does not set CPM_DOWNLOAD_VERSION")
    if digest is None:
        problems.append("cmake/cpm.cmake does not set CPM_SHA256")

    vendored = sorted(cmake_dir.glob(VENDORED_GLOB))
    if len(vendored) != 1:
        problems.append("expected exactly one cmake/%s, found %d: %s"
                        % (VENDORED_GLOB, len(vendored), ", ".join(p.name for p in vendored) or "none"))
        return problems

    only = vendored[0]
    if version is not None and only.name != "CPM_%s.cmake" % version:
        problems.append("%s does not match CPM_DOWNLOAD_VERSION %s" % (only.name, version))
    actual = hashlib.sha256(only.read_bytes()).hexdigest()
    if digest is not None and actual != digest.lower():
        problems.append("SHA256 of %s is %s, cmake/cpm.cmake records %s" % (only.name, actual, digest))
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=DEFAULT_ROOT)
    args = parser.parse_args(argv)
    problems = findings(args.root)
    for problem in problems:
        print("check_cpm_vendored: " + problem)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
