---
description: Check SonarCloud status for a pull request (alias for sonarcloud-status)
allowed-tools: Bash
---

Check SonarCloud status for a pull request.

Usage:
- /sonarcloud-branch 48    - Check PR #48
- /sonarcloud-branch       - Interactive: prompt for PR number

Use the `sonar` CLI to check quality gate status and issues:

```bash
# List issues on a PR (new code only)
sonar list issues --project shevchenkcoigor_storm --pull-request "$@" --format table

# Quality gate status for a PR
sonar api get "/api/qualitygates/project_status?projectKey=shevchenkcoigor_storm&pullRequest=$@"

# Or for a branch (develop/master/main)
sonar list issues --project shevchenkcoigor_storm --branch develop --format table
sonar api get "/api/qualitygates/project_status?projectKey=shevchenkcoigor_storm&branch=develop"
```

Note: Auth is stored in the OS keychain via `sonar auth login`.
