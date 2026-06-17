#!/usr/bin/env bash
#
# Server-side installer. Run this ON the Ubuntu/Debian host where you want the
# relay to live — not on your dev machine. (Use deploy.sh from the dev machine
# if you'd rather cross-compile + scp from your workstation.)
#
# What it does:
#   1. Make sure Go >= 1.22 is on PATH (apt's golang-go on 24.04 is fine; on
#      22.04 and older it's too old, so we drop in the official tarball)
#   2. Build cmd/relay from the local source tree
#   3. Create a dedicated 'relay' system user (no shell, no home)
#   4. Install binary + config.json + systemd unit into INSTALL_DIR
#   5. Enable + start the service
#
# Idempotent: re-run to upgrade. config.json is treated as operator state and
# never overwritten after the first install.
#
# Usage:
#   sudo bash deploy/install.sh
#   sudo INSTALL_DIR=/srv/relay LISTEN_ADDR=:9000 bash deploy/install.sh

set -euo pipefail

INSTALL_DIR="${INSTALL_DIR:-/opt/remote-desktop-relay}"
SERVICE_NAME="${SERVICE_NAME:-remote-desktop-relay}"
LISTEN_ADDR="${LISTEN_ADDR:-:7100}"
GO_VERSION="${GO_VERSION:-1.22.5}"

if [ "$(id -u)" -ne 0 ]; then
    echo "error: must run as root (try: sudo bash $0)" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RELAY_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ ! -f "$RELAY_DIR/go.mod" ]; then
    echo "error: expected $RELAY_DIR/go.mod — run from a relay/ checkout" >&2
    exit 1
fi

# ---- Ensure Go >= 1.22 ----------------------------------------------------
need_go=true
if command -v go >/dev/null 2>&1; then
    cur="$(go env GOVERSION 2>/dev/null | sed 's/^go//' || true)"
    # sort -V puts the smaller value first. If "1.22" comes first, cur >= 1.22.
    if [ -n "$cur" ] && [ "$(printf '1.22\n%s\n' "$cur" | sort -V | head -1)" = "1.22" ]; then
        echo "==> Using existing Go: go$cur"
        need_go=false
    else
        echo "==> Existing Go is go${cur:-unknown}, need >= 1.22 — replacing"
    fi
fi

if $need_go; then
    apt-get update -y
    apt-get install -y --no-install-recommends ca-certificates wget tar
    arch="$(dpkg --print-architecture)"
    case "$arch" in
        amd64) gotar="go${GO_VERSION}.linux-amd64.tar.gz" ;;
        arm64) gotar="go${GO_VERSION}.linux-arm64.tar.gz" ;;
        *) echo "unsupported architecture: $arch" >&2; exit 1 ;;
    esac
    echo "==> Downloading $gotar..."
    wget -q --show-progress "https://go.dev/dl/$gotar" -O "/tmp/$gotar"
    rm -rf /usr/local/go
    tar -C /usr/local -xzf "/tmp/$gotar"
    rm "/tmp/$gotar"
    ln -sf /usr/local/go/bin/go /usr/local/bin/go
    /usr/local/go/bin/go version
fi

# ---- Build ----------------------------------------------------------------
echo "==> Building relay..."
cd "$RELAY_DIR"
mkdir -p build
CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o build/relay-linux ./cmd/relay
ls -lh build/relay-linux

# ---- User -----------------------------------------------------------------
if ! id -u relay >/dev/null 2>&1; then
    echo "==> Creating system user 'relay'..."
    useradd --system --no-create-home --shell /usr/sbin/nologin relay
fi

# ---- Install files --------------------------------------------------------
echo "==> Installing to $INSTALL_DIR..."
mkdir -p "$INSTALL_DIR"
install -m 755 -o relay -g relay build/relay-linux        "$INSTALL_DIR/relay"
install -m 644 -o relay -g relay config.example.json      "$INSTALL_DIR/config.example.json"

# ---- First-install config -------------------------------------------------
# Resolution order:
#   1. $SCRIPT_DIR/config.production.json     — operator-frozen, committed to
#                                                the (private) repo so every
#                                                server ends up identical.
#                                                THIS IS THE NORMAL PATH FOR
#                                                CUSTOMER-FACING DEPLOYMENTS.
#   2. random tokens via openssl              — convenience for dev/test.
#
# Either way, an existing $INSTALL_DIR/config.json is preserved across reruns
# (upgrade-safe). To force-replace it with the production config after a
# token rotation: `sudo rm $INSTALL_DIR/config.json && bash install.sh`.
fresh_config=false
used_production_config=false
DEVICE_TOKEN=""
SESSION_TOKEN=""

if [ ! -f "$INSTALL_DIR/config.json" ]; then
    if [ -f "$SCRIPT_DIR/config.production.json" ]; then
        echo "==> Using committed production config (deploy/config.production.json)"
        install -m 600 -o relay -g relay \
            "$SCRIPT_DIR/config.production.json" "$INSTALL_DIR/config.json"
        used_production_config=true
        fresh_config=true
    else
        echo "==> No production config found — generating random tokens"
        if ! command -v openssl >/dev/null 2>&1; then
            apt-get install -y --no-install-recommends openssl
        fi
        DEVICE_TOKEN="$(openssl rand -hex 24)"
        SESSION_TOKEN="$(openssl rand -hex 24)"

        cat > "$INSTALL_DIR/config.json" <<EOF
{
  "devices": {
    "device-001": "$DEVICE_TOKEN"
  },
  "users": {
    "admin": {
      "session_token": "$SESSION_TOKEN",
      "allowed_devices": ["device-001"]
    }
  }
}
EOF
        chown relay:relay "$INSTALL_DIR/config.json"
        chmod 600 "$INSTALL_DIR/config.json"
        fresh_config=true
    fi
fi

# ---- systemd unit ---------------------------------------------------------
UNIT_FILE="/etc/systemd/system/$SERVICE_NAME.service"
sed -e "s|^ExecStart=.*|ExecStart=$INSTALL_DIR/relay -listen $LISTEN_ADDR -config $INSTALL_DIR/config.json|" \
    -e "s|^WorkingDirectory=.*|WorkingDirectory=$INSTALL_DIR|" \
    "$SCRIPT_DIR/remote_desktop_relay.service" > "$UNIT_FILE"
chmod 644 "$UNIT_FILE"

systemctl daemon-reload
systemctl enable "$SERVICE_NAME"
systemctl restart "$SERVICE_NAME"
sleep 1
systemctl status --no-pager --lines=5 "$SERVICE_NAME" || true

# ---- Closing notes --------------------------------------------------------
echo
echo "==> Done. Service: $SERVICE_NAME"
echo "    Listen address: $LISTEN_ADDR"
echo "    Logs:           journalctl -u $SERVICE_NAME -f"
echo "    Status:         systemctl status $SERVICE_NAME"

if $fresh_config; then
    # Best-effort: figure out the address clients should dial. Public IP first,
    # falling back to the primary local address if no internet access.
    SERVER_IP=""
    if command -v curl >/dev/null 2>&1; then
        SERVER_IP="$(curl -fsS --max-time 3 https://api.ipify.org 2>/dev/null || true)"
    fi
    [ -z "$SERVER_IP" ] && SERVER_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
    [ -z "$SERVER_IP" ] && SERVER_IP="<YOUR-SERVER-IP>"
    PORT="${LISTEN_ADDR#:}"

    if $used_production_config; then
        cat <<EOF

================================================================================
  Production config installed from deploy/config.production.json
================================================================================

  Relay endpoint:        $SERVER_IP:$PORT
  Live config on disk:   $INSTALL_DIR/config.json
  Source of truth:       $SCRIPT_DIR/config.production.json (commit to repo)

  Current devices / users:
$(sudo -u relay grep -E '^\s*"(devices|users|device-|allowed_devices|session_token)' "$INSTALL_DIR/config.json" 2>/dev/null | sed 's/^/      /')

  Customer-side commands stay the same on every server — the tokens live in
  config.production.json. Distribute agent.exe / viewer.exe with whichever
  device_id + token pair matches your customer.

  To rotate tokens:
      1. Edit relay/deploy/config.production.json, commit, push
      2. On each server:
           cd ~/remote-desktop && git pull
           sudo rm $INSTALL_DIR/config.json
           sudo bash relay/deploy/install.sh

  Open the relay port on your firewall (if ufw is on):
      sudo ufw allow $PORT/tcp
================================================================================
EOF
    else
        cat <<EOF

================================================================================
  Auto-generated credentials (also stored in $INSTALL_DIR/config.json):
================================================================================

  Relay endpoint:    $SERVER_IP:$PORT

  --- On the controlled Windows machine (agent) ---
  agent.exe ^
      -relay $SERVER_IP:$PORT ^
      -device-id device-001 ^
      -device-token $DEVICE_TOKEN

  --- On the management Windows machine (viewer) ---
  viewer.exe ^
      -relay $SERVER_IP:$PORT ^
      -account admin ^
      -session-token $SESSION_TOKEN ^
      -device-id device-001

  These tokens are random and unique to this server. To get the same tokens
  on every server (so customer-side binaries stay portable), drop a
  pre-filled relay/deploy/config.production.json into the repo — the next
  install will use it.

  To add more devices/users:
      sudo -u relay nano $INSTALL_DIR/config.json
      sudo systemctl restart $SERVICE_NAME

  Open the relay port on your firewall (if ufw is on):
      sudo ufw allow $PORT/tcp
================================================================================
EOF
    fi
fi
