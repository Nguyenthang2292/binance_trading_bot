#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT_DIR"

if [[ ! -f infra/.ec2-state ]]; then
    echo "ERROR: infra/.ec2-state not found"
    exit 1
fi

# shellcheck disable=SC1091
source infra/.ec2-state

SSH_OPTS="-i $KEY_PATH -o StrictHostKeyChecking=no -o ConnectTimeout=10"
HOST="ubuntu@${ELASTIC_IP}"

ssh $SSH_OPTS "$HOST" "sudo systemctl status binance-bot --no-pager -l"
