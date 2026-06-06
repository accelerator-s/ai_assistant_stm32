# STM32 语音交互助手

基于野火 STM32F103VET6 开发板实现的脱机独立语音对话设备。终端负责音频采集、外放与屏幕 UI 渲染，语音识别、大模型对话、语音合成及历史记录管理等计算密集型任务通过 Wi-Fi 网络透传至云端代理服务器处理。

## 硬件组成

| 模块   | 型号 / 说明                                                          |
| ------ | -------------------------------------------------------------------- |
| 主控   | 野火 STM32F103VET6，配置 FSMC 接口 LCD 彩屏与外部 SPI Flash 中文字库 |
| 网络   | ESP8266 串口 Wi-Fi 模块（已焊接在开发板，USART3，460800 baud）       |
| 麦克风 | INMP441 全向数字麦克风，I2S 接口                                     |
| 扬声器 | MAX98357A 数字功放模块 + 3W 小型扬声器                               |
| 按键   | 开发板自带 K1、K2 两个物理按键                                       |

## 代码模块说明

### 固件（`Core/`）

- **bsp**：板级支持包，包含 SPI Flash 驱动、按键驱动（`bsp_key`）及按键事件识别（`button_event`）。按键事件层支持短按、长按、双击、持续按住与释放五种事件类型，消抖由定时器驱动。
- **wifi**：ESP8266 驱动与配置。`esp8266` 以非阻塞状态机方式管理全部 AT 指令序列，负责 Wi-Fi 接入、TCP 连接建立、数据发送与接收。`wifi_config.h` 集中存放 SSID、密码、服务器 IP 与端口等部署参数。
- **audio**：音频处理流水线。`i2s_mic` 和 `microphone` 负责 I2S DMA 双缓冲采集；`audio_buffer` 管理音频帧队列；`audio_processor`、`audio_quality` 和 `dsp_config` 基于 CMSIS-DSP Q15 定点运算实现预加重、高通滤波、VAD（能量自适应静音检测）、AGC（自动增益控制）与噪声门限。音频处理预设存储于外部 SPI Flash，支持掉电保留。
- **lcd**：LCD 驱动与显示逻辑。`lcd` 为底层 FSMC 驱动，`display` 负责上层 UI 绘制，包括对话气泡、录音动效、历史列表、调试日志滚动显示与滚动条渲染。
- **cloud**：云端通信协议。`cloud_comm` 封装会话管理、音频帧流式上传、控制帧发送（新会话、取消、载入历史）及心跳维持（每 15 秒）。录音结束帧携带采样率信息（`REC_END:sr=<采样率>`），避免上下位机采样率不一致导致音频速率错误。上行音频发送完毕后，云端通过同一 TCP 连接流式回传 ASR 识别文本，开发板可在识别完成前逐步显示转写结果；下行云端同步流式下发 LLM 回复文本与 TTS 合成音频，开发板边接收文本边刷新 LCD，边接收音频边驱动扬声器外放，两路数据通过帧类型标识区分。
- **system**：系统层。`system_state` 定义全局状态枚举与运行上下文；`system_controller` 协调按键事件、麦克风、云端通信与显示刷新的主循环逻辑。
- **debug**：调试串口工具，通过 UART 输出格式化日志。

### WebUI 后端（`WebUI/Backend/`）

基于 Flask 构建，提供 REST API 与 TCP 设备服务。

- **services**：核心服务层。`device_manager` 开启 TCP 服务器（默认端口 8266）接受设备连接，管理音频流接收与控制指令分发；`chat_service` 协调 ASR 流式识别 → 识别文本回传 → LLM → TTS 全流程，ASR 识别结果实时流式推送至设备，LLM 回复文本与 TTS 合成音频并行流式下发；`speech_service` 封装腾讯云 ASR 与 TTS 接口调用；`async_job_manager` 管理异步任务队列。
- **api**：REST 接口蓝图，覆盖设备管理、对话历史、配置读写、用户认证、安全管理及麦克风/扬声器测试。
- **auth**：JWT 鉴权与 bcrypt 密码哈希。
- **database**：SQLite（WAL 模式，线程隔离连接），存储会话与消息记录，支持 IP 封禁。
- **security**：请求频率限制（60 次/分钟）与 IP 封禁中间件。
- **config**：线程安全 JSON 配置管理，支持点路径访问（如 `llm.base_url`）。

### WebUI 前端（`WebUI/Frontend/`）

Vue.js 单页应用，通过 CDN 引入，无需构建。包含登录、设备状态面板、对话历史、LLM/语音/高级参数配置及麦克风测试等页面组件。

---

## 快速开始

### 1. 配置固件部署参数

修改 `Core/Inc/wifi/wifi_config.h`，填写实际的 Wi-Fi 热点名称、密码及后端服务器地址：

```c
#define WIFI_SSID     "你的WiFi名称"
#define WIFI_PASSWORD "你的WiFi密码"
#define SERVER_IP     "运行WebUI的机器IP"
#define SERVER_PORT   8266
```

### 2. 配置 WebUI 后端

将 `WebUI/config/default_config.example.json` 复制为 `WebUI/config/default_config.json`，按实际情况填写各字段：

```json
{
  "device": {
    "tcp_port": 8266,
    "audio_sample_rate": 32000
  },
  "speech": {
    "provider": "tencent",
    "secret_id": "腾讯云 SecretId",
    "secret_key": "腾讯云 SecretKey"
  },
  "llm": {
    "base_url": "大模型 API 地址",
    "api_key": "大模型 API Key",
    "model": "gpt-4"
  },
  "tts": {
    "provider": "openai_tts",
    "base_url": "TTS API 地址",
    "api_key": "TTS API Key"
  },
  "advanced": {
    "service_port": 5000
  }
}
```

> `default_config.json` 已被 `.gitignore` 排除，不会提交到仓库。`tcp_port` 须与固件中 `SERVER_PORT` 保持一致。

### 3. 启动 WebUI 后端

```bash
cd WebUI
pip install -r requirements.txt
python Backend/run.py
```

启动后通过 `http://localhost:5000`（或配置中指定的端口）访问管理界面。

### 4. 构建并烧录固件

```bash
# 在项目根目录执行
make -j$(nproc)
```

构建产物位于 `build/` 目录，生成 `ai_assistant.elf`、`.hex` 与 `.bin` 三种格式。使用 STM32CubeProgrammer 或 FlyMcu 将 `.hex` / `.bin` 烧录至开发板。

---

## 系统数据流

```
上行（录音 + 识别结果回传）
  INMP441 → I2S DMA → audio_buffer → USART3 → ESP8266 → TCP 流式上传 → 云端服务器
    └─ 云端：持久化存储用户录音文件
           → ASR 流式识别 → 识别文本流式回传 → 开发板 LCD 实时显示
           → 识别完成 → LLM → 生成回复文本 → 持久化存储对话文字

下行（文本 + 语音同步流式回传，设备仅播放不存储）
  云端服务器 → TTS 合成音频（持久化存储）→ TCP 同步流式下发 → ESP8266 → STM32
    ├─ 文本流 → LCD 实时显示对话气泡（不在设备端持久化）
    └─ TTS 音频流 → MAX98357A → 扬声器实时外放（播放一次，不在设备端存储）

WebUI（独立于设备，直接读取后端数据库与存储）
  每轮对话：用户文字（ASR 识别结果）+ 用户录音音频可播放
           AI 回复文字 + TTS 合成音频可播放
```

## 终端状态机设计

系统采用事件驱动与状态跳转机制，核心状态定义及流转条件设计如下：

* **`STATE_IDLE` (待机状态)**
  * **职责**：维持 TCP 心跳，等待用户发起新对话或进行菜单切换。
  * **转移**：K1 按下触发进入 `STATE_RECORDING`；云端下发主动播报音频时进入 `STATE_PLAYING`；K2 双/长按进入浏览模式。
* **`STATE_RECORDING` (录音中)**
  * **职责**：启动 I2S DMA 采集，驱动 LCD 录音波形动效，向上推流。
  * **转移**：K1 释放时进入 `STATE_REC_PAUSED`。
* **`STATE_REC_PAUSED` (录音分段暂停)**
  * **职责**：支持多次按住中断以无缝拼接语音，暂停推流（不发送结束帧）。
  * **转移**：K1 再次按下返回 `STATE_RECORDING`；K2 短按发送录音完毕信号并转入 `STATE_WAITING`；K2 长按发送清空指令并退回 `STATE_IDLE`。
* **`STATE_WAITING` (执行与生成等待)**
  * **职责**：等待云端流式传回 ASR 文本并追加渲染屏幕。
  * **转移**：收到云端下行 TTS 音频首帧跳转至 `STATE_PLAYING`；若超时、网络错误或 K2 中止，则退回 `STATE_IDLE`。
* **`STATE_PLAYING` (流式响应播放)**
  * **职责**：处理下行文本与音频交织流。音频数据送外放，文本同步呈现并触发布局滚动。
  * **转移**：音频队列清空且收到结束标识时退回 `STATE_IDLE`；可被 K1 强行打断录音（跳转至 `STATE_RECORDING`）。
* **辅助浏览状态 (`STATE_CHAT_SCROLL` / `STATE_HISTORY` / `STATE_DEBUG_LOG`)**
  * **职责**：UI 交互模式，拦截无关操作。
  * **转移**：通过特定按键退回 `STATE_IDLE`。

## 按键交互设计

所有交互由 K1、K2 两键完成，底层支持短按、长按、双击事件。

### 主对话界面

| 操作                  | 动作                               |
| --------------------- | ---------------------------------- |
| K1 按住               | 开始录音，音频实时上传云端         |
| K1 松开               | 暂停录音（可多次按住实现分段拼接） |
| K2 短按（录音暂停时） | 确认发送，通知云端开始处理         |
| K2 长按（录音暂停时） | 取消本次录音，清空云端缓存         |
| K2 短按（待机时）     | 新建会话，归档上一轮对话           |
| K2 双击（待机时）     | 进入聊天滚动浏览模式               |
| K2 长按（待机时）     | 进入调试日志模式                   |
| K2 短按（等待时）     | 退回待机界面                       |

### 聊天滚动浏览模式

| 操作    | 动作                 |
| ------- | -------------------- |
| K1 按下 | 向上翻页（更早消息） |
| K2 短按 | 向下翻页（更新消息） |
| K2 双击 | 切换到历史会话列表   |
| K2 长按 | 返回主对话待机       |

### 历史会话列表模式

| 操作    | 动作                         |
| ------- | ---------------------------- |
| K1 短按 | 光标上移                     |
| K2 短按 | 光标下移                     |
| K2 双击 | 载入选中会话，返回主对话界面 |

### 调试日志模式

| 操作    | 动作                 |
| ------- | -------------------- |
| K1 按下 | 向上滚动             |
| K2 短按 | 向下滚动             |
| K2 长按 | 退出，返回主对话待机 |