"""WebUI 项目常量定义"""

# 项目版本
VERSION = "1.0.0"

# 数据库版本（用于后续迁移）
DB_VERSION = 1

# 默认管理员用户名
DEFAULT_ADMIN_USERNAME = "admin"

# 速率限制
RATE_LIMIT_MAX_REQUESTS = 60
RATE_LIMIT_WINDOW_SEC = 60

# 登录失败锁定
LOGIN_MAX_ATTEMPTS = 5
LOGIN_LOCKOUT_SEC = 300
