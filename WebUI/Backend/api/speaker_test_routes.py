import logging
import base64
import time
import uuid
import wave
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from flask import Blueprint, jsonify, request

from ..auth.dependencies import require_auth

logger = logging.getLogger(__name__)
speaker_test_bp = Blueprint("speaker_test", __name__)

LOCAL_AUDIO_SAMPLE_RATE = 8000
LOCAL_AUDIO_MAX_SECONDS = 30
LOCAL_AUDIO_CHUNK_BYTES = 384
LOCAL_AUDIO_CHUNK_SIZE_CANDIDATES = (384, 240, 160)

_IMA_INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)
_IMA_STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
)


@dataclass(frozen=True)
class SpeakerTestCase:
    case_id: str
    name: str
    description: str
    command: str
    expected: str
    timeout: float

    def to_dict(self) -> dict[str, Any]:
        return {
            "id": self.case_id,
            "name": self.name,
            "description": self.description,
            "command": self.command.strip(),
            "expected": self.expected,
            "timeout": self.timeout,
        }


SPEAKER_TEST_CASES = (
    SpeakerTestCase(
        case_id="hardware",
        name="硬件连通",
        description="检测功放、DAC/PWM 输出链路是否可响应。",
        command="SPK_TEST\n",
        expected="SPK_OK",
        timeout=5.0,
    ),
    SpeakerTestCase(
        case_id="tone",
        name="1 kHz 单音",
        description="播放 1 秒 1 kHz 测试音，确认扬声器能正常发声。",
        command="SPK_TONE:1000:1000\n",
        expected="SPK_TONE_DONE",
        timeout=6.0,
    ),
    SpeakerTestCase(
        case_id="sweep",
        name="扫频测试",
        description="从 300 Hz 扫到 3 kHz，检查破音、断续和频响异常。",
        command="SPK_SWEEP:300:3000:2000\n",
        expected="SPK_SWEEP_DONE",
        timeout=8.0,
    ),
    SpeakerTestCase(
        case_id="volume",
        name="音量阶梯",
        description="按 30%、60%、90% 三档播放提示音，验证音量控制。",
        command="SPK_VOLUME:30,60,90\n",
        expected="SPK_VOLUME_DONE",
        timeout=8.0,
    ),
)


def _get_services():
    from ..app import app_state

    return app_state.get("device_manager"), app_state.get("job_manager")


def get_speaker_test_case(case_id: str) -> SpeakerTestCase | None:
    return next((case for case in SPEAKER_TEST_CASES if case.case_id == case_id), None)


def list_speaker_test_cases() -> list[dict[str, Any]]:
    return [case.to_dict() for case in SPEAKER_TEST_CASES]


def _audio_upload_dir() -> Path:
    path = Path(__file__).resolve().parent.parent.parent / "data" / "audio"
    path.mkdir(parents=True, exist_ok=True)
    return path


def _pcm_sample_from_bytes(raw: bytes, offset: int, width: int) -> int:
    if width == 1:
        return (raw[offset] - 128) << 8
    if width == 2:
        return int.from_bytes(raw[offset: offset + 2], "little", signed=True)
    if width == 3:
        value = int.from_bytes(raw[offset: offset + 3], "little", signed=False)
        if value & 0x800000:
            value -= 0x1000000
        return value >> 8
    if width == 4:
        return int.from_bytes(raw[offset: offset + 4], "little", signed=True) >> 16
    raise RuntimeError("仅支持 8/16/24/32-bit PCM WAV")


def _load_wav_as_pcm16_mono(path: Path, target_rate: int) -> list[int]:
    with wave.open(str(path), "rb") as wavf:
        if wavf.getcomptype() != "NONE":
            raise RuntimeError("仅支持未压缩 PCM WAV 文件")

        channels = wavf.getnchannels()
        sample_width = wavf.getsampwidth()
        source_rate = wavf.getframerate()
        frame_count = wavf.getnframes()
        if channels < 1 or channels > 2:
            raise RuntimeError("仅支持单声道或双声道 WAV")
        if source_rate <= 0 or frame_count <= 0:
            raise RuntimeError("音频文件为空")

        duration = frame_count / float(source_rate)
        if duration > LOCAL_AUDIO_MAX_SECONDS:
            raise RuntimeError(f"音频过长，请选择 {LOCAL_AUDIO_MAX_SECONDS} 秒以内的 WAV")

        raw = wavf.readframes(frame_count)

    frame_size = channels * sample_width
    mono: list[int] = []
    for frame in range(frame_count):
        base = frame * frame_size
        acc = 0
        for ch in range(channels):
            acc += _pcm_sample_from_bytes(raw, base + ch * sample_width, sample_width)
        mono.append(int(acc / channels))

    if source_rate == target_rate:
        resampled = mono
    elif source_rate > target_rate:
        out_count = max(1, int(len(mono) * target_rate / source_rate))
        resampled = []
        for i in range(out_count):
            start = int(i * source_rate / target_rate)
            end = int((i + 1) * source_rate / target_rate)
            if end <= start:
                end = start + 1
            window = mono[start:min(end, len(mono))]
            resampled.append(int(sum(window) / len(window)))
    else:
        out_count = max(1, int(len(mono) * target_rate / source_rate))
        resampled = []
        for i in range(out_count):
            pos = i * source_rate / target_rate
            left = min(len(mono) - 1, int(pos))
            right = min(len(mono) - 1, left + 1)
            frac = pos - left
            sample = int(mono[left] * (1.0 - frac) + mono[right] * frac)
            resampled.append(sample)

    if len(resampled) % 2:
        resampled.append(resampled[-1])
    return resampled


def _ima_encode_nibble(sample: int, state: dict[str, int]) -> int:
    predictor = state["predictor"]
    index = state["index"]
    step = _IMA_STEP_TABLE[index]
    diff = sample - predictor
    nibble = 0

    if diff < 0:
        nibble = 8
        diff = -diff

    temp_step = step
    if diff >= temp_step:
        nibble |= 4
        diff -= temp_step
    temp_step >>= 1
    if diff >= temp_step:
        nibble |= 2
        diff -= temp_step
    temp_step >>= 1
    if diff >= temp_step:
        nibble |= 1

    diffq = step >> 3
    if nibble & 1:
        diffq += step >> 2
    if nibble & 2:
        diffq += step >> 1
    if nibble & 4:
        diffq += step

    if nibble & 8:
        predictor -= diffq
    else:
        predictor += diffq

    predictor = max(-32768, min(32767, predictor))
    index = max(0, min(88, index + _IMA_INDEX_TABLE[nibble & 0x0F]))
    state["predictor"] = predictor
    state["index"] = index
    return nibble & 0x0F


def _ima_adpcm_encode(samples: list[int]) -> bytes:
    state = {"predictor": 0, "index": 0}
    out = bytearray()
    for i in range(0, len(samples), 2):
        lo = _ima_encode_nibble(samples[i], state)
        hi = _ima_encode_nibble(samples[i + 1], state) if i + 1 < len(samples) else 0
        out.append(lo | (hi << 4))
    return bytes(out)


def _send_line_with_retry(device_manager, line: str, timeout: float = 2.0) -> bool:
    payload = line.encode("ascii")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if device_manager.send_raw_async(payload):
            return True
        time.sleep(0.02)
    return False


def _wait_device_audio_response(device_manager, expected: str, timeout: float) -> str:
    expected_waiter = device_manager.register_waiter(expected=expected, timeout=timeout)
    error_waiter = device_manager.register_waiter(expected="SPK_AUDIO_ERR", timeout=timeout)
    deadline = time.time() + timeout
    try:
        while time.time() < deadline:
            if error_waiter["event"].wait(timeout=0.02):
                response = error_waiter.get("response") or "SPK_AUDIO_ERR"
                raise RuntimeError(f"设备返回 {response}")
            if expected_waiter["event"].wait(timeout=0.02):
                response = expected_waiter.get("response")
                if response and expected in response:
                    return response
        raise RuntimeError(f"设备未返回 {expected}")
    finally:
        device_manager.cancel_waiter(expected_waiter["waiter_id"])
        device_manager.cancel_waiter(error_waiter["waiter_id"])


def _send_line_and_wait_audio_response(
    device_manager, line: str, expected: str, timeout: float
) -> str:
    expected_waiter = device_manager.register_waiter(expected=expected, timeout=timeout)
    error_waiter = device_manager.register_waiter(expected="SPK_AUDIO_ERR", timeout=timeout)
    deadline = time.time() + timeout
    try:
        if not _send_line_with_retry(device_manager, line):
            raise RuntimeError(f"下发 {expected} 相关命令失败")
        while time.time() < deadline:
            if error_waiter["event"].wait(timeout=0.02):
                response = error_waiter.get("response") or "SPK_AUDIO_ERR"
                raise RuntimeError(f"设备返回 {response}")
            if expected_waiter["event"].wait(timeout=0.02):
                response = expected_waiter.get("response")
                if response and expected in response:
                    return response
    finally:
        device_manager.cancel_waiter(expected_waiter["waiter_id"])
        device_manager.cancel_waiter(error_waiter["waiter_id"])

    raise RuntimeError(f"设备未返回 {expected}")


def _stream_local_adpcm_audio(
    ctx,
    device_manager,
    adpcm: bytes,
    sample_count: int,
    duration: float,
    playback_timeout: float,
    chunk_bytes: int,
) -> tuple[str, int]:
    start_line = f"SPK_AUDIO_START:IMA4:{LOCAL_AUDIO_SAMPLE_RATE}:{sample_count}\n"
    _send_line_and_wait_audio_response(
        device_manager, start_line, "SPK_AUDIO_READY", timeout=5.0
    )

    total_chunks = max(1, (len(adpcm) + chunk_bytes - 1) // chunk_bytes)
    for index in range(total_chunks):
        if ctx.is_cancelled():
            _send_line_with_retry(device_manager, "SPK_AUDIO_CANCEL\n", timeout=0.5)
            raise RuntimeError("任务已取消")

        start = index * chunk_bytes
        chunk = adpcm[start: start + chunk_bytes]
        encoded = base64.b64encode(chunk).decode("ascii")
        _send_line_and_wait_audio_response(
            device_manager,
            f"SPK_AUDIO_DATA:{encoded}\n",
            "SPK_AUDIO_CHUNK",
            timeout=12.0,
        )

        progress = 15 + int(((index + 1) / total_chunks) * 75)
        ctx.set_progress(
            progress,
            f"正在播放本地音频 {index + 1}/{total_chunks} ({chunk_bytes}B)",
        )

    ctx.set_progress(95, "等待设备播放完成")
    response = _send_line_and_wait_audio_response(
        device_manager, "SPK_AUDIO_END\n", "SPK_AUDIO_DONE", timeout=playback_timeout
    )
    return response, total_chunks


def _serialize_job(job: dict[str, Any] | None):
    if not job:
        return jsonify({"success": False, "error": "任务不存在"}), 404
    return jsonify({"success": True, "job": job})


def _run_speaker_test(ctx, case_id: str):
    device_manager, _ = _get_services()
    case = get_speaker_test_case(case_id)
    if not case:
        raise RuntimeError(f"未知音响测试项: {case_id}")
    if not device_manager:
        raise RuntimeError("设备管理器未初始化")
    if not device_manager.has_connections():
        raise RuntimeError("设备未连接")

    ctx.set_progress(10, f"正在下发测试命令: {case.command.strip()}")
    waiter = device_manager.register_waiter(expected=case.expected, timeout=case.timeout)

    try:
        if not device_manager.send_command_async(case.command):
            raise RuntimeError("测试命令发送失败")

        ctx.set_progress(35, f"等待设备返回 {case.expected}")
        response = device_manager.await_waiter(
            waiter["waiter_id"], timeout=case.timeout
        )
        if not response:
            raise RuntimeError(f"设备未在 {case.timeout:.0f} 秒内响应 {case.expected}")

        if case.expected not in response:
            raise RuntimeError(f"设备返回了非预期响应: {response}")

        ctx.set_progress(100, f"{case.name}完成")
        return {
            "success": True,
            "case_id": case.case_id,
            "name": case.name,
            "context": f"{case.name}通过，设备响应: {response}",
            "response": response,
        }
    finally:
        device_manager.cancel_waiter(waiter["waiter_id"])


def _run_local_audio_playback(ctx, audio_path: str):
    device_manager, _ = _get_services()
    path = Path(audio_path)
    if not device_manager:
        raise RuntimeError("设备管理器未初始化")
    if not device_manager.has_connections():
        raise RuntimeError("设备未连接")
    if not path.exists():
        raise RuntimeError("上传的音频文件不存在")

    ctx.set_progress(5, "正在解析本地 WAV")
    samples = _load_wav_as_pcm16_mono(path, LOCAL_AUDIO_SAMPLE_RATE)
    adpcm = _ima_adpcm_encode(samples)
    duration = len(samples) / float(LOCAL_AUDIO_SAMPLE_RATE)
    timeout = max(10.0, duration + 8.0)

    ctx.set_progress(15, f"正在下发音频: {duration:.1f}s / {LOCAL_AUDIO_SAMPLE_RATE}Hz")

    try:
        last_error: Exception | None = None
        used_chunk_bytes = LOCAL_AUDIO_CHUNK_BYTES
        total_chunks = 0
        response = ""
        for attempt, chunk_bytes in enumerate(LOCAL_AUDIO_CHUNK_SIZE_CANDIDATES):
            if attempt:
                ctx.set_progress(
                    15,
                    f"设备未确认上一种分片，降级为 {chunk_bytes}B 重试",
                )
                _send_line_with_retry(device_manager, "SPK_AUDIO_CANCEL\n", timeout=0.5)
                time.sleep(0.3)
            try:
                response, total_chunks = _stream_local_adpcm_audio(
                    ctx,
                    device_manager,
                    adpcm,
                    len(samples),
                    duration,
                    timeout,
                    chunk_bytes,
                )
                used_chunk_bytes = chunk_bytes
                last_error = None
                break
            except Exception as exc:
                last_error = exc
                _send_line_with_retry(device_manager, "SPK_AUDIO_CANCEL\n", timeout=0.5)
                if ctx.is_cancelled():
                    raise
        if last_error is not None:
            sizes = "/".join(str(size) for size in LOCAL_AUDIO_CHUNK_SIZE_CANDIDATES)
            raise RuntimeError(f"本地音频播放失败，已尝试 {sizes}B 分片: {last_error}")

        ctx.set_progress(100, "本地音频播放完成")
        return {
            "success": True,
            "context": f"本地音频播放完成，时长 {duration:.1f}s",
            "duration": duration,
            "sample_rate": LOCAL_AUDIO_SAMPLE_RATE,
            "chunks": total_chunks,
            "chunk_bytes": used_chunk_bytes,
            "response": response,
        }
    finally:
        try:
            path.unlink(missing_ok=True)
        except Exception:
            pass


@speaker_test_bp.route("/cases", methods=["GET"])
@require_auth
def get_cases():
    return jsonify({"success": True, "cases": list_speaker_test_cases()})


@speaker_test_bp.route("/run/<case_id>", methods=["POST"])
@require_auth
def run_case(case_id: str):
    _, job_manager = _get_services()
    if not get_speaker_test_case(case_id):
        return jsonify({"success": False, "context": "未知音响测试项"}), 404
    if not job_manager:
        return jsonify({"success": False, "context": "任务管理器未初始化"}), 500

    try:
        job_id = job_manager.submit("speaker_test", _run_speaker_test, case_id)
        return jsonify(
            {
                "success": True,
                "job_id": job_id,
                "status": "pending",
                "message": "音响测试任务已提交",
            }
        ), 202
    except Exception as exc:
        logger.exception("提交音响测试任务失败")
        return jsonify({"success": False, "context": str(exc)}), 500


@speaker_test_bp.route("/play-local", methods=["POST"])
@require_auth
def play_local_audio():
    _, job_manager = _get_services()
    if not job_manager:
        return jsonify({"success": False, "context": "任务管理器未初始化"}), 500

    upload = request.files.get("file")
    if not upload or not upload.filename:
        return jsonify({"success": False, "context": "请上传一个 WAV 音频文件"}), 400

    filename = upload.filename.lower()
    if not filename.endswith(".wav"):
        return jsonify({"success": False, "context": "当前仅支持 WAV 文件"}), 400

    try:
        saved_path = _audio_upload_dir() / f"speaker_{int(time.time())}_{uuid.uuid4().hex[:8]}.wav"
        upload.save(saved_path)
        job_id = job_manager.submit("speaker_local_audio", _run_local_audio_playback, str(saved_path))
        return jsonify(
            {
                "success": True,
                "job_id": job_id,
                "status": "pending",
                "message": "本地音频播放任务已提交",
            }
        ), 202
    except Exception as exc:
        logger.exception("提交本地音频播放任务失败")
        return jsonify({"success": False, "context": str(exc)}), 500


@speaker_test_bp.route("/jobs/<job_id>", methods=["GET"])
@require_auth
def get_job_status(job_id: str):
    _, job_manager = _get_services()
    if not job_manager:
        return jsonify({"success": False, "error": "任务管理器未初始化"}), 500

    return _serialize_job(job_manager.get_job_dict(job_id))


@speaker_test_bp.route("/jobs/<job_id>", methods=["DELETE"])
@require_auth
def cancel_job(job_id: str):
    _, job_manager = _get_services()
    if not job_manager:
        return jsonify({"success": False, "error": "任务管理器未初始化"}), 500

    exists = job_manager.get_job(job_id)
    if not exists:
        return jsonify({"success": False, "error": "任务不存在"}), 404

    job_manager.cancel_job(job_id)
    return jsonify(
        {
            "success": True,
            "job_id": job_id,
            "message": "已请求取消任务",
        }
    )
