# Water Softener Salt Level Detector

ESP32-C6 SuperMini + VL53L0X time-of-flight sensor that measures the distance
to the salt surface inside a brine tank and reports it via Zigbee every 12 hours.
Battery level is reported alongside the distance.

## Hardware

| Component | Details |
|-----------|---------|
| MCU | MakerGO ESP32-C6 SuperMini |
| Sensor | VL53L0X ToF distance sensor (I2C) |
| Power | 4.2V Li-Ion battery |
| Mounting | Underside of brine tank lid |

### Wiring

| Signal | ESP32-C6 Pin |
|--------|-------------|
| I2C SDA | GP0 |
| I2C SCL | GP1 |
| VL53L0X XSHUT | GP2 |
| Battery ADC | GP6 (via 1MΩ + 1MΩ voltage divider) |
| Credentials clear | GP9 (BOOT button) |

The voltage divider uses two equal 1 MΩ resistors: `Vbat → R1 → GP6 → R2 → GND`.
This halves the battery voltage so it stays within the ESP32's ADC input range.

## Files

```
water_softener/
└── water_softener.ino      Arduino sketch
water_softener_converter.js  Zigbee2MQTT external converter
```

---

## Build & Flash

### Prerequisites

```bash
# Install arduino-cli (macOS)
brew install arduino-cli

# Add the Espressif board index
arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json

# Install the ESP32 core (includes Zigbee library)
arduino-cli core update-index
arduino-cli core install esp32:esp32

# Install the VL53L0X library
arduino-cli lib install "VL53L0X"
```

### Compile

```bash
arduino-cli compile \
  --fqbn "esp32:esp32:makergo_c6_supermini:ZigbeeMode=ed,PartitionScheme=zigbee" \
  water_softener
```

Expected output: ~49% flash, ~10% RAM used.

### Find the device port

Plug in the ESP32-C6 via USB, then:

```bash
arduino-cli board list
```

Look for a USB serial port, e.g. `/dev/cu.usbserial-XXXX` (macOS) or `/dev/ttyUSB0` (Linux).

### Upload

```bash
arduino-cli upload \
  --fqbn "esp32:esp32:makergo_c6_supermini:ZigbeeMode=ed,PartitionScheme=zigbee" \
  --port /dev/cu.usbserial-XXXX \
  water_softener
```

Replace `/dev/cu.usbserial-XXXX` with the port from the previous step.

### Monitor serial output

```bash
arduino-cli monitor --port /dev/cu.usbserial-XXXX --config baudrate=115200
```

Press `Ctrl+C` to exit the monitor.

---

## Zigbee2MQTT Converter

### Install

Copy `water_softener_converter.js` to your Zigbee2MQTT `external_converters` directory,
then add it to `configuration.yaml`:

```yaml
external_converters:
  - water_softener_converter.js
```

Restart Zigbee2MQTT.

### Pairing

On first power-on (or after a credential clear), the device enters commissioning mode
and will appear in Zigbee2MQTT as `WaterSoftener-v1` once joined.

### Published values

| Key | Type | Description |
|-----|------|-------------|
| `distance` | number (cm) | Distance from sensor to salt surface |
| `battery` | number (%) | Estimated battery charge |

---

## Credential Reset

To re-pair the device with a different Zigbee network:

1. Hold the **BOOT button** (GPIO9) while applying power (or pressing RST)
2. Keep it held until the serial monitor shows `[ZB] Credentials erased`
3. Release — the device will start commissioning immediately

---

## Configuration

All tunable values are `#define` constants at the top of `water_softener.ino`:

| Constant | Default | Description |
|----------|---------|-------------|
| `PIN_SDA` | `0` | I2C SDA pin |
| `PIN_SCL` | `1` | I2C SCL pin |
| `PIN_VL53_XSHUT` | `2` | VL53L0X power control pin |
| `PIN_BATT_ADC` | `6` | Battery ADC pin |
| `PIN_CLEAR_CREDS` | `9` | Credential-clear button pin |
| `SLEEP_INTERVAL_US` | 12 hours | Deep sleep duration |
| `BATT_DIVIDER_RATIO` | `2.0` | ADC multiplier (matches resistor ratio) |
| `BATT_MAX_MV` | `4200` | Battery voltage at 100% (mV) |
| `BATT_MIN_MV` | `3000` | Battery voltage at 0% (mV) |
| `SENSOR_READINGS` | `5` | Readings per measurement (drop high+low, avg rest) |
| `ZIGBEE_JOIN_TIMEOUT` | `30000` | Max ms to wait for Zigbee connection |
| `REPORT_TIMEOUT_MS` | `10000` | Max ms to wait for attribute report ACK |
