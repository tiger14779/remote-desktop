#!/usr/bin/env bash
#
# One-shot deploy of the relay to a Linux host with systemd.
#
# What it does (in order):
#   1. cross-compiles cmd/relay for linux/amd64
#   2. scp the binary + service unit + config.example to the server
#   3. ssh in, create a dedicated 'relay' user, install the unit, restart
#
# Idempotent: re-running upgrades the binary in place and restarts the service.
# config.json is treated as operator state — created from the example on first
# deploy, never overwritten on subsequent runs.
#
# Usage:
#   ./deploy.sh user@host
#
# Environment overrides:
#   INSTALL_DIR    default: /opt/remote-desktop-relay
#   SERVICE_NAME   default: remote-desktop-relay
#   LISTEN_ADDR    default: :7100  (only takes effect on FIRST deploy because
#                                   the address is baked into the systemd unit)
#
# Requirements:
#   Local:   go (any 1.22+), ssh, scp
#   Remote:  sudo, systemd, useradd
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 user@host" >&2
    exit 1
fi

REMOTE="$1"
INSTALL_DIR="${INSTALL_DIR:-/opt/remote-desktop-relay}"
SERVICE_NAME="${SERVICE_NAME:-remote-desktop-relay}"
LISTEN_ADDR="${LISTEN_ADDR:-:7100}"

# Resolve repo paths regardless of where the user runs this from.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RELAY_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$RELAY_DIR/build/linux-amd64"
BIN_PATH="$BUILD_DIR/relay"

echo "==> Cross-compiling relay (linux/amd64) ..."
mkdir -p "$BUILD_DIR"
( cd "$RELAY_DIR" && GOOS=linux GOARCH=amd64 CGO_ENABLED=0 \
    go build -trimpath -ldflags="-s -w" -o "$BIN_PATH" ./cmd/relay )
ls -lh "$BIN_PATH"

# Generate a service unit with the right ExecStart line. We don't ship the
# verbatim template because users may want a different listen address baked in.
TMP_UNIT="$(mktemp)"
trap 'rm -f "$TMP_UNIT"' EXIT
sed -e "s|^ExecStart=.*|ExecStart=$INSTALL_DIR/relay -listen $LISTEN_ADDR -config $INSTALL_DIR/config.json|" \
    -e "s|^WorkingDirectory=.*|WorkingDirectory=$INSTALL_DIR|" \
    "$SCRIPT_DIR/remote_desktop_relay.service" > "$TMP_UNIT"

echo "==> Uploading to $REMOTE ..."
ssh "$REMOTE" "sudo mkdir -p $INSTALL_DIR && sudo chown \$USER:\$USER $INSTALL_DIR"
scp "$BIN_PATH" "$REMOTE:$INSTALL_DIR/relay.new"
scp "$RELAY_DIR/config.example.json" "$REMOTE:$INSTALL_DIR/config.example.json"
scp "$TMP_UNIT" "$REMOTE:/tmp/$SERVICE_NAME.service"

echo "==> Installing on remote ..."
ssh "$REMOTE" "sudo bash -s" <<EOF
set -euo pipefail

# Dedicated system user (idempotent).
if ! id -u relay >/dev/null 2>&1; then
    useradd --system --no-create-home --shell /usr/sbin/nologin relay
fi

# Atomic binary swap so an in-flight restart can't race the upgrade.
mv $INSTALL_DIR/relay.new $INSTALL_DIR/relay
chmod 755 $INSTALL_DIR/relay
chown relay:relay $INSTALL_DIR/relay
chown relay:relay $INSTALL_DIR/config.example.json

# First-time config — never clobber operator edits on later deploys.
if [ ! -f $INSTALL_DIR/config.json ]; then
    cp $INSTALL_DIR/config.example.json $INSTALL_DIR/config.json
    echo "    (created initial config.json from example — edit tokens before going live!)"
fi
chown relay:relay $INSTALL_DIR/config.json
chmod 600 $INSTALL_DIR/config.json

# Install/refresh systemd unit.
mv /tmp/$SERVICE_NAME.service /etc/systemd/system/$SERVICE_NAME.service
chmod 644 /etc/systemd/system/$SERVICE_NAME.service
systemctl daemon-reload
systemctl enable $SERVICE_NAME
systemctl restart $SERVICE_NAME
sleep 1
systemctl status --no-pager --lines=5 $SERVICE_NAME
EOF

echo
echo "==> Done."
echo "    Tail logs:  ssh $REMOTE 'journalctl -u $SERVICE_NAME -f'"
echo "    Edit config: ssh $REMOTE 'sudo -u relay nano $INSTALL_DIR/config.json && sudo systemctl restart $SERVICE_NAME'"
