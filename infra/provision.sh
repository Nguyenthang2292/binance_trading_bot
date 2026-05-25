#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
STATE_FILE="$SCRIPT_DIR/.ec2-state"
AWS_ENV_FILE="$SCRIPT_DIR/aws.env.local"
RUN_BOT_ON_EC2="${RUN_BOT_ON_EC2:-0}"

REGION_DEFAULT="ap-southeast-1"
INSTANCE_TYPE="${INSTANCE_TYPE:-t3.micro}"
KEY_NAME="binance-bot-key"
SG_NAME="binance-bot-sg"
APP_NAME="binance-bot"

TAG_SPEC_INST="ResourceType=instance,Tags=[{Key=Name,Value=${APP_NAME}},{Key=App,Value=${APP_NAME}},{Key=ManagedBy,Value=infra-script}]"
TAG_SPEC_VOL="ResourceType=volume,Tags=[{Key=Name,Value=${APP_NAME}-volume},{Key=App,Value=${APP_NAME}},{Key=ManagedBy,Value=infra-script}]"
TAG_SPEC_SG="ResourceType=security-group,Tags=[{Key=Name,Value=${APP_NAME}-sg},{Key=App,Value=${APP_NAME}},{Key=ManagedBy,Value=infra-script}]"
TAG_SPEC_EIP="ResourceType=elastic-ip,Tags=[{Key=Name,Value=${APP_NAME}-eip},{Key=App,Value=${APP_NAME}},{Key=ManagedBy,Value=infra-script}]"

log() { printf "==> %s\n" "$*"; }
warn() { printf "WARNING: %s\n" "$*"; }
fail() { printf "ERROR: %s\n" "$*" >&2; exit 1; }
strip_cr() { tr -d "\r"; }

load_aws_env() {
    if [[ -f "$AWS_ENV_FILE" ]]; then
        set -a
        # shellcheck disable=SC1090
        source "$AWS_ENV_FILE"
        set +a
        log "Loaded AWS credentials from $AWS_ENV_FILE"
        return
    fi

    if [[ -f "$PROJECT_ROOT/.env" ]]; then
        warn "Using fallback $PROJECT_ROOT/.env for AWS credentials; move them to infra/aws.env.local"
        set -a
        # shellcheck disable=SC1090
        source "$PROJECT_ROOT/.env"
        set +a
        return
    fi

    warn "No $AWS_ENV_FILE or .env found. Will use current shell AWS env vars if present."
}

validate_ipv4() {
    local ip="$1"
    [[ "$ip" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] || return 1
    local IFS=.
    read -r a b c d <<< "$ip"
    for octet in "$a" "$b" "$c" "$d"; do
        ((octet >= 0 && octet <= 255)) || return 1
    done
}

state_set() {
    local key="$1"
    local value="$2"
    touch "$STATE_FILE"
    if grep -q "^${key}=" "$STATE_FILE"; then
        sed -i "s#^${key}=.*#${key}=${value}#" "$STATE_FILE"
    else
        printf "%s=%s\n" "$key" "$value" >> "$STATE_FILE"
    fi
}

if [[ -f "$STATE_FILE" ]]; then
    fail "$STATE_FILE already exists. Run infra/teardown.sh first if you want reprovision."
fi

load_aws_env

REGION="${AWS_REGION:-$REGION_DEFAULT}"
KEY_PATH="$HOME/.ssh/${KEY_NAME}.pem"

log "Region: $REGION"
log "Checking AWS authentication..."
aws sts get-caller-identity --output text >/dev/null || fail "AWS auth failed. Check infra/aws.env.local or exported AWS_* variables."

log "Detecting local IP..."
MY_IP="$(curl -s --max-time 10 ifconfig.me || curl -s --max-time 10 api.ipify.org || true)"
[[ -n "$MY_IP" ]] || fail "Could not detect local IP"
validate_ipv4 "$MY_IP" || fail "Detected IP is invalid: $MY_IP"
log "Local IP: $MY_IP"

state_set REGION "$REGION"
state_set KEY_NAME "$KEY_NAME"
state_set KEY_PATH "$KEY_PATH"
state_set SG_NAME "$SG_NAME"
state_set INSTANCE_TYPE "$INSTANCE_TYPE"
state_set APP_NAME "$APP_NAME"
state_set MY_IP_AT_PROVISION "$MY_IP"
state_set PROVISIONED_AT "$(date -u +%Y-%m-%dT%H:%M:%SZ)"

log "Looking up latest Ubuntu 24.04 LTS AMI..."
AMI_ID="$(aws ec2 describe-images \
    --region "$REGION" \
    --owners 099720109477 \
    --filters \
        "Name=name,Values=ubuntu/images/hvm-ssd-gp3/ubuntu-noble-24.04-amd64-server-*" \
        "Name=state,Values=available" \
        "Name=architecture,Values=x86_64" \
    --query "sort_by(Images, &CreationDate)[-1].ImageId" \
    --output text | strip_cr)"
[[ -n "$AMI_ID" && "$AMI_ID" != "None" ]] || fail "Failed to resolve Ubuntu AMI"
state_set AMI_ID "$AMI_ID"
log "AMI ID: $AMI_ID"

log "Creating key pair '$KEY_NAME'..."
if aws ec2 describe-key-pairs --region "$REGION" --key-names "$KEY_NAME" >/dev/null 2>&1; then
    fail "AWS key pair '$KEY_NAME' already exists. Delete it or rename KEY_NAME in script."
fi
if [[ -f "$KEY_PATH" ]]; then
    fail "Local key file already exists at $KEY_PATH. Move/delete it first to avoid mismatch."
fi
mkdir -p "$(dirname "$KEY_PATH")"
aws ec2 create-key-pair \
    --region "$REGION" \
    --key-name "$KEY_NAME" \
    --key-type rsa \
    --key-format pem \
    --query "KeyMaterial" \
    --output text > "$KEY_PATH"
chmod 400 "$KEY_PATH"
log "Saved key: $KEY_PATH"

log "Creating security group '$SG_NAME'..."
VPC_ID="$(aws ec2 describe-vpcs --region "$REGION" --filters Name=isDefault,Values=true --query "Vpcs[0].VpcId" --output text | strip_cr)"
[[ -n "$VPC_ID" && "$VPC_ID" != "None" ]] || fail "Default VPC not found in region $REGION"
SG_ID="$(aws ec2 create-security-group \
    --region "$REGION" \
    --group-name "$SG_NAME" \
    --description "Binance bot SG" \
    --vpc-id "$VPC_ID" \
    --tag-specifications "$TAG_SPEC_SG" \
    --query "GroupId" \
    --output text | strip_cr)"
[[ -n "$SG_ID" && "$SG_ID" != "None" ]] || fail "Failed to create security group"
state_set SECURITY_GROUP_ID "$SG_ID"
log "Security Group ID: $SG_ID"

aws ec2 authorize-security-group-ingress \
    --region "$REGION" \
    --group-id "$SG_ID" \
    --protocol tcp \
    --port 22 \
    --cidr "${MY_IP}/32" >/dev/null
log "SSH inbound allowed from ${MY_IP}/32"

aws ec2 revoke-security-group-egress \
    --region "$REGION" \
    --group-id "$SG_ID" \
    --ip-permissions '[{"IpProtocol":"-1","IpRanges":[{"CidrIp":"0.0.0.0/0"}]}]' >/dev/null 2>&1 || true

aws ec2 authorize-security-group-egress \
    --region "$REGION" \
    --group-id "$SG_ID" \
    --ip-permissions '[{"IpProtocol":"tcp","FromPort":443,"ToPort":443,"IpRanges":[{"CidrIp":"0.0.0.0/0","Description":"HTTPS"}]}]' >/dev/null
aws ec2 authorize-security-group-egress \
    --region "$REGION" \
    --group-id "$SG_ID" \
    --ip-permissions '[{"IpProtocol":"tcp","FromPort":80,"ToPort":80,"IpRanges":[{"CidrIp":"0.0.0.0/0","Description":"HTTP"}]}]' >/dev/null
aws ec2 authorize-security-group-egress \
    --region "$REGION" \
    --group-id "$SG_ID" \
    --ip-permissions '[{"IpProtocol":"udp","FromPort":123,"ToPort":123,"IpRanges":[{"CidrIp":"0.0.0.0/0","Description":"NTP"}]}]' >/dev/null
log "Applied strict outbound rules: TCP 443, TCP 80, UDP 123"

log "Launching instance type $INSTANCE_TYPE..."
INSTANCE_ID="$(aws ec2 run-instances \
    --region "$REGION" \
    --image-id "$AMI_ID" \
    --instance-type "$INSTANCE_TYPE" \
    --credit-specification CpuCredits=standard \
    --key-name "$KEY_NAME" \
    --security-group-ids "$SG_ID" \
    --block-device-mappings "DeviceName=/dev/sda1,Ebs={VolumeSize=20,VolumeType=gp3,DeleteOnTermination=true}" \
    --tag-specifications "$TAG_SPEC_INST" "$TAG_SPEC_VOL" \
    --query "Instances[0].InstanceId" \
    --output text | strip_cr)"
[[ -n "$INSTANCE_ID" && "$INSTANCE_ID" != "None" ]] || fail "Failed to launch instance"
state_set INSTANCE_ID "$INSTANCE_ID"
log "Instance ID: $INSTANCE_ID"

log "Waiting for instance running..."
aws ec2 wait instance-running --region "$REGION" --instance-ids "$INSTANCE_ID"

log "Allocating Elastic IP..."
ALLOC_ID="$(aws ec2 allocate-address --region "$REGION" --domain vpc --tag-specifications "$TAG_SPEC_EIP" --query "AllocationId" --output text | strip_cr)"
ELASTIC_IP="$(aws ec2 describe-addresses --region "$REGION" --allocation-ids "$ALLOC_ID" --query "Addresses[0].PublicIp" --output text | strip_cr)"
[[ -n "$ALLOC_ID" && "$ALLOC_ID" != "null" ]] || fail "Failed to allocate Elastic IP"
[[ -n "$ELASTIC_IP" && "$ELASTIC_IP" != "null" ]] || fail "Failed to read Elastic IP"
state_set ALLOC_ID "$ALLOC_ID"
state_set ELASTIC_IP "$ELASTIC_IP"
log "Elastic IP: $ELASTIC_IP"

log "Associating Elastic IP..."
ASSOC_ID="$(aws ec2 associate-address \
    --region "$REGION" \
    --instance-id "$INSTANCE_ID" \
    --allocation-id "$ALLOC_ID" \
    --query "AssociationId" \
    --output text | strip_cr)"
state_set ASSOC_ID "$ASSOC_ID"

log "Waiting for SSH (max 4 minutes)..."
READY=0
for i in $(seq 1 24); do
    if ssh -i "$KEY_PATH" -o StrictHostKeyChecking=no -o ConnectTimeout=5 -o BatchMode=yes "ubuntu@${ELASTIC_IP}" "echo ok" 2>/dev/null | grep -q ok; then
        READY=1
        break
    fi
    printf "   attempt %s/24\n" "$i"
    sleep 10
done
[[ "$READY" -eq 1 ]] || fail "SSH did not become available in time"

log "Bootstrapping instance..."
ssh -i "$KEY_PATH" -o StrictHostKeyChecking=no "ubuntu@${ELASTIC_IP}" bash << "BOOTSTRAP"
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
sudo apt-get update -qq
sudo apt-get install -y -qq build-essential cmake git libssl-dev rsync ninja-build ca-certificates jq

if ! id -u binance-bot >/dev/null 2>&1; then
    sudo useradd --system --create-home --shell /usr/sbin/nologin binance-bot
fi

sudo mkdir -p /opt/binance-bot
sudo chown binance-bot:binance-bot /opt/binance-bot
mkdir -p "$HOME/binance-bot-build"

if [[ ! -f /swapfile ]]; then
    sudo fallocate -l 2G /swapfile
    sudo chmod 600 /swapfile
    sudo mkswap /swapfile
    sudo swapon /swapfile
    echo "/swapfile none swap sw 0 0" | sudo tee -a /etc/fstab >/dev/null
elif ! swapon --show=NAME | grep -q '^/swapfile$'; then
    sudo swapon /swapfile
fi
BOOTSTRAP

if [[ "$RUN_BOT_ON_EC2" == "1" ]]; then
log "Writing systemd unit..."
ssh -i "$KEY_PATH" -o StrictHostKeyChecking=no "ubuntu@${ELASTIC_IP}" bash << "SERVICE"
set -euo pipefail
sudo tee /etc/systemd/system/binance-bot.service >/dev/null << "EOF_SERVICE"
[Unit]
Description=Binance Trading Bot
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=binance-bot
Group=binance-bot
WorkingDirectory=/opt/binance-bot
EnvironmentFile=/opt/binance-bot/prod.env
ExecStart=/opt/binance-bot/binance_trading_bot
Restart=on-failure
RestartSec=10s
TimeoutStopSec=30s
KillSignal=SIGTERM
StandardOutput=journal
StandardError=journal
SyslogIdentifier=binance-bot
NoNewPrivileges=true
PrivateTmp=true
ProtectHome=true
ProtectSystem=strict
ReadWritePaths=/opt/binance-bot

[Install]
WantedBy=multi-user.target
EOF_SERVICE
sudo systemctl daemon-reload
sudo systemctl enable binance-bot
SERVICE
else
log "Skipping EC2 bot systemd unit because RUN_BOT_ON_EC2=$RUN_BOT_ON_EC2"
ssh -i "$KEY_PATH" -o StrictHostKeyChecking=no "ubuntu@${ELASTIC_IP}" bash << "SERVICE"
set -euo pipefail
sudo systemctl stop binance-bot.service 2>/dev/null || true
sudo systemctl disable binance-bot.service 2>/dev/null || true
if [[ -f /etc/systemd/system/binance-bot.service ]]; then
    sudo mv /etc/systemd/system/binance-bot.service /etc/systemd/system/binance-bot.service.disabled-local-only
fi
sudo systemctl daemon-reload
sudo systemctl mask binance-bot.service 2>/dev/null || true
SERVICE
fi

state_set PROVISION_STATUS "ready"

cat <<DONE

============================================================
PROVISION COMPLETE
Instance ID : $INSTANCE_ID
Elastic IP  : $ELASTIC_IP
SSH Key     : $KEY_PATH

Whitelist this IP on Binance API Management before running bot:
$ELASTIC_IP
============================================================
DONE
