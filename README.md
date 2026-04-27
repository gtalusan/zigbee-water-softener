# Water Softener Salt Level Detector

![Water softener detector build photo](IMG_8477.jpeg)

ESP32-C6 SuperMini + VL53L0X time-of-flight sensor that measures the distance
to the salt surface inside a brine tank and reports it via Zigbee every 12 hours.
Battery level is reported alongside the distance.

**V2 firmware** (ESP-IDF C, `esp32/`) replaces the original Arduino sketch with
bare-metal deep sleep, a graduated sleep ramp, and RTC-persisted telemetry.

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
| Battery ADC | GP5 (via 1MΩ + 1MΩ voltage divider) |
| Factory reset | GP9 (BOOT button) |

The voltage divider uses two equal 1 MΩ resistors: `Vbat → R1 → ADC pin → R2 → GND`.
This halves the battery voltage so it stays within the ESP32's ADC input range.

## Files

```
├── water_softener/
│   └── water_softener.ino              Arduino sketch (V1, legacy)
├── zigbee2mqtt/water_softener_converter.js         Zigbee2MQTT external converter (V1 + V2)
├── esp32/
│   ├── CMakeLists.txt                  Top-level project
│   ├── sdkconfig                       Full Kconfig
│   ├── sdkconfig.defaults              Minimal non-default Kconfig
│   ├── partitions.csv                  Custom partition table
│   ├── dependencies.lock               Component version lock
│   └── main/
│       ├── CMakeLists.txt              Component registration
│       ├── idf_component.yml           Dependencies (ESP-Zigbee-lib, IDF)
│       ├── main.c                      Application (sleep ramp, Zigbee, reporting)
│       ├── sensors.c / sensors.h       Battery ADC + distance wrapper
│       ├── vl53l0x.c / vl53l0x.h       VL53L0X ToF driver
│       └── custom_cluster.c / .h       Custom Zigbee cluster 0xFC00
```

---

## Build & Deploy

### ESP-IDF (V2 Firmware)

The V2 firmware is a bare-metal ESP-IDF C project in `esp32/`. It uses deep sleep
with a graduated sleep ramp (4× 60 s burst → 5 min → 12 h over a 12 h window)
and persists telemetry in RTC memory across sleep cycles.

#### Prerequisites

- ESP-IDF v5.2+ (tested with v5.5.4)
- ESP32-C6 toolchain (riscv32-esp-elf)

```bash
# Install ESP-IDF (if not already installed)
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32c6 && . ./export.sh
```

#### Compile & Flash

```bash
cd esp32
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/cu.usbmodem* flash
```

#### Monitor

```bash
idf.py -p /dev/cu.usbmodem* monitor
```

Expected output on boot:
```
WATER_SOFTENER: Reset reason: 1
WATER_SOFTENER: Cold boot — ramp reset
WATER_SOFTENER: Starting Zigbee stack
WATER_SOFTENER: dist=42.3 cm  batt=85%  wake=1  rt=2345 ms  prev_rt=0 ms
WATER_SOFTENER: Deep sleep 60 s  (elapsed=0  wake=1)
```

### Arduino (V1 Firmware, Legacy)

The original Arduino sketch (`water_softener.ino`) is deprecated in favor of the
ESP-IDF firmware but remains functional.

#### Prerequisites

```bash
# Install arduino-cli (macOS)
brew install arduino-cli

# Initialize arduino-cli config
arduino-cli config init

# Add the Espressif board index
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json

# Update board index and install ESP32 core
arduino-cli core update-index
arduino-cli core install esp32:esp32

# Install VL53L0X library
arduino-cli lib install "VL53L0X"
```

#### Compile

```bash
arduino-cli compile \
  --fqbn "esp32:esp32:makergo_c6_supermini:CDCOnBoot=cdc,ZigbeeMode=ed,PartitionScheme=zigbee" \
  water_softener
```

**Important:** `CDCOnBoot=cdc` enables USB CDC serial output. Without it, `Serial.printf()` produces no output.

Expected output: ~49% flash, ~10% RAM used.

#### Find the Device Port

Plug in the ESP32-C6 via USB, then:

```bash
arduino-cli board list
```

Look for a USB serial port:
- **macOS**: `/dev/cu.usbmodem*` or `/dev/cu.usbserial-*`
- **Linux**: `/dev/ttyUSB*`
- **Windows**: `COM*`

Example output:
```
Port                          FQBN                                                   Type
/dev/cu.usbmodem21201         esp32:esp32:makergo_c6_supermini                       Serial Port (USB)
```

#### Upload

```bash
arduino-cli upload \
  --fqbn "esp32:esp32:makergo_c6_supermini:CDCOnBoot=cdc,ZigbeeMode=ed,PartitionScheme=zigbee" \
  --port /dev/cu.usbmodem21201 \
  water_softener
```

Replace `/dev/cu.usbmodem21201` with your device port.

#### Monitor Serial Output

```bash
arduino-cli monitor --port /dev/cu.usbmodem21201 --config baudrate=921600
```

Expected output on boot:
```
[ZB] Init
[ZB] Zigbee initialized
[ZB] Measurement: distance=42.3cm, battery=85%
[ZB] Attribute report sent
```

Press `Ctrl+C` to exit.

---

## Zigbee2MQTT Setup

### Converter Installation

Copy `water_softener_converter.js` to your Zigbee2MQTT `external_converters` directory and add it to `configuration.yaml`:

```yaml
external_converters:
  - water_softener_converter.js
```

Restart Zigbee2MQTT.

### Configuration

Add device config to `configuration.yaml`:

```yaml
devices:
  '0x58e6c5fffe171d98':
    friendly_name: water-softener-salt-level
```

### Pairing

On first power-on (or after credential reset), the device enters commissioning mode and joins Zigbee2MQTT. Once paired, the device will publish distance and battery every ~12 hours (or sooner if values change significantly).

### Published Values

#### V2 (Custom Cluster 0xFC00)

| Key | Type | Description |
|-----|------|-------------|
| `distance` | number (cm) | Distance from sensor to salt surface (0–200 cm) |
| `battery` | number (%) | Estimated battery charge (0–100%) |
| `wake_count` | number | Total deep-sleep wake cycles since cold boot |
| `last_runtime_ms` | number (ms) | Duration of previous wake period |
| `vl53_error_count` | number | Count of VL53L0X read failures (distance=0) |
| `linkquality` | number (LQI) | Zigbee signal strength (0–255) |

#### V1 (Analog Input Cluster)

| Key | Type | Description |
|-----|------|-------------|
| `distance` | number (cm) | Distance from sensor to salt surface (0–200 cm) |
| `battery` | number (%) | Estimated battery charge (0–100%) |
| `linkquality` | number (LQI) | Zigbee signal strength (0–255) |

---

## Credential Reset

To re-pair the device with a different Zigbee network:

1. **Hold BOOT button** (GPIO9) while applying power or pressing RST
2. Keep held until serial monitor shows: `[ZB] Credentials erased`
3. Release — the device will start commissioning immediately

---

## Troubleshooting

### No serial output on monitor

**Cause:** `CDCOnBoot=cdc` missing from FQBN  
**Fix:** Recompile and reupload with the correct FQBN (see Compile section above)

### Device not appearing in Zigbee2MQTT

1. Check Z2M logs: `docker logs zigbee2mqtt | tail -50`
2. Verify the device is powered and can reach the Zigbee coordinator
3. Try credential reset (see above)


---

## Configuration

### V2 (ESP-IDF) — `esp32/main/main.c`

| Constant | Default | Description |
|----------|---------|-------------|
| `PIN_VL53_XSHUT` | `2` | VL53L0X power control pin |
| `PIN_BOOT_BUTTON` | `9` | Factory-reset button pin |
| `INITIAL_SLEEP_SEC` | `60` | Sleep duration for first 4 wake cycles |
| `INITIAL_SLEEP_COUNT` | `4` | Number of initial short-sleep cycles |
| `MIN_SLEEP_SEC` | `300` (5 min) | Minimum deep sleep duration |
| `MAX_SLEEP_SEC` | `43200` (12 h) | Maximum deep sleep duration |
| `RAMP_WINDOW_SEC` | `43200` (12 h) | Duration of the linear ramp phase |

### V2 (ESP-IDF) — `esp32/main/sensors.c`

| Constant | Default | Description |
|----------|---------|-------------|
| `ADC_ATTEN` | `ADC_ATTEN_DB_11` | ADC attenuation |
| `ADC_SAMPLES` | `5` | ADC readings per measurement |
| `BATT_DIVIDER_RATIO` | `2.0` | ADC multiplier (1M + 1M divider) |
| `BATT_MAX_MV` | `4200` | Battery voltage at 100% (mV) |
| `BATT_MIN_MV` | `3300` | Battery voltage at 0% (mV) |

### V2 (ESP-IDF) — `esp32/main/vl53l0x.c`

| Constant | Default | Description |
|----------|---------|-------------|
| `dist_samples` | `10` | VL53L0X readings per measurement (trimmed mean) |

### V1 (Arduino) — `water_softener/water_softener.ino`

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
