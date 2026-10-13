#!/usr/bin/env bash
# Registers a handful of demo devices and one automation rule against a running
# hub, so the WebUI has something to show. Safe to re-run: already-registered
# devices come back 409 and are reported as "exists".
#
#   ./scripts/seed-demo.sh                        # against localhost:8080
#   BASE_URL=http://host:8080/api/v1 ./scripts/seed-demo.sh
set -euo pipefail

BASE_URL="${BASE_URL:-http://localhost:8080/api/v1}"
HOUSEHOLD="${HOUSEHOLD:-00000000-0000-0000-0000-0000000000a1}"

if ! curl -sf "${BASE_URL%/api/v1}/healthz" >/dev/null; then
  echo "error: no hub answering at ${BASE_URL%/api/v1}/healthz" >&2
  echo "       start the stack first: docker compose up" >&2
  exit 1
fi

# post <path> <json> <label>
post() {
  local status
  status=$(curl -s -o /dev/null -w '%{http_code}' -X POST "${BASE_URL}$1" \
    -H 'Content-Type: application/json' -d "$2")
  case "${status}" in
    20*) printf '  %-24s created\n' "$3" ;;
    409) printf '  %-24s exists\n' "$3" ;;
    *) printf '  %-24s FAILED (HTTP %s)\n' "$3" "${status}" ;;
  esac
}

echo "Seeding demo data into ${BASE_URL}"

# A device must be registered as ONLINE to accept commands: there is no real
# hardware behind these, and the hub refuses commands to an unreachable device.
post /devices "{
  \"id\": \"11111111-0000-4000-8000-000000000001\",
  \"householdId\": \"${HOUSEHOLD}\",
  \"name\": \"Kitchen Ceiling\",
  \"ipAddress\": \"192.168.1.40\",
  \"type\": \"light\",
  \"status\": \"ONLINE\",
  \"attributes\": {\"brightness\": 80, \"color\": \"#ffaa00\"}
}" "light"

post /devices "{
  \"id\": \"22222222-0000-4000-8000-000000000002\",
  \"householdId\": \"${HOUSEHOLD}\",
  \"name\": \"Living Room Speaker\",
  \"ipAddress\": \"192.168.1.50\",
  \"type\": \"speaker\",
  \"status\": \"ONLINE\",
  \"attributes\": {\"connected\": true, \"volume\": 25}
}" "speaker"

post /devices "{
  \"id\": \"33333333-0000-4000-8000-000000000003\",
  \"householdId\": \"${HOUSEHOLD}\",
  \"name\": \"Garage Relay\",
  \"ipAddress\": \"192.168.1.60\",
  \"type\": \"generic\",
  \"status\": \"ONLINE\"
}" "generic device"

# Left OFFLINE on purpose: shows the disabled-controls path in the WebUI.
post /devices "{
  \"id\": \"44444444-0000-4000-8000-000000000004\",
  \"householdId\": \"${HOUSEHOLD}\",
  \"name\": \"Porch Light (offline)\",
  \"ipAddress\": \"192.168.1.70\",
  \"type\": \"light\",
  \"status\": \"OFFLINE\"
}" "offline light"

post /rules "{
  \"id\": \"55555555-0000-4000-8000-000000000005\",
  \"name\": \"Lights out at 22:00\",
  \"enabled\": true,
  \"triggerType\": \"SCHEDULE\",
  \"trigger\": {\"cron\": \"0 22 * * *\"},
  \"actions\": [
    {\"deviceId\": \"11111111-0000-4000-8000-000000000001\", \"command\": \"turnOff\", \"params\": {}}
  ]
}" "automation rule"

echo
echo "Done. Open the WebUI, or inspect the payload:"
echo "  curl -s ${BASE_URL}/dashboard | python3 -m json.tool"
