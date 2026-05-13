#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
STATE_FILE="$SCRIPT_DIR/.ec2-state"

# Load .env
if [ ! -f "$PROJECT_ROOT/.env" ]; then
    echo "ERROR: .env not found at $PROJECT_ROOT/.env"
    exit 1
fi
set -a; source "$PROJECT_ROOT/.env"; set +a

REGION="${AWS_REGION:-ap-southeast-1}"
KEY_NAME="binance-bot-key"
KEY_PATH="$HOME/.ssh/${KEY_NAME}.pem"
SG_NAME="binance-bot-sg"
INSTANCE_TYPE="t3.micro"
TAG_NAME="binance-bot"

if [ -f "$STATE_FILE" ]; then
    echo "ERROR: .ec2-state already exists — infrastructure may already be provisioned."
    echo "       Run teardown.sh first if you want to reprovision."
    exit 1
fi

echo "==> Region: $REGION"

# Get local IP for SSH whitelist
echo "==> Detecting local IP..."
MY_IP=$(curl -s --max-time 10 ifconfig.me || curl -s --max-time 10 api.ipify.org)
if [ -z "$MY_IP" ]; then
    echo "ERROR: Could not detect local IP. Check internet connection."
    exit 1
fi
echo "    Local IP: $MY_IP"

# Get latest Ubuntu 24.04 LTS AMI (Canonical owner ID: 099720109477)
echo "==> Looking up latest Ubuntu 24.04 LTS AMI..."
AMI_ID=$(aws ec2 describe-images \
    --region "$REGION" \
    --owners 099720109477 \
    --filters \
        "Name=name,Values=ubuntu/images/hvm-ssd-gp3/ubuntu-noble-24.04-amd64-server-*" \
        "Name=state,Values=available" \
        "Name=architecture,Values=x86_64" \
    --query "sort_by(Images, &CreationDate)[-1].ImageId" \
    --output text)
echo "    AMI ID: $AMI_ID"

# Create Key Pair
echo "==> Creating key pair '$KEY_NAME'..."
if [ -f "$KEY_PATH" ]; then
    echo "    WARNING: $KEY_PATH already exists — overwriting."
fi
aws ec2 create-key-pair \
    --region "$REGION" \
    --key-name "$KEY_NAME" \
    --key-type rsa \
    --key-format pem \
    --query "KeyMaterial" \
    --output text > "$KEY_PATH"
chmod 400 "$KEY_PATH"
echo "    Saved: $KEY_PATH"

# Create Security Group
echo "==> Creating security group '$SG_NAME'..."
SG_ID=$(aws ec2 create-security-group \
    --region "$REGION" \
    --group-name "$SG_NAME" \
    --description "Binance bot: SSH inbound restricted, all outbound" \
    --query "GroupId" \
    --output text)
echo "    Security Group ID: $SG_ID"

# SSH inbound — current IP only
aws ec2 authorize-security-group-ingress \
    --region "$REGION" \
    --group-id "$SG_ID" \
    --protocol tcp \
    --port 22 \
    --cidr "${MY_IP}/32" \
    --output text > /dev/null
echo "    SSH inbound allowed from: ${MY_IP}/32"

# Launch EC2
echo "==> Launching $INSTANCE_TYPE instance..."
INSTANCE_ID=$(aws ec2 run-instances \
    --region "$REGION" \
    --image-id "$AMI_ID" \
    --instance-type "$INSTANCE_TYPE" \
    --key-name "$KEY_NAME" \
    --security-group-ids "$SG_ID" \
    --block-device-mappings "DeviceName=/dev/sda1,Ebs={VolumeSize=20,VolumeType=gp3,DeleteOnTermination=true}" \
    --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=${TAG_NAME}}]" \
    --query "Instances[0].InstanceId" \
    --output text)
echo "    Instance ID: $INSTANCE_ID"

echo "==> Waiting for instance to be running..."
aws ec2 wait instance-running --region "$REGION" --instance-ids "$INSTANCE_ID"
echo "    Instance is running."

# Allocate Elastic IP
echo "==> Allocating Elastic IP..."
ALLOC_ID=$(aws ec2 allocate-address \
    --region "$REGION" \
    --domain vpc \
    --query "AllocationId" \
    --output text)

ELASTIC_IP=$(aws ec2 describe-addresses \
    --region "$REGION" \
    --allocation-ids "$ALLOC_ID" \
    --query "Addresses[0].PublicIp" \
    --output text)
echo "    Elastic IP: $ELASTIC_IP"

# Associate Elastic IP
echo "==> Associating Elastic IP to instance..."
aws ec2 associate-address \
    --region "$REGION" \
    --instance-id "$INSTANCE_ID" \
    --allocation-id "$ALLOC_ID" \
    --output text > /dev/null
echo "    Associated."

# Wait for SSH
echo "==> Waiting for SSH (max 3 minutes)..."
READY=0
for i in $(seq 1 18); do
    if ssh -i "$KEY_PATH" \
        -o StrictHostKeyChecking=no \
        -o ConnectTimeout=5 \
        -o BatchMode=yes \
        "ubuntu@${ELASTIC_IP}" "echo ok" 2>/dev/null | grep -q ok; then
        READY=1
        echo "    SSH ready."
        break
    fi
    echo "    Attempt $i/18 — waiting 10s..."
    sleep 10
done

if [ "$READY" -eq 0 ]; then
    echo "ERROR: SSH did not become available in time. Check the Security Group and instance."
    echo "       Instance ID: $INSTANCE_ID | Elastic IP: $ELASTIC_IP"
    exit 1
fi

# Bootstrap: install build tools
echo "==> Installing build tools on EC2..."
ssh -i "$KEY_PATH" \
    -o StrictHostKeyChecking=no \
    "ubuntu@${ELASTIC_IP}" bash << 'BOOTSTRAP'
set -e
export DEBIAN_FRONTEND=noninteractive
echo "-- apt update --"
sudo apt-get update -qq
echo "-- installing packages --"
sudo apt-get install -y -qq \
    build-essential \
    cmake \
    git \
    libssl-dev \
    rsync \
    ninja-build

# Verify cmake version
cmake --version | head -1

# Create app directories
sudo mkdir -p /opt/binance-bot
sudo chown ubuntu:ubuntu /opt/binance-bot

# Build cache directory (persists across deploys)
mkdir -p "$HOME/binance-bot-build"

echo "-- Bootstrap complete --"
BOOTSTRAP

# Register systemd service
echo "==> Registering systemd service..."
ssh -i "$KEY_PATH" \
    -o StrictHostKeyChecking=no \
    "ubuntu@${ELASTIC_IP}" bash << 'SERVICE'
set -e
sudo tee /etc/systemd/system/binance-bot.service > /dev/null << 'EOF'
[Unit]
Description=Binance Trading Bot
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=ubuntu
WorkingDirectory=/opt/binance-bot
EnvironmentFile=/opt/binance-bot/.env
ExecStart=/opt/binance-bot/binance_trading_bot
Restart=on-failure
RestartSec=10s
StandardOutput=journal
StandardError=journal
SyslogIdentifier=binance-bot

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable binance-bot
echo "-- systemd service enabled (not yet started) --"
SERVICE

# Write state file
cat > "$STATE_FILE" << EOF
INSTANCE_ID=${INSTANCE_ID}
ELASTIC_IP=${ELASTIC_IP}
ALLOC_ID=${ALLOC_ID}
KEY_NAME=${KEY_NAME}
KEY_PATH=${KEY_PATH}
SECURITY_GROUP_ID=${SG_ID}
REGION=${REGION}
PROVISIONED_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
MY_IP_AT_PROVISION=${MY_IP}
EOF
echo "    State saved: $STATE_FILE"

echo ""
echo "============================================================"
echo "  PROVISIONING COMPLETE"
echo "============================================================"
echo "  Instance ID  : $INSTANCE_ID"
echo "  Elastic IP   : $ELASTIC_IP"
echo "  SSH Key      : $KEY_PATH"
echo ""
echo "  ACTION REQUIRED:"
echo "  Whitelist this IP on Binance:"
echo "  Binance → API Management → Edit API Key"
echo "  → Restrict access to trusted IPs only"
echo "  → Add: $ELASTIC_IP"
echo ""
echo "  NEXT STEPS:"
echo "  1. Whitelist IP on Binance (above)"
echo "  2. Run: ./infra/deploy.sh           (upload source + build + start)"
echo "  3. Run: ./infra/ssh-proxy.sh        (for local dev via this IP)"
echo "============================================================"
