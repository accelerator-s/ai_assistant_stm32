"""IP 安全管理 API 路由"""

import logging
from flask import Blueprint, request, jsonify

from ..auth.dependencies import require_auth

logger = logging.getLogger(__name__)

security_bp = Blueprint("security", __name__, url_prefix="/api/security")


@security_bp.route("/bans", methods=["GET"])
@require_auth
def get_bans():
    """获取 IP 封禁列表"""
    from ..app import app_state

    db = app_state.get("db")
    if not db:
        return jsonify({"error": "数据库未初始化"}), 500

    bans = db.get_ip_bans()
    return jsonify({"success": True, "data": bans})


@security_bp.route("/bans", methods=["POST"])
@require_auth
def ban_ip():
    """封禁 IP"""
    from ..app import app_state

    db = app_state.get("db")
    if not db:
        return jsonify({"error": "数据库未初始化"}), 500

    body = request.get_json(silent=True) or {}
    ip = (body.get("ip") or "").strip()
    reason = (body.get("reason") or "").strip()
    permanent = body.get("permanent", False)

    if not ip:
        return jsonify({"error": "IP 地址不能为空"}), 422

    db.ban_ip(ip, reason=reason, permanent=permanent)
    logger.info(f"IP 已封禁: {ip}")
    return jsonify({"success": True, "message": f"已封禁 {ip}"})


@security_bp.route("/bans/<path:ip>", methods=["DELETE"])
@require_auth
def unban_ip(ip: str):
    """解除 IP 封禁"""
    from ..app import app_state

    db = app_state.get("db")
    if not db:
        return jsonify({"error": "数据库未初始化"}), 500

    db.unban_ip(ip)
    logger.info(f"IP 已解封: {ip}")
    return jsonify({"success": True, "message": f"已解封 {ip}"})
