import base64
import logging
import wave
from pathlib import Path

import requests

logger = logging.getLogger(__name__)

TENCENT_SENTENCE_MAX_BASE64_BYTES = 3 * 1024 * 1024


def recognize_audio(audio_file_path: str) -> str:
    """Call the configured speech recognition service and return recognized text."""
    from ..app import app_state

    try:
        config = app_state.get("config")
        if not config:
            return "出错了：配置未初始化"

        provider = (config.get("speech.provider", "openai_whisper") or "").strip().lower()

        if provider == "openai_whisper":
            return _recognize_openai_whisper(audio_file_path, config)

        if provider in {"tencent", "tencent_asr"}:
            return _recognize_tencent_sentence(audio_file_path, config)

        return f"不支持的语音识别服务商: {provider}"

    except Exception:
        logger.exception("语音识别调用失败")
        return ""


def _recognize_openai_whisper(audio_file_path: str, config) -> str:
    api_key = config.get("speech.api_key")
    base_url = config.get("speech.base_url")
    model = config.get("speech.model", "whisper-1") or "whisper-1"

    if not api_key:
        return "未配置语音识别 API 密钥"
    if not base_url:
        return "未配置 Whisper Base URL"

    url = base_url.rstrip("/") + "/audio/transcriptions"
    headers = {"Authorization": f"Bearer {api_key}"}
    with open(audio_file_path, "rb") as f:
        files = {"file": f}
        data = {"model": model}
        resp = requests.post(url, headers=headers, files=files, data=data, timeout=30)
        resp.raise_for_status()
        return (resp.json().get("text") or "").strip()


def _recognize_tencent_sentence(audio_file_path: str, config) -> str:
    try:
        from tencentcloud.asr.v20190614 import asr_client, models
        from tencentcloud.common import credential
        from tencentcloud.common.exception.tencent_cloud_sdk_exception import (
            TencentCloudSDKException,
        )
        from tencentcloud.common.profile.client_profile import ClientProfile
        from tencentcloud.common.profile.http_profile import HttpProfile
    except ImportError:
        logger.exception("腾讯云 SDK 未安装")
        return "未安装腾讯云 SDK，请先安装 tencentcloud-sdk-python"

    secret_id = config.get("speech.secret_id")
    secret_key = config.get("speech.secret_key")
    region = config.get("speech.region", "ap-shanghai") or "ap-shanghai"
    model = config.get("speech.model", "16k_zh") or "16k_zh"

    if not secret_id or not secret_key:
        return "未配置腾讯云 SecretId 或 SecretKey"

    audio_path = Path(audio_file_path)
    if not audio_path.exists():
        return "音频文件不存在"

    audio_payload = _build_tencent_audio_payload(audio_path, model)
    if len(base64.b64encode(audio_payload["data"])) > TENCENT_SENTENCE_MAX_BASE64_BYTES:
        return "音频文件过大：腾讯云一句话识别要求 Base64 后不超过 3MB"

    cred = credential.Credential(secret_id, secret_key)
    http_profile = HttpProfile()
    http_profile.endpoint = "asr.tencentcloudapi.com"
    client_profile = ClientProfile()
    client_profile.httpProfile = http_profile
    client = asr_client.AsrClient(cred, region, client_profile)

    req = models.SentenceRecognitionRequest()
    req.EngSerViceType = model
    req.SourceType = 1
    req.VoiceFormat = audio_payload["format"]
    req.Data = base64.b64encode(audio_payload["data"]).decode("ascii")
    req.DataLen = len(audio_payload["data"])
    req.ProjectId = 0
    req.SubServiceType = 2
    req.UsrAudioKey = audio_path.stem
    req.FilterDirty = 0
    req.FilterModal = 0
    req.FilterPunc = 0
    req.ConvertNumMode = 1

    if audio_payload.get("input_sample_rate"):
        req.InputSampleRate = audio_payload["input_sample_rate"]

    try:
        resp = client.SentenceRecognition(req)
    except TencentCloudSDKException as err:
        logger.error("腾讯云语音识别失败: %s %s", err.code, err.message)
        return f"腾讯云语音识别失败: {err.message}"

    return (resp.Result or "").strip()


def _build_tencent_audio_payload(audio_path: Path, model: str) -> dict:
    suffix = audio_path.suffix.lower().lstrip(".")
    if suffix == "wav":
        wav_payload = _try_build_pcm_payload_from_wav(audio_path, model)
        if wav_payload:
            return wav_payload

    data = audio_path.read_bytes()
    return {
        "data": data,
        "format": suffix or "wav",
        "input_sample_rate": None,
    }


def _try_build_pcm_payload_from_wav(audio_path: Path, model: str) -> dict | None:
    try:
        with wave.open(str(audio_path), "rb") as wav_file:
            channels = wav_file.getnchannels()
            sample_width = wav_file.getsampwidth()
            sample_rate = wav_file.getframerate()
            frames = wav_file.readframes(wav_file.getnframes())
    except (wave.Error, OSError):
        logger.warning("无法按 WAV 解析音频，改用原始文件上传: %s", audio_path)
        return None

    if channels != 1 or sample_width != 2:
        return None

    input_sample_rate = None
    if sample_rate == 8000 and model.startswith("16k_"):
        input_sample_rate = 8000

    return {
        "data": frames,
        "format": "pcm",
        "input_sample_rate": input_sample_rate,
    }
