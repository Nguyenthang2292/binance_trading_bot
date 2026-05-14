#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT_DIR"

set -a
# shellcheck disable=SC1091
source infra/aws.env.local
set +a

echo "==> AWS identity"
aws sts get-caller-identity
