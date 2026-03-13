"""设备通信管理器 — 管理 TCP 连接的 ESP8266 终端设备"""

import socket
import threading
import logging
import time

logger = logging.getLogger(__name__)


class DeviceConnection:
    """单个设备 TCP 连接"""

    def __init__(self, conn: socket.socket, addr: tuple):
        self.conn = conn
        self.addr = addr
        self.connected_at = time.time()
        self.last_active = time.time()

    def __repr__(self):
        return f"<DeviceConnection {self.addr[0]}:{self.addr[1]}>"


class DeviceManager:
    """TCP 服务器: 接受 ESP8266 设备连接，管理已连接的终端"""

    def __init__(self, host: str = "0.0.0.0", port: int = 8266):
        self._host = host
        self._port = port
        self._server_socket: socket.socket | None = None
        self._clients: dict[str, DeviceConnection] = {}
        self._lock = threading.Lock()
        self._running = False
        self._accept_thread: threading.Thread | None = None

    # ------------------------------------------------------------------
    # 启动与停止
    # ------------------------------------------------------------------
    def start(self) -> None:
        """启动 TCP 服务器监听"""
        if self._running:
            return
        self._running = True
        self._server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._server_socket.settimeout(1.0)
        self._server_socket.bind((self._host, self._port))
        self._server_socket.listen(5)

        self._accept_thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._accept_thread.start()
        logger.info(f"TCP 设备服务器已启动: {self._host}:{self._port}")

    def stop(self) -> None:
        """停止 TCP 服务器"""
        self._running = False
        if self._server_socket:
            try:
                self._server_socket.close()
            except Exception:
                pass
        with self._lock:
            for key, client in self._clients.items():
                try:
                    client.conn.close()
                except Exception:
                    pass
            self._clients.clear()
        logger.info("TCP 设备服务器已停止")

    # ------------------------------------------------------------------
    # 连接状态查询
    # ------------------------------------------------------------------
    def has_connections(self) -> bool:
        """是否有设备连接"""
        with self._lock:
            return len(self._clients) > 0

    def client_count(self) -> int:
        """已连接的设备数量"""
        with self._lock:
            return len(self._clients)

    def get_clients_info(self) -> list[dict]:
        """已连接设备信息列表"""
        with self._lock:
            return [
                {
                    "address": f"{c.addr[0]}:{c.addr[1]}",
                    "connected_at": c.connected_at,
                    "last_active": c.last_active,
                }
                for c in self._clients.values()
            ]

    @property
    def port(self) -> int:
        return self._port

    # ------------------------------------------------------------------
    # 内部
    # ------------------------------------------------------------------
    def _accept_loop(self):
        """接受新连接的主循环"""
        while self._running:
            try:
                conn, addr = self._server_socket.accept()
                key = f"{addr[0]}:{addr[1]}"
                device = DeviceConnection(conn, addr)
                with self._lock:
                    self._clients[key] = device
                logger.info(f"设备已连接: {key}")

                # 为每个连接启动独立处理线程
                handler = threading.Thread(
                    target=self._handle_client, args=(key, device), daemon=True
                )
                handler.start()
            except socket.timeout:
                continue
            except OSError:
                if self._running:
                    logger.error("TCP 接受连接异常")
                break

    def _handle_client(self, key: str, device: DeviceConnection):
        """处理单个设备连接的数据收发"""
        try:
            device.conn.settimeout(60)
            while self._running:
                try:
                    data = device.conn.recv(4096)
                    if not data:
                        break
                    device.last_active = time.time()
                    # 实际项目中此处分发给音频处理流水线
                except socket.timeout:
                    continue
                except ConnectionResetError:
                    break
        except Exception as e:
            logger.error(f"设备 {key} 处理异常: {e}")
        finally:
            with self._lock:
                self._clients.pop(key, None)
            try:
                device.conn.close()
            except Exception:
                pass
            logger.info(f"设备已断开: {key}")
