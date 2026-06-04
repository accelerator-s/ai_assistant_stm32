# CLAUDE.md

本文件为 Claude Code 提供该仓库的工作指引。

## 项目概述

基于野火 STM32F103VET6 开发板的语音交互助手。设备无需连接电脑即可独立运行：STM32 负责 LCD 界面渲染、音频采集与播放，语音识别、大模型对话、语音合成、历史管理等计算密集任务均通过 Wi-Fi 转发给云端代理服务器（WebUI 后端）处理。

### 硬件组成

| 部件 | 型号/说明 |
|------|-----------|
| 主控 | 野火 STM32F103VET6（Cortex-M3 @ 72MHz），板载外部 SPI Flash 中文字库，FSMC 接口 240×320 LCD |
| 网络 | ESP8266 串口 Wi-Fi 模块（焊接在板上，USART3 连接） |
| 麦克风 | INMP441 全向数字硅麦（I2S2，引脚 PB13/PB12/PB15） |
| 扬声器 | MAX98357A 数字功放 + 3W 小喇叭（I2S2 TX，共用总线） |
| 总线控制 | PB14 连接 74HC125N + MAX98357A SD 引脚；低电平启用麦克风，高电平启用扬声器 |

---

## 构建系统

### 固件构建（STM32）

```bash
make          # 编译
make clean    # 清理
```

- **工具链**：`arm-none-eabi-gcc`（Cortex-M3）
- **输出目录**：`build/`，产物为 `ai_assistant.elf / .hex / .bin`
- **调试符号**：Makefile 默认已设 `DEBUG = 1`，优化级别 `-Og`
- **字符编码**：编译时加 `-fexec-charset=GBK`，源文件及注释均为 GBK 编码中文
- **DSP 库**：链接 `libarm_cortexM3l_math`（`Drivers/CMSIS/Lib/GCC/`），头文件 `arm_math.h` 需先 `#define ARM_MATH_CM3`
- **链接脚本**：`STM32F103XX_FLASH.ld`

### WebUI 后端

```bash
cd WebUI
pip install -r requirements.txt
python Backend/run.py
```

- **框架**：Flask（多线程模式）+ SQLite（WAL 模式，每线程独立连接）
- **监听端口**：读取 `advanced.service_port`，默认 `5000`
- **配置文件**：`WebUI/config/default_config.json`（已 gitignore，首次运行自动从 `default_config.example.json` 复制）
- **数据库**：`WebUI/data/assistant.db`（首次运行自动创建）
- **音频文件**：设备上传的录音保存到 `WebUI/data/audio/`，TTS 合成音频保存到 `WebUI/data/tts/`

---

## 代码架构

### 固件目录结构

```
Core/
├── Inc/
│   ├── audio/          # 音频子系统（i2s_mic, wav_stream, audio_buffer, 处理流水线）
│   ├── bsp/            # 板级支持（bsp_key, bsp_spi_flash, button_event）
│   ├── button/         # 主应用逻辑（button_app, button_context 及各功能子模块）
│   ├── cloud/          # 云端通信协议（cloud_comm — 遗留，业务层已迁移到 button/）
│   ├── debug/          # 调试串口（debug_uart）
│   ├── lcd/            # LCD 驱动与显示（lcd, font, display）
│   ├── system/         # 状态机与控制器（system_state, system_controller — 遗留）
│   └── wifi/           # ESP8266 驱动与配置（esp8266, wifi_config.h）
├── Src/                # 与 Inc/ 结构对应的实现文件
└── Startup/            # 启动代码与链接脚本
```

> **重要**：项目的实际主循环和业务逻辑在 `Core/Src/button/` 下，而非 `Core/Src/system/`。`system_state.c/h` 和 `system_controller.c/h` 是早期遗留模块，当前已不再驱动运行时行为。

### 主应用模块（`Core/button/`）

这是固件的真正核心，`main.c` 调用 `button_app_init()` 初始化外设，然后进入 `button_app_run()` 无限主循环。

**`button_context.h`** — 全局状态与常量，被所有 button/ 子模块共享：

```c
typedef enum {
    STATE_IDLE = 0,      /* 待机 */
    STATE_RECORDING,     /* 录音中（K1 按住） */
    STATE_REC_PAUSED,    /* 录音暂停（K1 松开，等待发送或取消） */
    STATE_WAITING,       /* 已发送，等待 AI 回复 */
    STATE_HISTORY        /* 历史记录列表浏览模式 */
} sys_state_t;

typedef enum {
    KEY_EVENT_NONE = 0,
    KEY_EVENT_K1_PRESS,
    KEY_EVENT_K1_RELEASE,
    KEY_EVENT_K2_SHORT,
    KEY_EVENT_K2_LONG,
    KEY_EVENT_K2_DOUBLE
} key_event_t;
```

主要 button/ 子模块：

| 文件 | 职责 |
|------|------|
| `button_app.c` | 初始化入口 + 主循环（Wi-Fi 状态机、心跳、WAV 播放调度） |
| `button_state_handlers.c` | 各 `sys_state_t` 状态的按键事件处理（`handle_idle/recording/rec_paused/history`） |
| `button_tcp.c` | TCP 下行协议解析（`handle_tcp_downlink`）、心跳就绪判断 |
| `button_audio_upload.c` | 录音数据下采样（×4）、增益处理、上传到 ESP8266 发送队列 |
| `button_audio_buffer.c` | 上传用环形缓冲区（8192 样本） |
| `button_key.c` | K2 双击/长按/短按事件检测 |
| `button_mic_test.c` | MIC_TEST 探测流程与 MIC_REC 录音测试 |
| `button_speaker_test.c` | SPK_TEST/TONE/SWEEP/VOLUME/ODE 扬声器测试回调 |

### 按键交互逻辑

**主对话流程：**
- **K1 按住（待机/暂停）**：开始/继续录音 → 发送 `REC_START` → 实时上传 PCM 音频
- **K1 松开**：暂停录音，提示可发送或取消
- **K2 短按（暂停时）**：刷完缓冲区 → 发送 `REC_END` → 进入 `STATE_WAITING`
- **K2 长按（暂停时）**：取消录音 → 发送 `REC_CANCEL` → 返回待机
- **K2 短按（待机时）**：新建会话 → 清空消息 → 发送 `NEW_SESSION`
- **K2 短按（等待时）**：中断等待，返回待机
- **K2 双击（待机时）**：进入历史列表 → 发送 `GET_HISTORY`

**历史列表模式：**
- K1 按下：列表上移
- K2 短按：列表下移
- K2 双击：加载选中会话 → 发送 `LOAD_SESSION:<index>`

### 音频子系统（`Core/audio/`）

**采样流程（上行）：**

1. I2S2 Master RX + DMA 双缓冲，采样率 **32kHz**，16bit 单声道
2. DMA 半满/全满中断触发 `on_audio_data()` 回调
3. 在 `button_audio_upload.c` 中 **4倍下采样**（累加平均），上传实际约 **8kHz** PCM
4. 下采样后经可配置增益（默认 Q8 格式 512，即 ×2）写入 8192 样本的环形缓冲区
5. `audio_upload_poll()` 从缓冲区读取 1024 样本块，通过 `esp8266_tcp_send_async()` 发送

**播放流程（下行，WAV 流式）：**

1. 服务器发送 `SPK_WAV:<pcm_size>:<sample_rate>` 控制帧
2. `i2s_mic_play_wav_stream(sample_rate)` 将 I2S2 切换到 TX 模式，控制 PB14 切换总线到扬声器
3. `wav_stream_init()` 初始化 4KB 环形缓冲区，`wav_stream_service()` 在主循环中持续从 TCP 接收 PCM 数据填充缓冲区
4. 预缓冲达 50%（2KB）后启动 DMA Circular 播放
5. DMA ISR 调用 `wav_stream_dma_service_half()` 从环形缓冲区补充数据
6. 播放完成后发送 `WAV_DONE` 给服务器

**音频处理流水线（`audio_processor.c`，DSP 库，主要在 MIC_REC 测试流程中使用）：**
- 预加重滤波（pre-emphasis）
- 高通滤波（去除直流）
- 噪声门（noise gate）
- AGC（自动增益控制，峰值/RMS 两种模式）
- VAD（能量域语音活动检测，自适应噪声底）
- 配置预设：默认 / 安静环境 / 嘈杂环境 / 语音识别优化 / 音乐录制

### ESP8266 Wi-Fi 驱动（`Core/wifi/esp8266.c`）

完全非阻塞设计，状态机包含：
- `ESP8266_STATUS_IDLE → INITIALIZING → READY → CONNECTING_WIFI → WIFI_CONNECTED → WIFI_GOT_IP → TCP_CONNECTED`
- 发送队列（容量 4 项，单包最大 2048 字节），队列类型分 TCP 原始流、TCP 文本帧、AT 指令
- 接收：UART 中断缓冲 → `esp8266_tcp_read_line()` / `esp8266_tcp_read_raw()` 供主循环消费
- 心跳：每 15 秒在 TCP 空闲时发送 `HB\n`

**硬件引脚：**

| 信号 | 引脚 |
|------|------|
| UART TX | PB10 |
| UART RX | PB11 |
| CH_PD（使能） | PB8 |
| RST（复位） | PB9 |
| 波特率 | 460800（备用 115200） |

### LCD 显示（`Core/lcd/display.c`）

ChatGPT 风格聊天界面，分辨率 240×320：

| 区域 | Y 坐标 | 内容 |
|------|--------|------|
| 状态栏 | 0–23 | Wi-Fi 状态 + 会话标题 |
| 聊天消息区 | 24–279 | 消息气泡（最多缓存 20 条，环形覆盖） |
| 底部操作栏 | 280–319 | 录音状态 / 按键提示 |

界面状态：`DISPLAY_STATE_CHAT / RECORDING / HISTORY / CHAT_SCROLL / DEBUG_LOG`

消息角色：`MSG_ROLE_USER / ASSISTANT / SYSTEM`，消息最大长度 128 字节（GBK 编码）

**TCP 下行协议**（`button_tcp.c` 的 `handle_tcp_downlink()` 解析）：

| 帧前缀 | 含义 | 处理 |
|--------|------|------|
| `STT_PART:<text>` | 实时 ASR 中间结果 | 刷新最后一条用户气泡 |
| `STT_FINAL:<text>` | 实时 ASR 最终结果 | 更新最后一条用户气泡 |
| `STT:<text>` | 非流式 ASR 识别结果 | 追加用户气泡 |
| `AI:<text>` | LLM 回复文本（分块下发） | 追加助手气泡，返回待机 |
| `SYS:<text>` | 系统提示 | 居中小字提示 |
| `TITLE:<text>` | 更新会话标题 | 刷新状态栏标题 |
| `SPK_WAV:<size>:<rate>` | 启动 WAV 流式播放 | 初始化 wav_stream |
| `SPK_TONE:<freq>:<ms>` | 播放测试音 | I2S2 合成单音 |
| `SPK_SWEEP:<f1>:<f2>:<ms>` | 播放频率扫描 | I2S2 线性扫频 |
| `SPK_VOLUME:<v1>,<v2>,...` | 播放分级音量测试 | I2S2 多级音量 1kHz 音 |
| `SPK_ODE` | 播放欢乐颂旋律 | I2S2 实时合成 |
| `MIC_TEST` | 麦克风探测 | 短暂采集检查非零样本 |
| `MIC_REC:<sec>` | 录音测试（指定秒数） | 采集后发送 `MIC_REC_DONE:sr=<rate>` |

**TCP 上行协议**（设备发送给服务器）：

| 帧 | 含义 |
|----|------|
| `REC_START\n` | 开始录音，服务器启动音频接收 |
| `REC_END\n` | 录音结束（K2 短按发送），触发 ASR + LLM 处理 |
| `REC_CANCEL\n` | 取消录音 |
| `NEW_SESSION\n` | 新建对话会话 |
| `GET_HISTORY\n` | 请求历史会话列表 |
| `LOAD_SESSION:<n>\n` | 加载第 n 条历史会话 |
| `HB\n` | 心跳（每 15 秒），服务器回 `OK\n` |
| `MIC_REC_DONE:sr=<rate>\n` | 录音测试完成，携带实际采样率 |
| `SPK_TEST_OK\n` / `SPK_TONE_DONE\n` 等 | 扬声器测试完成响应 |
| `WAV_DONE\n` | WAV 流式播放完成 |

---

## WebUI 架构

```
WebUI/
├── Backend/
│   ├── api/                  # REST 蓝图
│   │   ├── auth_routes.py    # 登录/登出（JWT）
│   │   ├── chat_routes.py    # 对话接口
│   │   ├── config_routes.py  # 配置读写
│   │   ├── conversation_routes.py  # 历史会话管理
│   │   ├── device_routes.py  # 设备状态查询
│   │   ├── mic_test_routes.py     # 麦克风测试
│   │   ├── speaker_test_routes.py # 扬声器测试
│   │   ├── security_routes.py     # IP 封禁管理
│   │   └── user_routes.py         # 用户管理
│   ├── auth/                 # JWT 认证（bcrypt 密码哈希）
│   ├── config/config.py      # 线程安全 JSON 配置（点分路径读写）
│   ├── database/db.py        # SQLite（WAL 模式，会话/消息/IP 封禁）
│   ├── security/             # 速率限制（60 次/分钟）
│   ├── services/
│   │   ├── device_manager.py       # TCP 服务器（端口 8266），管理 STM32 连接
│   │   ├── chat_service.py         # LLM 调用（OpenAI 兼容接口）
│   │   ├── speech_service.py       # ASR（Whisper / 腾讯云一句话识别）
│   │   ├── tencent_streaming_asr.py # 腾讯云实时流式 ASR（WebSocket）
│   │   ├── tts_service.py          # TTS（Azure TTS，输出 16kHz WAV）
│   │   └── async_job_manager.py    # 异步任务队列（线程池，最多 4 工作线程）
│   ├── app.py                # Flask 应用工厂（蓝图注册、安全中间件）
│   └── run.py                # 启动入口
├── Frontend/                 # Vue.js SPA（无构建步骤，CDN 加载）
│   ├── js/api.js             # 统一 API 调用封装
│   ├── js/app.js             # Vue 应用引导
│   └── js/components/        # LoginPage, StatusPanel, DeviceConfig, LLMConfig,
│                             #   SpeechConfig, TTSConfig, MicTest, SpeakerTest,
│                             #   ConversationHistory, SecurityPanel, UserSettings,
│                             #   AdvancedSettings, SecuritySettings
├── config/
│   ├── default_config.json        # 运行时配置（gitignored）
│   └── default_config.example.json # 配置模板（提交到仓库）
├── data/                     # 运行时数据（gitignored）
│   ├── assistant.db          # SQLite 数据库
│   ├── audio/                # 设备上传录音（.pcm / .wav）
│   └── tts/                  # TTS 合成音频（.wav）
├── constant.py               # 全局常量（版本号、速率限制、登录锁定参数）
└── requirements.txt          # Flask, bcrypt, PyJWT, tencentcloud-sdk-python,
                              #   websocket-client, gunicorn, httpx
```

### 云端处理完整数据流

```
设备采集                 服务器处理
─────────               ───────────────────────────────────────────────────
K1 按住 → REC_START ──→ DeviceManager._start_audio_capture()
                            ↓ 同时启动实时 ASR（腾讯云 WebSocket）
PCM 音频流 ──────────→ DeviceManager._append_audio_bytes()
                            ↓ 每包同步推送给腾讯云 ASR
K2 短按 → REC_END ───→ DeviceManager._finish_audio_capture()
                            ↓ PCM → WAV
                        TencentStreamingAsrSession.finish()  →  识别文本
                            ↓
                        get_llm_reply()  →  OpenAI 兼容接口  →  回复文本
                            ↓
                        send_text_to_device("AI:<text>")  →  TCP 分块下发（每块 ≤96 字节 GBK）
```

### `device_manager.py` 关键设计

- 每个 TCP 连接独立接收线程 + 独立发送线程（通过 `Queue` 解耦，避免阻塞）
- 流模式（`audio_capture_active=True`）：将字节流数据写入 PCM 文件，遇到 `REC_END\n` / `REC_CANCEL\n` / `MIC_REC_DONE:sr=...\n` 后切回文本模式
- 同一 IP 重连时自动踢掉旧连接（`_kick_same_ip`）
- 下发文本按 GBK 编码，单帧不超过 96 字节（STM32 显示缓冲区限制）
- 心跳超时阈值 45 秒（设备心跳周期 15 秒）

---

## 重要配置说明

### `Core/Inc/wifi/wifi_config.h`（**gitignored**）

此文件不提交到仓库，从 `wifi_config.example.h` 复制后修改：

```c
#define WIFI_SSID        "你的热点名"
#define WIFI_PASSWORD    "你的密码"
#define SERVER_IP        "服务器IP地址"
#define SERVER_PORT      8266
```

其余内容（ESP8266 引脚、波特率、超时）通常无需修改。

### `WebUI/config/default_config.json`（**gitignored**）

首次运行自动从 `default_config.example.json` 生成，需填写：

| 配置路径 | 说明 |
|----------|------|
| `speech.provider` | `tencent`（实时流式）或 `openai_whisper` |
| `speech.app_id / secret_id / secret_key` | 腾讯云 ASR 凭据 |
| `llm.base_url / api_key / model` | OpenAI 兼容接口地址和密钥 |
| `tts.azure_region / azure_key / azure_voice` | Azure TTS 凭据 |
| `device.tcp_port` | TCP 监听端口，必须与 `wifi_config.h` 的 `SERVER_PORT` 一致（默认 8266） |
| `device.audio_sample_rate` | I2S 采样率（默认 32000） |
| `device.audio_upload_downsample` | 上传下采样倍数（默认 4，实际上传 8kHz） |
| `advanced.service_port` | WebUI HTTP 端口（默认 5000） |

---

## 代码风格规范

- **文件名**：全小写加下划线（如 `audio_buffer.c`）
- **函数名**：全小写加下划线（如 `i2s_mic_start()`）
- **变量名**：全小写加下划线（如 `wav_stream_active`）
- **常量/宏**：全大写加下划线（如 `WAV_RING_BUF_SIZE`）
- **注释**：必须用中文；不得看起来像 AI 自动生成
- **模块化**：每个功能模块放独立文件夹/文件，单文件不得过长

---

## 开发环境（Windows）

### 前提条件

1. **ARM GCC 工具链**：GNU Arm Embedded Toolchain（`arm-none-eabi-gcc`）
2. **Make**：MSYS2、Chocolatey 或 Cygwin 提供
3. **Python 3.8+**：用于 WebUI 后端
4. **VSCode 扩展**：C/C++、Cortex-Debug、Python、Makefile Tools

### 编译固件

```bash
make clean && make
```

### 烧写到开发板

- **ST-Link**：STM32CubeProgrammer 或 OpenOCD
- **串口 ISP**：FlyMCU，BOOT0 拨到高电平
- **VSCode 调试**：Cortex-Debug + ST-Link

### 启动 WebUI

```bash
cd WebUI
pip install -r requirements.txt
python Backend/run.py
```

详细 Windows 安装步骤见 `WINDOWS_SETUP.md`（已 gitignore，不在仓库中）。
