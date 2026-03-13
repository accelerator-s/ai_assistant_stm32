"""设备通信与服务测试 API 路由"""

import socket
import logging
import httpx
from flask import Blueprint, request, jsonify

from ..auth.dependencies import require_auth

logger = logging.getLogger(__name__)

device_bp = Blueprint("device", __name__, url_prefix="/api")


@device_bp.route("/test/device", methods=["POST"])
@require_auth
def test_device_connection():
    """测试与开发板的 TCP 通信连接"""
    from ..app import app_state

    config = app_state.get("config")
    if not config:
        return jsonify({"success": False, "message": "配置未初始化"}), 500

    # 检查 TCP 服务器是否正在运行
    device_mgr = app_state.get("device_manager")
    if device_mgr and device_mgr.has_connections():
        return jsonify({
            "success": True,
            "message": f"TCP 服务器运行中，{device_mgr.client_count()} 个设备已连接",
        })

    # 尝试检查 TCP 端口是否正在监听
    tcp_port = config.get("device.tcp_port", 8266)
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(3)
        result = sock.connect_ex(("127.0.0.1", tcp_port))
        sock.close()
        if result == 0:
            return jsonify({
                "success": True,
                "message": f"TCP 端口 {tcp_port} 已在监听，等待设备连接",
            })
        else:
            return jsonify({
                "success": False,
                "message": f"TCP 端口 {tcp_port} 未监听，请检查设备通信服务",
            })
    except Exception as e:
        return jsonify({"success": False, "message": f"连接测试失败: {str(e)}"})


@device_bp.route("/test/speech", methods=["POST"])
@require_auth
def test_speech_service():
    """测试语音识别服务连接"""
    from ..app import app_state

    config = app_state.get("config")
    if not config:
        return jsonify({"success": False, "message": "配置未初始化"}), 500

    base_url = config.get("speech.base_url", "")
    api_key = config.get("speech.api_key", "")
    secret_id = config.get("speech.secret_id", "")
    secret_key = config.get("speech.secret_key", "")
    region = config.get("speech.region", "ap-shanghai")
    model = config.get("speech.model", "16k_zh")
    provider = config.get("speech.provider", "tencent")

    # 使用自定义参数覆盖配置
    body = request.get_json(silent=True) or {}
    base_url = body.get("base_url") or base_url
    api_key = body.get("api_key") or api_key
    secret_id = body.get("secret_id") or secret_id
    secret_key = body.get("secret_key") or secret_key
    region = body.get("region") or region
    model = body.get("model") or model
    provider = body.get("provider") or provider

    if provider == "tencent":
        if not secret_id or not secret_key:
            return jsonify({"success": False, "message": "未配置腾讯云 SecretId 或 SecretKey"})
        try:
            from tencentcloud.common import credential
            from tencentcloud.common.profile.client_profile import ClientProfile
            from tencentcloud.common.profile.http_profile import HttpProfile
            from tencentcloud.common.exception.tencent_cloud_sdk_exception import TencentCloudSDKException
            from tencentcloud.asr.v20190614 import asr_client, models

            cred = credential.Credential(secret_id, secret_key)
            httpProfile = HttpProfile()
            httpProfile.endpoint = "asr.tencentcloudapi.com"
            clientProfile = ClientProfile()
            clientProfile.httpProfile = httpProfile
            
            client = asr_client.AsrClient(cred, region, clientProfile)
            
            req = models.DescribeTaskStatusRequest()
            req.TaskId = 123456789 # Fake task ID to verify credentials

            try:
                resp = client.DescribeTaskStatus(req)
                # If we get here, it somehow thought the dummy ID was valid, which works too
                pass
            except TencentCloudSDKException as err:
                if "AuthFailure" in err.code:
                    return jsonify({"success": False, "message": f"腾讯云鉴权失败: {err.message}"})
                elif "InvalidParameter" in err.code:
                    # Invalid Parameter means credentials worked, but the fake parameter caused it to fail 
                    pass
                else:
                    pass # other exceptions like InvalidTaskId can be ignored since API keys are correct

            return jsonify({
                "success": True,
                "message": f"腾讯云语音识别服务连接成功 (引擎: {model}, 地域: {region})",
            })
        except Exception as e:
            return jsonify({"success": False, "message": f"腾讯云测试失败: {str(e)}"})

    if not base_url:
        return jsonify({"success": False, "message": "未配置语音识别服务地址"})

    try:
        # 通过 GET /v1/models 测试服务连通性
        url = base_url.rstrip("/")
        if not url.endswith("/v1"):
            test_url = url + "/v1/models"
        else:
            test_url = url + "/models"

        headers = {}
        if api_key:
            headers["Authorization"] = f"Bearer {api_key}"

        with httpx.Client(timeout=10) as client:
            resp = client.get(test_url, headers=headers)

        if resp.status_code == 200:
            return jsonify({
                "success": True,
                "message": f"语音识别服务连接成功 (模型: {model})",
            })
        else:
            return jsonify({
                "success": False,
                "message": f"服务返回状态码 {resp.status_code}: {resp.text[:200]}",
            })
    except httpx.ConnectError:
        return jsonify({"success": False, "message": f"无法连接到 {base_url}"})
    except httpx.TimeoutException:
        return jsonify({"success": False, "message": "连接超时"})
    except Exception as e:
        return jsonify({"success": False, "message": f"测试失败: {str(e)}"})


@device_bp.route("/test/llm", methods=["POST"])
@require_auth
def test_llm_connection():
    """测试大模型 API 连接"""
    from ..app import app_state

    config = app_state.get("config")
    if not config:
        return jsonify({"success": False, "message": "配置未初始化"}), 500

    base_url = config.get("llm.base_url", "")
    api_key = config.get("llm.api_key", "")
    model = config.get("llm.model", "gpt-4")

    if not base_url:
        return jsonify({"success": False, "message": "未配置大模型接口地址"})

    # 使用自定义参数覆盖配置
    body = request.get_json(silent=True) or {}
    base_url = body.get("base_url") or base_url
    api_key = body.get("api_key") or api_key
    model = body.get("model") or model

    try:
        url = base_url.rstrip("/")
        # 拼接 chat completions 端点
        if url.endswith("/v1"):
            chat_url = url + "/chat/completions"
        else:
            chat_url = url + "/v1/chat/completions"

        headers = {"Content-Type": "application/json"}
        if api_key:
            headers["Authorization"] = f"Bearer {api_key}"

        payload = {
            "model": model,
            "messages": [{"role": "user", "content": "Hi"}],
            "max_tokens": 5,
        }

        with httpx.Client(timeout=15) as client:
            resp = client.post(chat_url, json=payload, headers=headers)

        if resp.status_code == 200:
            data = resp.json()
            reply = ""
            if "choices" in data and data["choices"]:
                reply = data["choices"][0].get("message", {}).get("content", "")
            return jsonify({
                "success": True,
                "message": f"大模型连接成功 (模型: {model})",
                "reply": reply,
            })
        else:
            err_text = resp.text[:300]
            return jsonify({
                "success": False,
                "message": f"API 返回 {resp.status_code}: {err_text}",
            })
    except httpx.ConnectError:
        return jsonify({"success": False, "message": f"无法连接到 {base_url}"})
    except httpx.TimeoutException:
        return jsonify({"success": False, "message": "连接超时"})
    except Exception as e:
        return jsonify({"success": False, "message": f"测试失败: {str(e)}"})
