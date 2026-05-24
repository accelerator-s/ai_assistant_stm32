"""Flask 主应用 — STM32 语音交互助手 WebUI 后端"""

import logging
import os
import requests
import io
from pathlib import Path

from flask import Flask, jsonify, request, send_from_directory, send_file

from .auth import AuthManager
from .config import Config
from .database import DatabaseManager
from .security import RateLimiter
from .services.async_job_manager import AsyncJobManager
from .services.device_manager import DeviceManager

# 配置日志
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
)
logger = logging.getLogger(__name__)

# 全局状态（模仿 qq_with_openai 的 app_state 风格）
app_state = {
    "config": None,
    "db": None,
    "auth_manager": None,
    "device_manager": None,
    "rate_limiter": None,
    "job_manager": None,
}

# 路径常量
_ROOT_DIR = Path(__file__).parent.parent
_CONFIG_PATH = _ROOT_DIR / "config" / "default_config.json"
_DB_PATH = _ROOT_DIR / "data" / "assistant.db"
_FRONTEND_DIR = _ROOT_DIR / "Frontend"
_MEDIA_DIR = _ROOT_DIR / "data" / "audio"


def create_app() -> Flask:
    """应用工厂函数"""
    app = Flask(__name__, static_folder=None)
    app.config["JSON_AS_ASCII"] = False
    _MEDIA_DIR.mkdir(parents=True, exist_ok=True)

    # ---- 初始化核心组件 ----
    _init_components()

    # ---- 注册蓝图 ----
    from .api.auth_routes import auth_bp
    from .api.chat_routes import chat_bp
    from .api.config_routes import config_bp
    from .api.conversation_routes import conversation_bp
    from .api.device_routes import device_bp
    from .api.mic_test_routes import mic_test_bp
    from .api.security_routes import security_bp
    from .api.speaker_test_routes import speaker_test_bp
    from .api.user_routes import user_bp

    app.register_blueprint(auth_bp)
    app.register_blueprint(config_bp)
    app.register_blueprint(device_bp)
    app.register_blueprint(conversation_bp)
    app.register_blueprint(security_bp)
    app.register_blueprint(user_bp, url_prefix="/api/user")
    app.register_blueprint(chat_bp, url_prefix="/api/chat")
    app.register_blueprint(mic_test_bp, url_prefix="/api/test/mic")
    app.register_blueprint(speaker_test_bp, url_prefix="/api/test/speaker")

    # ---- 安全中间件 ----
    @app.before_request
    def security_checks():
        """每次请求前执行安全检查: IP 封禁 + 速率限制"""
        ip = _get_client_ip()

        # IP 封禁检查
        db = app_state.get("db")
        if db and db.is_ip_banned(ip):
            logger.warning(f"已封禁 IP 访问被拒: {ip}")
            return jsonify({"error": "访问被拒绝"}), 403

        # 速率限制（仅对 API 端点）
        if request.path.startswith("/api/"):
            # 异步任务轮询端点需要高频访问，避免误触发全局限流
            if (
                (
                    request.path.startswith("/api/test/mic/jobs/")
                    or request.path.startswith("/api/test/speaker/jobs/")
                )
                and request.method == "GET"
            ):
                return None

            limiter = app_state.get("rate_limiter")
            if limiter and not limiter.is_allowed(ip):
                logger.warning(f"速率限制触发: {ip}")
                return jsonify({"error": "请求过于频繁"}), 429

    # ---- 安全响应头 ----
    @app.after_request
    def security_headers(response):
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["X-Frame-Options"] = "DENY"
        response.headers["X-XSS-Protection"] = "1; mode=block"
        response.headers["Referrer-Policy"] = "strict-origin-when-cross-origin"
        if request.is_secure:
            response.headers["Strict-Transport-Security"] = (
                "max-age=31536000; includeSubDomains"
            )
        return response

    # ---- 静态文件服务 ----
    @app.route("/")
    def serve_index():
        return send_from_directory(str(_FRONTEND_DIR), "index.html")

    @app.route("/static/<path:filename>")
    def serve_static(filename):
        """按子目录分发静态资源"""
        return send_from_directory(str(_FRONTEND_DIR), filename)

    @app.route("/media/<path:filename>")
    def serve_media(filename):
        """分发设备录音文件，供页面回放。"""
        return send_from_directory(str(_MEDIA_DIR), filename)

        # ---- TTS 测试接口 ----
    @app.route("/api/tts/test", methods=["POST"])
    def test_tts():

        data = request.json or {}

        text = data.get("text", "你好，这是测试语音")

        base_url = data.get("base_url", "").rstrip("/")
        api_key = data.get("api_key", "")
        model = data.get("model", "tts-1")
        voice = data.get("voice", "alloy")

        if not base_url:
            return jsonify({
                "success": False,
                "message": "缺少 Base URL"
            }), 400

        if not api_key:
            return jsonify({
                "success": False,
                "message": "缺少 API Key"
            }), 400

        try:

            response = requests.post(

                f"{base_url}/audio/speech",

                headers={
                    "Authorization": f"Bearer {api_key}",
                    "Content-Type": "application/json",
                },

                json={
                    "model": model,
                    "input": text,
                    "voice": voice,
                },

                timeout=60,
            )

            if response.status_code != 200:

                logger.error(
                    f"TTS 请求失败: {response.text}"
                )

                return jsonify({
                    "success": False,
                    "message": response.text,
                }), 500

            return send_file(

                io.BytesIO(response.content),

                mimetype="audio/mpeg",

                as_attachment=False,

                download_name="tts.mp3",
            )

        except Exception as e:

            logger.exception("TTS 接口异常")

            return jsonify({
                "success": False,
                "message": str(e),
            }), 500
        # ---- 优雅关闭 ----
    import atexit

    atexit.register(_shutdown_components)

    return app


def _init_components():
    """初始化所有后端组件"""
    # 配置管理
    app_state["config"] = Config(str(_CONFIG_PATH))

    config = app_state["config"]
    log_level = config.get("advanced.log_level", "INFO")
    logging.getLogger().setLevel(getattr(logging, log_level, logging.INFO))

    # 数据库
    app_state["db"] = DatabaseManager(str(_DB_PATH))

    # 认证管理（数据库存储密码哈希）
    app_state["auth_manager"] = AuthManager(app_state["db"])
    app_state["auth_manager"].initialize()

    # 速率限制
    app_state["rate_limiter"] = RateLimiter(max_requests=60, window_sec=60)

    # 异步任务管理器
    app_state["job_manager"] = AsyncJobManager(max_workers=4, max_jobs=1000)

    # TCP 设备通信服务器
    tcp_host = config.get("device.tcp_host", "0.0.0.0")
    tcp_port = config.get("device.tcp_port", 8266)
    app_state["device_manager"] = DeviceManager(host=tcp_host, port=tcp_port)
    app_state["device_manager"].start()

    logger.info("所有组件初始化完成")


def _shutdown_components():
    """关闭所有组件"""
    device_mgr = app_state.get("device_manager")
    if device_mgr:
        device_mgr.stop()

    job_manager = app_state.get("job_manager")
    if job_manager:
        job_manager.shutdown(wait=False)

    logger.info("所有组件已关闭")


def _get_client_ip() -> str:
    """从请求中提取真实客户端 IP"""
    # 优先使用反向代理传递的真实 IP
    forwarded = request.headers.get("X-Forwarded-For", "")
    if forwarded:
        return forwarded.split(",")[0].strip()
    real_ip = request.headers.get("X-Real-IP", "")
    if real_ip:
        return real_ip.strip()
    return request.remote_addr or "unknown"
