# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

The S.P.I.R.I.T. Board (Special Peripheral Interface for Receiving Internet Transmissions) is an open-source ESP32-C6 based controller for the original 1998 Furby toy. The project consists of three main parts:

- **Hardware** (`hardware/`): KiCAD 9.0 PCB design files, component libraries, and outputs
- **Firmware** (`firmware/`): PlatformIO-based firmware for the ESP32-C6 microcontroller
- **Software** (`software/`): Python library for controlling the Spirit Board over TCP

## Firmware Development

### Building and Flashing with PlatformIO

The firmware uses [PlatformIO](https://platformio.org/) for building and uploading. Source code is in `firmware/src/main.cpp`.

**Build environments:**
| Environment | Target | Upload Method |
|-------------|--------|---------------|
| `spirit` (default) | S.P.I.R.I.T. Board | OTA (WiFi) |
| `spirit_usb` | S.P.I.R.I.T. Board | USB Serial |
| `feather` | Adafruit Feather ESP32-C6 | OTA (WiFi) |
| `feather_usb` | Adafruit Feather ESP32-C6 | USB Serial |

**Common commands:**
```bash
cd firmware

# Build (default: spirit target)
uv run pio run

# Build specific environment
uv run pio run -e feather

# Upload via OTA
uv run pio run -e spirit -t upload

# Upload via USB
uv run pio run -e spirit_usb -t upload

# Monitor serial output
uv run pio device monitor
```

PlatformIO is installed as part of the project's uv environment (see `pyproject.toml`).

### WiFi Configuration

WiFi credentials are externalized to a gitignored file:

1. Copy `firmware/platformio_override.ini.example` to `firmware/platformio_override.ini`
2. Edit with your credentials:
```ini
[credentials]
build_flags =
    -D WIFI_SSID=\"YourNetworkName\"
    -D WIFI_PASSWORD=\"YourPassword\"
```

### OTA Updates

After initial USB flash, the board enables OTA updates over WiFi:
- **Spirit Board hostname**: `TheSPIRITBoard.local`
- **Feather hostname**: `feather.local`
- **Default password**: `admin` (configurable in source)

### TCP Control Protocol

The invoker firmware listens on **TCP port 5000** and accepts ASCII hex commands. Each command starts with a control byte:

- `01` - Initialize coprocessor (Spirit only)
- `02 <nibbles>` - Send nibble stream to Furby sound coprocessor (Spirit only)
- `03 <64-bit hex>` - Drive motor forward (duration in microseconds) (Spirit only)
- `04 <64-bit hex>` - Drive motor backward (duration in microseconds) (Spirit only)
- `05` - Read event timeline (RTS/CTS edges, data nibbles, motor events) (Spirit only)
- `06 <5-bit pattern>` - Debug: directly drive CTS + D1-D4 pins (Spirit only)
- `07` - Poll /RTS + sensor buttons and store snapshot (Spirit only)
- `08 <count>` - Blink LED N times (Feather only)
- `FF` - Ping (responds with `31337`)

**Example testing with netcat:**
```bash
echo "FF" | nc <board-ip> 5000  # Should respond with "31337"
```

## Hardware Development

### KiCAD Project

- **KiCAD Version**: Designed with KiCAD 9.0
- **Project files**: `hardware/the-spirit-board.kicad_pro`
- **Main files**:
  - Schematic: `hardware/the-spirit-board.kicad_sch`
  - PCB layout: `hardware/the-spirit-board.kicad_pcb`
  - Component library: `hardware/lib/the-spirit-board.kicad_sym`
  - Footprints: `hardware/lib/the-spirit-board.pretty/*.kicad_mod`
  - 3D models: `hardware/lib/the-spirit-board.3dshapes/*.step`

### Adding New Components

When adding new components to the library (from Mouser archives):

1. Copy `.kicad_mod` footprint file to `hardware/lib/the-spirit-board.pretty/`
2. Edit the `.kicad_mod` file to prepend `lib/the-spirit-board.3dshapes/` to the STEP file reference
3. Copy `.step` 3D model file to `hardware/lib/the-spirit-board.3dshapes/`
4. Merge symbol from `.kicad_sym` into `hardware/lib/the-spirit-board.kicad_sym`
5. Save and commit all changes

## Architecture

### Firmware Architecture

The `invoker` firmware (`firmware/src/main.cpp`) is responsible for:

- **Nibble bus interface**: Drives the Furby sound coprocessor using timed 4-bit nibble vectors over D1-D4 pins
- **RTS/CTS handshaking**: Monitors `/RTS` pin to detect sound completion and bus readiness; toggles `/CTS` for handshake
- **Motor control**: Drives Furby motors forward/backward with timed pulses
- **WiFi connectivity**: Connects to WiFi and provides OTA updates
- **TCP server**: Exposes control protocol on port 5000 for remote commanding

### Conditional Compilation

The firmware supports two targets via preprocessor defines:

- `TARGET_SPIRIT` - Full Furby control: nibble bus, motor, sensors
- `TARGET_FEATHER` - Network testing only: ping, LED blink command

### Pin Mapping

Pin assignments are defined at the top of `firmware/src/main.cpp`:

```cpp
// Furby bus pins (Spirit only)
PIN_D1, PIN_D2, PIN_D3, PIN_D4    // 4-bit nibble data
PIN_CTS                            // Clear To Send (output)
PIN_RTS                            // Request To Send (input, read-only)
PIN_INIT                           // Coprocessor init

// Motor control (Spirit only)
PIN_MOTOR_FWD, PIN_MOTOR_BWD

// Sensor buttons (Spirit only)
PIN_BTN_FEED, PIN_BTN_TUMMY, PIN_BTN_BACK

// Status LED (Feather only)
PIN_LED
```

**When changing hardware:** Update these pin constants in `main.cpp` and verify footprints in `hardware/`.

### Timing Parameters

Critical timing constants in `firmware/src/main.cpp`:

```cpp
RTS_TIMEOUT_US      // Timeout waiting for /RTS response
CTS_PULSE_US        // CTS pulse width
INIT_PULSE_MS       // Initialization pulse duration
INIT_RTS_GAP_US     // Gap marking end of init sequence
MAX_MOTOR_US        // Maximum motor run duration
```

These control the RTS/CTS handshake protocol and motor timing. Modify with care.

## Key Patterns and Conventions

### Firmware Coding Style

- Uses Arduino core APIs: `pinMode()`, `digitalWrite()`, `digitalRead()`, `delayMicroseconds()`, `micros()`
- All timing-sensitive operations use microsecond precision (`micros()`, `delayMicroseconds()`)
- Pin modes and timing constants declared as `const` values at top of file
- TCP protocol uses ASCII hex encoding with helper functions `parseHexNibbles()` and `processCommand()`

### Protocol Stability

The TCP ASCII hex protocol is a stable interface for external tools. When adding firmware features:

- Maintain backwards compatibility
- Don't change existing control byte meanings
- Add new control bytes for new features
- Document protocol changes in firmware comments

## Python Library (`software/`)

The `software/spirit_board.py` module provides a Python interface for controlling the Spirit Board:

### Usage

```python
from spirit_board import SpiritBoard, get_furby_command

board = SpiritBoard(host="192.168.1.100")
board.ping()  # Returns '31337' if connected
board.init_coprocessor()
board.send_nibble_stream(get_furby_command("mee-mee"))
```

### Event Timeline Analysis

The `software/spirit_board.py` module provides timeline analysis functions:

```python
from spirit_board import parse_timeline, render_timeline, plot_timeline

# Capture timeline after sending commands
board.send_nibble_stream(get_furby_command("mee-mee"))
events = parse_timeline(board.get_rts_timing())

# Display timeline as text
print(render_timeline(events, max_events=50))

# Visualize timeline (requires matplotlib)
import matplotlib.pyplot as plt
fig, axes = plot_timeline(events, title="Mee-Mee Command Timeline")
plt.show()
```

**Timeline Event Types:**
- `RTS_RISE` / `RTS_FALL` - Request-to-send signal edges
- `CTS_RISE` / `CTS_FALL` - Clear-to-send signal edges
- `DATA` - Data nibble changes (includes nibble value)
- `MOTOR_FWD+` / `MOTOR_FWD-` - Motor forward on/off
- `MOTOR_BWD+` / `MOTOR_BWD-` - Motor backward on/off

The timeline uses 64-bit microsecond timestamps from `esp_timer_get_time()` for precise timing analysis.

### Key Classes and Functions

- `SpiritBoard` - Main class for board communication
  - `ping()` - Test connectivity
  - `init_coprocessor()` - Initialize Furby coprocessor
  - `send_nibble_stream(hex_string)` - Send nibble commands (up to 3000 nibbles)
  - `motor_forward(microseconds)` / `motor_backward(microseconds)` - Motor control
  - `get_rts_timing()` - Retrieve timing data (up to 3000 samples)
- `get_furby_command(name)` - Get predefined command by name
- `parse_nibble_echo(response)` - Parse firmware response into nibbles

### Example Notebooks

- `notebooks/SpiritBoardExample.ipynb` - Canonical examples of all library functions
- `notebooks/SpiritBoardWorking.ipynb` - Working copy (git-ignored) for development

## Data Analysis Environment

The project uses `uv` (a fast Python package manager) to manage the Jupyter Lab environment for data analysis and notebook development.

### Setup and Usage

**Initial setup:**
```bash
uv sync  # Install dependencies and create virtual environment
```

**Launch Jupyter Lab:**
```bash
uv run jupyter lab
```

### Dependencies

The environment is defined in `pyproject.toml` and includes:
- **jupyterlab** (>=4.5.1) - Interactive notebook environment
- **ipympl** (>=0.9.8) - Interactive matplotlib widgets (fixes widget compatibility issues)
- **numpy** (>=2.2.6) - Numerical computing
- **matplotlib** (>=3.10.8) - Plotting and visualization
- **pandas** (>=2.2.0) - Data analysis and manipulation

### Adding New Dependencies

To add packages for data analysis:
```bash
uv add <package-name>  # e.g., uv add scikit-learn
```

**Important:** Always commit both `pyproject.toml` AND `uv.lock` after adding dependencies to ensure reproducible environments across different machines.

### Python Version

The project requires Python >=3.10. The `.python-version` file pins the environment to Python 3.10 for consistency.

### Using Interactive Widgets

Enable matplotlib widgets in notebooks with:
```python
%matplotlib widget
```

This enables interactive plots and widgets using the `ipympl` backend, which resolves common widget compatibility issues in Jupyter Lab.

## Reference Documentation

Key files to read when working on this project:

- `README.md` - Project purpose, KiCAD workflow, useful links
- **`docs/soundcard_spec.md`** - Complete Furby soundcard protocol specification with timing diagrams and command examples
- `firmware/README.md` - Firmware build instructions and protocol reference
- `firmware/src/main.cpp` - Complete firmware implementation with protocol details
- `software/spirit_board.py` - Python library for controlling the board
- `.github/copilot-instructions.md` - Detailed architectural notes and integration points
- `hardware/the-spirit-board.kicad_sch` - Complete hardware schematic
- `notebooks/FurbyAdvancedScraping.ipynb` - Logic analyzer captures with RTS timing analysis

External references (see `README.md` for URLs):
- ESP32-C6 technical reference manual and datasheet
- Adafruit ESP32-C6 Feather schematic (design reference)
- 1998 Furby technical information and reverse-engineered schematic
- Original Furby patent (US6544098)
