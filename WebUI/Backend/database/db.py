"""SQLite 数据库管理器 — 管理员认证与对话记录持久化"""

import sqlite3
import threading
import logging
from pathlib import Path
from contextlib import contextmanager

logger = logging.getLogger(__name__)

_DB_SCHEMA = """
CREATE TABLE IF NOT EXISTS admins (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    username        TEXT    NOT NULL UNIQUE DEFAULT 'admin',
    password_hash   TEXT    NOT NULL,
    jwt_secret      TEXT    NOT NULL,
    session_expiry_hours INTEGER NOT NULL DEFAULT 24,
    created_at      TEXT    NOT NULL DEFAULT (datetime('now')),
    updated_at      TEXT    NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS sessions (
    id              TEXT    PRIMARY KEY,
    device_id       TEXT    NOT NULL DEFAULT 'default',
    title           TEXT    NOT NULL DEFAULT '新对话',
    summary         TEXT    DEFAULT '',
    is_archived     INTEGER NOT NULL DEFAULT 0,
    created_at      TEXT    NOT NULL DEFAULT (datetime('now')),
    updated_at      TEXT    NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS messages (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id      TEXT    NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    role            TEXT    NOT NULL CHECK(role IN ('user', 'assistant', 'system')),
    content         TEXT    NOT NULL,
    created_at      TEXT    NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS ip_bans (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    ip_address      TEXT    NOT NULL UNIQUE,
    reason          TEXT    DEFAULT '',
    is_permanent    INTEGER NOT NULL DEFAULT 0,
    banned_at       TEXT    NOT NULL DEFAULT (datetime('now')),
    expires_at      TEXT
);

CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id);
CREATE INDEX IF NOT EXISTS idx_sessions_device  ON sessions(device_id);
CREATE INDEX IF NOT EXISTS idx_ip_bans_ip       ON ip_bans(ip_address);
"""


class DatabaseManager:
    """线程安全的 SQLite 数据库管理器"""

    def __init__(self, db_path: str):
        self._path = Path(db_path)
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._local = threading.local()
        self._init_schema()

    # ------------------------------------------------------------------
    # 连接管理
    # ------------------------------------------------------------------
    def _get_conn(self) -> sqlite3.Connection:
        """每个线程维护独立连接"""
        if not hasattr(self._local, "conn") or self._local.conn is None:
            conn = sqlite3.connect(str(self._path), check_same_thread=False)
            conn.row_factory = sqlite3.Row
            conn.execute("PRAGMA journal_mode=WAL")
            conn.execute("PRAGMA foreign_keys=ON")
            self._local.conn = conn
        return self._local.conn

    @contextmanager
    def cursor(self):
        """获取游标的上下文管理器，自动提交或回滚"""
        conn = self._get_conn()
        cur = conn.cursor()
        try:
            yield cur
            conn.commit()
        except Exception:
            conn.rollback()
            raise
        finally:
            cur.close()

    def _init_schema(self):
        """初始化数据库表结构"""
        conn = self._get_conn()
        conn.executescript(_DB_SCHEMA)
        conn.commit()
        logger.info(f"数据库已初始化: {self._path}")

    # ------------------------------------------------------------------
    # 管理员操作
    # ------------------------------------------------------------------
    def get_admin(self, username: str = "admin") -> dict | None:
        """查询管理员记录"""
        with self.cursor() as cur:
            cur.execute("SELECT * FROM admins WHERE username = ?", (username,))
            row = cur.fetchone()
            return dict(row) if row else None

    def create_admin(self, username: str, password_hash: str, jwt_secret: str) -> None:
        """创建管理员账户"""
        with self.cursor() as cur:
            cur.execute(
                "INSERT INTO admins (username, password_hash, jwt_secret) VALUES (?, ?, ?)",
                (username, password_hash, jwt_secret),
            )
        logger.info(f"管理员账户已创建: {username}")

    def update_admin_password(self, username: str, password_hash: str) -> None:
        """更新密码哈希"""
        with self.cursor() as cur:
            cur.execute(
                "UPDATE admins SET password_hash = ?, updated_at = datetime('now') WHERE username = ?",
                (password_hash, username),
            )

    def update_admin_session_expiry(self, username: str, hours: int) -> None:
        """更新会话有效时长"""
        with self.cursor() as cur:
            cur.execute(
                "UPDATE admins SET session_expiry_hours = ?, updated_at = datetime('now') WHERE username = ?",
                (hours, username),
            )

    # ------------------------------------------------------------------
    # 会话操作
    # ------------------------------------------------------------------
    def create_session(self, session_id: str, device_id: str = "default", title: str = "新对话") -> None:
        """创建对话会话"""
        with self.cursor() as cur:
            cur.execute(
                "INSERT OR IGNORE INTO sessions (id, device_id, title) VALUES (?, ?, ?)",
                (session_id, device_id, title),
            )

    def get_sessions(self, device_id: str | None = None, limit: int = 50) -> list[dict]:
        """查询会话列表"""
        with self.cursor() as cur:
            if device_id:
                cur.execute(
                    "SELECT * FROM sessions WHERE device_id = ? ORDER BY updated_at DESC LIMIT ?",
                    (device_id, limit),
                )
            else:
                cur.execute("SELECT * FROM sessions ORDER BY updated_at DESC LIMIT ?", (limit,))
            return [dict(r) for r in cur.fetchall()]

    def get_session(self, session_id: str) -> dict | None:
        """查询单个会话"""
        with self.cursor() as cur:
            cur.execute("SELECT * FROM sessions WHERE id = ?", (session_id,))
            row = cur.fetchone()
            return dict(row) if row else None

    def update_session(self, session_id: str, **kwargs) -> None:
        """更新会话字段"""
        allowed = {"title", "summary", "is_archived"}
        fields = {k: v for k, v in kwargs.items() if k in allowed}
        if not fields:
            return
        set_clause = ", ".join(f"{k} = ?" for k in fields)
        values = list(fields.values()) + [session_id]
        with self.cursor() as cur:
            cur.execute(
                f"UPDATE sessions SET {set_clause}, updated_at = datetime('now') WHERE id = ?",
                values,
            )

    def delete_session(self, session_id: str) -> None:
        """删除会话及其消息"""
        with self.cursor() as cur:
            cur.execute("DELETE FROM sessions WHERE id = ?", (session_id,))

    # ------------------------------------------------------------------
    # 消息操作
    # ------------------------------------------------------------------
    def add_message(self, session_id: str, role: str, content: str) -> int:
        """添加消息记录，返回消息 ID"""
        with self.cursor() as cur:
            cur.execute(
                "INSERT INTO messages (session_id, role, content) VALUES (?, ?, ?)",
                (session_id, role, content),
            )
            # 同步更新会话时间
            cur.execute(
                "UPDATE sessions SET updated_at = datetime('now') WHERE id = ?",
                (session_id,),
            )
            return cur.lastrowid

    def get_messages(self, session_id: str, limit: int = 100) -> list[dict]:
        """查询会话消息"""
        with self.cursor() as cur:
            cur.execute(
                "SELECT * FROM messages WHERE session_id = ? ORDER BY created_at ASC LIMIT ?",
                (session_id, limit),
            )
            return [dict(r) for r in cur.fetchall()]

    # ------------------------------------------------------------------
    # IP 封禁操作
    # ------------------------------------------------------------------
    def get_ip_bans(self) -> list[dict]:
        """查询所有 IP 封禁"""
        with self.cursor() as cur:
            cur.execute("SELECT * FROM ip_bans ORDER BY banned_at DESC")
            return [dict(r) for r in cur.fetchall()]

    def is_ip_banned(self, ip: str) -> bool:
        """检查 IP 是否被封禁"""
        with self.cursor() as cur:
            cur.execute(
                "SELECT * FROM ip_bans WHERE ip_address = ? AND "
                "(is_permanent = 1 OR expires_at IS NULL OR expires_at > datetime('now'))",
                (ip,),
            )
            return cur.fetchone() is not None

    def ban_ip(self, ip: str, reason: str = "", permanent: bool = False, expires_at: str | None = None) -> None:
        """封禁 IP"""
        with self.cursor() as cur:
            cur.execute(
                "INSERT OR REPLACE INTO ip_bans (ip_address, reason, is_permanent, expires_at) VALUES (?, ?, ?, ?)",
                (ip, reason, int(permanent), expires_at),
            )

    def unban_ip(self, ip: str) -> None:
        """解除 IP 封禁"""
        with self.cursor() as cur:
            cur.execute("DELETE FROM ip_bans WHERE ip_address = ?", (ip,))
