"""配置与状态 API 路由"""

import logging
from flask import Blueprint, request, jsonify

from ..auth.dependencies import require_auth

logger = logging.getLogger(__name__)

config_bp = Blueprint("config", __name__, url_prefix="/api")


@config_bp.route("/config", methods=["GET"])
@require_auth
def get_config():
    """获取完整配置"""
    from ..app import app_state

    config = app_state.get("config")
    if not config:
        return jsonify({"error": "配置未初始化"}), 500

    return jsonify({"success": True, "data": config.get_all()})


@config_bp.route("/config", methods=["POST"])
@require_auth
def save_config():
    """保存配置"""
    from ..app import app_state

    config = app_state.get("config")
    if not config:
        return jsonify({"error": "配置未初始化"}), 500

    body = request.get_json(silent=True)
    if not body or not isinstance(body, dict):
        return jsonify({"error": "无效的配置数据"}), 422

    # 安全过滤: 只允许指定的顶层键
    allowed_keys = {"device", "speech", "llm", "tts", "advanced"}
    filtered = {k: v for k, v in body.items() if k in allowed_keys}

    config.update_all(filtered)
    logger.info("配置已更新")
    return jsonify({"success": True, "message": "配置已保存"})


@config_bp.route("/status", methods=["GET"])
@require_auth
def get_status():
    """获取系统状态"""
    from ..app import app_state

    device_mgr = app_state.get("device_manager")
    config = app_state.get("config")

    device_connected = False
    device_info = ""
    connected_clients = 0

    if device_mgr:
        device_connected = device_mgr.has_connections()
        connected_clients = device_mgr.client_count()
        device_info = f"{connected_clients} 个设备在线"

    tcp_port = config.get("device.tcp_port", 8266) if config else 8266

    return jsonify({
        "device_connected": device_connected,
        "device_info": device_info,
        "connected_clients": connected_clients,
        "tcp_port": tcp_port,
        "device_name": config.get("device.device_name", "STM32F103VET6") if config else "STM32F103VET6",
    })
