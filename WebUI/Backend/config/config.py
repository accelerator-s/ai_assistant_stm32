"""JSON 配置管理器 — 支持点分路径读写"""

import json
import copy
import logging
from pathlib import Path
from threading import Lock

logger = logging.getLogger(__name__)


class Config:
    """线程安全的 JSON 配置管理器"""

    def __init__(self, config_path: str):
        self._path = Path(config_path)
        self._lock = Lock()
        self._data: dict = {}
        self._load()

    # ------------------------------------------------------------------
    # 读取
    # ------------------------------------------------------------------
    def get(self, key: str, default=None):
        """支持点分路径，如 'llm.base_url'"""
        with self._lock:
            parts = key.split(".")
            node = self._data
            for part in parts:
                if isinstance(node, dict) and part in node:
                    node = node[part]
                else:
                    return default
            return copy.deepcopy(node)

    def get_all(self) -> dict:
        """返回完整配置的深拷贝"""
        with self._lock:
            return copy.deepcopy(self._data)

    # ------------------------------------------------------------------
    # 写入
    # ------------------------------------------------------------------
    def set(self, key: str, value) -> None:
        """设置单个键值"""
        with self._lock:
            parts = key.split(".")
            node = self._data
            for part in parts[:-1]:
                if part not in node or not isinstance(node[part], dict):
                    node[part] = {}
                node = node[part]
            node[parts[-1]] = value
            self._save_unlocked()

    def update_all(self, data: dict) -> None:
        """整体更新配置"""
        with self._lock:
            self._deep_merge(self._data, data)
            self._save_unlocked()

    # ------------------------------------------------------------------
    # 内部
    # ------------------------------------------------------------------
    def _load(self) -> None:
        if self._path.exists():
            with open(self._path, "r", encoding="utf-8") as f:
                self._data = json.load(f)
            logger.info(f"配置已加载: {self._path}")
        else:
            # 配置缺省时：优先从 example 模板初始化，确保首次拉取可直接运行
            template_path = self._path.with_name("default_config.example.json")
            if template_path.exists():
                with open(template_path, "r", encoding="utf-8") as f:
                    self._data = json.load(f)
                self._save_unlocked()
                logger.warning(f"配置文件不存在，已从模板初始化: {self._path}")
            else:
                # 兜底默认值，避免模板缺失导致启动失败
                self._data = {
                    "device": {
                        "tcp_port": 8266,
                        "tcp_host": "0.0.0.0",
                        "device_name": "STM32F103VET6",
                        "audio_sample_rate": 16000,
                        "audio_bit_depth": 16
                    },
                    "speech": {
                        "provider": "tencent",
                        "secret_id": "",
                        "secret_key": "",
                        "region": "ap-shanghai",
                        "model": "16k_zh",
                        "language": "zh"
                    },
                    "llm": {
                        "base_url": "",
                        "api_key": "",
                        "model": "gpt-4",
                        "system_prompt": "你是一个智能语音助手，运行在STM32嵌入式设备上。请用简洁友好的中文回答用户的问题。",
                        "max_tokens": 512,
                        "temperature": 0.7
                    },
                    "tts": {
                        "provider": "openai_tts",
                        "base_url": "",
                        "api_key": "",
                        "model": "tts-1",
                        "voice": "alloy"
                    },
                    "advanced": {
                        "service_port": 5000,
                        "log_level": "INFO",
                        "session_expiry_hours": 24,
                        "max_history_per_session": 50,
                        "audio_buffer_timeout_sec": 30
                    }
                }
                self._save_unlocked()
                logger.warning(f"配置文件与模板都不存在，已写入兜底默认配置: {self._path}")

    def _save_unlocked(self) -> None:
        self._path.parent.mkdir(parents=True, exist_ok=True)
        with open(self._path, "w", encoding="utf-8") as f:
            json.dump(self._data, f, indent=2, ensure_ascii=False)

    @staticmethod
    def _deep_merge(base: dict, override: dict) -> None:
        """递归合并字典"""
        for key, value in override.items():
            if key in base and isinstance(base[key], dict) and isinstance(value, dict):
                Config._deep_merge(base[key], value)
            else:
                base[key] = value
