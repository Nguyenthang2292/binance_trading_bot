#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
STATE_FILE="${REPO_ROOT}/infra/.ec2-state"
LOCAL_ENV_FILE="${REPO_ROOT}/infra/prod.env"

if [[ ! -f "${STATE_FILE}" ]]; then
  echo "ERROR: missing ${STATE_FILE}" >&2
  exit 1
fi

if [[ ! -f "${LOCAL_ENV_FILE}" ]]; then
  echo "ERROR: missing ${LOCAL_ENV_FILE}" >&2
  exit 1
fi

# shellcheck disable=SC1090
source "${STATE_FILE}"

if [[ -z "${ELASTIC_IP:-}" || -z "${KEY_PATH:-}" ]]; then
  echo "ERROR: infra/.ec2-state must define ELASTIC_IP and KEY_PATH" >&2
  exit 1
fi

REMOTE_ENV="/tmp/binance-account-smoke.env"
REMOTE_SCRIPT="/tmp/binance-account-smoke.sh"

SSH_OPTS=(
  -i "${KEY_PATH}"
  -o StrictHostKeyChecking=no
  -o UserKnownHostsFile=/dev/null
  -o ConnectTimeout=30
)

cleanup_remote() {
  ssh "${SSH_OPTS[@]}" "ubuntu@${ELASTIC_IP}" rm -f "${REMOTE_ENV}" "${REMOTE_SCRIPT}" >/dev/null 2>&1 || true
}
trap cleanup_remote EXIT

echo "Uploading temporary env to EC2 ${ELASTIC_IP}..."
scp "${SSH_OPTS[@]}" "${LOCAL_ENV_FILE}" "ubuntu@${ELASTIC_IP}:${REMOTE_ENV}" >/dev/null

ssh "${SSH_OPTS[@]}" "ubuntu@${ELASTIC_IP}" "cat > '${REMOTE_SCRIPT}' && chmod 700 '${REMOTE_SCRIPT}'" <<'REMOTE'
#!/usr/bin/env bash
set -euo pipefail

ENV_FILE="${1:?env file required}"
set -a
# shellcheck disable=SC1090
source <(sed 's/\r$//' "${ENV_FILE}")
set +a

API_KEY="${BINANCE_API_KEY:-${API_KEY:-}}"
API_SECRET="${BINANCE_SECRET_KEY:-${API_SECRET:-}}"
BASE_URL="${BINANCE_BASE_URL:-https://fapi.binance.com}"

if [[ -z "${API_KEY}" || -z "${API_SECRET}" ]]; then
  echo "ERROR: env must define BINANCE_API_KEY/BINANCE_SECRET_KEY or API_KEY/API_SECRET" >&2
  exit 1
fi

command -v curl >/dev/null || { echo "ERROR: curl not found" >&2; exit 1; }
command -v openssl >/dev/null || { echo "ERROR: openssl not found" >&2; exit 1; }

server_json="$(curl -sS "${BASE_URL}/fapi/v1/time")"
server_time="$(printf '%s' "${server_json}" | sed -n 's/.*"serverTime"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p')"
if [[ -z "${server_time}" ]]; then
  echo "ERROR: cannot read Binance futures server time: ${server_json}" >&2
  exit 1
fi

local_time="$(date +%s%3N)"
time_offset="$((server_time - local_time))"

timestamp_ms() {
  echo "$(( $(date +%s%3N) + time_offset ))"
}

sign_query() {
  local query="$1"
  printf '%s' "${query}" | openssl dgst -sha256 -hmac "${API_SECRET}" | sed 's/^.* //'
}

extract_json_field() {
  local body="$1"
  local field="$2"
  printf '%s' "${body}" | sed -n "s/.*\"${field}\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p"
}

extract_json_bool() {
  local body="$1"
  local field="$2"
  printf '%s' "${body}" | sed -n "s/.*\"${field}\"[[:space:]]*:[[:space:]]*\\(true\\|false\\).*/\\1/p"
}

extract_json_code() {
  local body="$1"
  printf '%s' "${body}" | sed -n 's/.*"code"[[:space:]]*:[[:space:]]*\(-\{0,1\}[0-9][0-9]*\).*/\1/p'
}

count_token() {
  local body="$1"
  local token="$2"
  printf '%s' "${body}" | grep -o "\"${token}\"" | wc -l | tr -d ' '
}

call_signed() {
  local name="$1"
  local path="$2"
  local params="$3"

  local query
  query="${params}timestamp=$(timestamp_ms)&recvWindow=5000"
  local signature
  signature="$(sign_query "${query}")"

  local response
  response="$(curl -sS -H "X-MBX-APIKEY: ${API_KEY}" -w $'\nHTTP_STATUS:%{http_code}' "${BASE_URL}${path}?${query}&signature=${signature}")"
  local body="${response%$'\n'HTTP_STATUS:*}"
  local http_code="${response##*HTTP_STATUS:}"
  local binance_code msg verdict
  binance_code="$(extract_json_code "${body}")"
  msg="$(extract_json_field "${body}" "msg")"

  verdict="FAIL"
  if [[ "${http_code}" =~ ^2 ]]; then
    verdict="PASS"
  fi

  printf '%-6s %-16s %-24s http=%s' "${verdict}" "${name}" "${path}" "${http_code}"
  if [[ -n "${binance_code}" ]]; then
    printf ' binance_code=%s' "${binance_code}"
  fi
  if [[ -n "${msg}" ]]; then
    printf ' msg="%s"' "${msg}"
  fi
  printf '\n'

  if [[ "${name}" == "account" && "${verdict}" == "PASS" ]]; then
    local can_trade total_wallet available_balance assets_count positions_count
    can_trade="$(extract_json_bool "${body}" "canTrade")"
    total_wallet="$(extract_json_field "${body}" "totalWalletBalance")"
    available_balance="$(extract_json_field "${body}" "availableBalance")"
    assets_count="$(count_token "${body}" "asset")"
    positions_count="$(count_token "${body}" "symbol")"
    echo "       canTrade=${can_trade} totalWalletBalance=${total_wallet} availableBalance=${available_balance} assets=${assets_count} positions=${positions_count}"
  fi

  if [[ "${name}" == "balance" && "${verdict}" == "PASS" ]]; then
    local rows
    rows="$(count_token "${body}" "asset")"
    echo "       balance_rows=${rows}"
  fi

  if [[ "${name}" == "positionRisk" && "${verdict}" == "PASS" ]]; then
    local rows
    rows="$(count_token "${body}" "symbol")"
    echo "       position_rows=${rows}"
  fi
}

echo "Binance Futures account API smoke test"
echo "base_url=${BASE_URL}"
echo
call_signed "account" "/fapi/v2/account" ""
call_signed "balance" "/fapi/v2/balance" ""
call_signed "positionRisk" "/fapi/v2/positionRisk" ""
REMOTE

echo "Running account smoke test on EC2..."
ssh "${SSH_OPTS[@]}" "ubuntu@${ELASTIC_IP}" "${REMOTE_SCRIPT}" "${REMOTE_ENV}"

cleanup_remote
if ssh "${SSH_OPTS[@]}" "ubuntu@${ELASTIC_IP}" test ! -e "${REMOTE_ENV}" -a ! -e "${REMOTE_SCRIPT}" >/dev/null 2>&1; then
  echo "Remote temporary files cleaned."
else
  echo "WARNING: remote temporary files were not fully removed." >&2
fi
trap - EXIT

