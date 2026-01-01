# Spirit Board Library Refactoring Notes

## Summary

The common functionality from the Jupyter notebooks has been extracted into a reusable Python library at `software/spirit_board.py`.

## Created Files

### 1. `software/spirit_board.py`
A comprehensive Python library containing:
- **`SpiritBoard` class**: Object-oriented interface for all Spirit Board operations
  - Connection management to TCP server
  - High-level methods: `ping()`, `init_coprocessor()`, `send_nibble_stream()`, etc.
  - Motor control: `motor_forward()`, `motor_backward()`
  - Low-level: `send_hex_command()` for raw control

- **Helper functions**:
  - `parse_nibble_echo()`: Parse firmware nibble echo responses
  - `get_furby_command()`: Retrieve predefined command by name
  - `list_furby_commands()`: List all available commands

- **Predefined commands**: Dictionary of known Furby command sequences
  - init, yawn, me, mee-mee, cockadoodledoo, giggle, peek-boo-kiss

### 2. `notebooks/SpiritBoardExample.ipynb`
A canonical example notebook demonstrating:
- Library import and setup
- Connection testing with ping
- Coprocessor initialization
- Executing predefined commands
- Parsing responses
- Motor control
- Chunked command sending
- Custom command sequences
- Complete workflow examples

## Changes Needed in Existing Notebooks

### `notebooks/FurbyInvoker.ipynb`

This notebook currently contains inline function definitions that should be replaced with library imports. Here are the recommended changes:

#### Replace the entire connection and function definition section with:

```python
import sys
sys.path.insert(0, '../software')

from spirit_board import SpiritBoard, parse_nibble_echo, get_furby_command, list_furby_commands

# Configure your Spirit Board
ESP32_HOST = "192.168.66.133"  # Update as needed
board = SpiritBoard(host=ESP32_HOST)
```

#### Update function calls:

**Old code:**
```python
response = cmd_ping()
response = cmd_init_coprocessor()
response = cmd_send_nibble_stream(init_hex)
response = cmd_motor_forward_us(duration_us)
response = cmd_motor_backward_us(duration_us)
response = cmd_get_rts_timing()
```

**New code:**
```python
response = board.ping()
response = board.init_coprocessor()
response = board.send_nibble_stream(init_hex)
response = board.motor_forward(duration_us)
response = board.motor_backward(duration_us)
response = board.get_rts_timing()
```

#### Simplify predefined commands:

**Old code:**
```python
furby_dict = {}
furby_dict["yawn"] = "F7F326F7F719..."
furby_dict["me"] = "F7F3C3F7F71C..."
# etc...

# Then use like:
response = cmd_send_nibble_stream(furby_dict["yawn"])
```

**New code:**
```python
# Commands are built into the library
response = board.send_nibble_stream(get_furby_command("yawn"))

# Or list all available:
print(list_furby_commands())
```

#### Update chunked sending:

#### Cells to remove/replace:
- **Cell with `send_hex_command()` definition**: Remove (now in library)
- **Cell with `cmd_ping()`, `cmd_init_coprocessor()`, etc.**: Remove (now methods of SpiritBoard class)
- **Cell with `_format_duration_us()` definition**: Remove (now private method of SpiritBoard)
- **Cell with `furby_dict` definitions**: Remove (now `FURBY_COMMANDS` constant in library)
- **Cell with `parse_nibble_echo()` definition**: Remove (now standalone function in library)

### `notebooks/FurbyAdvancedScraping.ipynb`

This notebook is focused on data analysis and doesn't directly use the Spirit Board connection, so it requires minimal changes. However, if you want to add the ability to send scraped sequences:

#### Add library import:
```python
import sys
sys.path.insert(0, '..')

from spirit_board import SpiritBoard
```

#### Add a cell to test scraped sequences:
```python
# After extracting hex sequences from logic analyzer data
# you can send them to the Furby:

ESP32_HOST = "192.168.66.133"  # Update as needed
board = SpiritBoard(host=ESP32_HOST)

# Initialize
board.init_coprocessor()
time.sleep(0.1)
board.send_nibble_stream(init_hex)
time.sleep(0.5)

# Send your scraped sequence
board.send_nibble_stream(intro_hex)
```

## Benefits of Refactoring

1. **Code Reusability**: No need to copy-paste function definitions between notebooks
2. **Maintainability**: Bug fixes and improvements only need to be made once
3. **Cleaner Notebooks**: Focus on the analysis/experiments, not boilerplate code
4. **Documentation**: All functions have proper docstrings
5. **Type Hints**: Better IDE support and error checking
6. **Object-Oriented**: Encapsulates connection state in the `SpiritBoard` class
7. **Predefined Commands**: All known Furby commands in one place
8. **Easy Testing**: Can test connection and commands without a full notebook

## Usage Pattern

The typical workflow becomes much simpler:

```python
# Import
from spirit_board import SpiritBoard, get_furby_command

# Connect
board = SpiritBoard(host="192.168.66.133")

# Test connection
print(board.ping())

# Initialize Furby
board.init_coprocessor()
time.sleep(0.1)
board.send_nibble_stream(get_furby_command("init"))
time.sleep(0.5)

# Execute commands
board.send_nibble_stream(get_furby_command("giggle"))
time.sleep(1.0)
board.send_nibble_stream(get_furby_command("mee-mee"))
```

## Future Enhancements

Potential improvements to the library:

1. Add context manager support for automatic connection cleanup
2. Add async support for non-blocking operations
3. Add retry logic for failed commands
4. Add logging support for debugging
5. Add validation for command sequences
6. Add methods to save/load custom command sequences
7. Create a CLI tool based on the library
8. Add more detailed response parsing functions

## Migration Checklist

For each existing notebook:

- [ ] Add library import at the top
- [ ] Replace `ESP32_HOST` variable with `SpiritBoard` instance creation
- [ ] Replace all `cmd_*()` function calls with `board.*()` method calls
- [ ] Replace `furby_dict` references with `get_furby_command()` calls
- [ ] Remove inline function definitions
- [ ] Test all cells to ensure they work with the new library
- [ ] Update any documentation/comments to reference the library

## Notes

- The library file is placed at the project root for easy access from notebooks and other scripts
- The library has no external dependencies beyond Python standard library (socket, re, typing)
- All predefined commands from the FurbyInvoker notebook are preserved in `FURBY_COMMANDS`
- The library maintains backward compatibility with the function signatures from the original notebooks
