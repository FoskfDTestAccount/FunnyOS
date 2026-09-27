#!/bin/bash
#
# FunnyOS -- GitHub repository setup.
#
# Prerequisite: gh is installed and authenticated.
#   gh auth login --hostname github.com --git-protocol https --web
#
# This script does four things:
#   1. Reads your identity from the GitHub API and configures this
#      repository's commit identity from it.
#   2. Commits the current state.
#   3. Creates the repository under your account.
#   4. Pushes.
#
# Idempotent: if the repository already exists it is reused, not an error.
#
set -e

GH="/c/Program Files/GitHub CLI/gh.exe"
[ -x "$GH" ] || GH="$(command -v gh || true)"
if [ ! -x "$GH" ]; then
    echo "ERROR: gh not found. Install with: winget install --id GitHub.cli" >&2
    exit 1
fi

PROJ="/c/FunnyOS"
cd "$PROJ"

REPO_NAME="FunnyOS"
# Visibility: private is the safe default -- private to public is one
# click, whereas public to private cannot recall content that was already
# indexed. Change this to "public" if you want it public from the start.
VISIBILITY="private"
DESCRIPTION="Native x86-64 operating system with a built-in 8086 virtual machine for running DOS programs"

# ------------------------------------------------------------- 1. identity
if ! "$GH" auth status >/dev/null 2>&1; then
    echo "ERROR: gh is not authenticated." >&2
    echo "Run:" >&2
    echo "  gh auth login --hostname github.com --git-protocol https --web" >&2
    exit 1
fi

echo "=== Reading GitHub account identity ==="
LOGIN=$("$GH" api user --jq .login)
USER_ID=$("$GH" api user --jq .id)
echo "  Account: $LOGIN (id=$USER_ID)"

# GitHub's noreply address format: <user id>+<login>@users.noreply.github.com
# Committing with it hides your real address while still linking the
# commit to your account. Set COMMIT_EMAIL below to override.
COMMIT_EMAIL="${USER_ID}+${LOGIN}@users.noreply.github.com"

echo "=== Configuring this repository's commit identity ==="
git config user.name  "$LOGIN"
git config user.email "$COMMIT_EMAIL"
echo "  user.name  = $LOGIN"
echo "  user.email = $COMMIT_EMAIL"

# ------------------------------------------------------------- 2. commit
echo
echo "=== Staging and committing ==="
git add -A

if git diff --cached --quiet; then
    echo "  Nothing to commit."
else
    git commit -F - <<'COMMIT_MSG'
Project skeleton

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
COMMIT_MSG
    echo "  Committed: $(git rev-parse --short HEAD)"
fi

# ------------------------------------------------------------- 3. repository
echo
echo "=== Creating GitHub repository ==="
if "$GH" repo view "$LOGIN/$REPO_NAME" >/dev/null 2>&1; then
    echo "  Repository already exists: $LOGIN/$REPO_NAME, reusing it."
else
    "$GH" repo create "$REPO_NAME" \
        --"$VISIBILITY" \
        --description "$DESCRIPTION" \
        --source=. \
        --remote=origin
    echo "  Created: $LOGIN/$REPO_NAME ($VISIBILITY)"
fi

# Make sure origin exists even when the repository already existed but the
# local clone had no remote configured.
if ! git remote get-url origin >/dev/null 2>&1; then
    git remote add origin "https://github.com/$LOGIN/$REPO_NAME.git"
    echo "  Added origin"
fi

# ------------------------------------------------------------- 4. push
echo
echo "=== Pushing ==="
git push -u origin main

echo
echo "Done: https://github.com/$LOGIN/$REPO_NAME"
