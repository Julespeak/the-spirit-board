# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

The S.P.I.R.I.T. Board (Special Peripheral Interface for Receiving Internet Transmissions) is an open-source ESP32-C6 based controller for the original 1998 Furby toy. The project consists of three main parts:

- **Hardware** (`hardware/`): KiCAD 9.0 PCB design files, component libraries, and outputs
- **Firmware** (`firmware/`): Arduino-based firmware for the ESP32-C6 microcontroller
- **Software** (`software/`): Python library for controlling the Spirit Board over TCP

## Firmware Development

### Building and Flashing

The firmware is an Arduino `.ino` sketch located at `firmware/invoker/invoker.ino`:

- **Arduino IDE**: Open `firmware/invoker/invoker.ino` and upload to ESP32-C6 board
- **OTA Updates**: After initial flash, the board enables OTA updates over WiFi
  - OTA hostname: `TheSPIRITBoard` (configurable in `OTA_HOSTNAME`)
  - Default password: `admin` (configurable in `OTA_PASSWORD`)
  - Use Arduino IDE's network port feature for OTA uploads

### WiFi Configuration

Update WiFi credentials in `firmware/invoker/invoker.ino`:
```cpp
const char* WIFI_SSID     = "RenegadeScience_Guest";
const char* WIFI_PASSWORD = "ForScience!";
```

### TCP Control Protocol

The invoker firmware listens on **TCP port 5000** and accepts ASCII hex commands. Each command starts with a control byte:

- `01` - Initialize coprocessor
- `02 <nibbles>` - Send nibble stream to Furby sound coprocessor
- `03 <64-bit hex>` - Drive motor forward (duration in microseconds)
- `04 <64-bit hex>` - Drive motor backward (duration in microseconds)
- `05` - Read last /RTS high-time samples
- `06 <5-bit pattern>` - Debug: directly drive CTS + D1-D4 pins
- `07` - Poll /RTS + sensor buttons and store snapshot
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

The `invoker` firmware (`firmware/invoker/`) is responsible for:

- **Nibble bus interface**: Drives the Furby sound coprocessor using timed 4-bit nibble vectors over D1-D4 pins
- **RTS/CTS handshaking**: Monitors `/RTS` pin to detect sound completion and bus readiness; toggles `/CTS` for handshake
- **Motor control**: Drives Furby motors forward/backward with timed pulses
- **WiFi connectivity**: Connects to WiFi and provides OTA updates
- **TCP server**: Exposes control protocol on port 5000 for remote commanding

### Pin Mapping

Pin assignments are hard-coded at the top of `firmware/invoker/invoker.ino`:

```cpp
// Furby bus pins
PIN_D1, PIN_D2, PIN_D3, PIN_D4    // 4-bit nibble data
PIN_CTS                            // Clear To Send (output)
PIN_RTS                            // Request To Send (input, read-only)
PIN_INIT                           // Coprocessor init

// Motor control
PIN_MOTOR_FWD, PIN_MOTOR_BWD

// Sensor buttons
PIN_BTN_FEED, PIN_BTN_TUMMY, PIN_BTN_BACK
```

**When changing hardware:** Update these pin constants in `invoker.ino` and verify footprints in `hardware/`.

### Timing Parameters

Critical timing constants in `firmware/invoker/invoker.ino`:

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

## Reference Documentation

Key files to read when working on this project:

- `README.md` - Project purpose, KiCAD workflow, useful links
- `firmware/invoker/README.md` - Firmware module overview and responsibilities
- `firmware/invoker/invoker.ino` - Complete firmware implementation with protocol details
- `software/spirit_board.py` - Python library for controlling the board
- `.github/copilot-instructions.md` - Detailed architectural notes and integration points
- `hardware/the-spirit-board.kicad_sch` - Complete hardware schematic

External references (see `README.md` for URLs):
- ESP32-C6 technical reference manual and datasheet
- Adafruit ESP32-C6 Feather schematic (design reference)
- 1998 Furby technical information and reverse-engineered schematic
- Original Furby patent (US6544098)
