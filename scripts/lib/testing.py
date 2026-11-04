"""Helpers shared by the Python self-tests under scripts/*/tests/ (issue #763).

Only what two or more tests actually use lives here. Stdlib only, no syntax newer than
Python 3.7 (macOS system python3 runs these). A test imports it by path:

    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "lib"))
    import testing
"""
import contextlib
import os
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


def environment_without_git_hook_variables(also_drop=()):
    """os.environ without the variables a git hook exports, as a new dict.

    commit.sh runs the git-driving self-tests from the pre-commit hook, where git exports
    GIT_INDEX_FILE (and, for some invocations, GIT_DIR). Left set, every `git` call in a
    throwaway repo would act on the REAL repository - staging scenario files into the commit
    being checked. `also_drop` names further variables to remove (the hook also exports
    STORM_PRE_COMMIT_HOOK, which would put a "manual run" scenario into hook mode).

    Raises if git cannot list its variables: returning os.environ unchanged then would fail
    open into exactly the corruption this exists to prevent.
    """
    if isinstance(also_drop, str):
        # set("NAME") would silently be a set of single characters and drop nothing.
        raise TypeError("also_drop takes a sequence of names, not one string: %r" % (also_drop,))
    listing = run(["git", "rev-parse", "--local-env-vars"])
    if listing.returncode != 0:
        raise RuntimeError("git rev-parse --local-env-vars failed: " + listing.stderr.strip())
    drop = set(listing.stdout.split()) | set(also_drop)
    return {key: value for key, value in os.environ.items() if key not in drop}


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
