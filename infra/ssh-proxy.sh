#!/usr/bin/env bash
# Open a SOCKS5 proxy tunnel through EC2 for local dev.
# Local bot traffic → localhost:1080 → EC2 → Binance
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STATE_FILE="$SCRIPT_DIR/.ec2-state"

if [ ! -f "$STATE_FILE" ]; then
    echo "ERROR: .ec2-state not found. Run provision.sh first."
    exit 1
fi

source "$STATE_FILE"

PROXY_PORT="${1:-1080}"

echo "============================================================"
echo "  Opening SOCKS5 proxy tunnel"
echo "  EC2 Elastic IP : $ELASTIC_IP"
echo "  Local port     : $PROXY_PORT"
echo ""
echo "  Run your bot with:"
echo "  ALL_PROXY=socks5h://localhost:${PROXY_PORT} ./build/bin/binance_trading_bot"
echo ""
echo "  Press Ctrl+C to close the tunnel."
echo "============================================================"

# -D  SOCKS5 dynamic port forwarding
# -N  no remote command
# -o ServerAliveInterval  keepalive ping every 30s
# -o ExitOnForwardFailure  exit if forwarding setup fails
exec ssh \
    -i "$KEY_PATH" \
    -o StrictHostKeyChecking=no \
    -o ServerAliveInterval=30 \
    -o ServerAliveCountMax=3 \
    -o ExitOnForwardFailure=yes \
    -D "${PROXY_PORT}" \
    -N \
    "ubuntu@${ELASTIC_IP}"
