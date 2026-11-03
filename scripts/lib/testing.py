"""Helpers shared by the Python self-tests under scripts/*/tests/ (issue #763).

Only what two or more tests actually use lives here. Stdlib only, no syntax newer than
Python 3.7 (macOS system python3 runs these). A test imports it by path:

    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "lib"))
    import testing
"""
import contextlib
import pathlib
import subprocess
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]


def run(command, env=None, cwd=None, input=None, merge_stderr=False):
    """Run `command` to completion in text mode and return the CompletedProcess.

    merge_stderr=True folds stderr into stdout, the shell's `2>&1`, for tests that grep the
    combined output of a tool.
    """
    return subprocess.run([str(part) for part in command], env=env, cwd=cwd, input=input,
                          stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT if merge_stderr else subprocess.PIPE,
                          universal_newlines=True)


@contextlib.contextmanager
def scratch_dir_in_repo(prefix):
    """A temporary directory INSIDE the repository, under its gitignored build/.

    For tests whose subject confines paths to the tree (ninjalog_stats, time_trace): a /tmp
    fixture would be rejected before any of the parsing is exercised. build/ is ignored, so
    nothing here can be committed by accident; the directory is removed on exit.
    """
    build = REPO_ROOT / "build"
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=str(build), prefix=prefix) as tmp:
        yield pathlib.Path(tmp)
