#!/usr/bin/env bash
#
# One-liner bootstrap: clones the repo and runs install.sh.
#
# Intended usage from a fresh Ubuntu/Debian server (run AS root or with sudo):
#
#   # Public repo:
#   curl -fsSL https://raw.githubusercontent.com/tiger14779/remote-desktop/main/relay/deploy/bootstrap.sh | sudo bash
#
#   # Private repo, via a Personal Access Token (PAT):
#   GITHUB_TOKEN=ghp_xxx \
#       curl -fsSL https://raw.githubusercontent.com/tiger14779/remote-desktop/main/relay/deploy/bootstrap.sh | sudo -E bash
#
# Or grab the script first, then pass arguments:
#
#   sudo GITHUB_TOKEN=ghp_xxx bash bootstrap.sh
#
# What it does:
#   1. apt install git + curl if missing
#   2. clones the repo to /opt/remote-desktop-src (or REPO_DIR override)
#   3. delegates to relay/deploy/install.sh
#
# Idempotent: re-run to upgrade (git pull + reinstall + systemctl restart).

set -euo pipefail

REPO_URL="${REPO_URL:-https://github.com/tiger14779/remote-desktop.git}"
REPO_DIR="${REPO_DIR:-/opt/remote-desktop-src}"
BRANCH="${BRANCH:-main}"

if [ "$(id -u)" -ne 0 ]; then
    echo "error: must run as root (try: sudo $0)" >&2
    exit 1
fi

# Pull credentials into the URL if a PAT is provided (private repo flow).
if [ -n "${GITHUB_TOKEN:-}" ]; then
    REPO_URL="$(echo "$REPO_URL" | sed "s|https://|https://${GITHUB_TOKEN}@|")"
fi

echo "==> Installing prerequisites (git, curl)..."
apt-get update -y
apt-get install -y --no-install-recommends git curl ca-certificates

if [ -d "$REPO_DIR/.git" ]; then
    echo "==> Updating existing checkout at $REPO_DIR"
    git -C "$REPO_DIR" fetch --depth 1 origin "$BRANCH"
    git -C "$REPO_DIR" reset --hard "origin/$BRANCH"
else
    echo "==> Cloning $REPO_URL -> $REPO_DIR (branch: $BRANCH)"
    rm -rf "$REPO_DIR"
    git clone --depth 1 --branch "$BRANCH" "$REPO_URL" "$REPO_DIR"
fi

echo "==> Handing off to relay/deploy/install.sh"
exec bash "$REPO_DIR/relay/deploy/install.sh"
