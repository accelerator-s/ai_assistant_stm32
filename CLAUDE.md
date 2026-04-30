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
- **Character encoding**: Makefile uses `-fexec-charset=GBK` — source files and comments are GBK-encoded Chinese
- **DSP library**: Links `libarm_cortexM3l_math` from CMSIS DSP, included via `Drivers/CMSIS/DSP/Include/arm_math.h` (guarded by `ARM_MATH_CM3`)

### WebUI Backend
- **Location**: `WebUI/Backend/`
- **Dependencies**: Install with `pip install -r WebUI/requirements.txt`
- **Run server**: `python WebUI/Backend/run.py` (default port read from config, fallback 5000)
- **Framework**: Flask with SQLite database (WAL mode, per-thread connections)
- **Config file**: `WebUI/config/default_config.json` (auto-created from `default_config.example.json` if missing)
- **Database file**: `WebUI/data/assistant.db` (auto-created on first run)

### Development Environment
- VSCode configurations support both Windows and Linux/macOS
- Select "STM32 (Windows)" in C/C++ configuration for Windows
- Debug configurations available for OpenOCD and ST-Link
- For detailed Windows setup, see `WINDOWS_SETUP.md`

## Code Architecture

### STM32 Firmware Structure
```
Core/
├── Inc/                    # Header files organized by module
│   ├── bsp/                # Board support package (key, SPI flash, button events)
│   ├── lcd/                # LCD display drivers and fonts
│   ├── wifi/               # ESP8266 Wi-Fi module interface and config
│   ├── debug/              # Debug UART utilities
│   ├── audio/              # Audio processing pipeline (I2S, VAD, AGC, DSP filters)
│   ├── system/             # System state machine and main controller
│   ├── cloud/              # Cloud server communication protocol
│   └── main.h              # Main application header (STM32 HAL boilerplate)
├── Src/                    # Source files mirroring Inc/ structure
└── Startup/                # Startup code and linker script
```

**Key Modules:**

- `bsp_key.c/h` + `button_event.c/h`: Physical button handling with debouncing and event detection (short press, long press, double click, hold, release)
- `esp8266.c/h`: Non-blocking state machine for ESP8266 Wi-Fi module with AT command handling and TCP connection management
- `display.c/h`: LCD screen rendering with UI updates for system status, recording, playback, and history
- `system_state.c/h`: Central state machine and system context tracking
- `system_controller.c/h`: Main controller coordinating all subsystems (buttons, mic, cloud, display)
- `cloud_comm.c/h`: Cloud communication protocol with session management, audio frame streaming, control frames, and heartbeat
- `audio_buffer.c/h`: Audio data buffering and frame management
- `audio_processor.c/h` + `audio_quality.c/h` + `dsp_config.c/h`: CMSIS-DSP based audio pipeline
- `i2s_mic.c/h` + `microphone.c/h`: I2S microphone driver and abstraction
- `main.c`: Main application loop, peripheral initialization, interrupt handlers

### Audio Processing Pipeline
The audio subsystem uses CMSIS DSP (Q15 fixed-point) for real-time processing:
- **VAD** (Voice Activity Detection): Energy-based with adaptive noise floor, configurable silence/hangover durations
- **AGC** (Automatic Gain Control): Peak or RMS mode with configurable attack/release, optional limiter
- **Filters**: Pre-emphasis, high-pass filter, noise gate with configurable thresholds
- **Quality monitoring**: Overload detection, SNR estimation, distortion detection
- **Presets**: Default, quiet environment, noisy environment, voice recognition, music recording
- Configuration persisted to SPI Flash via `audio_config_save_to_flash()` / `audio_config_load_from_flash()`

### State Machine
The system uses the following states (defined in `system_state.h`):

| State | Description |
|---|---|
| `SYSTEM_STATE_IDLE` | Standby, waiting for user interaction |
| `SYSTEM_STATE_RECORDING` | K1 held down, actively recording audio |
| `SYSTEM_STATE_PAUSED` | K1 released, recording paused (can resume or send) |
| `SYSTEM_STATE_PLAYING` | Cloud response audio playing through speaker |
| `SYSTEM_STATE_HISTORY_LIST` | Browsing conversation history list |
| `SYSTEM_STATE_ERROR` | Error condition |

The `system_context_t` struct holds all runtime state including session info, recording/playback timers, history list (max 20 items), button events, and error details.

### Button Interaction Logic
**Main Dialogue:**
- **K1 hold**: Start/continue recording → sends raw PCM audio via TCP to cloud
- **K1 release**: Pause recording
- **K2 short press (paused)**: Send audio to cloud for processing
- **K2 long press (paused)**: Cancel and discard current recording
- **K2 short press (idle)**: Start a new conversation session
- **K2 short press (playing)**: Interrupt playback
- **K2 double-click (idle)**: Enter history list mode

**History List Mode:**
- K1: Move cursor up
- K2: Move cursor down
- K2 double-click: Load selected conversation

### WebUI Architecture
```
WebUI/
├── Backend/               # Flask application
│   ├── api/               # REST blueprints: auth, chat, config, conversation, device, mic_test, security, user
│   ├── auth/              # JWT-based authentication with bcrypt password hashing
│   ├── config/            # Thread-safe JSON config manager with dot-path access (e.g., "llm.base_url")
│   ├── database/          # SQLite with WAL mode, per-thread connections, IP banning, session/message persistence
│   ├── security/          # Rate limiter (60 req/min) and IP ban enforcement
│   ├── services/          # Business logic: device_manager (TCP server on port 8266), chat_service, speech_service, async_job_manager
│   ├── app.py             # Flask application factory with security middleware (before_request/after_request)
│   └── run.py             # Entry point — creates app, reads port from config, runs Flask dev server
├── Frontend/              # Vue.js SPA (no build step — loaded via CDN script tags)
│   ├── js/                # Components: LoginPage, StatusPanel, DeviceConfig, LLMConfig, SpeechConfig,
│   │                      #   AdvancedSettings, ConversationHistory, SecurityPanel, MicTest, UserSettings, etc.
│   ├── js/api.js          # Centralized API helper for backend calls
│   ├── js/app.js          # Main Vue application bootstrap
│   ├── css/               # Stylesheets
│   └── index.html         # Main HTML page (Vue mounts here)
└── requirements.txt       # Python dependencies (Flask, httpx, bcrypt, PyJWT, tencentcloud-sdk-python, gunicorn)
```

### Data Flow
1. **Uplink**: STM32 → I2S microphone (16kHz, 16bit, mono) → DMA double-buffer → audio_buffer → UART → ESP8266 → TCP → Cloud server (port 8266)
2. **Downlink**: Cloud server → Text-to-speech audio stream → ESP8266 → STM32 → audio processing → MAX98357A amplifier → Speaker
3. **Cloud processing**: Audio concatenation → Speech-to-text (Tencent ASR) → LLM API → Text-to-speech → SQLite database storage
4. **Recording end frame**: `REC_END:sr=<采样率>` sent to server to prevent sample rate mismatch between device and server
5. **Heartbeat**: Every 15 seconds to maintain TCP connection

## Code Style Guidelines

From `.github/instructions/stm32.instructions.md`:
- **Source files**: Lowercase with underscores (e.g., `audio_buffer.c`)
- **Header files**: `.h` extension
- **Functions**: Lowercase with underscores (e.g., `init_gpio()`, `read_sensor()`)
- **Variables**: Lowercase with underscores (e.g., `led_state`, `sensor_value`)
- **Constants**: Uppercase with underscores (e.g., `AUDIO_SAMPLE_RATE`)
- **Comments**: Must be in Chinese, must not appear AI-generated
- **Modular design**: Each functional module in separate folders/files; single files must not be excessively long

## Important Configuration Points

- **`Core/Inc/wifi/wifi_config.h`**: Contains hardcoded Wi-Fi credentials (SSID/password), server IP, and hardware pin mappings. These must be updated for each deployment environment.
- **`WebUI/config/default_config.json`**: Runtime config for WebUI — LLM API keys, speech provider credentials, device settings (sample rate, TCP port). The file is gitignored; `default_config.example.json` serves as the template.
- **`WebUI/Backend/config/config.py`**: Thread-safe JSON config with dot-path keys. Auto-initializes from example template on first run. Falls back to hardcoded defaults if template is missing.
- **TCP device port**: Default 8266 — used by both `wifi_config.h` (server IP) and `config.py` (device.tcp_port). These must agree.

## Network Protocol Notes

- ESP8266 communicates with STM32 via UART (USART3, 460800 baud) using AT commands
- Non-blocking state machine manages AT command sequences (connect to Wi-Fi, open TCP socket, send/receive data)
- Audio data sent as raw PCM streams with frame headers during recording
- Control commands (start/stop recording, new conversation, cancel) sent as separate control frames
- Heartbeat mechanism every 15 seconds to maintain connection
- Cloud response parsing handles multiple response types (OK, error, session created, audio received, processing, result ready, history list/content)

## Windows Development Setup

### Prerequisites
1. **ARM GCC Toolchain**: GNU Arm Embedded Toolchain (arm-none-eabi-gcc)
2. **Make Tool**: MSYS2, Chocolatey, or Cygwin
3. **Python 3.8+**: For WebUI backend
4. **VSCode Extensions**: C/C++, Cortex-Debug, Python, Makefile Tools

### Building on Windows
```cmd
cd E:\stm32\ai_assistant_stm32
make clean
make
```

### Flashing to Board
- **ST-Link**: STM32CubeProgrammer or OpenOCD
- **Serial ISP**: FlyMCU with BOOT0=1 mode
- **VSCode Debug**: Cortex-Debug extension with ST-Link

### WebUI Backend
```cmd
cd WebUI\Backend
pip install -r requirements.txt
python run.py
```

For detailed setup instructions, see `WINDOWS_SETUP.md`.
