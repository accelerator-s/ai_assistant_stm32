"""认证管理器 — bcrypt 密码哈希 + JWT 签发/验证（数据库存储）"""

import secrets
import logging
from datetime import datetime, timezone, timedelta
from typing import Optional

import bcrypt
import jwt

from ..database import DatabaseManager

logger = logging.getLogger(__name__)

_JWT_ALGORITHM = "HS256"
_JWT_EXPIRY_HOURS_DEFAULT = 24


class AuthManager:
    """管理员认证: bcrypt 密码 + JWT 会话，密码哈希存储在 SQLite 数据库"""

    def __init__(self, db: DatabaseManager):
        self._db = db

    # ------------------------------------------------------------------
    # 初始化（首次启动时生成密码并打印到控制台）
    # ------------------------------------------------------------------
    def initialize(self) -> None:
        """检查数据库中是否存在管理员账户，不存在则创建"""
        admin = self._db.get_admin("admin")
        if admin:
            logger.info("管理员账户已存在，跳过初始化")
            return

        # 生成随机初始密码
        plain_password = secrets.token_urlsafe(12)
        salt = bcrypt.gensalt(rounds=12)
        password_hash = bcrypt.hashpw(plain_password.encode(), salt).decode()
        jwt_secret = secrets.token_hex(32)

        self._db.create_admin("admin", password_hash, jwt_secret)

        print("\n" + "=" * 50)
        print("  管理员初始密码: " + plain_password)
        print("  请登录后立即修改密码!")
        print("=" * 50 + "\n")
        logger.info("已生成初始管理员密码（见控制台输出）")

    # ------------------------------------------------------------------
    # 密码操作
    # ------------------------------------------------------------------
    def verify_password(self, plain: str) -> bool:
        """校验明文密码是否与数据库中的哈希匹配"""
        admin = self._db.get_admin("admin")
        if not admin:
            return False
        stored = admin.get("password_hash", "")
        if not stored:
            return False
        return bcrypt.checkpw(plain.encode(), stored.encode())

    def change_password(self, old_plain: str, new_plain: str) -> bool:
        """修改密码，需先验证旧密码"""
        if not self.verify_password(old_plain):
            return False
        salt = bcrypt.gensalt(rounds=12)
        new_hash = bcrypt.hashpw(new_plain.encode(), salt).decode()
        self._db.update_admin_password("admin", new_hash)
        logger.info("管理员密码已修改")
        return True

    # ------------------------------------------------------------------
    # JWT 令牌
    # ------------------------------------------------------------------
    @property
    def session_expiry_hours(self) -> int:
        """获取会话有效时长（小时），0 表示永不过期"""
        admin = self._db.get_admin("admin")
        if not admin:
            return _JWT_EXPIRY_HOURS_DEFAULT
        return admin.get("session_expiry_hours", _JWT_EXPIRY_HOURS_DEFAULT)

    def set_session_expiry_hours(self, hours: int) -> None:
        """设置会话有效时长"""
        self._db.update_admin_session_expiry("admin", hours)
        logger.info(f"会话有效时长已修改为 {hours} 小时" if hours else "会话设置为永不过期")

    def create_token(self) -> str:
        """签发 JWT 令牌"""
        admin = self._db.get_admin("admin")
        secret = admin["jwt_secret"] if admin else secrets.token_hex(32)
        now = datetime.now(timezone.utc)
        expiry = self.session_expiry_hours
        payload = {"sub": "admin", "iat": now}
        if expiry > 0:
            payload["exp"] = now + timedelta(hours=expiry)
        return jwt.encode(payload, secret, algorithm=_JWT_ALGORITHM)

    def validate_token(self, token: str) -> Optional[dict]:
        """验证 JWT 令牌，返回 payload 或 None"""
        admin = self._db.get_admin("admin")
        if not admin:
            return None
        secret = admin["jwt_secret"]
        try:
            options = {}
            if self.session_expiry_hours == 0:
                options["verify_exp"] = False
            return jwt.decode(token, secret, algorithms=[_JWT_ALGORITHM], options=options)
        except (jwt.ExpiredSignatureError, jwt.InvalidTokenError):
            return None
