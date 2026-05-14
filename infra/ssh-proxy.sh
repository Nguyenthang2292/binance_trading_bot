#!/usr/bin/env bash
# Open a SOCKS5 tunnel via EC2 for proxy-aware tools.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STATE_FILE="$SCRIPT_DIR/.ec2-state"

[[ -f "$STATE_FILE" ]] || { echo "ERROR: .ec2-state not found. Run provision.sh first."; exit 1; }
# shellcheck disable=SC1090
source "$STATE_FILE"

PROXY_PORT="${1:-1080}"

echo "============================================================"
echo "Opening SOCKS5 proxy tunnel"
echo "EC2 Elastic IP : $ELASTIC_IP"
echo "Local port     : $PROXY_PORT"
echo
echo "Note: this only works for clients that explicitly support SOCKS5."
echo "Current C++ bot transport does NOT auto-use ALL_PROXY/HTTPS_PROXY."
echo
echo "Proxy smoke test:"
echo "HTTPS_PROXY=socks5h://localhost:${PROXY_PORT} curl https://fapi.binance.com/fapi/v1/ping"
echo
echo "Press Ctrl+C to close the tunnel."
echo "============================================================"

exec ssh \
    -i "$KEY_PATH" \
    -o StrictHostKeyChecking=no \
    -o ServerAliveInterval=30 \
    -o ServerAliveCountMax=3 \
    -o ExitOnForwardFailure=yes \
    -D "${PROXY_PORT}" \
    -N \
    "ubuntu@${ELASTIC_IP}"
