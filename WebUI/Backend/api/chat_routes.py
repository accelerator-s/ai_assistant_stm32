from flask import Blueprint, request, jsonify
from ..auth.dependencies import require_auth
import logging

logger = logging.getLogger(__name__)
chat_bp = Blueprint('chat', __name__)

@chat_bp.route('/send', methods=['POST'])
@require_auth
def send_message():
    """接收 WebUI 发送的聊天消息，或者处理硬件端发来的消息"""
    from ..app import app_state
    db = app_state.get('db')
    device_manager = app_state.get('device_manager')
    data = request.json
    if not data:
        return jsonify({'error': '无效的数据'}), 400

    session_id = data.get('session_id')
    content = data.get('content', '').strip()

    if not session_id or not content:
        return jsonify({'error': '会话 ID 和内容不能为空'}), 400

    try:
        # 1. 保存用户消息
        if db:
            db.add_message(session_id, 'user', content)

        # 2. 同步推送到硬件端显示 (假设有此接口)
        if device_manager:
            device_manager.send_text_to_device(content, role='user')

        # 3. 调用 LLM 获取回复 (由 chat_service 处理)
        from ..services.chat_service import get_llm_reply
        reply_text = get_llm_reply(session_id, content)

        if not reply_text:
            return jsonify({'error': '模型无回复'}), 500

        # 4. 保存 AI 回复
        if db:
            db.add_message(session_id, 'assistant', reply_text)

        # 5. 同步推送到硬件端显示
        if device_manager:
            device_manager.send_text_to_device(reply_text, role='assistant')

        # 如果此后还有 TTS 也可以在这里触发
        return jsonify({'success': True, 'reply': reply_text})

    except Exception as e:
        logger.exception("发送消息失败")
        return jsonify({'error': str(e)}), 500

@chat_bp.route('/sessions', methods=['POST'])
@require_auth
def create_session():
    """创建新会话"""
    from ..app import app_state
    db = app_state.get('db')
    data = request.json
    session_id = data.get('session_id')
    title = data.get('title', '新对话')

    if not session_id:
        import uuid
        session_id = str(uuid.uuid4())

    try:
        if db:
            db.create_session(session_id=session_id, title=title)
        return jsonify({'success': True, 'session_id': session_id})
    except Exception as e:
        return jsonify({'error': str(e)}), 500

@chat_bp.route('/sessions/<session_id>/switch', methods=['POST'])
@require_auth
def switch_session(session_id):
    """切换当前活跃会话，推送上下文到 STM32"""
    from ..app import app_state
    device_manager = app_state.get('device_manager')
    try:
        # 下发 LOAD_SESSION 或直接清屏加载历史
        device_manager.send_command(f'LOAD_SESSION:{session_id}\n')
        return jsonify({'success': True})
    except Exception as e:
        return jsonify({'error': str(e)}), 500
