#!/usr/bin/env bash
# Destroy EC2 infrastructure created by infra/provision.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STATE_FILE="$SCRIPT_DIR/.ec2-state"
AWS_ENV_FILE="$SCRIPT_DIR/aws.env.local"
APP_NAME="binance-bot"

log() { printf "==> %s\n" "$*"; }
warn() { printf "WARNING: %s\n" "$*"; }
fail() { printf "ERROR: %s\n" "$*" >&2; exit 1; }
strip_cr() { tr -d "\r"; }

if [[ -f "$AWS_ENV_FILE" ]]; then
    set -a
    # shellcheck disable=SC1090
    source "$AWS_ENV_FILE"
    set +a
fi

REGION="${AWS_REGION:-${REGION:-ap-southeast-1}}"
INSTANCE_ID="${INSTANCE_ID:-}"
ALLOC_ID="${ALLOC_ID:-}"
ELASTIC_IP="${ELASTIC_IP:-}"
SECURITY_GROUP_ID="${SECURITY_GROUP_ID:-}"
KEY_NAME="${KEY_NAME:-binance-bot-key}"
KEY_PATH="${KEY_PATH:-$HOME/.ssh/binance-bot-key.pem}"

if [[ -f "$STATE_FILE" ]]; then
    # shellcheck disable=SC1090
    source "$STATE_FILE"
fi

log "Region: $REGION"
aws sts get-caller-identity --output text >/dev/null || fail "AWS auth failed"

echo "============================================================"
echo "TEARDOWN WARNING"
echo "This will destroy EC2 resources tagged with App=$APP_NAME."
echo "State file: $STATE_FILE"
echo "============================================================"
if [[ "${FORCE_TEARDOWN:-}" == "yes" ]]; then
    log "FORCE_TEARDOWN=yes set, skipping interactive confirmation."
else
    printf "Type 'yes' to confirm: "
    read -r CONFIRM
    [[ "$CONFIRM" == "yes" ]] || { echo "Aborted."; exit 0; }
fi

discover_from_tags() {
    if [[ -z "${INSTANCE_ID:-}" ]]; then
        INSTANCE_ID="$(aws ec2 describe-instances \
            --region "$REGION" \
            --filters "Name=tag:App,Values=$APP_NAME" "Name=instance-state-name,Values=pending,running,stopping,stopped" \
            --query "Reservations[].Instances[].InstanceId" \
            --output text | strip_cr | awk "{print \$1}")"
    fi

    if [[ -z "${ALLOC_ID:-}" ]]; then
        ALLOC_ID="$(aws ec2 describe-addresses \
            --region "$REGION" \
            --filters "Name=tag:App,Values=$APP_NAME" \
            --query "Addresses[0].AllocationId" \
            --output text 2>/dev/null | strip_cr || true)"
        [[ "$ALLOC_ID" == "None" ]] && ALLOC_ID=""
    fi

    if [[ -z "${SECURITY_GROUP_ID:-}" ]]; then
        SECURITY_GROUP_ID="$(aws ec2 describe-security-groups \
            --region "$REGION" \
            --filters "Name=tag:App,Values=$APP_NAME" \
            --query "SecurityGroups[0].GroupId" \
            --output text 2>/dev/null | strip_cr || true)"
        [[ "$SECURITY_GROUP_ID" == "None" ]] && SECURITY_GROUP_ID=""
    fi
}

discover_from_tags

if [[ -n "${ALLOC_ID:-}" ]]; then
    log "Releasing Elastic IP allocation $ALLOC_ID..."
    ASSOC_ID="$(aws ec2 describe-addresses --region "$REGION" --allocation-ids "$ALLOC_ID" --query "Addresses[0].AssociationId" --output text 2>/dev/null | strip_cr || true)"
    if [[ -n "$ASSOC_ID" && "$ASSOC_ID" != "None" ]]; then
        aws ec2 disassociate-address --region "$REGION" --association-id "$ASSOC_ID" >/dev/null || warn "Disassociate failed"
    fi
    aws ec2 release-address --region "$REGION" --allocation-id "$ALLOC_ID" >/dev/null || warn "Release address failed"
else
    warn "No Elastic IP allocation found"
fi

if [[ -n "${INSTANCE_ID:-}" ]]; then
    log "Terminating instance $INSTANCE_ID..."
    aws ec2 terminate-instances --region "$REGION" --instance-ids "$INSTANCE_ID" >/dev/null || warn "Terminate call failed"
    aws ec2 wait instance-terminated --region "$REGION" --instance-ids "$INSTANCE_ID" || warn "Wait terminate failed"
else
    warn "No instance found"
fi

if [[ -n "${SECURITY_GROUP_ID:-}" ]]; then
    log "Deleting security group $SECURITY_GROUP_ID..."
    aws ec2 delete-security-group --region "$REGION" --group-id "$SECURITY_GROUP_ID" >/dev/null || warn "Delete SG failed"
else
    warn "No security group found"
fi

log "Deleting key pair '$KEY_NAME' from AWS..."
aws ec2 delete-key-pair --region "$REGION" --key-name "$KEY_NAME" >/dev/null || warn "Delete key pair failed"

if [[ -f "$STATE_FILE" ]]; then
    rm -f "$STATE_FILE"
fi

echo
echo "============================================================"
echo "TEARDOWN COMPLETE"
echo "Local private key (not removed): $KEY_PATH"
echo "============================================================"
