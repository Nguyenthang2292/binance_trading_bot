#!/usr/bin/env bash
# Destroy all EC2 infrastructure. IRREVERSIBLE.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
STATE_FILE="$SCRIPT_DIR/.ec2-state"

if [ ! -f "$STATE_FILE" ]; then
    echo "ERROR: .ec2-state not found. Nothing to tear down."
    exit 1
fi

source "$STATE_FILE"

echo "============================================================"
echo "  TEARDOWN WARNING"
echo "  This will permanently destroy:"
echo "    Instance   : $INSTANCE_ID"
echo "    Elastic IP : $ELASTIC_IP"
echo "    Key Pair   : $KEY_NAME (AWS side)"
echo "    Security Group: $SECURITY_GROUP_ID"
echo ""
echo "  Local key file ($KEY_PATH) will NOT be deleted automatically."
echo "============================================================"
echo ""
printf "Type 'yes' to confirm: "
read -r CONFIRM
if [ "$CONFIRM" != "yes" ]; then
    echo "Aborted."
    exit 0
fi

# Load .env for AWS credentials
if [ -f "$PROJECT_ROOT/.env" ]; then
    set -a; source "$PROJECT_ROOT/.env"; set +a
fi

# Disassociate and release Elastic IP
echo "==> Releasing Elastic IP $ELASTIC_IP..."
ASSOC_ID=$(aws ec2 describe-addresses \
    --region "$REGION" \
    --allocation-ids "$ALLOC_ID" \
    --query "Addresses[0].AssociationId" \
    --output text 2>/dev/null || echo "")

if [ -n "$ASSOC_ID" ] && [ "$ASSOC_ID" != "None" ]; then
    aws ec2 disassociate-address \
        --region "$REGION" \
        --association-id "$ASSOC_ID" \
        --output text > /dev/null
    echo "    Disassociated."
fi

aws ec2 release-address \
    --region "$REGION" \
    --allocation-id "$ALLOC_ID" \
    --output text > /dev/null
echo "    Elastic IP released."

# Terminate EC2 instance
echo "==> Terminating instance $INSTANCE_ID..."
aws ec2 terminate-instances \
    --region "$REGION" \
    --instance-ids "$INSTANCE_ID" \
    --output text > /dev/null
echo "    Termination initiated."

echo "==> Waiting for instance to be terminated (this takes ~1 minute)..."
aws ec2 wait instance-terminated --region "$REGION" --instance-ids "$INSTANCE_ID"
echo "    Terminated."

# Delete Security Group
echo "==> Deleting security group $SECURITY_GROUP_ID..."
aws ec2 delete-security-group \
    --region "$REGION" \
    --group-id "$SECURITY_GROUP_ID" \
    --output text > /dev/null
echo "    Deleted."

# Delete Key Pair (AWS side)
echo "==> Deleting key pair '$KEY_NAME' from AWS..."
aws ec2 delete-key-pair \
    --region "$REGION" \
    --key-name "$KEY_NAME" \
    --output text > /dev/null
echo "    Deleted."

# Remove state file
rm "$STATE_FILE"
echo "    .ec2-state removed."

echo ""
echo "============================================================"
echo "  TEARDOWN COMPLETE"
echo ""
echo "  Local key file still exists at: $KEY_PATH"
echo "  Remove manually if no longer needed:"
echo "  rm $KEY_PATH"
echo "============================================================"
