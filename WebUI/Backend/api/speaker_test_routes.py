import logging
from dataclasses import dataclass
from typing import Any

from flask import Blueprint, jsonify

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
