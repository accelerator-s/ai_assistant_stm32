"""对话历史 API 路由"""

import logging
from flask import Blueprint, request, jsonify

from ..auth.dependencies import require_auth

logger = logging.getLogger(__name__)

conversation_bp = Blueprint("conversation", __name__, url_prefix="/api/conversations")


@conversation_bp.route("/sessions", methods=["GET"])
@require_auth
def get_sessions():
    """获取会话列表"""
    from ..app import app_state

    db = app_state.get("db")
    if not db:
        return jsonify({"error": "数据库未初始化"}), 500

    device_id = request.args.get("device_id")
    limit = min(int(request.args.get("limit", 50)), 200)
    sessions = db.get_sessions(device_id=device_id, limit=limit)
    return jsonify({"success": True, "data": sessions})


@conversation_bp.route("/sessions/<session_id>", methods=["GET"])
@require_auth
def get_session_detail(session_id: str):
    """获取单个会话详情及消息"""
    from ..app import app_state

    db = app_state.get("db")
    if not db:
        return jsonify({"error": "数据库未初始化"}), 500

    session = db.get_session(session_id)
    if not session:
        return jsonify({"error": "会话不存在"}), 404

    messages = db.get_messages(session_id)
    return jsonify({"success": True, "data": {"session": session, "messages": messages}})


@conversation_bp.route("/sessions/<session_id>", methods=["DELETE"])
@require_auth
def delete_session(session_id: str):
    """删除指定会话"""
    from ..app import app_state

    db = app_state.get("db")
    if not db:
        return jsonify({"error": "数据库未初始化"}), 500

    db.delete_session(session_id)
    return jsonify({"success": True, "message": "会话已删除"})


@conversation_bp.route("/sessions/<session_id>/archive", methods=["POST"])
@require_auth
def archive_session(session_id: str):
    """归档会话"""
    from ..app import app_state

    db = app_state.get("db")
    if not db:
        return jsonify({"error": "数据库未初始化"}), 500

    db.update_session(session_id, is_archived=1)
    return jsonify({"success": True, "message": "会话已归档"})
