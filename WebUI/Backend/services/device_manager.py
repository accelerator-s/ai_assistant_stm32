"""设备通信管理器 — 管理 TCP 连接的 ESP8266 终端设备

通信协议:
  - 设备每 15 秒发送 "HB\n" 心跳包
  - 服务端回复 "OK\n" 确认
  - 超过 45 秒未收到任何数据视为断线
  - 同一 IP 新连接到来时自动踢掉旧连接（避免重复计数）
"""

import logging
import socket
import threading
import time
import uuid
import wave
from pathlib import Path
from collections import deque
from queue import Empty, Queue

logger = logging.getLogger(__name__)

# 心跳超时阈值（秒）：设备心跳为 15s，超时必须大于该周期
HEARTBEAT_TIMEOUT = 45

# recv 超时（秒）：用于周期性检查心跳是否过期
RECV_TIMEOUT = 10

# Keep each downlink text frame below the STM32 line buffer (160 bytes) and
# display message buffer (128 bytes). The payload is GBK-encoded before send.
DEVICE_TEXT_FRAME_BYTES = 96
DEVICE_TEXT_FRAME_GAP_SEC = 0.02


class DeviceConnection:
    """单个设备 TCP 连接"""

    def __init__(self, conn: socket.socket, addr: tuple):
        self.conn = conn
        self.addr = addr
        self.connected_at = time.time()
        self.last_active = time.time()
        self._line_buf = bytearray()
        self._line_queue = deque(maxlen=128)
        self._line_cv = threading.Condition()
        self.send_queue: Queue[bytes | None] = Queue(maxsize=256)
        self.sender_thread: threading.Thread | None = None
        self._stream_buf = bytearray()
        self.audio_capture_active = False
        self.audio_pcm_path: Path | None = None
        self.audio_bytes = 0
        self.audio_started_at = 0.0
        self.audio_asr_session = None

    def __repr__(self):
        return f"<DeviceConnection {self.addr[0]}:{self.addr[1]}>"

    def push_recv_data(self, data: bytes) -> list[str]:
        """将接收字节流按换行切分为文本帧，返回本次新增的文本行。"""
        lines: list[str] = []
        self._line_buf.extend(data)
        while True:
            sep = self._line_buf.find(b"\n")
            if sep < 0:
                break
            raw = self._line_buf[:sep]
            del self._line_buf[: sep + 1]
            line = raw.replace(b"\r", b"").decode("utf-8", errors="ignore").strip()
            if line:
                lines.append(line)
        if lines:
            with self._line_cv:
                for line in lines:
                    self._line_queue.append(line)
                self._line_cv.notify_all()
        return lines

    def clear_lines(self) -> None:
        with self._line_cv:
            self._line_queue.clear()

    def wait_line(self, timeout: float) -> str | None:
        end = time.time() + timeout
        with self._line_cv:
            while not self._line_queue:
                remaining = end - time.time()
                if remaining <= 0:
                    return None
                self._line_cv.wait(timeout=remaining)
            return self._line_queue.popleft()


class DeviceManager:
    """TCP 服务器: 接受 ESP8266 设备连接，管理已连接的终端"""

    def __init__(self, host: str = "0.0.0.0", port: int = 8266):
        self._host = host
        self._port = port
        self._server_socket: socket.socket | None = None
        self._clients: dict[str, DeviceConnection] = {}
        self._lock = threading.Lock()
        self._running = False
        self._accept_thread: threading.Thread | None = None
        self._waiters: dict[str, dict[str, object]] = {}
        self._waiters_lock = threading.Lock()
        self._audio_dir = (
            Path(__file__).resolve().parent.parent.parent / "data" / "audio"
        )
        self._audio_dir.mkdir(parents=True, exist_ok=True)
        self._latest_audio: dict[str, object] | None = None
        self._audio_lock = threading.Lock()

    # ------------------------------------------------------------------
    # 启动与停止
    # ------------------------------------------------------------------
    def start(self) -> None:
        """启动 TCP 服务器监听"""
        if self._running:
            return
        self._running = True
        self._server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._server_socket.settimeout(1.0)
        self._server_socket.bind((self._host, self._port))
        self._server_socket.listen(5)

        self._accept_thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._accept_thread.start()
        logger.info(f"TCP 设备服务器已启动: {self._host}:{self._port}")

    def stop(self) -> None:
        """停止 TCP 服务器"""
        self._running = False
        if self._server_socket:
            try:
                self._server_socket.close()
            except Exception:
                pass
        with self._lock:
            for key, client in self._clients.items():
                try:
                    client.send_queue.put_nowait(None)
                except Exception:
                    pass
                try:
                    client.conn.close()
                except Exception:
                    pass
            self._clients.clear()
        logger.info("TCP 设备服务器已停止")

    # ------------------------------------------------------------------
    # 连接状态查询
    # ------------------------------------------------------------------
    def has_connections(self) -> bool:
        """是否有设备连接"""
        with self._lock:
            return len(self._clients) > 0

    def is_running(self) -> bool:
        """TCP 设备服务器是否正在运行。"""
        return self._running and self._server_socket is not None

    def client_count(self) -> int:
        """已连接的设备数量"""
        with self._lock:
            return len(self._clients)

    def get_clients_info(self) -> list[dict]:
        """已连接设备信息列表"""
        with self._lock:
            return [
                {
                    "address": f"{c.addr[0]}:{c.addr[1]}",
                    "connected_at": c.connected_at,
                    "last_active": c.last_active,
                }
                for c in self._clients.values()
            ]

    def manual_heartbeat_probe(self) -> dict:
        """手动心跳探测：用于前端“测试设备连接/刷新状态”按钮触发。

        逻辑：
        1. 先清理心跳超时连接；
        2. 对剩余连接发送轻量探测帧 PING；
        3. 发送失败则立即剔除连接。
        """
        now = time.time()
        timeout_keys: list[str] = []
        send_fail_keys: list[str] = []

        with self._lock:
            items = list(self._clients.items())

            for key, client in items:
                if now - client.last_active > HEARTBEAT_TIMEOUT:
                    timeout_keys.append(key)
                    continue
                try:
                    client.conn.sendall(b"PING\n")
                except Exception:
                    send_fail_keys.append(key)

            for key in timeout_keys + send_fail_keys:
                client = self._clients.pop(key, None)
                if client:
                    try:
                        client.conn.close()
                    except Exception:
                        pass

            alive = len(self._clients)

        if timeout_keys:
            logger.warning(f"手动探测清理超时连接: {timeout_keys}")
        if send_fail_keys:
            logger.warning(f"手动探测清理发送失败连接: {send_fail_keys}")

        return {
            "alive": alive,
            "timeout_removed": len(timeout_keys),
            "send_fail_removed": len(send_fail_keys),
        }

    def send_command(
        self, command: str, client: DeviceConnection | None = None
    ) -> bool:
        """向当前活跃设备发送一条命令（自动补换行，非阻塞入队）。"""
        client = client or self._get_primary_client()
        if not client:
            logger.warning("设备下发失败: no active client, command=%r", command[:48])
            return False

        payload = command if command.endswith("\n") else f"{command}\n"
        try:
            # The STM32 LCD renderer indexes a GB2312/GBK font from external flash.
            # Keep ASCII protocol prefixes unchanged, but encode Chinese payloads as
            # GBK so the firmware does not interpret UTF-8 byte triples as GBK pairs.
            encoded = payload.encode("gbk", errors="replace")
            client.send_queue.put_nowait(encoded)
            logger.info(
                "设备下发入队: %s bytes=%d command=%r",
                client,
                len(encoded),
                command[:48],
            )
            return True
        except Exception:
            logger.exception("设备下发入队失败: %s command=%r", client, command[:48])
            self._remove_client(client)
            return False

    def send_command_async(self, command: str) -> bool:
        """兼容显式异步调用，实际与 send_command 一样走发送队列。"""
        return self.send_command(command)

    def send_raw_async(self, payload: bytes) -> bool:
        """向当前活跃设备异步发送原始字节数据。"""
        client = self._get_primary_client()
        if not client:
            return False

        try:
            client.send_queue.put_nowait(payload)
            return True
        except Exception:
            self._remove_client(client)
            return False

    @staticmethod
    def _split_device_text(
        text: str, max_bytes: int = DEVICE_TEXT_FRAME_BYTES
    ) -> list[str]:
        chunks: list[str] = []
        current: list[str] = []
        current_len = 0

        for ch in text:
            encoded_len = len(ch.encode("gbk", errors="replace"))
            if current and current_len + encoded_len > max_bytes:
                chunks.append("".join(current))
                current = []
                current_len = 0
            current.append(ch)
            current_len += encoded_len

        if current:
            chunks.append("".join(current))
        return chunks

    def send_text_to_device(
        self,
        text: str,
        role: str = "assistant",
        client: DeviceConnection | None = None,
    ) -> bool:
        """下发文本到设备显示。

        协议约定：
        - role=user      -> STT:<文本>
        - role=assistant -> AI:<文本>
        - 其他           -> SYS:<文本>
        """
        if not text:
            return False

        normalized = text.replace("\r", " ").replace("\n", " ").strip()
        if not normalized:
            return False

        if role == "user":
            prefix = "STT:"
        elif role == "user_partial":
            prefix = "STT_PART:"
        elif role == "user_final":
            prefix = "STT_FINAL:"
        elif role == "assistant":
            prefix = "AI:"
        else:
            prefix = "SYS:"

        chunks = (
            self._split_device_text(normalized)
            if role == "assistant" or prefix == "SYS:"
            else [normalized]
        )

        ok = True
        for idx, chunk in enumerate(chunks):
            ok = self.send_command(f"{prefix}{chunk}", client=client) and ok
            if idx + 1 < len(chunks):
                time.sleep(DEVICE_TEXT_FRAME_GAP_SEC)
        return ok

    def wait_response(
        self, command: str, expected: str, timeout: float = 3.0
    ) -> str | None:
        """发送命令并等待包含 expected 的设备文本响应。"""
        waiter = self.register_waiter(expected=expected, timeout=timeout)
        if not self.send_command(command):
            self.cancel_waiter(waiter["waiter_id"])
            return None
        return self.await_waiter(waiter["waiter_id"], timeout=timeout)

    def register_waiter(self, expected: str, timeout: float = 3.0) -> dict[str, object]:
        """注册一个异步响应等待器。"""
        waiter_id = f"waiter-{time.time_ns()}"
        waiter = {
            "waiter_id": waiter_id,
            "expected": expected,
            "event": threading.Event(),
            "response": None,
            "created_at": time.time(),
            "timeout": timeout,
        }
        with self._waiters_lock:
            self._waiters[waiter_id] = waiter
        return waiter

    def await_waiter(self, waiter_id: str, timeout: float | None = None) -> str | None:
        """阻塞等待某个等待器完成；供后台线程调用，避免在请求线程直接做设备 IO。"""
        with self._waiters_lock:
            waiter = self._waiters.get(waiter_id)
            if not waiter:
                return None

        actual_timeout = timeout if timeout is not None else float(waiter["timeout"])
        event = waiter["event"]
        event.wait(timeout=actual_timeout)

        with self._waiters_lock:
            waiter = self._waiters.pop(waiter_id, None)
            if not waiter:
                return None
            return waiter["response"]

    def cancel_waiter(self, waiter_id: str) -> None:
        """取消并移除等待器。"""
        with self._waiters_lock:
            waiter = self._waiters.pop(waiter_id, None)
            if waiter:
                waiter["event"].set()

    def _notify_waiters(self, line: str) -> None:
        """将收到的文本行分发给匹配的异步等待器。"""
        with self._waiters_lock:
            for waiter in self._waiters.values():
                expected = waiter["expected"]
                if expected in line:
                    waiter["response"] = line
                    waiter["event"].set()

    def get_latest_audio(self) -> dict[str, object] | None:
        """获取最近一次接收完成的音频信息。"""
        with self._audio_lock:
            if not self._latest_audio:
                return None
            return dict(self._latest_audio)

    def _get_audio_sample_rate(self) -> int:
        try:
            from ..app import app_state

            config = app_state.get("config")
            if config:
                src_rate = int(config.get("device.audio_sample_rate", 16000))
                downsample = int(config.get("device.audio_upload_downsample", 2))
                if downsample <= 0:
                    downsample = 1
                return max(1000, src_rate // downsample)
        except Exception:
            pass
        return 8000

    def _start_audio_capture(self, device: DeviceConnection) -> None:
        if device.audio_capture_active:
            return

        name = f"rec_{int(time.time())}_{uuid.uuid4().hex[:8]}"
        pcm_path = self._audio_dir / f"{name}.pcm"
        device.audio_pcm_path = pcm_path
        device.audio_capture_active = True
        device.audio_bytes = 0
        device.audio_started_at = time.time()

        with self._audio_lock:
            self._latest_audio = None

        logger.info("开始接收设备音频流: %s", pcm_path.name)
        self._start_streaming_asr(device)

    def _start_streaming_asr(self, device: DeviceConnection) -> None:
        try:
            from ..app import app_state
            from .tencent_streaming_asr import TencentStreamingAsrSession

            config = app_state.get("config")
            provider = (config.get("speech.provider", "") if config else "").lower()
            if provider not in {"tencent", "tencent_asr"}:
                device.audio_asr_session = None
                return

            def on_text(text: str, is_final: bool) -> None:
                self.send_text_to_device(
                    text,
                    role="user_final" if is_final else "user_partial",
                    client=device,
                )

            session = TencentStreamingAsrSession(
                config=config,
                sample_rate=self._get_audio_sample_rate(),
                on_text=on_text,
            )
            if session.start():
                device.audio_asr_session = session
                logger.info("腾讯云实时 ASR 会话已建立")
            else:
                device.audio_asr_session = None
                if session.error:
                    self.send_text_to_device(session.error, role="system", client=device)
        except Exception:
            device.audio_asr_session = None
            logger.exception("启动腾讯云实时 ASR 失败")
            self.send_text_to_device("启动腾讯云实时识别失败", role="system", client=device)

    def _append_audio_bytes(self, device: DeviceConnection, payload: bytes) -> None:
        if not device.audio_capture_active:
            return
        if not payload:
            return
        if not device.audio_pcm_path:
            return
        try:
            with open(device.audio_pcm_path, "ab") as f:
                f.write(payload)
            device.audio_bytes += len(payload)
            session = device.audio_asr_session
            if session:
                session.send_audio(payload)
        except Exception:
            logger.exception("写入音频分片失败")

    def _finish_audio_capture(
        self, device: DeviceConnection, sample_rate_override: int | None = None
    ) -> dict[str, object] | None:
        if not device.audio_capture_active:
            return None

        device.audio_capture_active = False
        pcm_path = device.audio_pcm_path
        device.audio_pcm_path = None
        asr_session = device.audio_asr_session
        device.audio_asr_session = None

        if not pcm_path or not pcm_path.exists():
            if asr_session:
                asr_session.close()
            return None

        if device.audio_bytes <= 0:
            try:
                pcm_path.unlink(missing_ok=True)
            except Exception:
                pass
            if asr_session:
                asr_session.close()
            return None

        wav_path = pcm_path.with_suffix(".wav")
        sample_rate = (
            int(sample_rate_override)
            if sample_rate_override and sample_rate_override > 0
            else self._get_audio_sample_rate()
        )
        try:
            with open(pcm_path, "rb") as src, wave.open(str(wav_path), "wb") as wavf:
                wavf.setnchannels(1)
                wavf.setsampwidth(2)
                wavf.setframerate(sample_rate)
                wavf.writeframes(src.read())
        except Exception:
            logger.exception("PCM 转 WAV 失败")
            return None

        duration = device.audio_bytes / float(sample_rate * 2)
        url = f"/media/{wav_path.name}"
        audio_info = {
            "path": str(wav_path),
            "url": url,
            "bytes": int(device.audio_bytes),
            "duration": duration,
            "sample_rate": sample_rate,
            "created_at": time.time(),
            "asr_session": asr_session,
        }
        with self._audio_lock:
            latest_audio = dict(audio_info)
            latest_audio.pop("asr_session", None)
            self._latest_audio = latest_audio

        logger.info("音频接收完成: %s (%.2fs)", wav_path.name, duration)
        return audio_info

    def _process_completed_recording_async(
        self, device: DeviceConnection, audio_info: dict[str, object]
    ) -> None:
        worker = threading.Thread(
            target=self._process_completed_recording,
            args=(device, audio_info),
            daemon=True,
        )
        worker.start()

    def _process_completed_recording(
        self, device: DeviceConnection, audio_info: dict[str, object]
    ) -> None:
        audio_path = str(audio_info.get("path") or "")
        if not audio_path:
            self.send_text_to_device("录音文件路径无效", role="system", client=device)
            return

        try:
            from ..app import app_state
            from .chat_service import get_llm_reply
            from .speech_service import recognize_audio

            asr_session = audio_info.get("asr_session")
            config = app_state.get("config")
            provider = (config.get("speech.provider", "") if config else "").lower()
            if not asr_session and provider in {"tencent", "tencent_asr"}:
                self.send_text_to_device(
                    "腾讯云实时识别会话未建立，请检查 AppID/网络/密钥配置",
                    role="system",
                    client=device,
                )
                return

            if asr_session:
                recognized_text = (asr_session.finish() or "").strip()
            else:
                recognized_text = (recognize_audio(audio_path) or "").strip()

            if not recognized_text:
                self.send_text_to_device("未识别到有效语音", role="system", client=device)
                return

            error_prefixes = (
                "出错了",
                "未配置",
                "未安装",
                "音频文件",
                "腾讯云语音识别失败",
                "不支持的语音识别服务商",
            )
            if recognized_text.startswith(error_prefixes):
                self.send_text_to_device(recognized_text, role="system", client=device)
                return

            if asr_session and recognized_text != getattr(asr_session, "last_emitted_text", ""):
                self.send_text_to_device(recognized_text, role="user_final", client=device)
            elif not asr_session:
                self.send_text_to_device(recognized_text, role="user", client=device)

            db = app_state.get("db")
            session_id = f"device_{device.addr[0].replace('.', '_')}"
            if db:
                if not db.get_session(session_id):
                    db.create_session(session_id=session_id, title="设备语音会话")
                db.add_message(session_id, "user", recognized_text)

            reply_text = (get_llm_reply(session_id, recognized_text) or "").strip()
            if not reply_text:
                self.send_text_to_device("大模型未返回有效回复", role="system", client=device)
                return

            if db:
                db.add_message(session_id, "assistant", reply_text)
            self.send_text_to_device(reply_text, role="assistant", client=device)
        except Exception:
            logger.exception("处理设备录音对话失败")
            self.send_text_to_device("云端处理录音失败", role="system", client=device)

    def _handle_text_line(self, device: DeviceConnection, text: str) -> None:
        if text == "HB":
            try:
                device.send_queue.put_nowait(b"OK\n")
            except Exception:
                return
            return

        if text == "REC_START":
            self._start_audio_capture(device)
            return

        if text == "REC_CANCEL":
            self._finish_audio_capture(device)
            self._notify_waiters(text)
            return

        if text == "REC_END":
            # 普通录音场景的唯一结束标记
            audio_info = self._finish_audio_capture(device)
            self._notify_waiters(text)
            if audio_info:
                self._process_completed_recording_async(device, audio_info)
            return

        if text.startswith("MIC_REC_DONE"):
            # MIC_REC 测试流程的结束标记，携带采样率参数
            sample_rate_override = None
            if ":" in text:
                suffix = text.split(":", 1)[1].strip()
                if suffix.startswith("sr="):
                    try:
                        sample_rate_override = int(suffix.split("=", 1)[1])
                    except Exception:
                        sample_rate_override = None
            self._finish_audio_capture(device, sample_rate_override=sample_rate_override)
            self._notify_waiters(text)
            return

        self._notify_waiters(text)

    def _parse_stream(self, device: DeviceConnection, data: bytes) -> None:
        device._stream_buf.extend(data)
        end_markers = [b"REC_END\n", b"REC_CANCEL\n"]
        # MIC_REC_DONE 单独处理：需要捕获完整行包括参数（如 :sr=16000）
        mic_done_prefix = b"MIC_REC_DONE"

        while True:
            if device.audio_capture_active:
                hit_idx = -1
                hit_marker = b""
                hit_full_line = b""

                # 检查固定结束标记
                for marker in end_markers:
                    idx = device._stream_buf.find(marker)
                    if idx >= 0 and (hit_idx < 0 or idx < hit_idx):
                        hit_idx = idx
                        hit_marker = marker
                        hit_full_line = marker

                # 检查 MIC_REC_DONE（需读取到换行符以捕获 :sr=XXXX 参数）
                mic_idx = device._stream_buf.find(mic_done_prefix)
                if mic_idx >= 0 and (hit_idx < 0 or mic_idx < hit_idx):
                    # 找到 MIC_REC_DONE，但需要等待换行符
                    nl_after = device._stream_buf.find(b"\n", mic_idx + len(mic_done_prefix))
                    if nl_after >= 0:
                        hit_idx = mic_idx
                        hit_full_line = bytes(device._stream_buf[mic_idx:nl_after + 1])
                        hit_marker = hit_full_line
                    else:
                        # MIC_REC_DONE 已到但换行未到，先刷出之前的音频，保留标记
                        if mic_idx > 0:
                            self._append_audio_bytes(device, bytes(device._stream_buf[:mic_idx]))
                            del device._stream_buf[:mic_idx]
                        break

                if hit_idx >= 0 and len(hit_marker) > 0:
                    if hit_idx > 0:
                        self._append_audio_bytes(device, bytes(device._stream_buf[:hit_idx]))
                    del device._stream_buf[: hit_idx + len(hit_marker)]
                    line = hit_marker.decode("utf-8", errors="ignore").strip()
                    self._handle_text_line(device, line)
                    continue

                # 保留尾部，防止标记被截断
                all_markers = end_markers + [mic_done_prefix]
                keep_tail = max(len(m) for m in all_markers)
                if len(device._stream_buf) > keep_tail:
                    payload = bytes(device._stream_buf[:-keep_tail])
                    self._append_audio_bytes(device, payload)
                    del device._stream_buf[:-keep_tail]
                break

            nl = device._stream_buf.find(b"\n")
            if nl < 0:
                if len(device._stream_buf) > 8192:
                    device._stream_buf.clear()
                break

            raw = bytes(device._stream_buf[:nl])
            del device._stream_buf[: nl + 1]
            line = raw.replace(b"\r", b"").decode("utf-8", errors="ignore").strip()
            if line:
                self._handle_text_line(device, line)

    @property
    def port(self) -> int:
        return self._port

    def _get_primary_client(self) -> DeviceConnection | None:
        with self._lock:
            if not self._clients:
                return None
            return max(self._clients.values(), key=lambda c: c.last_active)

    def _remove_client(self, device: DeviceConnection) -> None:
        with self._lock:
            stale_key = None
            for key, client in self._clients.items():
                if client is device:
                    stale_key = key
                    break
            if stale_key is not None:
                self._clients.pop(stale_key, None)
        try:
            device.send_queue.put_nowait(None)
        except Exception:
            pass
        try:
            device.conn.close()
        except Exception:
            pass

    # ------------------------------------------------------------------
    # 内部
    # ------------------------------------------------------------------
    @staticmethod
    def _enable_tcp_keepalive(conn: socket.socket) -> None:
        """启用 TCP 内核级 keepalive，加速死连接检测"""
        if hasattr(socket, "TCP_NODELAY"):
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        if hasattr(socket, "TCP_KEEPIDLE"):
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 10)
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 5)
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, 3)

    def _kick_same_ip(self, new_ip: str) -> None:
        """踢掉来自相同 IP 的旧连接（同一物理设备重连时清理残留）"""
        with self._lock:
            stale_keys = [k for k, c in self._clients.items() if c.addr[0] == new_ip]
            for key in stale_keys:
                old = self._clients.pop(key, None)
                if old:
                    try:
                        old.conn.close()
                    except Exception:
                        pass
                    logger.info(f"踢掉旧连接: {key} (同 IP 设备重连)")

    def _register_client(self, key: str, device: DeviceConnection) -> None:
        """收到首个数据帧后登记为有效设备连接。"""
        self._kick_same_ip(device.addr[0])
        with self._lock:
            self._clients[key] = device
        logger.info(f"设备已连接: {key}")

    def _accept_loop(self):
        """接受新连接的主循环"""
        while self._running:
            try:
                conn, addr = self._server_socket.accept()
                key = f"{addr[0]}:{addr[1]}"

                # 启用 TCP keepalive
                self._enable_tcp_keepalive(conn)

                device = DeviceConnection(conn, addr)

                device.sender_thread = threading.Thread(
                    target=self._send_loop, args=(key, device), daemon=True
                )
                device.sender_thread.start()

                # 为每个连接启动独立处理线程
                handler = threading.Thread(
                    target=self._handle_client, args=(key, device), daemon=True
                )
                handler.start()
            except socket.timeout:
                continue
            except OSError:
                if self._running:
                    logger.error("TCP 接受连接异常")
                break

    def _handle_client(self, key: str, device: DeviceConnection):
        """处理单个设备连接的数据接收。"""
        registered = False
        try:
            device.conn.settimeout(RECV_TIMEOUT)
            while self._running:
                try:
                    data = device.conn.recv(4096)
                    if not data:
                        break
                    device.last_active = time.time()
                    if not registered:
                        self._register_client(key, device)
                        registered = True
                    self._parse_stream(device, data)

                except socket.timeout:
                    # 检查心跳超时
                    if time.time() - device.last_active > HEARTBEAT_TIMEOUT:
                        logger.warning(
                            f"设备 {key} 心跳超时 ({HEARTBEAT_TIMEOUT}s)，断开连接"
                        )
                        break
                    continue
                except ConnectionResetError:
                    break
                except OSError:
                    # 被踢掉的旧连接 socket 已关闭，正常退出
                    break
        except Exception as e:
            logger.error(f"设备 {key} 处理异常: {e}")
        finally:
            if registered:
                with self._lock:
                    # 只移除属于自己的连接（可能已被新连接替换过）
                    if self._clients.get(key) is device:
                        self._clients.pop(key, None)
            try:
                device.send_queue.put_nowait(None)
            except Exception:
                pass
            try:
                device.conn.close()
            except Exception:
                pass
            self._finish_audio_capture(device)
            if registered:
                logger.info(f"设备已断开: {key}")

    def _send_loop(self, key: str, device: DeviceConnection):
        """处理单个设备连接的数据发送，避免业务线程阻塞在 socket.sendall。"""
        while self._running:
            try:
                payload = device.send_queue.get(timeout=1.0)
            except Empty:
                continue

            if payload is None:
                break

            try:
                device.conn.sendall(payload)
                logger.info("设备下发完成: %s bytes=%d", key, len(payload))
            except Exception:
                logger.warning(f"设备 {key} 异步发送失败，关闭连接")
                self._remove_client(device)
                break
