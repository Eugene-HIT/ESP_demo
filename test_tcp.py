import socket
import threading
import time

HOST = '172.20.10.3'  # 替换成 ESP32 的 IP 地址
PORT = 3333

def receive_thread(s):
    while True:
        try:
            data = s.recv(1024)
            if not data:
                break
            print(f"\n[收到消息]: {data.decode().strip()}")
        except Exception as e:
            print(f"接收出错: {e}")
            break

try:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        print(f"正在连接到 {HOST}:{PORT} ...")
        s.connect((HOST, PORT))
        print("连接成功！\n操作说明：\n1. 输入 'L'：交替闪烁 LED1/LED2\n2. 输入 'F'：电机正转\n3. 输入 'R'：电机反转\n4. 输入 'S'：电机停止\n5. 按下 ESP32 上的 BOOT 键：收到通知\n")
        
        # 启动接收线程
        t = threading.Thread(target=receive_thread, args=(s,))
        t.daemon = True
        t.start()
        
        while True:
            msg = input() # 等待用户输入
            if msg:
                s.sendall(msg.encode())

except KeyboardInterrupt:
    print("\n退出程序")
except Exception as e:
    print(f"通信出错: {e}")