"""认证相关 API 路由"""

import logging
from flask import Blueprint, request, jsonify, make_response

logger = logging.getLogger(__name__)

auth_bp = Blueprint("auth", __name__, url_prefix="/api/auth")


@auth_bp.route("/login", methods=["POST"])
def login():
    """管理员登录"""
    from ..app import app_state

    auth_manager = app_state.get("auth_manager")
    if not auth_manager:
        return jsonify({"error": "认证服务未初始化"}), 500

    body = request.get_json(silent=True) or {}
    password = (body.get("password") or "").strip()
    if not password or len(password) > 128:
        return jsonify({"error": "密码格式错误"}), 422

    if not auth_manager.verify_password(password):
        logger.warning("登录失败: 密码错误")
        return jsonify({"error": "密码错误"}), 401

    token = auth_manager.create_token()
    resp = make_response(jsonify({"success": True, "message": "登录成功"}))

    # 根据请求协议动态设置 Cookie 的 secure 标志
    forwarded_proto = (request.headers.get("X-Forwarded-Proto") or "").split(",")[0].strip().lower()
    secure_cookie = request.is_secure or forwarded_proto == "https"

    expiry_hours = auth_manager.session_expiry_hours
    cookie_max_age = expiry_hours * 3600 if expiry_hours > 0 else 31536000

    resp.set_cookie(
        key="session_token",
        value=token,
        httponly=True,
        secure=secure_cookie,
        samesite="Strict",
        max_age=cookie_max_age,
        path="/",
    )
    logger.info(f"管理员登录成功 (secure={secure_cookie})")
    return resp


@auth_bp.route("/logout", methods=["POST"])
def logout():
    """退出登录"""
    resp = make_response(jsonify({"success": True, "message": "已退出登录"}))
    resp.delete_cookie("session_token", path="/")
    return resp


@auth_bp.route("/check", methods=["GET"])
def check_auth():
    """检查当前登录状态"""
    from ..app import app_state

    auth_manager = app_state.get("auth_manager")
    if not auth_manager:
        return jsonify({"authenticated": False}), 500

    token = request.cookies.get("session_token")
    if not token:
        return jsonify({"authenticated": False}), 401

    payload = auth_manager.validate_token(token)
    if payload is None:
        return jsonify({"authenticated": False}), 401

    return jsonify({"authenticated": True})


@auth_bp.route("/change-password", methods=["POST"])
def change_password():
    """修改管理员密码"""
    from ..app import app_state
    from ..auth.dependencies import require_auth

    # 手动验证认证
    auth_manager = app_state.get("auth_manager")
    if not auth_manager:
        return jsonify({"error": "认证服务未初始化"}), 500

    token = request.cookies.get("session_token")
    if not token or not auth_manager.validate_token(token):
        return jsonify({"error": "未登录"}), 401

    body = request.get_json(silent=True) or {}
    old_password = (body.get("old_password") or "").strip()
    new_password = (body.get("new_password") or "").strip()

    if not old_password or not new_password:
        return jsonify({"error": "密码不能为空"}), 422

    if len(new_password) < 8:
        return jsonify({"error": "新密码至少 8 位"}), 422

    if len(new_password) > 128:
        return jsonify({"error": "密码过长"}), 422

    if not auth_manager.change_password(old_password, new_password):
        return jsonify({"error": "当前密码错误"}), 400

    return jsonify({"success": True, "message": "密码修改成功，请重新登录"})
