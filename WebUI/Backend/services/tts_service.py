"""Azure TTS 语音合成服务"""

import html
import io
import logging
import time
import wave
from pathlib import Path

import requests

logger = logging.getLogger(__name__)

AZURE_TTS_OUTPUT_FORMAT = "riff-16khz-16bit-mono-pcm"
TTS_AUDIO_FORMAT = "wav"
TTS_CONTENT_TYPE = "audio/wav"


def _validate_pcm_wav(payload: bytes) -> tuple[bool, str]:
    if len(payload) <= 44:
        return False, "Azure TTS 返回了空音频"

    try:
        with wave.open(io.BytesIO(payload), "rb") as wavf:
            if wavf.getnchannels() != 1:
                return False, "Azure TTS 返回的音频不是单声道"
            if wavf.getsampwidth() != 2:
                return False, "Azure TTS 返回的音频不是 16-bit PCM"
            if wavf.getframerate() != 16000:
                return False, "Azure TTS 返回的音频不是 16kHz"
            if wavf.getnframes() <= 0:
                return False, "Azure TTS 返回了空音频"
    except (wave.Error, EOFError) as exc:
        return False, f"Azure TTS 返回了无效 WAV: {exc}"

    return True, ""


def synthesize(
    region: str,
    subscription_key: str,
    text: str,
    output_dir: Path,
    voice: str = "zh-CN-XiaoxiaoNeural",
) -> dict:
    """调用 Azure TTS 合成语音并保存到文件。

    Returns:
        dict: {"success": True, "audio_url": "/tts/xxx.wav", "audio_format": "wav"}
              或 {"success": False, "message": "错误信息"}
    """
    region = (region or "").strip()
    subscription_key = (subscription_key or "").strip()
    voice = (voice or "zh-CN-XiaoxiaoNeural").strip()
    escaped_text = html.escape(text or "", quote=False)
    escaped_voice = html.escape(voice, quote=True)

    if not region:
        return {"success": False, "message": "缺少 Azure 区域"}
    if not subscription_key:
        return {"success": False, "message": "缺少 Azure 订阅密钥"}
    if not escaped_text:
        return {"success": False, "message": "缺少待合成文本"}

    tts_url = f"https://{region}.tts.speech.microsoft.com/cognitiveservices/v1"

    ssml = (
        f"<speak version='1.0' xml:lang='zh-CN'>"
        f"<voice xml:lang='zh-CN' name='{escaped_voice}'>"
        f"{escaped_text}"
        f"</voice></speak>"
    )

    headers = {
        "Ocp-Apim-Subscription-Key": subscription_key,
        "Content-Type": "application/ssml+xml",
        "X-Microsoft-OutputFormat": AZURE_TTS_OUTPUT_FORMAT,
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

        valid, validation_error = _validate_pcm_wav(response.content)
        if not valid:
            logger.error(validation_error)
            return {"success": False, "message": validation_error}

        filename = f"tts_{int(time.time() * 1000)}.{TTS_AUDIO_FORMAT}"
        filepath = output_dir / filename
        output_dir.mkdir(parents=True, exist_ok=True)
        filepath.write_bytes(response.content)

        return {
            "success": True,
            "audio_url": f"/tts/{filename}",
            "audio_filename": filename,
            "audio_format": TTS_AUDIO_FORMAT,
            "content_type": TTS_CONTENT_TYPE,
        }

    except Exception as e:
        logger.exception("Azure TTS 接口异常")
        return {"success": False, "message": str(e)}
