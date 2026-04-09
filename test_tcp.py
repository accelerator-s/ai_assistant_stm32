#!/usr/bin/env python3
"""测试 TCP 服务器连接"""

import socket
import sys

def test_tcp_server(host='127.0.0.1', port=8266):
    """测试 TCP 服务器连接"""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(3)

        print(f"尝试连接到 {host}:{port}...")
        result = sock.connect_ex((host, port))

        if result == 0:
            print(f"✓ 成功连接到 {host}:{port}")

            # 尝试发送测试数据
            test_data = b"TEST\n"
            sock.sendall(test_data)
            print(f"  发送测试数据: {test_data.decode().strip()}")

            # 尝试接收响应
            try:
                response = sock.recv(1024)
                if response:
                    print(f"  收到响应: {response.decode().strip()}")
                else:
                    print("  没有收到响应")
            except socket.timeout:
                print("  接收超时（正常，服务器可能不会立即响应）")

            sock.close()
            return True
        else:
            print(f"✗ 无法连接到 {host}:{port} (错误代码: {result})")
            return False

    except Exception as e:
        print(f"✗ 连接测试失败: {e}")
        return False

if __name__ == "__main__":
    # 测试本地连接
    print("=== 测试本地 TCP 服务器 ===")
    local_success = test_tcp_server('127.0.0.1', 8266)

    print("\n=== 测试局域网 TCP 服务器 ===")
    lan_success = test_tcp_server('192.168.43.202', 8266)

    print("\n=== 总结 ===")
    if local_success and lan_success:
        print("✓ TCP 服务器运行正常，本地和局域网都可访问")
    elif local_success and not lan_success:
        print("⚠ TCP 服务器仅本地可访问，局域网可能被防火墙阻止")
    else:
        print("✗ TCP 服务器无法访问")