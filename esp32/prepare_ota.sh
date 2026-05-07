#!/bin/bash
# prepare_ota.sh — build and wrap firmware for Z2M OTA deployment
#
# Usage:
#   ./prepare_ota.sh              # build first, then wrap
#   ./prepare_ota.sh --no-build   # skip build, just wrap existing binary

set -euo pipefail

cd "$(dirname "$0")"
REPO_ROOT="$(cd .. && pwd)"
OTA_BIN_DIR="$REPO_ROOT/ota-files"
Z2M_OTA_DIR="$REPO_ROOT/zigbee2mqtt/ota"
Z2M_INDEX="$REPO_ROOT/zigbee2mqtt/ota_index.json"
IMAGE_BUILDER="$HOME/esp/esp-zigbee-sdk/tools/image_builder_tool/image_builder_tool.py"
SRC_BIN="build/water-softener.bin"

# —— parse #defines from main.c ——
parse_define() {
    local name="$1"
    grep -E "^#define[[:space:]]+${name}[[:space:]]+" main/main.c \
        | awk '{print $3}' \
        | tr -d '()UuLl'
}

MANUF_CODE=$(parse_define ESP_MANUF_CODE)
IMAGE_TYPE=$(parse_define ESP_IMAGE_TYPE)
FILE_VER=$(parse_define ESP_OTA_FILE_VERSION)

# —— compute Z2M-visible values ——
# SDK serialization drops the low byte of manufacturerCode:
#   0xBEEF → Z2M reads 0xBE00 = 48640
#   0x0001 → Z2M reads 0x0001 = 1 (passes through)
#   0x01000000 → Z2M reads 0x00000100 = 256
Z2M_MANUF_DEC="$(( ($MANUF_CODE) & 0xFF00 ))"
Z2M_MANUF=$(printf "%04X" "$Z2M_MANUF_DEC")
Z2M_IMAGE_DEC="$(($IMAGE_TYPE))"
Z2M_IMAGE=$(printf "%04X" "$Z2M_IMAGE_DEC")
# Firmware file version: one tick above mangled device version
OTA_FILE_VER_DEC=$(( ($FILE_VER) + 1 ))
OTA_FILE_VER=$(printf "%08X" "$OTA_FILE_VER_DEC")

# —— format for filenames (use Z2M-visible manuf code) ——
MF="$Z2M_MANUF"
IT="$Z2M_IMAGE"
FV=$(printf "%08X" "$FILE_VER")

MANUF_DEC="$Z2M_MANUF_DEC"
IMAGE_DEC="$Z2M_IMAGE_DEC"
FILE_VER_DEC="$((FILE_VER))"

# —— build ——
if [ "${1:-}" != "--no-build" ]; then
    echo "==> Building firmware..."
    idf.py build
fi

if [ ! -f "$SRC_BIN" ]; then
    echo "ERROR: $SRC_BIN not found — build may have failed"
    exit 1
fi

BIN_SIZE=$(wc -c < "$SRC_BIN" | tr -d ' ')

# —— wrap with OTA header using ESP Zigbee SDK tool ——
echo "==> Wrapping firmware with OTA header..."

OTA_FILE="${MF}-${IT}-${FV}.zigbee"

python3 "$IMAGE_BUILDER" \
    -m "$Z2M_MANUF_DEC" \
    -i "$Z2M_IMAGE_DEC" \
    -v "$OTA_FILE_VER_DEC" \
    -g "water-softener" \
    --tag 0x0000 "$BIN_SIZE" "$SRC_BIN"

# The tool outputs a file like BE00-0001-01000001-ota-file.zigbee — rename it
GENERATED="${Z2M_MANUF}-${Z2M_IMAGE}-${OTA_FILE_VER}-ota-file.zigbee"
if [ -f "$GENERATED" ]; then
    mv "$GENERATED" "$OTA_FILE"
fi

# —— copy to output dirs ——
mkdir -p "$OTA_BIN_DIR" "$Z2M_OTA_DIR"
cp "$OTA_FILE" "$OTA_BIN_DIR/$OTA_FILE"
cp "$OTA_FILE" "$Z2M_OTA_DIR/$OTA_FILE"

# —— write ota_index.json ——
cat > "$Z2M_INDEX" << INDEXEOF
[
    {
        "url": "ota/${OTA_FILE}",
        "imageType": ${IMAGE_DEC},
        "manufacturerCode": ${MANUF_DEC},
        "fileVersion": ${OTA_FILE_VER_DEC},
        "force": true
    }
]
INDEXEOF

echo ""
echo "============================================"
echo "  OTA files ready"
echo "============================================"
echo "  Firmware:    $OTA_BIN_DIR/$OTA_FILE"
echo "               $Z2M_OTA_DIR/$OTA_FILE"
echo "  Index:       $Z2M_INDEX"
echo "  Size:        $(wc -c < "$OTA_FILE" | tr -d ' ') bytes (binary: $BIN_SIZE bytes)"
echo "  Manuf Code:  $MF (Z2M sees $MANUF_DEC, device sends $MANUF_CODE)"
echo "  Image Type:  $IT ($IMAGE_DEC)"
echo "  File Ver:    $FV (OTA file ver: $OTA_FILE_VER_DEC)"
echo ""
echo "  Z2M configuration.yaml:"
echo "    ota:"
echo "      zigbee_ota_override_index_location: ota_index.json"
echo "============================================"
