import logging
import os
import struct
import tempfile
import time
import wave
from dataclasses import dataclass
from typing import Any

from flask import Blueprint, jsonify, request

from ..auth.dependencies import require_auth

logger = logging.getLogger(__name__)
speaker_test_bp = Blueprint("speaker_test", __name__)


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
    SpeakerTestCase(
        case_id="ode_to_joy",
        name="播放欢乐颂",
        description="在 STM32F103VET6 的 I2S2/MAX98357A 链路上播放一段《欢乐颂》旋律。",
        command="SPK_ODE\n",
        expected="SPK_ODE_DONE",
        timeout=22.0,
    ),
)


def _get_services():
    from ..app import app_state

    return app_state.get("device_manager"), app_state.get("job_manager")


def get_speaker_test_case(case_id: str) -> SpeakerTestCase | None:
    return next((case for case in SPEAKER_TEST_CASES if case.case_id == case_id), None)


def list_speaker_test_cases() -> list[dict[str, Any]]:
    return [case.to_dict() for case in SPEAKER_TEST_CASES]


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


TARGET_SAMPLE_RATE = 8000
TARGET_CHANNELS = 1
TARGET_SAMPLE_WIDTH = 2  # 16-bit
TARGET_PCM_BYTES_PER_SEC = TARGET_SAMPLE_RATE * TARGET_CHANNELS * TARGET_SAMPLE_WIDTH
WAV_STREAM_CHUNK_SIZE = 512
WAV_STREAM_PACE_FACTOR = 0.985


def _convert_sample_width(data: bytes, src_width: int, dst_width: int) -> bytes:
    """将采样位深从 src_width 字节转换为 dst_width 字节（仅支持 1/2/3/4 → 2）。"""
    if src_width == dst_width:
        return data

    # 每个采样的 struct 格式
    if src_width == 1:
        # 8-bit 无符号 → 16-bit 有符号
        n_samples = len(data)
        out = bytearray(n_samples * 2)
        for i in range(n_samples):
            val = (data[i] - 128) << 8
            struct.pack_into("<h", out, i * 2, val)
        return bytes(out)

    if src_width == 3:
        # 24-bit 有符号小端 → 16-bit：取高 16 位
        n_samples = len(data) // 3
        out = bytearray(n_samples * 2)
        for i in range(n_samples):
            # 24-bit 小端：低、中、高字节
            lo = data[i * 3]
            mid = data[i * 3 + 1]
            hi = data[i * 3 + 2]
            # 组成 24-bit 有符号值，取高 16 位
            val = (hi << 16) | (mid << 8) | lo
            if val >= 0x800000:
                val -= 0x1000000
            val16 = max(-32768, min(32767, val >> 8))
            struct.pack_into("<h", out, i * 2, val16)
        return bytes(out)

    if src_width == 4:
        # 32-bit 有符号 → 16-bit：取高 16 位
        n_samples = len(data) // 4
        out = bytearray(n_samples * 2)
        for i in range(n_samples):
            (val,) = struct.unpack_from("<i", data, i * 4)
            val16 = max(-32768, min(32767, val >> 16))
            struct.pack_into("<h", out, i * 2, val16)
        return bytes(out)

    raise ValueError(f"不支持的源采样位深: {src_width} 字节")


def _stereo_to_mono(data: bytes, channels: int, sample_width: int) -> bytes:
    """多声道 → 单声道，取各声道平均值。仅处理 16-bit 数据。"""
    if channels == 1:
        return data

    frame_size = channels * sample_width
    n_frames = len(data) // frame_size
    out = bytearray(n_frames * sample_width)

    for i in range(n_frames):
        offset = i * frame_size
        total = 0
        for ch in range(channels):
            (val,) = struct.unpack_from("<h", data, offset + ch * sample_width)
            total += val
        avg = total // channels
        avg = max(-32768, min(32767, avg))
        struct.pack_into("<h", out, i * sample_width, avg)

    return bytes(out)


def _resample_linear(data: bytes, src_rate: int, dst_rate: int) -> bytes:
    """简单线性插值重采样，输入输出均为 16-bit 小端单声道 PCM。"""
    if src_rate == dst_rate:
        return data

    n_src = len(data) // 2
    if n_src == 0:
        return data

    ratio = src_rate / dst_rate
    n_dst = int(n_src / ratio)
    if n_dst == 0:
        return data

    # 将源数据解包为采样值列表
    src_samples = struct.unpack(f"<{n_src}h", data)
    out = bytearray(n_dst * 2)

    for i in range(n_dst):
        src_pos = i * ratio
        idx = int(src_pos)
        frac = src_pos - idx

        if idx + 1 < n_src:
            val = src_samples[idx] + frac * (src_samples[idx + 1] - src_samples[idx])
        else:
            val = src_samples[min(idx, n_src - 1)]

        val = max(-32768, min(32767, int(val)))
        struct.pack_into("<h", out, i * 2, val)

    return bytes(out)


def _run_wav_playback(ctx, wav_path: str):
    """读取 WAV 文件，转换格式后流式下发到设备播放。"""
    device_manager, _ = _get_services()
    if not device_manager:
        raise RuntimeError("设备管理器未初始化")
    if not device_manager.has_connections():
        raise RuntimeError("设备未连接")

    try:
        ctx.set_progress(5, "正在解析 WAV 文件")

        with wave.open(wav_path, "rb") as wf:
            src_channels = wf.getnchannels()
            src_sample_width = wf.getsampwidth()
            src_sample_rate = wf.getframerate()
            n_frames = wf.getnframes()
            raw_data = wf.readframes(n_frames)

        logger.info(
            "WAV 文件参数: channels=%d, sample_width=%d, rate=%d, frames=%d",
            src_channels, src_sample_width, src_sample_rate, n_frames,
        )

        ctx.set_progress(10, "正在转换音频格式")

        # 步骤1: 位深转换 → 16-bit
        pcm_data = _convert_sample_width(raw_data, src_sample_width, TARGET_SAMPLE_WIDTH)

        # 步骤2: 声道转换 → 单声道
        pcm_data = _stereo_to_mono(pcm_data, src_channels, TARGET_SAMPLE_WIDTH)

        # 步骤3: 采样率转换 → 目标采样率
        pcm_data = _resample_linear(pcm_data, src_sample_rate, TARGET_SAMPLE_RATE)

        pcm_size = len(pcm_data)
        logger.info(
            "转换后 PCM 大小: %d 字节 (%dkHz/mono/16bit)",
            pcm_size,
            TARGET_SAMPLE_RATE // 1000,
        )

        ctx.set_progress(20, "正在下发播放指令")

        # 注册等待器
        waiter = device_manager.register_waiter(
            expected="SPK_WAV_", timeout=60.0
        )

        try:
            # 发送文本头帧
            header = f"SPK_WAV:{pcm_size}:{TARGET_SAMPLE_RATE}"
            if not device_manager.send_command(header):
                raise RuntimeError("WAV 播放指令发送失败")

            # 等待设备处理 SPK_WAV 命令并设置 wav_stream_active
            time.sleep(0.05)

            # 流式发送 PCM 数据。8kHz/mono/16bit 是 16KB/s，
            # 下发速率要略快于播放速率，否则 STM32 端会反复 underrun。
            chunk_size = WAV_STREAM_CHUNK_SIZE
            chunk_interval = (chunk_size / TARGET_PCM_BYTES_PER_SEC) * WAV_STREAM_PACE_FACTOR
            total_chunks = (pcm_size + chunk_size - 1) // chunk_size
            sent_chunks = 0

            ctx.set_progress(25, f"正在流式传输 PCM 数据 (共 {total_chunks} 块)")

            for offset in range(0, pcm_size, chunk_size):
                chunk = pcm_data[offset : offset + chunk_size]
                if not device_manager.send_raw_async(chunk):
                    raise RuntimeError(
                        f"PCM 数据发送失败 (块 {sent_chunks + 1}/{total_chunks})"
                    )
                sent_chunks += 1

                # 更新进度：25% ~ 85% 区间用于数据传输
                progress = 25 + int(60 * sent_chunks / total_chunks)
                if sent_chunks % 50 == 0 or sent_chunks == total_chunks:
                    ctx.set_progress(
                        progress,
                        f"已发送 {sent_chunks}/{total_chunks} 块",
                    )

                time.sleep(chunk_interval)

            ctx.set_progress(85, "PCM 数据发送完毕，等待设备播放完成")

            # 等待设备回复
            response = device_manager.await_waiter(waiter["waiter_id"], timeout=60.0)
            if not response:
                raise RuntimeError("设备未在 60 秒内响应 SPK_WAV_DONE")

            if "SPK_WAV_DONE" not in response:
                raise RuntimeError(f"设备返回了 WAV 错误响应: {response}")

            ctx.set_progress(100, "WAV 文件播放完成")
            return {
                "success": True,
                "case_id": "wav_play",
                "name": "WAV 文件播放",
                "context": f"WAV 播放完成，已发送 {pcm_size} 字节 PCM 数据",
                "response": response,
            }
        finally:
            device_manager.cancel_waiter(waiter["waiter_id"])
    finally:
        # 清理临时文件
        try:
            os.unlink(wav_path)
            logger.info("已清理临时 WAV 文件: %s", wav_path)
        except OSError:
            logger.warning("清理临时文件失败: %s", wav_path)


@speaker_test_bp.route("/wav/play", methods=["POST"])
@require_auth
def wav_play():
    """上传 WAV 文件并流式下发到设备播放。"""
    _, job_manager = _get_services()
    if not job_manager:
        return jsonify({"success": False, "error": "任务管理器未初始化"}), 500

    if "file" not in request.files:
        return jsonify({"success": False, "error": "缺少文件字段 'file'"}), 400

    uploaded = request.files["file"]
    if not uploaded.filename:
        return jsonify({"success": False, "error": "未选择文件"}), 400

    # 保存到临时文件
    try:
        fd, wav_path = tempfile.mkstemp(suffix=".wav")
        os.close(fd)
        uploaded.save(wav_path)
    except Exception as exc:
        logger.exception("保存上传文件失败")
        return jsonify({"success": False, "error": f"保存文件失败: {exc}"}), 500

    # 快速校验是否为合法 WAV
    try:
        with wave.open(wav_path, "rb") as wf:
            wf.getparams()
    except Exception as exc:
        os.unlink(wav_path)
        logger.warning("上传的文件不是合法 WAV: %s", exc)
        return jsonify({"success": False, "error": f"无效的 WAV 文件: {exc}"}), 400

    try:
        job_id = job_manager.submit("wav_play", _run_wav_playback, wav_path)
        return jsonify(
            {
                "success": True,
                "job_id": job_id,
                "status": "pending",
                "message": "WAV 播放任务已提交",
            }
        ), 202
    except Exception as exc:
        os.unlink(wav_path)
        logger.exception("提交 WAV 播放任务失败")
        return jsonify({"success": False, "error": str(exc)}), 500


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
