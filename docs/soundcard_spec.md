# Furby Soundcard Communication Protocol Specification

**Version:** 1.0
**Last Updated:** 2026-01-02
**Target Hardware:** 1998 Furby Sound Coprocessor

---

## Table of Contents

1. [Overview](#overview)
2. [Physical Interface](#physical-interface)
3. [Signal Descriptions](#signal-descriptions)
4. [Protocol Layers](#protocol-layers)
5. [Timing Specifications](#timing-specifications)
6. [Message Format](#message-format)
7. [Known Commands](#known-commands)
8. [Debugging Information](#debugging-information)
9. [References](#references)

---

## 1. Overview

The Furby soundcard communication protocol is a proprietary 4-bit parallel interface used to communicate with the sound coprocessor in the original 1998 Furby toy. The protocol uses a handshaking mechanism with Request-To-Send (RTS) and Clear-To-Send (CTS) signals to synchronize data transmission between a host controller and the Furby's sound coprocessor.

### What the Protocol Controls

- **Sound Playback**: Commands trigger pre-recorded audio samples stored in the coprocessor's ROM
- **Sound Synthesis**: Controls the Furby's speech and sound effects engine
- **Coprocessor State**: Manages initialization and operating modes of the sound hardware

The protocol does NOT directly control motors, sensors, or other Furby subsystems (those are handled separately).

### Protocol Characteristics

- **Bus Width**: 4-bit parallel data (D1-D4)
- **Data Unit**: Nibble (4 bits)
- **Synchronization**: Hardware handshaking with /RTS and /CTS signals
- **Timing**: Microsecond-precision timing required
- **Voltage Levels**: Standard digital logic levels (typically 3.3V or 5V)

---

## 2. Physical Interface

### Pin Assignments (ESP32-C6 Implementation)

The Spirit Board implementation uses the following ESP32-C6 GPIO assignments:

| Signal Name | ESP32 GPIO | Direction | Description |
|-------------|-----------|-----------|-------------|
| D1          | GPIO 3    | Output    | Data bit 0 (LSB) |
| D2          | GPIO 4    | Output    | Data bit 1 |
| D3          | GPIO 5    | Output    | Data bit 2 |
| D4          | GPIO 6    | Output    | Data bit 3 (MSB) |
| /CTS        | GPIO 1    | Output    | Clear To Send (active-low) |
| /RTS        | GPIO 2    | Input     | Request To Send (active-low) |
| /INIT       | GPIO 0    | Output    | Coprocessor initialization (active-low) |

**Note**: Pin assignments are defined in `firmware/invoker/invoker.ino` lines 49-56.

### Connector Interface

The signals connect to the Furby's main PCB via the sound coprocessor interface. The exact connector pinout varies by Furby model, but the signal protocol remains consistent.

### Electrical Characteristics

- **Logic Levels**: TTL-compatible (0V = LOW, 3.3V/5V = HIGH)
- **Output Drive**: Standard GPIO output capability (~20mA per pin)
- **Input Protection**: Internal pull-ups/pull-downs as configured
- **Rise/Fall Times**: < 10μs typical for data signals

---

## 3. Signal Descriptions

### 3.1 Data Signals (D1-D4)

**Direction**: Host → Coprocessor (Output from ESP32)
**Format**: 4-bit parallel nibble
**Bit Ordering**: D1 is LSB, D4 is MSB

Each nibble represents a 4-bit value (0x0 to 0xF) transmitted in parallel on these lines.

**Example Nibble Encoding**:
```
Nibble Value: 0xA (decimal 10, binary 1010)
  D4 = 1 (bit 3)
  D3 = 0 (bit 2)
  D2 = 1 (bit 1)
  D1 = 0 (bit 0)
```

### 3.2 Clear-To-Send (/CTS)

**Direction**: Host → Coprocessor (Output from ESP32)
**Active Level**: Low (active-low signal)
**Default State**: HIGH (idle)

The /CTS signal indicates when the host is presenting valid data on the D1-D4 lines.

**Operation**:
1. Host places nibble data on D1-D4 lines
2. Host pulls /CTS LOW (falling edge starts handshake)
3. Coprocessor reads the nibble and begins processing
4. Host waits for /RTS handshake completion
5. Host returns /CTS HIGH (rising edge completes handshake)

### 3.3 Request-To-Send (/RTS)

**Direction**: Coprocessor → Host (Input to ESP32)
**Active Level**: Low (active-low signal)
**Default State**: LOW (ready)

The /RTS signal indicates the coprocessor's busy/ready status during nibble reception.

**Behavior**:
- **LOW**: Coprocessor is ready to receive data or has completed processing
- **HIGH**: Coprocessor is busy processing the nibble
- **Pulse Duration**: Varies based on command complexity and internal processing

**Typical /RTS Response Patterns**:
- Simple commands: 38-85 μs high time
- Sound playback commands: 25-60 ms high time (during audio playback)
- Initialization: 740 μs pulses during init sequence

### 3.4 Initialization (/INIT)

**Direction**: Host → Coprocessor (Output from ESP32)
**Active Level**: Low (active-low signal)
**Default State**: HIGH (normal operation)

The /INIT signal performs a hardware reset of the sound coprocessor.

**Operation**:
1. Pull /INIT LOW for 10 ms
2. Return /INIT HIGH
3. Coprocessor performs internal initialization (firmware waits 0.5s overall timeout)
4. Monitor /RTS for initialization pulse train (sequence of HIGH pulses)
5. Wait for /RTS to remain LOW for 10+ ms (indicates init complete)

---

## 4. Protocol Layers

### 4.1 Nibble Transmission Protocol

The fundamental operation is transmitting a single 4-bit nibble with hardware handshaking.

**Nibble Transmission Sequence**:

```
┌─────────────┐
│ 1. Set Data │  Place nibble value on D1-D4 lines
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ 2. Settle   │  Delay 3μs for signal stabilization
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ 3. CTS Low  │  Pull /CTS LOW (start handshake)
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ 4. Wait RTS │  Wait for /RTS to go HIGH (coprocessor acknowledges)
│    High     │  Timeout: 500ms
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ 5. Delay    │  Wait 5μs while RTS is HIGH
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ 6. CTS High │  Return /CTS HIGH (release handshake)
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ 7. Wait RTS │  Wait for /RTS to return LOW (processing complete)
│    Low      │  Timeout: 500ms total
└──────┬──────┘
       │
       ▼
┌─────────────┐
│  Complete   │  Nibble successfully transmitted
└─────────────┘
```

**Implementation Reference**: See `sendNibble()` function in `firmware/invoker/invoker.ino` lines 277-362.

### 4.2 RTS/CTS Handshaking Mechanism

The handshake protocol ensures reliable data transfer by synchronizing the host and coprocessor.

**Timing Diagram**:

```
       ┌─────────────────────────────────┐
D1-D4  │      Valid Nibble Data          │
       └─────────────────────────────────┘
            ▲                       ▲
            │ Data Setup            │ Data Hold

            ┌───┐                   ┌───
/CTS  ──────┘   └───────────────────┘
        (1) (2)     (3)         (4)

                ┌───────────┐
/RTS  ──────────┘           └──────────────
                    (5)

Timeline:
  (1) CTS falls    - Host initiates transfer
  (2) Data setup   - 3μs settling time before CTS edge
  (3) RTS rises    - Coprocessor acknowledges receipt
  (4) CTS rises    - Host releases after 5μs
  (5) RTS falls    - Coprocessor completes processing
```

**Critical Timing Constraints**:
- Data must be stable 3μs before /CTS falling edge
- /CTS must remain LOW until /RTS rises
- /CTS rising edge occurs 5μs after /RTS rises
- /CTS must remain HIGH until /RTS falls (data hold time)

### 4.3 Initialization Sequence

Before sending commands, the coprocessor must be initialized.

**Initialization Steps**:

1. **Hardware Reset**:
   - Pull /INIT LOW for 10 ms
   - Return /INIT HIGH
   - Clear RTS timing history buffer

2. **Monitor Init Pulse Train**:
   - Coprocessor generates series of /RTS pulses during initialization
   - Typical pattern: ~740μs HIGH pulses with ~740μs LOW intervals
   - Number of pulses varies (typically 8-12 pulses observed)

3. **Detect Completion**:
   - Wait for /RTS to remain LOW continuously for 10+ ms
   - This gap indicates initialization is complete
   - Overall timeout: 500 ms safety limit

4. **Return Init Timing Data**:
   - Firmware records all /RTS pulse durations during init
   - Returns count and pulse times for diagnostics

**Implementation Reference**: See `doInitCoprocessor()` function in `firmware/invoker/invoker.ino` lines 402-466.

**Observed Init Pattern** (from logic analyzer data):
```
Pulse #  | RTS High Duration
---------|------------------
1        | 37.8 μs
2        | 40.9 μs
3        | 37.8 μs
...      | (continues for 40+ pulses)
38       | 37830.6 μs (long pulse)
39       | 76.2 μs
40       | 37774.5 μs (long pulse)
...      | (pattern continues)
```

Note: The initialization pulse train includes both short (~38μs) and very long (~38ms) pulses. The exact meaning of this pattern is not fully documented.

---

## 5. Timing Specifications

### 5.1 Critical Timing Parameters

| Parameter | Constant Name | Value | Description |
|-----------|---------------|-------|-------------|
| RTS Timeout | `RTS_TIMEOUT_US` | 500,000 μs (500 ms) | Maximum time to wait for /RTS response |
| CTS Pulse Width | `CTS_PULSE_US` | 5 μs | Duration /CTS held LOW during handshake |
| Data Setup Time | (hardcoded) | 3 μs | Delay after setting data before /CTS edge |
| Data Hold Time | (hardcoded) | 5 μs | Delay after /CTS rising edge |
| Init Pulse Duration | `INIT_PULSE_MS` | 10 ms | Duration to hold /INIT LOW |
| Init Gap Detect | `INIT_RTS_GAP_US` | 10,000 μs (10 ms) | Low period marking end of init |
| Overall Init Timeout | (hardcoded) | 500,000 μs (500 ms) | Safety timeout for initialization |

**Source**: `firmware/invoker/invoker.ino` lines 80-86.

### 5.2 Observed RTS High Times

RTS high duration varies significantly based on command type and coprocessor activity.

**From Logic Analyzer Captures** (`notebooks/FurbyAdvancedScraping.ipynb`):

#### Initialization Sequence
- **Count**: 46 nibbles
- **RTS Min**: 37.7 μs
- **RTS Max**: 38,266.2 μs
- **RTS Mean**: 10,341.7 μs

#### Command Sequences (15 segments analyzed)

| Segment | Nibbles | RTS Min (μs) | RTS Max (μs) | RTS Mean (μs) | Description |
|---------|---------|--------------|--------------|---------------|-------------|
| 1 (yawn) | 844 | 37.8 | 57,729.9 | 2,767.2 | Long audio playback |
| 2 (sequence-0) | 2,451 | 37.8 | 49,695.5 | 1,676.1 | Complex sequence |
| 3 (me) | 1,254 | 37.8 | 38,767.4 | 2,186.2 | Speech command |
| 4 (mee-mee) | 542 | 38.0 | 54,564.5 | 1,932.6 | Short speech |
| 5-15 | Various | 37.9-38.0 | 23,083-706,978 | 1,271-3,097 | Other commands |

**Key Observations**:
1. **Minimum RTS times** (~38μs) are remarkably consistent across all commands
2. **Maximum RTS times** vary dramatically (23ms to 707ms) based on audio playback duration
3. **Mean RTS times** correlate with command complexity (more audio = longer average)
4. Segment 12 shows exceptionally long max RTS time (707ms) - likely a long audio sample

### 5.3 Timing Constraints Summary

**Minimum Timings** (must be met):
- Data setup before /CTS: **≥ 3 μs**
- /CTS pulse width: **≥ 5 μs**
- Data hold after /CTS: **≥ 5 μs**

**Timeout Values** (for error detection):
- RTS response timeout: **500 ms**
- Initialization timeout: **500 ms**
- Init gap detection: **10 ms** of continuous LOW

**Typical RTS Response Times**:
- Fast commands: **38-200 μs**
- Audio playback: **25-60 ms** per sound segment
- Very long audio: **up to 700+ ms** (rare)

---

## 6. Message Format

### 6.1 Initialization Message Structure

The initialization sequence prepares the coprocessor to receive commands.

**Standard Initialization Sequence**:
```
F7F347F7F71C000000000000000000000000F000FFFFFF
```

**Breakdown**:
- Total length: 46 nibbles
- Structure appears to be:
  - Header: `F7F347F7F71C` (12 nibbles)
  - Zero padding: `000000000000000000000000` (24 nibbles)
  - Footer: `F000FFFFFF` (10 nibbles)

**Purpose**: Resets coprocessor audio state machine and prepares for command reception.

**Implementation**: Defined in `FURBY_COMMANDS["init"]` in `software/spirit_board.py` line 250.

### 6.2 Command Message Structure

Commands follow a general pattern observed across multiple captured sequences.

**Generic Command Structure**:
```
[HEADER] [COMMAND_DATA] [FOOTER]
```

**Header Patterns** (first 12-24 nibbles):
- Most commands start with `F7F3XX` where XX varies by command type
- Common variants:
  - `F7F326` - Short utterances
  - `F7F341` - Complex sequences
  - `F7F347` - Speech commands
  - `F7F3C1`, `F7F3C3`, `F7F3CA`, `F7F3CE` - Other command types

**Footer Patterns** (last 8-12 nibbles):
- Nearly all commands end with `FFFFFF` or `F000FFFFFF`
- Appears to be a terminator sequence

**Command Data Section**:
- Variable length (hundreds to thousands of nibbles)
- Contains encoded audio parameters or sample references
- Structure not fully reverse-engineered

### 6.3 Multi-Part Commands

Some commands consist of multiple sequences sent in succession:

**Example: "me" command** (from `software/spirit_board.py` line 256):
```
Part 1: F7F3C3F7F71C000000000000000000000000F000FFFFFF
Part 2: F7F347F7F71C4321484A36176B8C...FFFFFF0000F000FFFFFF
```

**Structure**:
1. First part appears to be a setup/mode change command
2. Second part contains the actual audio playback data
3. Each part is a complete message with header and footer

---

## 7. Known Commands

### 7.1 Predefined Command Library

The following commands have been captured from actual Furby communication and are available in `software/spirit_board.py`.

| Command Name | Nibble Count | Description | Notes |
|--------------|--------------|-------------|-------|
| `init` | 46 | Initialization sequence | Must be sent after coprocessor reset |
| `yawn` | 844 | Yawn sound effect | Long audio playback (~58s max RTS) |
| `me` | 476 | "Me" utterance | Two-part command |
| `mee-mee` | 289 | "Mee-mee" speech | Short speech sound |
| `giggle` | 225 | Giggling sound | Short playback |
| `cockadoodledoo` | 1,230 | "Cock-a-doodle-doo" | Complex vocalization |
| `peek-boo-kiss` | 1,043 | "Peek-a-boo" with kiss | Multi-sound sequence |
| `sequence-0` through `sequence-12` | Various | Captured sequences | Purpose varies, some corrupted |

**Source**: `FURBY_COMMANDS` dictionary in `software/spirit_board.py` lines 247-290.

### 7.2 Command Usage Examples

**Python Library Usage**:

```python
from spirit_board import SpiritBoard, get_furby_command

board = SpiritBoard(host="192.168.1.100")

# Initialize the coprocessor
board.init_coprocessor()

# Send initialization sequence
board.send_nibble_stream(get_furby_command("init"))

# Execute a command
board.send_nibble_stream(get_furby_command("mee-mee"))
```

**Direct TCP/Netcat Usage**:

```bash
# Control byte 0x01 = Initialize coprocessor
echo "01" | nc 192.168.1.100 5000

# Control byte 0x02 + nibble data = Send command
echo "02F7F347F7F71C000000000000000000000000F000FFFFFF" | nc 192.168.1.100 5000
```

### 7.3 Command Composition Guidelines

When creating custom commands or modifying existing ones:

1. **Always include header**: Start with `F7F3XX` pattern
2. **Always include footer**: End with `FFFFFF` or `F000FFFFFF`
3. **Respect timing**: Long commands may have very long RTS response times
4. **Test incrementally**: Start with known commands, modify small sections
5. **Monitor RTS timing**: Use `get_rts_timing()` to debug transmission issues

---

## 8. Debugging Information

### 8.1 Common Timing Issues

**Symptom**: /RTS timeout errors
**Possible Causes**:
- Coprocessor not initialized properly
- Invalid command data (malformed header/footer)
- Data corruption during transmission
- Electrical connection issues

**Debug Steps**:
1. Verify initialization: Check `/RTS` pulse train during init
2. Test with known-good command (e.g., "mee-mee")
3. Inspect RTS timing with `board.get_rts_timing()`
4. Check for consistent minimum RTS times (~38μs)

**Symptom**: Inconsistent command execution
**Possible Causes**:
- Race conditions in /CTS timing
- Insufficient data setup/hold times
- Electrical noise on signal lines

**Debug Steps**:
1. Increase data setup time from 3μs to 10μs (firmware modification)
2. Add pull-up/pull-down resistors on data lines
3. Use logic analyzer to verify signal integrity
4. Check for ground loops or power supply noise

### 8.2 RTS Timing Patterns

**Normal Behavior**:
- Each nibble produces exactly one /RTS pulse
- Minimum pulse width: ~38μs (consistent)
- Maximum pulse width: varies by command (up to 700ms for long audio)
- No /RTS activity between commands (should stay LOW when idle)

**Abnormal Patterns**:

| Pattern | Indication | Action |
|---------|------------|--------|
| RTS always LOW | Coprocessor not responding | Re-initialize, check power |
| RTS always HIGH | Coprocessor stuck/busy | Hardware reset required |
| Missing RTS pulses | Nibbles not acknowledged | Slow down transmission, check wiring |
| Very short RTS (<20μs) | Signal integrity issue | Check electrical connections |
| No RTS during init | Init failed | Verify /INIT signal, check coprocessor power |

### 8.3 Diagnostic Firmware Commands

The Spirit Board firmware provides debugging tools:

**Control Byte 0x05: Read RTS History**
```bash
echo "05" | nc 192.168.1.100 5000
```
Returns last 3000 RTS high-time samples in microseconds. Use this to:
- Verify nibble count matches expected command length
- Detect missing or corrupted nibbles (0μs entries)
- Analyze timing patterns for specific commands

**Control Byte 0x06: Debug Bus Drive**
```bash
# Drive CTS=1, D4=1, D3=0, D2=1, D1=0 (pattern 0x1A)
echo "061A" | nc 192.168.1.100 5000
```
Directly drives bus signals for continuity testing. Pattern format (5 bits):
```
Bit 4: /CTS
Bit 3: D4
Bit 2: D3
Bit 1: D2
Bit 0: D1
```

**Control Byte 0x07: Poll Inputs**
```bash
echo "07" | nc 192.168.1.100 5000
```
Samples /RTS and button states for diagnostics.

### 8.4 Logic Analyzer Verification

For deep protocol debugging, use a logic analyzer to capture:

**Minimum Channels Required**:
- D1, D2, D3, D4 (data)
- /CTS (host timing)
- /RTS (coprocessor response)
- /INIT (initialization events)

**Trigger Configuration**:
- Trigger on /CTS falling edge (start of nibble transmission)
- Capture window: 100ms minimum (to see full RTS response)
- Sample rate: ≥1 MHz (10 samples per microsecond minimum)

**What to Look For**:
1. Data stable before /CTS falls (3μs setup)
2. /RTS rises within 500ms of /CTS falling
3. /CTS rises before /RTS falls (proper handshake order)
4. Consistent ~38μs minimum RTS pulses for all nibbles
5. No glitches or runt pulses on any signal

### 8.5 Common Error Codes

**Firmware Response Messages**:

| Message | Meaning | Remedy |
|---------|---------|--------|
| `[ERROR] RTS timeout` | Coprocessor didn't respond in 500ms | Re-initialize, check wiring |
| `[ERROR] No payload` | Command sent without nibble data | Include nibbles after control byte 0x02 |
| `[NIBBLES] count=0` | No valid hex data parsed | Check hex string format |
| `[RTS] 0 samples` | No RTS pulses recorded | Verify coprocessor is powered and initialized |

---

## 9. References

### 9.1 Source Files

- **Firmware Implementation**: `firmware/invoker/invoker.ino`
  - `sendNibble()` function: lines 277-362
  - `doInitCoprocessor()` function: lines 402-466
  - Pin definitions: lines 49-62
  - Timing constants: lines 80-88

- **Python Library**: `software/spirit_board.py`
  - `SpiritBoard` class: lines 16-194
  - Command definitions: lines 247-290
  - Protocol documentation: lines 1-9

- **Logic Analyzer Data**: `notebooks/FurbyAdvancedScraping.ipynb`
  - Nibble extraction: cell `extract_nibbles_from_window`
  - Timing analysis: cells showing RTS statistics
  - 15 command sequences captured with timing data

- **Hardware Schematic**: `hardware/the-spirit-board.kicad_sch`
  - Pin connections and electrical interface

### 9.2 External Documentation

- **Furby Technical Information**: See `README.md` for links to:
  - 1998 Furby reverse-engineering resources
  - Original Furby patent (US6544098)
  - Community-documented protocol information

- **ESP32-C6 Reference**:
  - GPIO timing characteristics
  - Pin multiplexing and configuration

### 9.3 Related Protocols

This specification documents only the **sound coprocessor interface**. The Furby has other subsystems:

- **Motor Control**: Separate H-bridge driver (not part of nibble protocol)
- **Sensor Inputs**: Direct GPIO reads (feed button, tummy sensor, back button)
- **Main CPU**: Separate microcontroller that orchestrates all subsystems

The Spirit Board provides motor control via separate TCP commands (0x03, 0x04) but these bypass the soundcard protocol entirely.

---

## Appendix A: Quick Reference

### Signal Summary
```
D1-D4:  4-bit parallel data (output)
/CTS:   Clear-to-send handshake (output, active-low)
/RTS:   Request-to-send response (input, active-low)
/INIT:  Coprocessor reset (output, active-low)
```

### Timing Summary
```
Data setup:         3 μs
CTS pulse:          5 μs
Data hold:          5 μs
RTS timeout:        500 ms
Init pulse:         10 ms
Init gap detect:    10 ms
```

### Command Flow
```
1. Initialize coprocessor (0x01 command)
2. Send init sequence (0x02 + init nibbles)
3. Send command sequence (0x02 + command nibbles)
4. Monitor RTS timing (0x05 for diagnostics)
```

### Error Recovery
```
- RTS timeout → Re-initialize coprocessor
- No RTS pulses → Check power and /INIT signal
- Corrupted audio → Verify command data integrity
- Intermittent failures → Check signal integrity with logic analyzer
```

---

**Document Revision History**:
- v1.0 (2026-01-02): Initial comprehensive specification based on firmware implementation, logic analyzer captures, and Python library analysis.
