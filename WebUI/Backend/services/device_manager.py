"""设备通信管理器 — 管理 TCP 连接的 ESP8266 终端设备

通信协议:
  - 设备每 15 秒发送 "HB\n" 心跳包
  - 服务端回复 "OK\n" 确认
  - 超过 45 秒未收到任何数据视为断线
  - 同一 IP 新连接到来时自动踢掉旧连接（避免重复计数）
"""

import socket
import threading
import logging
import time

logger = logging.getLogger(__name__)

# 心跳超时阈值（秒）：超过此时间未收到任何数据则认定连接死亡
HEARTBEAT_TIMEOUT = 12

# recv 超时（秒）：用于周期性检查心跳是否过期
RECV_TIMEOUT = 10


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

    def manual_heartbeat_probe(self) -> dict:
        """手动心跳探测：用于前端“测试设备连接/刷新状态”按钮触发。

        逻辑：
        1. 先清理心跳超时连接；
        2. 对剩余连接发送轻量探测帧 PING；
        3. 发送失败则立即剔除连接。
        """
        now = time.time()
        timeout_keys: list[str] = []
        send_fail_keys: list[str] = []

        with self._lock:
            items = list(self._clients.items())

            for key, client in items:
                if now - client.last_active > HEARTBEAT_TIMEOUT:
                    timeout_keys.append(key)
                    continue
                try:
                    client.conn.sendall(b"PING\n")
                except Exception:
                    send_fail_keys.append(key)

            for key in timeout_keys + send_fail_keys:
                client = self._clients.pop(key, None)
                if client:
                    try:
                        client.conn.close()
                    except Exception:
                        pass

            alive = len(self._clients)

        if timeout_keys:
            logger.warning(f"手动探测清理超时连接: {timeout_keys}")
        if send_fail_keys:
            logger.warning(f"手动探测清理发送失败连接: {send_fail_keys}")

        return {
            "alive": alive,
            "timeout_removed": len(timeout_keys),
            "send_fail_removed": len(send_fail_keys),
        }

    @property
    def port(self) -> int:
        return self._port

    # ------------------------------------------------------------------
    # 内部
    # ------------------------------------------------------------------
    @staticmethod
    def _enable_tcp_keepalive(conn: socket.socket) -> None:
        """启用 TCP 内核级 keepalive，加速死连接检测"""
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        if hasattr(socket, "TCP_KEEPIDLE"):
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 10)
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 5)
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, 3)

    def _kick_same_ip(self, new_ip: str) -> None:
        """踢掉来自相同 IP 的旧连接（同一物理设备重连时清理残留）"""
        with self._lock:
            stale_keys = [k for k, c in self._clients.items()
                          if c.addr[0] == new_ip]
            for key in stale_keys:
                old = self._clients.pop(key, None)
                if old:
                    try:
                        old.conn.close()
                    except Exception:
                        pass
                    logger.info(f"踢掉旧连接: {key} (同 IP 设备重连)")

    def _accept_loop(self):
        """接受新连接的主循环"""
        while self._running:
            try:
                conn, addr = self._server_socket.accept()
                ip = addr[0]
                key = f"{addr[0]}:{addr[1]}"

                # 启用 TCP keepalive
                self._enable_tcp_keepalive(conn)

                # 同一 IP 新连接到来：先踢掉旧连接
                self._kick_same_ip(ip)

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
            device.conn.settimeout(RECV_TIMEOUT)
            while self._running:
                try:
                    data = device.conn.recv(4096)
                    if not data:
                        break
                    device.last_active = time.time()

                    # 处理心跳包：设备发送 "HB\n"，回复 "OK\n"
                    text = data.decode("utf-8", errors="ignore").strip()
                    if text == "HB":
                        try:
                            device.conn.sendall(b"OK\n")
                        except Exception:
                            break
                        continue

                    # 后续扩展：实际数据分发给音频处理流水线

                except socket.timeout:
                    # 检查心跳超时
                    if time.time() - device.last_active > HEARTBEAT_TIMEOUT:
                        logger.warning(f"设备 {key} 心跳超时 ({HEARTBEAT_TIMEOUT}s)，断开连接")
                        break
                    continue
                except ConnectionResetError:
                    break
        except Exception as e:
            logger.error(f"设备 {key} 处理异常: {e}")
        finally:
            with self._lock:
                # 只移除属于自己的连接（可能已被新连接替换过）
                if self._clients.get(key) is device:
                    self._clients.pop(key, None)
            try:
                device.conn.close()
            except Exception:
                pass
            logger.info(f"设备已断开: {key}")
