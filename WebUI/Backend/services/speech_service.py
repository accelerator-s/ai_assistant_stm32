import logging

logger = logging.getLogger(__name__)

def recognize_audio(audio_file_path: str) -> str:
    """调用语音识别服务将音频转为文本"""
    from ..app import app_state
    import requests

    try:
        config = app_state.get('config')
        provider = config.get('speech.provider', 'openai_whisper') if config else 'openai_whisper'
        api_key = config.get('speech.api_key') if config else None
        base_url = config.get('speech.base_url') if config else None

        if not api_key:
            return "未配置识别 API 密钥"

        if provider == "openai_whisper":
            if not base_url:
                return "未配置 Whisper Base URL"
            url = base_url.rstrip('/') + '/audio/transcriptions'
            headers = {"Authorization": f"Bearer {api_key}"}
            with open(audio_file_path, 'rb') as f:
                files = {'file': f}
                data = {'model': 'whisper-1'}
                resp = requests.post(url, headers=headers, files=files, data=data, timeout=10)
                resp.raise_for_status()
                return resp.json().get('text', '')

        elif provider == "tencent_asr":
            # 腾讯云 ASR 实现逻辑...
            return "暂不支持腾讯云ASR，请使用 OpenAI Whisper"
        
        return "不支持的语音识别服务商"

    except Exception as e:
        logger.exception("语音识别调用失败")
        return ""
