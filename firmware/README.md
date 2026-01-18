# INVOKER Firmware

**INVOKER** - *Infernal Nibble-Vector Oracle & Kinetic Event Receiver* - is the
firmware module responsible for:

- Driving the Furby sound coprocessor with timed 4-bit nibble vectors
- Monitoring `/RTS` to measure sound completion and bus readiness
- Providing a higher-level interface for "invoking" audio phrases
  from the SPIRIT board

## Building with PlatformIO

This firmware uses [PlatformIO](https://platformio.org/) for building and uploading.

### Prerequisites

PlatformIO is installed as part of the project's uv environment. First, sync the environment:
```bash
cd /path/to/the-spirit-board
uv sync
```

### Build Environments

| Environment | Target Board | Upload Method |
|-------------|--------------|---------------|
| `spirit` | S.P.I.R.I.T. Board | OTA (WiFi) |
| `spirit_usb` | S.P.I.R.I.T. Board | USB Serial |
| `feather` | Adafruit Feather ESP32-C6 | OTA (WiFi) |
| `feather_usb` | Adafruit Feather ESP32-C6 | USB Serial |

### WiFi Credentials

1. Copy `platformio_override.ini.example` to `platformio_override.ini`
2. Edit `platformio_override.ini` with your WiFi credentials:
   ```ini
   [credentials]
   build_flags =
       -D WIFI_SSID=\"YourNetworkName\"
       -D WIFI_PASSWORD=\"YourPassword\"
   ```

The `platformio_override.ini` file is gitignored to keep credentials private.

### Build Commands

```bash
cd firmware

# Build for S.P.I.R.I.T. Board (default)
uv run pio run

# Build for specific environment
uv run pio run -e spirit
uv run pio run -e feather

# Upload via OTA (default)
uv run pio run -e spirit -t upload

# Upload via USB
uv run pio run -e spirit_usb -t upload

# Monitor serial output
uv run pio device monitor
```

### Conditional Compilation

The firmware uses preprocessor defines to enable/disable features per target:

- `TARGET_SPIRIT` - Full Furby control: nibble bus, motor, sensors
- `TARGET_FEATHER` - Network testing only: ping, LED blink

### TCP Protocol

Both targets listen on **TCP port 5000**. Commands available depend on target:

| Command | Spirit | Feather | Description |
|---------|--------|---------|-------------|
| `01` | Yes | No | Initialize coprocessor |
| `02 <nibbles>` | Yes | No | Send nibble stream |
| `03 <64-bit hex>` | Yes | No | Motor forward |
| `04 <64-bit hex>` | Yes | No | Motor backward |
| `05` | Yes | No | Read event timeline |
| `06 <5-bit>` | Yes | No | Debug bus drive |
| `07` | Yes | No | Poll inputs |
| `08 <count>` | No | Yes | Blink LED N times |
| `FF` | Yes | Yes | Ping (returns `31337`) |
