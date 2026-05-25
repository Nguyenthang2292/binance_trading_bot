#!/usr/bin/env bash
# Upload source to EC2, build on EC2, and optionally install/start an EC2 bot.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
STATE_FILE="$SCRIPT_DIR/.ec2-state"
PROD_ENV_FILE="$SCRIPT_DIR/prod.env"
RUN_BOT_ON_EC2="${RUN_BOT_ON_EC2:-0}"
RESTART_SERVICE="${RESTART_SERVICE:-0}"

fail() { printf "ERROR: %s\n" "$*" >&2; exit 1; }
log() { printf "==> %s\n" "$*"; }

[[ -f "$STATE_FILE" ]] || fail ".ec2-state not found. Run provision.sh first."
if [[ "$RUN_BOT_ON_EC2" != "1" ]]; then
    fail "EC2 bot deployment is disabled for local-only mode. Use infra/ssh-proxy.sh for the Binance whitelist tunnel. Set RUN_BOT_ON_EC2=1 only if you intentionally want the bot to run on EC2."
fi
[[ -f "$PROD_ENV_FILE" ]] || fail "Missing infra/prod.env. Copy from infra/prod.env.example and fill Binance keys."

if grep -n "^[[:space:]]*AWS_" "$PROD_ENV_FILE" >/dev/null; then
    fail "infra/prod.env contains AWS_* variables. Remove them before deploy."
fi

# shellcheck disable=SC1090
source "$STATE_FILE"

[[ -n "${ELASTIC_IP:-}" ]] || fail "ELASTIC_IP missing in .ec2-state"
[[ -n "${KEY_PATH:-}" ]] || fail "KEY_PATH missing in .ec2-state"

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

SSH_OPTS="-i $KEY_PATH -o StrictHostKeyChecking=no -o ConnectTimeout=10"
HOST="ubuntu@${ELASTIC_IP}"

log "Deploy to $ELASTIC_IP"
log "Checking SSH connectivity..."
ssh $SSH_OPTS "$HOST" "echo ok" >/dev/null

log "Uploading runtime env to /opt/binance-bot/prod.env..."
scp $SSH_OPTS "$PROD_ENV_FILE" "$HOST:/tmp/binance-bot.prod.env"
ssh $SSH_OPTS "$HOST" "sudo mv /tmp/binance-bot.prod.env /opt/binance-bot/prod.env && sudo chown binance-bot:binance-bot /opt/binance-bot/prod.env && sudo chmod 600 /opt/binance-bot/prod.env"

log "Syncing source to EC2..."
rsync -az --delete \
    -e "ssh $SSH_OPTS" \
    --exclude="build/" \
    --exclude=".git/" \
    --exclude=".env" \
    --exclude="infra/.ec2-state" \
    --exclude="infra/aws.env.local" \
    --exclude="infra/prod.env" \
    --exclude="*.pem" \
    "$PROJECT_ROOT/" \
    "$HOST:~/binance-bot-src/"

log "Building on EC2..."
ssh $SSH_OPTS "$HOST" bash << "BUILD"
set -euo pipefail
cd ~/binance-bot-src
BUILD_DIR="$HOME/binance-bot-build"

if [[ ! -f /swapfile ]]; then
    sudo fallocate -l 2G /swapfile
    sudo chmod 600 /swapfile
    sudo mkswap /swapfile
    sudo swapon /swapfile
    echo "/swapfile none swap sw 0 0" | sudo tee -a /etc/fstab >/dev/null
elif ! swapon --show=NAME | grep -q '^/swapfile$'; then
    sudo swapon /swapfile
fi

cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF

cmake --build "$BUILD_DIR" \
    --target binance_trading_bot \
    --parallel 1

ls -lh "$BUILD_DIR/bin/binance_trading_bot"
BUILD

log "Installing binary..."
ssh $SSH_OPTS "$HOST" "sudo install -o binance-bot -g binance-bot -m 755 \$HOME/binance-bot-build/bin/binance_trading_bot /opt/binance-bot/binance_trading_bot"

log "Installing runtime config..."
scp $SSH_OPTS "$PROJECT_ROOT/config.json" "$HOST:/tmp/binance-bot.config.json"
ssh $SSH_OPTS "$HOST" "sudo mv /tmp/binance-bot.config.json /opt/binance-bot/config.json && sudo chown binance-bot:binance-bot /opt/binance-bot/config.json && sudo chmod 644 /opt/binance-bot/config.json"

if [[ "$RESTART_SERVICE" == "1" ]]; then
    log "Restarting service..."
    ssh $SSH_OPTS "$HOST" "sudo systemctl restart binance-bot"
    sleep 2
else
    log "Skipping service restart because RESTART_SERVICE=$RESTART_SERVICE"
fi

log "Service status:"
ssh $SSH_OPTS "$HOST" "sudo systemctl status binance-bot --no-pager -l" || true

echo
echo "============================================================"
echo "DEPLOY COMPLETE"
echo "Logs: ssh -i $KEY_PATH ubuntu@$ELASTIC_IP"
echo "      sudo journalctl -u binance-bot -f"
echo "============================================================"
