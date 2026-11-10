# SonarCloud Integration Setup for Claude Code

This guide explains how to use the SonarCloud slash command in Claude Code.

## Prerequisites

1. **SonarCloud Account**: You need a SonarCloud account linked to your GitHub
2. **SonarCloud Token**: Generate an access token from SonarCloud

## Setup Steps

### 1. Get Your SonarCloud Token

1. Go to https://sonarcloud.io
2. Sign in with your GitHub account
3. Click on your avatar → **My Account** → **Security**
4. Under **Generate Tokens**, create a new token:
   - Name: `claude-code-integration`
   - Type: User Token
   - Click **Generate**
5. **Copy the token** (you won't see it again!)

### 2. Set Environment Variable

Add the token to your shell environment:

**For bash (~/.bashrc or ~/.bash_profile):**
```bash
export SONAR_TOKEN="your_sonarcloud_token_here"
```

**For zsh (~/.zshrc):**
```bash
export SONAR_TOKEN="your_sonarcloud_token_here"
```

**For fish (~/.config/fish/config.fish):**
```fish
set -gx SONAR_TOKEN "your_sonarcloud_token_here"
```

Then reload your shell:
```bash
source ~/.zshrc  # or ~/.bashrc, etc.
```

### 3. Verify Setup

Check that the token is available:
```bash
echo $SONAR_TOKEN
```

You should see your token printed.

## Using the Slash Command

Once setup is complete, you can use the command in Claude Code:

```
/sonarcloud-status
```

This will:
- Fetch the current quality gate status
- Show project metrics (coverage, bugs, vulnerabilities, code smells)
- List recent issues
- Provide actionable recommendations

## SonarCloud Dashboard

View the full dashboard at:
https://sonarcloud.io/dashboard?id=shevchenkcoigor_storm

## Troubleshooting

### "SONAR_TOKEN not set"
- Make sure you exported the variable in your shell config
- Reload your shell or restart your terminal
- Verify with `echo $SONAR_TOKEN`

### "401 Unauthorized"
- Your token may have expired
- Generate a new token from SonarCloud
- Update your environment variable

### "No data returned"
- Your project may not be set up on SonarCloud yet
- Make sure the GitHub Actions workflow has run at least once
- Check that the project key is `shevchenkcoigor_storm`

## Security Note

Never commit your SONAR_TOKEN to version control. It's stored in your environment variables only.

The `.claude/` directory is already in `.gitignore`, but if you create any files with tokens, make sure they're excluded from git.
