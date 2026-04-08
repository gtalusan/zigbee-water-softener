#!/usr/bin/env bash
set -euo pipefail

BROKER="10.0.1.58"
PORT=1883
BASE_TOPIC="zigbee2mqtt"
CONVERTER_NAME="water_softener_converter.js"

PAYLOAD=$(jq -n --arg name "$CONVERTER_NAME" '{"name": $name}')

echo "Unpublishing $CONVERTER_NAME from $BROKER:$PORT ..."

RESPONSE_FILE=$(mktemp)
trap 'rm -f "$RESPONSE_FILE"' EXIT

# Subscribe for response before publishing to avoid a race condition
mosquitto_sub \
    -h "$BROKER" -p "$PORT" \
    -t "${BASE_TOPIC}/bridge/response/converter/remove" \
    -C 1 -W 10 > "$RESPONSE_FILE" &
SUB_PID=$!

mosquitto_pub \
    -h "$BROKER" -p "$PORT" \
    -t "${BASE_TOPIC}/bridge/request/converter/remove" \
    -m "$PAYLOAD"

wait $SUB_PID
RESPONSE=$(cat "$RESPONSE_FILE")

if echo "$RESPONSE" | jq -e '.status == "ok"' &>/dev/null; then
    echo "✓ Converter unpublished successfully."
elif [[ -n "$RESPONSE" ]]; then
    echo "✗ Unexpected response: $RESPONSE" >&2
    exit 1
else
    echo "✓ Unpublished (no response received — check Z2M logs)." >&2
fi
