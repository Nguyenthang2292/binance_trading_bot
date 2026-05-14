#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT_DIR"

if [[ ! -f infra/.ec2-state ]]; then
    echo "ERROR: infra/.ec2-state not found"
    exit 1
fi

set -a
# shellcheck disable=SC1091
source infra/aws.env.local
# shellcheck disable=SC1091
source infra/.ec2-state
set +a

echo "==> EC2 instance state"
aws ec2 describe-instances \
    --region "$REGION" \
    --instance-ids "$INSTANCE_ID" \
    --query "Reservations[0].Instances[0].{State:State.Name,PublicIp:PublicIpAddress,LaunchTime:LaunchTime,Type:InstanceType}" \
    --output table

echo
echo "Elastic IP for Binance whitelist: $ELASTIC_IP"
