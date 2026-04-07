#!/usr/bin/env bash
set -euo pipefail

BROKER="10.0.1.58"
PORT=1883
BASE_TOPIC="zigbee2mqtt"
CONVERTER_FILE="$(dirname "$0")/water_softener_converter.js"
CONVERTER_NAME="water_softener_converter.js"

if [[ ! -f "$CONVERTER_FILE" ]]; then
    echo "Error: converter file not found: $CONVERTER_FILE" >&2
    exit 1
fi

PAYLOAD=$(jq -n \
    --arg name "$CONVERTER_NAME" \
    --rawfile code "$CONVERTER_FILE" \
    '{"name": $name, "code": $code}')

echo "Publishing $CONVERTER_NAME to $BROKER:$PORT ..."

RESPONSE_FILE=$(mktemp)
trap 'rm -f "$RESPONSE_FILE"' EXIT

# Subscribe for response before publishing to avoid a race condition
mosquitto_sub \
    -h "$BROKER" -p "$PORT" \
    -t "${BASE_TOPIC}/bridge/response/converter/save" \
    -C 1 -W 10 > "$RESPONSE_FILE" &
SUB_PID=$!

mosquitto_pub \
    -h "$BROKER" -p "$PORT" \
    -t "${BASE_TOPIC}/bridge/request/converter/save" \
    -m "$PAYLOAD"

wait $SUB_PID
RESPONSE=$(cat "$RESPONSE_FILE")

if echo "$RESPONSE" | jq -e '.status == "ok"' &>/dev/null; then
    echo "✓ Converter published successfully."
elif [[ -n "$RESPONSE" ]]; then
    echo "✗ Unexpected response: $RESPONSE" >&2
    exit 1
else
    echo "✓ Published (no response received — check Z2M logs)." >&2
fi
