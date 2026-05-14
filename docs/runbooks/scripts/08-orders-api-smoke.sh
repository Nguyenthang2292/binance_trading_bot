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

REMOTE_ENV="/tmp/binance-orders-smoke.env"
REMOTE_SCRIPT="/tmp/binance-orders-smoke.sh"

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
# Normalize CRLF without modifying the uploaded secret file.
# shellcheck disable=SC1090
source <(sed 's/\r$//' "${ENV_FILE}")
set +a

API_KEY="${BINANCE_API_KEY:-${API_KEY:-}}"
API_SECRET="${BINANCE_SECRET_KEY:-${API_SECRET:-}}"
BASE_URL="${BINANCE_BASE_URL:-https://fapi.binance.com}"
SYMBOL="${ORDERS_SMOKE_SYMBOL:-BTCUSDT}"

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
run_id="$(date +%s)"

timestamp_ms() {
  echo "$(($(date +%s%3N) + time_offset))"
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

extract_json_code() {
  local body="$1"
  printf '%s' "${body}" | sed -n 's/.*"code"[[:space:]]*:[[:space:]]*\(-\{0,1\}[0-9][0-9]*\).*/\1/p'
}

print_result() {
  local status="$1"
  local name="$2"
  local method="$3"
  local path="$4"
  local http_code="$5"
  local code="$6"
  local msg="$7"
  printf '%-14s %-34s %-6s %-28s http=%s' "${status}" "${name}" "${method}" "${path}" "${http_code}"
  if [[ -n "${code}" ]]; then
    printf ' binance_code=%s' "${code}"
  fi
  if [[ -n "${msg}" ]]; then
    printf ' msg="%s"' "${msg}"
  fi
  printf '\n'
}

call_signed() {
  local name="$1"
  local method="$2"
  local path="$3"
  local params="$4"
  local expectation="$5"

  local query="${params}&timestamp=$(timestamp_ms)&recvWindow=5000"
  local signature
  signature="$(sign_query "${query}")"

  local response
  response="$(curl -sS -X "${method}" -H "X-MBX-APIKEY: ${API_KEY}" -w $'\nHTTP_STATUS:%{http_code}' "${BASE_URL}${path}?${query}&signature=${signature}")"
  local body="${response%$'\n'HTTP_STATUS:*}"
  local http_code="${response##*HTTP_STATUS:}"
  local binance_code msg verdict
  binance_code="$(extract_json_code "${body}")"
  msg="$(extract_json_field "${body}" "msg")"

  case "${expectation}" in
    ok)
      if [[ "${http_code}" =~ ^2 ]]; then verdict="PASS"; else verdict="FAIL"; fi
      ;;
    not_found_ok)
      if [[ "${http_code}" =~ ^2 || "${binance_code}" == "-2011" || "${binance_code}" == "-2013" ]]; then
        verdict="PASS_EXPECTED"
      else
        verdict="FAIL"
      fi
      ;;
    *)
      verdict="FAIL"
      ;;
  esac

  print_result "${verdict}" "${name}" "${method}" "${path}" "${http_code}" "${binance_code}" "${msg}"
}

print_skip() {
  local name="$1"
  local method="$2"
  local path="$3"
  local reason="$4"
  printf '%-14s %-34s %-6s %-28s %s\n' "SKIP_RISK" "${name}" "${method}" "${path}" "${reason}"
}

echo "Binance Futures orders API smoke test"
echo "base_url=${BASE_URL}"
echo "symbol=${SYMBOL}"
echo

print_result "PASS" "publicServerTime" "GET" "/fapi/v1/time" "200" "" ""

print_skip "market" "POST" "/fapi/v1/order" "real MARKET order risk"
print_skip "limit" "POST" "/fapi/v1/order" "real LIMIT order risk"
print_skip "closeByMarket" "POST" "/fapi/v1/order" "real reduceOnly MARKET order risk"
print_skip "batchNormal" "POST" "/fapi/v1/batchOrders" "real batch order risk"
print_skip "amendLimitOrder" "PUT" "/fapi/v1/order" "requires existing order and mutates it"

call_signed "cancelNormalByClientOrderId" "DELETE" "/fapi/v1/order" "symbol=${SYMBOL}&origClientOrderId=codex-smoke-nonexistent-${run_id}" "not_found_ok"
print_skip "cancelNormalByOrderId" "DELETE" "/fapi/v1/order" "orderId could match a real open order"
print_skip "cancelAllNormal" "DELETE" "/fapi/v1/allOpenOrders" "cancels all real open orders for symbol"

call_signed "queryNormalByOrderId" "GET" "/fapi/v1/order" "symbol=${SYMBOL}&orderId=1" "not_found_ok"
call_signed "queryNormalByClientOrderId" "GET" "/fapi/v1/order" "symbol=${SYMBOL}&origClientOrderId=codex-smoke-nonexistent-${run_id}" "not_found_ok"
call_signed "openNormalOrders" "GET" "/fapi/v1/openOrders" "symbol=${SYMBOL}" "ok"
call_signed "queryAllNormal" "GET" "/fapi/v1/allOrders" "symbol=${SYMBOL}&limit=1" "ok"
call_signed "queryOrderFillSummary" "GET" "/fapi/v1/userTrades" "symbol=${SYMBOL}&limit=1" "ok"

print_skip "stopEntry" "POST" "/fapi/v1/order" "real STOP entry order risk"
print_skip "protection" "POST" "/fapi/v1/order" "real protection order risk"
print_skip "cancelAlgoByAlgoId" "DELETE" "n/a" "spec does not define REST endpoint in this file"
print_skip "cancelAlgoByClientAlgoId" "DELETE" "n/a" "spec does not define REST endpoint in this file"
print_skip "queryAlgoByAlgoId" "GET" "n/a" "spec does not define REST endpoint in this file"
print_skip "queryAlgoByClientAlgoId" "GET" "n/a" "spec does not define REST endpoint in this file"
REMOTE

echo "Running smoke test on EC2..."
ssh "${SSH_OPTS[@]}" "ubuntu@${ELASTIC_IP}" "${REMOTE_SCRIPT}" "${REMOTE_ENV}"

cleanup_remote
if ssh "${SSH_OPTS[@]}" "ubuntu@${ELASTIC_IP}" test ! -e "${REMOTE_ENV}" -a ! -e "${REMOTE_SCRIPT}" >/dev/null 2>&1; then
  echo "Remote temporary files cleaned."
else
  echo "WARNING: remote temporary files were not fully removed." >&2
fi
trap - EXIT
