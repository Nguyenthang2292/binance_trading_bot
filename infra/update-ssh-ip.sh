#!/usr/bin/env bash
# Update SG SSH ingress to current public IP.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STATE_FILE="$SCRIPT_DIR/.ec2-state"
AWS_ENV_FILE="$SCRIPT_DIR/aws.env.local"

fail() { printf "ERROR: %s\n" "$*" >&2; exit 1; }
log() { printf "==> %s\n" "$*"; }
strip_cr() { tr -d "\r"; }

[[ -f "$STATE_FILE" ]] || fail ".ec2-state not found. Run provision.sh first."

if [[ -f "$AWS_ENV_FILE" ]]; then
    set -a
    # shellcheck disable=SC1090
    source "$AWS_ENV_FILE"
    set +a
fi

# shellcheck disable=SC1090
source "$STATE_FILE"

[[ -n "${SECURITY_GROUP_ID:-}" ]] || fail "SECURITY_GROUP_ID missing in .ec2-state"
[[ -n "${REGION:-}" ]] || REGION="${AWS_REGION:-ap-southeast-1}"

NEW_IP="$( (curl -s --max-time 10 ifconfig.me || curl -s --max-time 10 api.ipify.org || true) | strip_cr)"
[[ -n "$NEW_IP" ]] || fail "Could not detect current public IP"
NEW_CIDR="${NEW_IP}/32"

log "Reading current SSH rules from SG $SECURITY_GROUP_ID..."
CURRENT_CIDRS_RAW="$(aws ec2 describe-security-groups \
    --region "$REGION" \
    --group-ids "$SECURITY_GROUP_ID" \
    --query "SecurityGroups[0].IpPermissions[?IpProtocol=='tcp' && FromPort==\`22\` && ToPort==\`22\`].IpRanges[].CidrIp" \
    --output text | strip_cr)"

if [[ -n "$CURRENT_CIDRS_RAW" ]]; then
    for CIDR in $CURRENT_CIDRS_RAW; do
        if [[ "$CIDR" != "$NEW_CIDR" ]]; then
            log "Revoking old SSH CIDR: $CIDR"
            aws ec2 revoke-security-group-ingress \
                --region "$REGION" \
                --group-id "$SECURITY_GROUP_ID" \
                --protocol tcp \
                --port 22 \
                --cidr "$CIDR" >/dev/null || true
        fi
    done
fi

if [[ "$CURRENT_CIDRS_RAW" == *"$NEW_CIDR"* ]]; then
    log "SSH rule already up to date: $NEW_CIDR"
else
    log "Authorizing SSH CIDR: $NEW_CIDR"
    aws ec2 authorize-security-group-ingress \
        --region "$REGION" \
        --group-id "$SECURITY_GROUP_ID" \
        --protocol tcp \
        --port 22 \
        --cidr "$NEW_CIDR" >/dev/null
fi

if grep -q "^MY_IP_AT_PROVISION=" "$STATE_FILE"; then
    sed -i "s#^MY_IP_AT_PROVISION=.*#MY_IP_AT_PROVISION=${NEW_IP}#" "$STATE_FILE"
else
    printf "MY_IP_AT_PROVISION=%s\n" "$NEW_IP" >> "$STATE_FILE"
fi

echo
echo "SSH inbound is now restricted to $NEW_CIDR"
