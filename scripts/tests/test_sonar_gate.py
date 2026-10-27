#!/usr/bin/env python3
"""Tests for scripts/sonar_gate.py (issue #711).

The gate turns the SonarCloud app's `SonarCloud Code Analysis` check run into
a job inside ci.yml, so `CI gate` can require it and the merge queue never
waits on a check the app does not post on merge-group commits. Its failure
modes are all silent passes: a pending run read as green, an old run read in
place of the newest, a timeout read as success. Each is pinned below with a
fake fetch/sleep/clock, so no network is touched. Stdlib unittest only, no
syntax newer than Python 3.7.

    python3 scripts/tests/test_sonar_gate.py
"""
import io
import pathlib
import sys
import unittest
import urllib.error
from unittest import mock

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
HELPER = REPO_ROOT / "scripts" / "sonar_gate.py"

sys.path.insert(0, str(HELPER.parent))
import sonar_gate as sg  # noqa: E402

SHA = "0123456789abcdef0123456789abcdef01234567"
PR_ENV = {"EVENT_NAME": "pull_request", "GITHUB_REPOSITORY": "org/storm", "HEAD_SHA": SHA, "GITHUB_TOKEN": "t"}


def run(run_id=1, status="completed", conclusion="success"):
    return {"id": run_id, "status": status, "conclusion": conclusion,
            "html_url": "https://example.test/run/%d" % run_id}


class FakeClock:
    def __init__(self):
        self.t = 0.0
        self.sleeps = []

    def now(self):
        return self.t

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.t += seconds


class VerdictTest(unittest.TestCase):
    def test_no_run_yet_is_pending(self):
        self.assertEqual(sg.verdict(None)[0], sg.PENDING)

    def test_in_progress_is_pending_not_pass(self):
        self.assertEqual(sg.verdict(run(status="in_progress", conclusion=None))[0], sg.PENDING)

    def test_success_passes(self):
        self.assertEqual(sg.verdict(run())[0], sg.PASS)

    def test_failure_fails_and_links_the_run(self):
        state, message = sg.verdict(run(conclusion="failure"))
        self.assertEqual(state, sg.FAIL)
        self.assertIn("failure", message)
        self.assertIn("https://example.test/run/1", message)

    def test_anything_but_success_fails(self):
        for conclusion in ("neutral", "cancelled", "timed_out", "action_required", "skipped", "stale"):
            self.assertEqual(sg.verdict(run(conclusion=conclusion))[0], sg.FAIL, conclusion)


class LatestRunTest(unittest.TestCase):
    def test_empty_is_none(self):
        self.assertIsNone(sg.latest_run([]))

    def test_newest_run_wins_whatever_the_order(self):
        # A re-analysis posts a new run; the stale red (or green) one must not decide.
        runs = [run(7, conclusion="failure"), run(9, conclusion="success"), run(8, conclusion="failure")]
        self.assertEqual(sg.latest_run(runs)["id"], 9)


class WaitTest(unittest.TestCase):
    def wait(self, responses, timeout=100, interval=10):
        clock = FakeClock()
        replies = iter(responses)

        def fetch():
            reply = next(replies)
            if isinstance(reply, Exception):
                raise reply
            return reply

        out = io.StringIO()
        result = sg.wait_for_verdict(fetch, clock.sleep, clock.now, timeout, interval, out)
        return result, clock

    def test_polls_until_the_run_completes(self):
        (state, _), clock = self.wait([[], [run(status="queued", conclusion=None)], [run()]])
        self.assertEqual(state, sg.PASS)
        self.assertEqual(clock.sleeps, [10, 10])

    def test_red_run_fails_without_waiting_further(self):
        (state, _), clock = self.wait([[run(conclusion="failure")]])
        self.assertEqual(state, sg.FAIL)
        self.assertEqual(clock.sleeps, [])

    def test_timeout_fails_rather_than_passing(self):
        (state, message), _ = self.wait([[]] * 20, timeout=30, interval=10)
        self.assertEqual(state, sg.FAIL)
        self.assertIn("timed out", message)

    def test_transient_fetch_error_is_retried(self):
        (state, _), _ = self.wait([OSError("connection reset"), [run()]])
        self.assertEqual(state, sg.PASS)

    def test_auth_or_not_found_errors_fail_at_once(self):
        # A missing `checks: read` cannot fix itself; waiting 30 min only hides it.
        for code in (401, 403, 404):
            error = urllib.error.HTTPError("https://api.test", code, "nope", {}, None)
            (state, message), clock = self.wait([error])
            self.assertEqual(state, sg.FAIL, code)
            self.assertIn(str(code), message)
            self.assertEqual(clock.sleeps, [], code)

    def test_server_errors_are_retried(self):
        error = urllib.error.HTTPError("https://api.test", 502, "bad gateway", {}, None)
        (state, _), _ = self.wait([error, [run()]])
        self.assertEqual(state, sg.PASS)

    def test_persistent_fetch_error_times_out_naming_the_error(self):
        (state, message), _ = self.wait([OSError("HTTP 403")] * 20, timeout=30, interval=10)
        self.assertEqual(state, sg.FAIL)
        self.assertIn("HTTP 403", message)


class MainTest(unittest.TestCase):
    def main(self, env, fetch_runs=None):
        def fetch(repo, sha, token):
            if fetch_runs is None:
                raise AssertionError("fetch must not be called")
            return fetch_runs

        clock = FakeClock()
        with mock.patch.object(sg, "fetch_check_runs", side_effect=fetch), \
                mock.patch("sys.stdout", new_callable=io.StringIO) as out, \
                mock.patch("sys.stderr", new_callable=io.StringIO) as err:
            code = sg.main(env, clock.sleep, clock.now)
        return code, out.getvalue() + err.getvalue()

    def test_non_pull_request_events_pass_without_polling(self):
        # The merge queue's whole reason for this job: no Sonar run on merge_group.
        for event in ("merge_group", "push", "schedule", "workflow_dispatch"):
            code, out = self.main(dict(PR_ENV, EVENT_NAME=event))
            self.assertEqual(code, 0, event)
            self.assertIn("nothing to check", out)

    def test_pull_request_success_passes(self):
        self.assertEqual(self.main(PR_ENV, [run()])[0], 0)

    def test_pull_request_failure_fails_with_an_annotation(self):
        code, out = self.main(PR_ENV, [run(conclusion="failure")])
        self.assertEqual(code, 1)
        self.assertIn("::error::", out)

    def test_malformed_or_missing_inputs_fail_closed(self):
        for key, value in (("HEAD_SHA", ""), ("HEAD_SHA", "abc"), ("HEAD_SHA", SHA + "; rm"),
                           ("GITHUB_REPOSITORY", "no-slash"), ("GITHUB_REPOSITORY", "a/b/c"),
                           ("GITHUB_TOKEN", "")):
            code, out = self.main(dict(PR_ENV, **{key: value}))
            self.assertEqual(code, 1, (key, value))
            self.assertIn("::error::", out)


class FetchUrlTest(unittest.TestCase):
    def test_url_names_the_sonar_check_on_the_head_sha(self):
        url = sg.check_runs_url("org/storm", SHA)
        self.assertTrue(url.startswith("https://api.github.com/repos/org/storm/commits/%s/check-runs?" % SHA))
        self.assertIn("check_name=SonarCloud%20Code%20Analysis", url)


if __name__ == "__main__":
    unittest.main()
