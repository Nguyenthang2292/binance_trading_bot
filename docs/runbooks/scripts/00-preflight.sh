#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT_DIR"

echo "==> Preflight checks"

for cmd in aws ssh scp rsync cmake; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "ERROR: missing command: $cmd"
        exit 1
    fi
done

if [[ ! -f infra/aws.env.local ]]; then
    echo "ERROR: infra/aws.env.local not found"
    echo "Copy infra/aws.env.local.example and fill AWS credentials"
    exit 1
fi

if [[ ! -f infra/prod.env ]]; then
    echo "ERROR: infra/prod.env not found"
    echo "Copy infra/prod.env.example and fill Binance credentials"
    exit 1
fi

if grep -n "^[[:space:]]*AWS_" infra/prod.env >/dev/null; then
    echo "ERROR: infra/prod.env contains AWS_* variables"
    exit 1
fi

echo "OK: preflight passed"
