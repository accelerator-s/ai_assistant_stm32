"""Azure TTS 语音合成服务"""

import logging
import time
from pathlib import Path

import requests

logger = logging.getLogger(__name__)


def synthesize(
    region: str,
    subscription_key: str,
    text: str,
    output_dir: Path,
    voice: str = "zh-CN-XiaoxiaoNeural",
) -> dict:
    """调用 Azure TTS 合成语音并保存到文件。

    Returns:
        dict: {"success": True, "audio_url": "/tts/xxx.mp3"}
              或 {"success": False, "message": "错误信息"}
    """
    region = (region or "").strip()
    subscription_key = (subscription_key or "").strip()

    if not region:
        return {"success": False, "message": "缺少 Azure 区域"}
    if not subscription_key:
        return {"success": False, "message": "缺少 Azure 订阅密钥"}

    tts_url = f"https://{region}.tts.speech.microsoft.com/cognitiveservices/v1"

    ssml = (
        f"<speak version='1.0' xml:lang='zh-CN'>"
        f"<voice xml:lang='zh-CN' name='{voice}'>"
        f"{text}"
        f"</voice></speak>"
    )

    headers = {
        "Ocp-Apim-Subscription-Key": subscription_key,
        "Content-Type": "application/ssml+xml",
        "X-Microsoft-OutputFormat": "audio-16khz-128kbitrate-mono-mp3",
        "User-Agent": "STM32-AI-Assistant",
    }

    try:
        response = requests.post(
            tts_url, headers=headers, data=ssml.encode("utf-8"), timeout=60
        )

        if response.status_code != 200:
            logger.error(
                "Azure TTS 请求失败 [%d]: %s", response.status_code, response.text
            )
            return {
                "success": False,
                "message": f"Azure TTS 错误: {response.status_code} {response.reason}",
            }

        filename = f"tts_{int(time.time() * 1000)}.mp3"
        filepath = output_dir / filename
        filepath.write_bytes(response.content)

        return {"success": True, "audio_url": f"/tts/{filename}"}

    except Exception as e:
        logger.exception("Azure TTS 接口异常")
        return {"success": False, "message": str(e)}
