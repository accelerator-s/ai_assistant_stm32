from flask import Blueprint, request, jsonify
from ..auth.dependencies import require_auth

user_bp = Blueprint('user', __name__)

@user_bp.route('/settings', methods=['GET'])
@require_auth
def get_user_settings():
    """获取用户个性化设置"""
    from ..app import app_state
    db = app_state.get('db')
    try:
        settings = db.get_user_settings() if db else {}
        return jsonify(settings)
    except Exception as e:
        return jsonify({'error': str(e)}), 500

@user_bp.route('/settings', methods=['POST'])
@require_auth
def save_user_settings():
    """保存用户个性化设置并推送到设备"""
    from ..app import app_state
    db = app_state.get('db')
    device_manager = app_state.get('device_manager')
    data = request.json
    if not data:
        return jsonify({'error': '无效的数据'}), 400

    username = data.get('username', '').strip()
    avatar = data.get('avatar', '').strip()

    if not username:
        return jsonify({'error': '用户名不能为空'}), 400

    try:
        db.save_user_settings(username, avatar)
        
        # 将用户名下发到设备 (通过 TCP 推送)
        device_manager.send_command(f'SET_USER:{username}\n')
        
        return jsonify({'success': True})
    except Exception as e:
        return jsonify({'error': str(e)}), 500
