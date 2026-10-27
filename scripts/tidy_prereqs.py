#!/usr/bin/env python3
"""Do clang-tidy's module prerequisites exist for the staged files? (#557, #688)

clang-tidy replays build/release/compile_commands.json and only PARSES, so
commit.sh builds just the BMIs and mock binaries it needs instead of the full
release target set (~90% of a cold full build is test TUs clang-tidy never
opens). This answers whether that targeted build was enough.

    scripts/tidy_prereqs.py <build-dir> <staged-file>...

Prints exactly `complete` when every module BMI referenced by the modmap of each
staged file's compile command exists. EVERY other outcome prints `incomplete`
and exits 0 — no staged files, a missing or corrupt compile_commands.json, an
unreadable modmap, a staged file with no entry. That direction is the point: a
missing BMI does not make clang-tidy fail, it degrades it to a PARTIAL PARSE that
emits false warnings, and the hook runs clang-tidy with --fix. So any
unanticipated failure must make the caller rebuild, never skip.

Stdlib only, no syntax newer than Python 3.7, so macOS system python3 runs it.
"""
import json
import pathlib
import re
import shlex
import sys

MODULE_FILE_FLAG = re.compile(r'-fmodule-file="?(?:[^=\s"]+=)?([^"\s]+)')


def command_args(entry):
    if "arguments" in entry:
        return entry["arguments"]
    return shlex.split(entry.get("command", ""))


def references_missing_bmi(entry, build_dir):
    """True when the entry's modmap, or a BMI it names, is absent on disk."""
    root = pathlib.Path(entry.get("directory", str(build_dir)))
    for arg in command_args(entry):
        if not arg.startswith("@") or not arg.endswith(".modmap"):
            continue
        modmap = root / arg[1:]
        if not modmap.exists():
            return True
        for ref in MODULE_FILE_FLAG.findall(modmap.read_text()):
            if not (root / ref).exists() and not pathlib.Path(ref).exists():
                return True
    return False


def prereqs_complete(build_dir, staged_files):
    build_dir = pathlib.Path(build_dir)
    staged = {pathlib.Path(f).resolve() for f in staged_files}
    if not staged:
        return False
    entries = json.loads((build_dir / "compile_commands.json").read_text())

    # Only the TUs clang-tidy will actually replay matter. Every other target's
    # modmap legitimately references BMIs this targeted build does not produce,
    # so checking the whole tree would always answer "incomplete".
    seen = set()
    for entry in entries:
        path = pathlib.Path(entry["file"]).resolve()
        if path not in staged:
            continue
        seen.add(path)
        if references_missing_bmi(entry, build_dir):
            return False

    # A staged input with no entry at all was never verified. clang-tidy infers a
    # command for it from a neighbouring entry (this is how headers are linted —
    # the database holds no header entries), so "no entry" means "unknown".
    return staged == seen


def main(argv):
    try:
        complete = len(argv) > 2 and prereqs_complete(argv[1], argv[2:])
    except Exception:  # noqa: BLE001 - fail closed on anything unanticipated
        complete = False
    print("complete" if complete else "incomplete")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
