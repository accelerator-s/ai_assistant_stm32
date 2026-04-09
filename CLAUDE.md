# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an STM32-based voice interaction assistant project using the Wildfire STM32F103VET6 development board. The system implements a standalone voice conversation device without requiring a host computer. The terminal handles screen UI rendering, audio data collection, and sound playback, while complex tasks like speech recognition, large model dialogue requests, speech synthesis, and history management are handled by a cloud proxy server via network transmission.

### Hardware Components
- Main controller: Wildfire STM32F103VET6 development board with external SPI Flash Chinese font library and FSMC interface LCD color screen
- Network communication: ESP8266 serial Wi-Fi module (soldered on the board)
- Voice input: INMP441 omnidirectional digital microphone module using I2S digital interface
- Voice output: MAX98357A digital audio amplifier module with 3W small speaker

## Build System

### STM32 Firmware Build
- **Build command**: `make` (uses GNU Arm Embedded Toolchain)
- **Clean build**: `make clean`
- **Build output**: Located in `build/` directory
- **Target files**: `ai_assistant.elf`, `ai_assistant.hex`, `ai_assistant.bin`
- **Toolchain**: `arm-none-eabi-gcc` (Cortex-M3)
- **Debug build**: Set `DEBUG = 1` in Makefile for debug symbols

### WebUI Backend
- **Location**: `WebUI/Backend/`
- **Dependencies**: Install with `pip install -r WebUI/requirements.txt`
- **Run server**: `python WebUI/Backend/run.py` (default port: 5000)
- **Framework**: Flask with SQLite database

### Development Environment
- **VSCode配置**: 包含Windows和Linux/macOS的多平台配置
- **Windows开发**: 参考 `WINDOWS_SETUP.md` 进行环境设置
- **构建任务**:
  - "Build STM32 Project" (Linux/macOS, 使用 `make -j${command:cmake.maxNumberOfJobs}`)
  - "Build STM32 Project (Windows)" (Windows CMD环境)
  - "Clean Build" (清理构建产物)
- **调试配置**: 支持OpenOCD和ST-Link两种调试器

## Code Architecture

### STM32 Firmware Structure
```
Core/
├── Inc/           # Header files organized by module
│   ├── bsp/       # Board support package (key, SPI flash)
│   ├── lcd/       # LCD display drivers and fonts
│   ├── wifi/      # ESP8266 Wi-Fi module interface
│   ├── debug/     # Debug UART utilities
│   └── main.h     # Main application header
├── Src/           # Source files mirroring Inc/ structure
└── Startup/       # Startup code and linker script
```

**Key Modules:**
- `bsp_key.c/h`: Physical button handling with debouncing and event detection (short press, long press, double click)
- `esp8266.c/h`: Non-blocking state machine for ESP8266 Wi-Fi module with TCP connection management
- `display.c/h`: LCD screen rendering with UI updates for system status
- `main.c`: Main application loop with state management for voice recording, playback, and history navigation

### State Machine & Button Interaction
The system uses two physical buttons (K1, K2) with the following interaction logic:

**Main Dialogue Interface:**
- **K1**: Voice recording control (hold to record, release to pause)
- **K2**: Dialogue control and state switching
  - Short press during pause: Send audio to cloud
  - Long press during pause: Cancel recording
  - Short press in standby: Start new conversation
  - Short press during playback: Interrupt playback
  - Double click in standby: Open history list

**History List Mode:**
- K1: Move cursor up
- K2: Move cursor down
- Any button double click: Load selected conversation

### WebUI Architecture
```
WebUI/
├── Backend/       # Flask application
│   ├── api/       # REST API routes (auth, config, devices, conversations)
│   ├── auth/      # Authentication and authorization
│   ├── config/    # Configuration management
│   ├── database/  # SQLite database management
│   ├── security/  # Rate limiting and security
│   ├── services/  # Business logic (device manager)
│   ├── app.py     # Flask application factory
│   └── run.py     # Application entry point
├── Frontend/      # Vue.js frontend
│   ├── css/       # Stylesheets
│   ├── js/        # JavaScript modules
│   └── index.html # Main HTML page
└── requirements.txt # Python dependencies
```

### Data Flow
1. **Uplink**: STM32 → I2S microphone (16kHz, 16bit) → DMA → UART → ESP8266 → TCP → Cloud server
2. **Downlink**: Cloud server → Text-to-speech → Audio stream → ESP8266 → STM32 → Audio amplifier → Speaker
3. **Cloud processing**: Audio拼接 → Speech-to-text → LLM API → Text-to-speech → Database storage

## Code Style Guidelines

Based on `.github/instructions/stm32.instructions.md`:

### Naming Conventions
- **Source files**: Lowercase with underscores (e.g., `main.c`, `gpio.c`)
- **Header files**: `.h` extension (e.g., `main.h`, `gpio.h`)
- **Functions**: Lowercase with underscores (e.g., `init_gpio()`, `read_sensor()`)
- **Variables**: Lowercase with underscores (e.g., `led_state`, `sensor_value`)
- **Constants**: Uppercase with underscores (e.g., `LED_PIN`, `SENSOR_THRESHOLD`)

### Development Practices
- **Modular design**: Each functional module in separate folder/files
- **File length**: Single files should not be excessively long
- **Comments**: Must be in Chinese and follow development standards (no AI-generated comments)
- **Project structure**: Clear separation of concerns with well-defined interfaces

## Common Development Tasks

### Adding New Hardware Peripheral
1. Create driver files in appropriate `Core/Inc/` and `Core/Src/` subdirectories
2. Follow existing naming conventions (e.g., `bsp_*.c/h` for board support)
3. Add source files to `C_SOURCES` in Makefile
4. Add include paths to `C_INCLUDES` in Makefile
5. Implement non-blocking patterns similar to `esp8266.c` state machine

### Modifying UI Display
1. Update `display.c/h` functions for new screen elements
2. Modify `main.c` to call display updates at appropriate state transitions
3. Ensure Chinese font support via SPI Flash font library

### Extending WebUI Functionality
1. Add new API routes in `WebUI/Backend/api/`
2. Update database schema in `WebUI/Backend/database/db.py` if needed
3. Modify frontend JavaScript in `WebUI/Frontend/js/` for new features
4. Test API endpoints with appropriate authentication

### Debugging
- Use `debug_uart.c/h` for serial debug output
- Check ESP8266 status via `esp8266_get_status()` in main loop
- Monitor TCP connection state and data transmission
- Use VSCode debug configuration in `.vscode/launch.json`

## Network Protocol Notes

- ESP8266 communicates with STM32 via UART with AT commands
- TCP connection to cloud server for audio streaming
- Audio data sent as raw PCM streams during recording
- Control commands (start/stop recording, new conversation) sent as special frames
- Heartbeat mechanism every 15 seconds to maintain connection

## Testing Considerations

- Test button interactions with various timing patterns
- Verify audio recording and playback quality
- Test Wi-Fi connectivity and TCP reconnection scenarios
- Validate WebUI authentication and device management
- Check database persistence of conversation history

## Windows Development Setup

### Prerequisites
1. **ARM GCC Toolchain**: Install GNU Arm Embedded Toolchain from https://developer.arm.com
2. **Make Tool**: Install via MSYS2, Chocolatey, or Cygwin
3. **Python 3.8+**: For WebUI backend
4. **VSCode Extensions**: C/C++, Cortex-Debug, Python, Makefile Tools

### Configuration
- VSCode configurations support both Windows and Linux/macOS
- Select "STM32 (Windows)" in C/C++ configuration
- Use "Build STM32 Project (Windows)" task for Windows builds
- Debug configurations available for OpenOCD and ST-Link

### Building on Windows
```cmd
# Install dependencies first (see WINDOWS_SETUP.md)
cd E:\stm32\ai_assistant_stm32
make clean
make
```

### Flashing to Board
- **ST-Link**: Use STM32CubeProgrammer or OpenOCD
- **Serial ISP**: Use FlyMCU with BOOT0=1 mode
- **VSCode Debug**: Use Cortex-Debug extension with ST-Link

### WebUI Backend
```cmd
cd WebUI\Backend
pip install -r requirements.txt
python run.py
```

For detailed setup instructions, see `WINDOWS_SETUP.md`.