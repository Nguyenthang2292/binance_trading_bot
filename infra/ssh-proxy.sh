#!/usr/bin/env bash
# Open a SOCKS5 tunnel via EC2 for proxy-aware tools.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STATE_FILE="$SCRIPT_DIR/.ec2-state"

[[ -f "$STATE_FILE" ]] || { echo "ERROR: .ec2-state not found. Run provision.sh first."; exit 1; }
# shellcheck disable=SC1090
source "$STATE_FILE"

PROXY_PORT="${1:-1080}"
TMP_KEY_PATH=""
cleanup_tmp_key() {
    if [[ -n "$TMP_KEY_PATH" ]]; then
        rm -f "$TMP_KEY_PATH"
    fi
}
trap cleanup_tmp_key EXIT

TMP_KEY_PATH="$(mktemp)"
cp "$KEY_PATH" "$TMP_KEY_PATH"
chmod 600 "$TMP_KEY_PATH"
KEY_PATH="$TMP_KEY_PATH"

echo "============================================================"
echo "Opening SOCKS5 proxy tunnel"
echo "EC2 Elastic IP : $ELASTIC_IP"
echo "Local port     : $PROXY_PORT"
echo
echo "Bot supports SOCKS5 tunnel when BINANCE_SOCKS5_PROXY (or ALL_PROXY) is set."
echo "Example: export BINANCE_SOCKS5_PROXY=socks5://127.0.0.1:${PROXY_PORT}"
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
