"""启动入口 — STM32 语音交互助手 WebUI"""

import os
import sys

# 将项目根目录添加到 Python 路径
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from Backend.app import create_app, app_state


def main():
    app = create_app()

    config = app_state.get("config")
    port = config.get("advanced.service_port", 5000) if config else 5000

    print("\n" + "=" * 50)
    print(f"  STM32 语音交互助手 WebUI")
    print(f"  访问地址: http://localhost:{port}")
    print("=" * 50 + "\n")

    app.run(
        host="0.0.0.0",
        port=port,
        debug=False,
        threaded=True,
    )


if __name__ == "__main__":
    main()
