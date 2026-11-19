#!/usr/bin/env python3
"""Vendor a CPM.cmake release into cmake/ (issue #851). A manual, one-off step.

    scripts/update_cpm.py <version>        e.g. 0.40.6

Downloads https://github.com/cpm-cmake/CPM.cmake/releases/download/v<version>/CPM.cmake, writes
cmake/CPM_<version>.cmake, deletes any other cmake/CPM_*.cmake, and prints the two lines to put
in cmake/cpm.cmake. scripts/ci/check_cpm_vendored.py then verifies the result. Stdlib only, no
syntax newer than Python 3.7.
"""
import hashlib
import pathlib
import re
import sys
import urllib.request

CMAKE_DIR = pathlib.Path(__file__).resolve().parents[1] / "cmake"
URL = "https://github.com/cpm-cmake/CPM.cmake/releases/download/v{version}/CPM.cmake"
VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")
MARKER = b"CPM.cmake - CMake's missing package manager"
TIMEOUT_SECONDS = 60


def main(argv):
    if len(argv) != 2 or not VERSION.fullmatch(argv[1]):
        print(__doc__)
        return 2
    version = argv[1]
    url = URL.format(version=version)
    try:
        with urllib.request.urlopen(url, timeout=TIMEOUT_SECONDS) as response:
            data = response.read()
    except OSError as error:
        print("download failed: %s: %s" % (url, error), file=sys.stderr)
        return 1
    if MARKER not in data:
        print("%s does not look like CPM.cmake (%d bytes)" % (url, len(data)), file=sys.stderr)
        return 1
    target = CMAKE_DIR / ("CPM_%s.cmake" % version)
    target.write_bytes(data)
    for old in CMAKE_DIR.glob("CPM_*.cmake"):
        if old != target:
            old.unlink()
    print("wrote %s (%d bytes)" % (target, len(data)))
    print("set(CPM_DOWNLOAD_VERSION %s)" % version)
    print("set(CPM_SHA256 %s)" % hashlib.sha256(data).hexdigest())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
