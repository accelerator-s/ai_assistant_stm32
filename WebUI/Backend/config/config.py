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
            self._data = {}
            logger.warning(f"配置文件不存在，使用空配置: {self._path}")

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
