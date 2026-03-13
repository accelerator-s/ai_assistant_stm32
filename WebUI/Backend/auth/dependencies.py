"""Flask 认证装饰器 — 从 Cookie 中提取 JWT 并验证"""

import functools
import logging

from flask import request, jsonify, g

logger = logging.getLogger(__name__)


def require_auth(fn):
    """装饰器: 要求请求携带有效的 session_token Cookie"""

    @functools.wraps(fn)
    def wrapper(*args, **kwargs):
        from ..app import app_state

        auth_manager = app_state.get("auth_manager")
        if not auth_manager:
            return jsonify({"error": "认证服务未初始化"}), 500

        token = request.cookies.get("session_token")
        if not token:
            return jsonify({"error": "未登录"}), 401

        payload = auth_manager.validate_token(token)
        if payload is None:
            return jsonify({"error": "登录已过期"}), 401

        g.auth_payload = payload
        return fn(*args, **kwargs)

    return wrapper
