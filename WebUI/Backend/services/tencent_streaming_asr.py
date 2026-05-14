import base64
import hashlib
import hmac
import json
import logging
import random
import threading
import time
import uuid
from urllib.parse import quote, urlencode

logger = logging.getLogger(__name__)


class TencentStreamingAsrSession:
    """Tencent Cloud realtime ASR session over WebSocket."""

    def __init__(self, config, sample_rate: int, on_text):
        self._config = config
        self._sample_rate = int(sample_rate or 16000)
        self._on_text = on_text
        self._ws = None
        self._recv_thread = None
        self._lock = threading.Lock()
        self._done = threading.Event()
        self._ready = threading.Event()
        self._failed = False
        self._error = ""
        self._closed = False
        self._pending = bytearray()
        self._final_segments: dict[int, str] = {}
        self._last_text = ""
        self._last_emitted = ""
        self._chunk_size = 3200 if self._sample_rate <= 8000 else 6400

    @property
    def error(self) -> str:
        return self._error

    @property
    def failed(self) -> bool:
        return self._failed

    @property
    def last_emitted_text(self) -> str:
        return self._last_emitted

    def start(self) -> bool:
        try:
            import websocket
        except ImportError:
            self._fail("未安装 websocket-client，无法使用腾讯云实时识别")
            return False

        app_id = str(self._config.get("speech.app_id", "") or "").strip()
        secret_id = str(self._config.get("speech.secret_id", "") or "").strip()
        secret_key = str(self._config.get("speech.secret_key", "") or "").strip()

        if not app_id:
            self._fail("未配置腾讯云 AppID，无法使用实时语音识别")
            return False
        if not secret_id or not secret_key:
            self._fail("未配置腾讯云 SecretId 或 SecretKey")
            return False

        url = self._build_url(app_id, secret_id, secret_key)
        try:
            self._ws = websocket.create_connection(url, timeout=10)
            first = self._ws.recv()
            self._handle_message(first)
            if self._failed:
                return False
            if not self._ready.wait(timeout=1.0):
                self._fail("腾讯云实时识别握手超时")
                return False

            self._recv_thread = threading.Thread(target=self._recv_loop, daemon=True)
            self._recv_thread.start()
            return True
        except Exception as exc:
            self._fail(f"腾讯云实时识别连接失败: {exc}")
            self.close()
            return False

    def send_audio(self, data: bytes) -> None:
        if not data or self._failed or self._closed or not self._ws:
            return

        with self._lock:
            self._pending.extend(data)
            while len(self._pending) >= self._chunk_size:
                chunk = bytes(self._pending[: self._chunk_size])
                del self._pending[: self._chunk_size]
                self._send_binary_locked(chunk)

    def finish(self, timeout: float = 8.0) -> str:
        if not self._ws:
            return self.final_text()

        with self._lock:
            if self._pending and not self._failed and not self._closed:
                self._send_binary_locked(bytes(self._pending))
                self._pending.clear()
            if not self._failed and not self._closed:
                try:
                    self._ws.send(json.dumps({"type": "end"}))
                except Exception as exc:
                    self._fail(f"发送实时识别结束帧失败: {exc}")

        self._done.wait(timeout=timeout)
        self.close()
        return self.final_text()

    def close(self) -> None:
        self._closed = True
        try:
            if self._ws:
                self._ws.close()
        except Exception:
            pass

    def final_text(self) -> str:
        if self._final_segments:
            return "".join(self._final_segments[i] for i in sorted(self._final_segments)).strip()
        return self._last_text.strip()

    def _build_url(self, app_id: str, secret_id: str, secret_key: str) -> str:
        now = int(time.time())
        voice_id = str(uuid.uuid4())
        model = self._config.get("speech.model", "16k_zh") or "16k_zh"
        params = {
            "engine_model_type": model,
            "expired": now + 24 * 60 * 60,
            "filter_dirty": 0,
            "filter_empty_result": 1,
            "filter_modal": 0,
            "filter_punc": 0,
            "needvad": 1,
            "nonce": random.randint(100000, 999999999),
            "secretid": secret_id,
            "timestamp": now,
            "voice_format": 1,
            "voice_id": voice_id,
        }
        if self._sample_rate == 8000 and str(model).startswith("16k_"):
            params["input_sample_rate"] = 8000

        sorted_params = dict(sorted(params.items()))
        query = urlencode(sorted_params)
        sign_text = f"asr.cloud.tencent.com/asr/v2/{app_id}?{query}"
        digest = hmac.new(
            secret_key.encode("utf-8"),
            sign_text.encode("utf-8"),
            hashlib.sha1,
        ).digest()
        signature = base64.b64encode(digest).decode("ascii")
        return f"wss://asr.cloud.tencent.com/asr/v2/{app_id}?{query}&signature={quote(signature, safe='')}"

    def _send_binary_locked(self, chunk: bytes) -> None:
        try:
            self._ws.send_binary(chunk)
        except Exception as exc:
            self._fail(f"发送实时识别音频分片失败: {exc}")

    def _recv_loop(self) -> None:
        while not self._closed and not self._failed:
            try:
                message = self._ws.recv()
            except Exception as exc:
                if not self._closed:
                    logger.debug("腾讯云实时识别接收结束: %s", exc)
                break
            if not message:
                break
            self._handle_message(message)
            if self._done.is_set():
                break

    def _handle_message(self, message) -> None:
        if isinstance(message, bytes):
            message = message.decode("utf-8", errors="ignore")
        try:
            data = json.loads(message)
        except Exception:
            logger.warning("腾讯云实时识别返回非 JSON 消息: %r", message)
            return

        code = int(data.get("code", 0) or 0)
        if code != 0:
            self._fail(f"腾讯云实时识别失败: {data.get('message', code)}")
            self._done.set()
            return

        if data.get("message") == "success" and "result" not in data:
            self._ready.set()

        result = data.get("result")
        if isinstance(result, dict):
            self._handle_result(result)

        if int(data.get("final", 0) or 0) == 1:
            self._done.set()

    def _handle_result(self, result: dict) -> None:
        text = str(result.get("voice_text_str") or "").strip()
        if not text:
            return

        index = int(result.get("index", 0) or 0)
        slice_type = int(result.get("slice_type", 1) or 1)

        if slice_type == 2:
            self._final_segments[index] = text
            full_text = self.final_text()
            is_final = True
        else:
            stable_prefix = self.final_text()
            full_text = (stable_prefix + text).strip() if stable_prefix else text
            is_final = False

        self._last_text = full_text
        if full_text and full_text != self._last_emitted:
            self._last_emitted = full_text
            try:
                self._on_text(full_text, is_final)
            except Exception:
                logger.exception("实时识别文本回调失败")

    def _fail(self, message: str) -> None:
        self._failed = True
        self._error = message
        logger.error(message)
