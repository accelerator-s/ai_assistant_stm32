"""请求速率限制器 — 基于内存的滑动窗口"""

import time
import threading
import logging
from collections import defaultdict

logger = logging.getLogger(__name__)


class RateLimiter:
    """简易滑动窗口速率限制器"""

    def __init__(self, max_requests: int = 60, window_sec: int = 60):
        self._max = max_requests
        self._window = window_sec
        self._lock = threading.Lock()
        self._buckets: dict[str, list[float]] = defaultdict(list)

    def is_allowed(self, ip: str) -> bool:
        """检查 IP 是否在限制范围内"""
        now = time.time()
        cutoff = now - self._window
        with self._lock:
            bucket = self._buckets[ip]
            # 清除过期记录
            self._buckets[ip] = [t for t in bucket if t > cutoff]
            if len(self._buckets[ip]) >= self._max:
                return False
            self._buckets[ip].append(now)
            return True

    def cleanup(self) -> None:
        """清理过期的桶"""
        now = time.time()
        cutoff = now - self._window
        with self._lock:
            empty_keys = []
            for ip, bucket in self._buckets.items():
                self._buckets[ip] = [t for t in bucket if t > cutoff]
                if not self._buckets[ip]:
                    empty_keys.append(ip)
            for ip in empty_keys:
                del self._buckets[ip]
