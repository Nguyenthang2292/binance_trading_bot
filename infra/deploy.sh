#!/usr/bin/env bash
# Upload source to EC2, build on EC2, restart service.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
STATE_FILE="$SCRIPT_DIR/.ec2-state"

if [ ! -f "$STATE_FILE" ]; then
    echo "ERROR: .ec2-state not found. Run provision.sh first."
    exit 1
fi

source "$STATE_FILE"

SSH_OPTS="-i $KEY_PATH -o StrictHostKeyChecking=no -o ConnectTimeout=10"
HOST="ubuntu@${ELASTIC_IP}"

echo "==> Deploy to $ELASTIC_IP"

# Verify SSH connectivity
echo "==> Checking SSH connectivity..."
ssh $SSH_OPTS "$HOST" "echo ok" > /dev/null
echo "    Connected."

# Sync source to EC2 (exclude build/, .git, .env)
echo "==> Syncing source to EC2..."
rsync -az --delete \
    -e "ssh $SSH_OPTS" \
    --exclude="build/" \
    --exclude=".git/" \
    --exclude=".env" \
    --exclude="infra/.ec2-state" \
    --exclude="*.pem" \
    "$PROJECT_ROOT/" \
    "$HOST:~/binance-bot-src/"
echo "    Source synced."

# Upload .env to EC2 app directory
echo "==> Uploading .env..."
scp $SSH_OPTS "$PROJECT_ROOT/.env" "$HOST:/opt/binance-bot/.env"
ssh $SSH_OPTS "$HOST" "chmod 600 /opt/binance-bot/.env"
echo "    .env uploaded."

# Build on EC2
echo "==> Building on EC2 (this may take several minutes on first run)..."
ssh $SSH_OPTS "$HOST" bash << 'BUILD'
set -e
cd ~/binance-bot-src
BUILD_DIR="$HOME/binance-bot-build"

echo "-- cmake configure --"
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF

echo "-- cmake build --"
cmake --build "$BUILD_DIR" \
    --target binance_trading_bot \
    --parallel "$(nproc)"

echo "-- build complete --"
ls -lh "$BUILD_DIR/bin/binance_trading_bot"
BUILD

# Install binary
echo "==> Installing binary to /opt/binance-bot/..."
ssh $SSH_OPTS "$HOST" bash << 'INSTALL'
set -e
cp "$HOME/binance-bot-build/bin/binance_trading_bot" /opt/binance-bot/binance_trading_bot
chmod 755 /opt/binance-bot/binance_trading_bot
echo "-- installed --"
INSTALL

# Restart service
echo "==> Restarting binance-bot service..."
ssh $SSH_OPTS "$HOST" "sudo systemctl restart binance-bot"
sleep 2

# Show status
echo "==> Service status:"
ssh $SSH_OPTS "$HOST" "sudo systemctl status binance-bot --no-pager -l" || true

echo ""
echo "============================================================"
echo "  DEPLOY COMPLETE"
echo "  View logs: ssh -i $KEY_PATH ubuntu@$ELASTIC_IP"
echo "             sudo journalctl -u binance-bot -f"
echo "============================================================"
