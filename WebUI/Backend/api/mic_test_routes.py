import logging
import time
from typing import Any

from flask import Blueprint, jsonify

from ..auth.dependencies import require_auth

logger = logging.getLogger(__name__)
mic_test_bp = Blueprint("mic_test", __name__)

HARDWARE_TEST_TIMEOUT = 5.0
HARDWARE_TEST_RETRIES = 2
RECORD_TEST_TIMEOUT = 25.0
RECORD_UPLOAD_GRACE_TIMEOUT = 35.0
RECOGNIZE_TEST_TIMEOUT = 30.0


def _get_services():
    from ..app import app_state

    return app_state.get("device_manager"), app_state.get("job_manager")


def _serialize_job(job: dict[str, Any] | None):
    if not job:
        return jsonify({"success": False, "error": "任务不存在"}), 404
    return jsonify({"success": True, "job": job})


def _wait_latest_audio(device_manager, timeout: float = 5.0):
    end = time.time() + timeout
    while time.time() < end:
        latest = device_manager.get_latest_audio()
        if latest and latest.get("path"):
            return latest
        time.sleep(0.1)
    return None


def _run_hardware_test(ctx):
    device_manager, _ = _get_services()
    if not device_manager:
        raise RuntimeError("设备管理器未初始化")
    if not device_manager.has_connections():
        raise RuntimeError("设备未连接")

    response = None

    for attempt in range(1, HARDWARE_TEST_RETRIES + 1):
        waiter = device_manager.register_waiter(
            expected="MIC_", timeout=HARDWARE_TEST_TIMEOUT
        )

        try:
            progress = 10 if attempt == 1 else 10 + (attempt - 1) * 25
            ctx.set_progress(progress, f"正在下发硬件检测命令（第 {attempt} 次）")

            if not device_manager.send_command_async("MIC_TEST\n"):
                raise RuntimeError("硬件检测命令发送失败")

            wait_progress = min(progress + 20, 90)
            ctx.set_progress(wait_progress, f"等待设备返回检测结果（第 {attempt} 次）")
            response = device_manager.await_waiter(
                waiter["waiter_id"], timeout=HARDWARE_TEST_TIMEOUT
            )
            if response:
                break

            if attempt < HARDWARE_TEST_RETRIES:
                ctx.set_progress(
                    wait_progress,
                    f"设备暂未响应，准备重试 MIC_TEST（第 {attempt + 1} 次）",
                )
        finally:
            device_manager.cancel_waiter(waiter["waiter_id"])

    if not response:
        raise RuntimeError(
            f"设备未在 {HARDWARE_TEST_TIMEOUT * HARDWARE_TEST_RETRIES:.0f} 秒内响应 MIC_TEST"
        )

    if response.startswith("MIC_OK"):
        ctx.set_progress(100, "硬件通信检测完成")
        return {
            "success": True,
            "context": "通信正常！成功读取到麦克风数据帧。",
            "response": response,
        }

    if response.startswith("MIC_FAIL"):
        raise RuntimeError("麦克风硬件检测失败，请检查麦克风连接")

    raise RuntimeError(f"设备返回了非预期响应: {response}")


def _run_record_test(ctx):
    device_manager, _ = _get_services()
    if not device_manager:
        raise RuntimeError("设备管理器未初始化")
    if not device_manager.has_connections():
        raise RuntimeError("设备未连接")

    record_seconds = 3
    record_timeout = max(RECORD_TEST_TIMEOUT, record_seconds + RECORD_UPLOAD_GRACE_TIMEOUT)
    ctx.set_progress(10, f"正在下发 {record_seconds} 秒录音命令")
    logger.info("麦克风录音测试: 请求 %d 秒录音", record_seconds)

    waiter = device_manager.register_waiter(
        expected="MIC_REC_DONE", timeout=record_timeout
    )

    try:
        if not device_manager.send_command_async(f"MIC_REC:{record_seconds}\n"):
            raise RuntimeError("录音测试命令发送失败")

        ctx.set_progress(30, f"设备正在录制 {record_seconds} 秒音频…")
        response = device_manager.await_waiter(
            waiter["waiter_id"], timeout=record_timeout
        )
        if not response:
            raise RuntimeError(
                f"设备未在 {record_timeout:.0f} 秒内返回 MIC_REC_DONE"
            )

        if not response.startswith("MIC_REC_DONE"):
            raise RuntimeError(f"设备返回了非预期响应: {response}")

        logger.info("设备录音完成响应: %s", response)

        ctx.set_progress(80, "等待音频数据写入完成…")
        latest_audio = _wait_latest_audio(device_manager, timeout=5.0)
        audio_url = ""
        if latest_audio:
            audio_url = str(latest_audio.get("url") or "")

        if not audio_url:
            raise RuntimeError("录音已结束，但未接收到可播放音频")

        duration = latest_audio.get("duration", 0)
        pcm_bytes = latest_audio.get("bytes", 0)
        sample_rate = latest_audio.get("sample_rate", 0)
        logger.info(
            "录音测试完成: 时长=%.2fs, PCM=%d字节, 采样率=%dHz",
            duration, pcm_bytes, sample_rate,
        )

        # 音频时长异常短时附加提示
        context_msg = "录音采集成功。请点击播放按钮试听。"
        if duration < record_seconds * 0.5:
            context_msg += (
                f"\n⚠ 实际时长仅 {duration:.1f}s（预期 {record_seconds}s），"
                "可能存在数据丢失，请检查网络稳定性。"
            )

        ctx.set_progress(100, "录音采集完成")
        return {
            "success": True,
            "audio_url": audio_url,
            "audio_path": latest_audio.get("path") if latest_audio else "",
            "context": context_msg,
            "response": response,
            "duration": round(duration, 2),
            "sample_rate": sample_rate,
        }
    finally:
        device_manager.cancel_waiter(waiter["waiter_id"])


def _run_recognize_test(ctx):
    from ..app import app_state
    from ..services.chat_service import get_llm_reply
    from ..services.speech_service import recognize_audio

    device_manager, _ = _get_services()
    if not device_manager:
        raise RuntimeError("设备管理器未初始化")

    latest_audio = _wait_latest_audio(device_manager, timeout=5.0)
    if not latest_audio:
        raise RuntimeError("未找到可识别的录音，请先完成录音采集")

    audio_path = str(latest_audio.get("path") or "")
    if not audio_path:
        raise RuntimeError("录音文件路径无效")

    ctx.set_progress(20, "正在上传音频到云端识别")
    recognized_text = (recognize_audio(audio_path) or "").strip()
    if not recognized_text:
        raise RuntimeError("云端语音识别失败或返回空结果")

    ctx.set_progress(60, "识别完成，正在调用大模型")
    db = app_state.get("db")
    session_id = "mic_test_session"
    if db:
        if not db.get_session(session_id):
            db.create_session(session_id=session_id, title="麦克风测试会话")
        db.add_message(session_id, "user", recognized_text)

    reply_text = (get_llm_reply(session_id, recognized_text) or "").strip()
    if not reply_text:
        raise RuntimeError("大模型未返回有效回复")

    if db:
        db.add_message(session_id, "assistant", reply_text)

    device_manager.send_text_to_device(recognized_text, role="user")
    device_manager.send_text_to_device(reply_text, role="assistant")

    ctx.set_progress(100, "云端识别与对话完成")
    return {
        "success": True,
        "text": recognized_text,
        "reply": reply_text,
        "audio_url": str(latest_audio.get("url") or ""),
    }


@mic_test_bp.route("/hardware", methods=["POST"])
@require_auth
def test_hardware():
    """步骤 1：提交硬件通信检测任务"""
    _, job_manager = _get_services()
    if not job_manager:
        return jsonify({"success": False, "context": "任务管理器未初始化"}), 500

    try:
        job_id = job_manager.submit("mic_hardware_test", _run_hardware_test)
        return jsonify(
            {
                "success": True,
                "job_id": job_id,
                "status": "pending",
                "message": "硬件通信检测任务已提交",
            }
        ), 202
    except Exception as e:
        logger.exception("提交麦克风硬件检测任务失败")
        return jsonify({"success": False, "context": str(e)}), 500


@mic_test_bp.route("/record", methods=["POST"])
@require_auth
def test_record():
    """步骤 2：提交录音回放测试任务"""
    _, job_manager = _get_services()
    if not job_manager:
        return jsonify({"success": False, "context": "任务管理器未初始化"}), 500

    try:
        job_id = job_manager.submit("mic_record_test", _run_record_test)
        return jsonify(
            {
                "success": True,
                "job_id": job_id,
                "status": "pending",
                "message": "录音回放测试任务已提交",
            }
        ), 202
    except Exception as e:
        logger.exception("提交麦克风录音测试任务失败")
        return jsonify({"success": False, "context": str(e)}), 500


@mic_test_bp.route("/recognize", methods=["POST"])
@require_auth
def test_recognize():
    """步骤 3：提交语音识别测试任务"""
    _, job_manager = _get_services()
    if not job_manager:
        return jsonify({"success": False, "context": "任务管理器未初始化"}), 500

    try:
        job_id = job_manager.submit("mic_recognize_test", _run_recognize_test)
        return jsonify(
            {
                "success": True,
                "job_id": job_id,
                "status": "pending",
                "message": "语音识别测试任务已提交",
            }
        ), 202
    except Exception as e:
        logger.exception("提交麦克风识别测试任务失败")
        return jsonify({"success": False, "context": str(e)}), 500


@mic_test_bp.route("/jobs/<job_id>", methods=["GET"])
@require_auth
def get_job_status(job_id: str):
    """查询麦克风测试任务状态"""
    _, job_manager = _get_services()
    if not job_manager:
        return jsonify({"success": False, "error": "任务管理器未初始化"}), 500

    return _serialize_job(job_manager.get_job_dict(job_id))


@mic_test_bp.route("/jobs/<job_id>", methods=["DELETE"])
@require_auth
def cancel_job(job_id: str):
    """取消麦克风测试任务"""
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
