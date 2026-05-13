#!/usr/bin/env bash
# Update Security Group SSH inbound rule when local IP changes.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STATE_FILE="$SCRIPT_DIR/.ec2-state"

if [ ! -f "$STATE_FILE" ]; then
    echo "ERROR: .ec2-state not found. Run provision.sh first."
    exit 1
fi

source "$STATE_FILE"

# Get current allowed IP from Security Group
echo "==> Reading current SSH rule from Security Group $SECURITY_GROUP_ID..."
OLD_CIDR=$(aws ec2 describe-security-groups \
    --region "$REGION" \
    --group-ids "$SECURITY_GROUP_ID" \
    --query "SecurityGroups[0].IpPermissions[?FromPort==\`22\`].IpRanges[0].CidrIp" \
    --output text)

if [ -z "$OLD_CIDR" ] || [ "$OLD_CIDR" = "None" ]; then
    echo "ERROR: No SSH inbound rule found. Was it manually removed?"
    exit 1
fi
echo "    Current allowed IP: $OLD_CIDR"

# Get new local IP
echo "==> Detecting new local IP..."
NEW_IP=$(curl -s --max-time 10 ifconfig.me || curl -s --max-time 10 api.ipify.org)
if [ -z "$NEW_IP" ]; then
    echo "ERROR: Could not detect local IP."
    exit 1
fi
NEW_CIDR="${NEW_IP}/32"
echo "    New local IP: $NEW_CIDR"

if [ "$OLD_CIDR" = "$NEW_CIDR" ]; then
    echo "==> IP unchanged. Nothing to do."
    exit 0
fi

# Revoke old rule
echo "==> Revoking old rule: $OLD_CIDR..."
aws ec2 revoke-security-group-ingress \
    --region "$REGION" \
    --group-id "$SECURITY_GROUP_ID" \
    --protocol tcp \
    --port 22 \
    --cidr "$OLD_CIDR" \
    --output text > /dev/null
echo "    Revoked."

# Authorize new rule
echo "==> Authorizing new rule: $NEW_CIDR..."
aws ec2 authorize-security-group-ingress \
    --region "$REGION" \
    --group-id "$SECURITY_GROUP_ID" \
    --protocol tcp \
    --port 22 \
    --cidr "$NEW_CIDR" \
    --output text > /dev/null
echo "    Authorized."

# Update state file with new IP
sed -i "s/MY_IP_AT_PROVISION=.*/MY_IP_AT_PROVISION=${NEW_IP}/" "$STATE_FILE"

echo ""
echo "==> SSH IP updated: $OLD_CIDR → $NEW_CIDR"
echo "    You can now SSH and run ssh-proxy.sh as normal."
