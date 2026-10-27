#!/usr/bin/env python3
"""Turn the SonarCloud check run on a PR into a ci.yml job (issue #711).

SonarCloud's automatic analysis posts `SonarCloud Code Analysis` on pull
requests only — never on the merge queue's merge-group commits. Required
directly, it would leave every queued PR waiting until the check timeout. So
ci.yml's `sonar-gate` job runs this instead, and `CI gate` (the one required
context) depends on that job:

  pull_request  poll the checks API for the newest `SonarCloud Code Analysis`
                run on the PR head SHA; pass only on conclusion `success`.
  anything else pass at once — merge_group commits were gated on their PR,
                push/schedule runs have no PR to gate.

Fails closed: a pending run is never read as green, a timeout or a missing
input is a failure. Needs only GITHUB_TOKEN with `checks: read`; no Sonar
token. Stdlib only, no syntax newer than Python 3.7.

    EVENT_NAME=pull_request GITHUB_REPOSITORY=owner/repo HEAD_SHA=<sha> \\
        GITHUB_TOKEN=<token> scripts/sonar_gate.py

Exit: 0 = gate passed (or not applicable), 1 = failed, timed out, or bad input.
"""
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

CHECK_NAME = "SonarCloud Code Analysis"
API_ROOT = "https://api.github.com"
# Sonar's analysis usually reports within minutes; ci.yml's job timeout sits above this.
TIMEOUT_S = 30 * 60
INTERVAL_S = 20
HTTP_TIMEOUT_S = 30
# Retrying cannot fix these; anything else (5xx, a dropped connection) is retried.
FATAL_HTTP_CODES = (401, 403, 404)

# Matched with fullmatch: `$` would also accept a trailing newline.
SHA_RE = re.compile(r"[0-9a-f]{40}")
REPO_RE = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")

PENDING, PASS, FAIL = "pending", "pass", "fail"


def check_runs_url(repo, sha):
    query = urllib.parse.urlencode({"check_name": CHECK_NAME}, quote_via=urllib.parse.quote)
    return "%s/repos/%s/commits/%s/check-runs?%s" % (API_ROOT, repo, sha, query)


def fetch_check_runs(repo, sha, token):
    request = urllib.request.Request(check_runs_url(repo, sha), headers={
        "Accept": "application/vnd.github+json",
        "Authorization": "Bearer %s" % token,
        "X-GitHub-Api-Version": "2022-11-28",
    })
    with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT_S) as response:
        return json.load(response).get("check_runs", [])


def latest_run(runs):
    """The newest run: a re-analysis posts a new one, and only it counts."""
    return max(runs, key=lambda r: r["id"]) if runs else None


def verdict(run):
    if run is None:
        return PENDING, "%s has not reported yet" % CHECK_NAME
    if run.get("status") != "completed":
        return PENDING, "%s is %s" % (CHECK_NAME, run.get("status"))
    url = run.get("html_url", "")
    if run.get("conclusion") == "success":
        return PASS, "%s passed: %s" % (CHECK_NAME, url)
    return FAIL, "%s concluded '%s': %s" % (CHECK_NAME, run.get("conclusion"), url)


def wait_for_verdict(fetch, sleep, now, timeout, interval, out):
    """Poll until the run completes or `timeout` seconds pass. A fetch error is
    retried like a pending run: one dropped request must not fail the gate."""
    start = now()
    while True:
        try:
            state, message = verdict(latest_run(fetch()))
        except urllib.error.HTTPError as error:
            if error.code in FATAL_HTTP_CODES:
                return FAIL, "checks API refused the request (HTTP %d) — check the job's " \
                             "`checks: read` permission and the repo/SHA" % error.code
            state, message = PENDING, "checks API request failed: %s" % error
        except (OSError, ValueError) as error:
            state, message = PENDING, "checks API request failed: %s" % error
        if state != PENDING:
            return state, message
        if now() - start >= timeout:
            return FAIL, "timed out after %ds waiting — last state: %s" % (timeout, message)
        out.write("%s; retrying in %ds\n" % (message, interval))
        out.flush()
        sleep(interval)


def input_problem(env):
    if not SHA_RE.fullmatch(env.get("HEAD_SHA", "")):
        return "HEAD_SHA must be a 40-character hex commit SHA"
    if not REPO_RE.fullmatch(env.get("GITHUB_REPOSITORY", "")):
        return "GITHUB_REPOSITORY must be owner/repo"
    if not env.get("GITHUB_TOKEN"):
        return "GITHUB_TOKEN is empty"
    return None


def main(env, sleep=time.sleep, now=time.monotonic):
    event = env.get("EVENT_NAME", "")
    if event != "pull_request":
        print("Event '%s': no PR head to gate on — nothing to check." % event)
        return 0
    problem = input_problem(env)
    if problem:
        print("::error::sonar-gate: %s" % problem, file=sys.stderr)
        return 1
    repo, sha, token = env["GITHUB_REPOSITORY"], env["HEAD_SHA"], env["GITHUB_TOKEN"]
    state, message = wait_for_verdict(lambda: fetch_check_runs(repo, sha, token),
                                      sleep, now, TIMEOUT_S, INTERVAL_S, sys.stdout)
    if state != PASS:
        print("::error::%s" % message, file=sys.stderr)
        return 1
    print(message)
    return 0


if __name__ == "__main__":
    sys.exit(main(os.environ))
