import cv2
import os
import time
import numpy as np
import serial

# ===================== 配置 =====================
FACE_DIR = "faces"
if not os.path.exists(FACE_DIR):
    os.makedirs(FACE_DIR, exist_ok=True)

# 串口配置（统一 115200 波特率，与 ESP32 保持一致）
SERIAL_PORT = "/dev/ttyTHS1"  # Jetson 40-Pin 串口引脚对应的设备节点
BAUDRATE = 115200
TIMEOUT = 1

ser = None

# ===================== 串口初始化 =====================
def init_serial():
    global ser
    try:
        ser = serial.Serial(SERIAL_PORT, BAUDRATE, timeout=TIMEOUT)
        time.sleep(2)
        print(f"✅ 串口连接成功：{SERIAL_PORT} @ {BAUDRATE}")
        return True
    except Exception as e:
        print(f"❌ 串口连接失败：{e}")
        print("⚠️  请检查接线和权限，并执行：sudo chmod 666 /dev/ttyTHS1")
        ser = None
        return False

# ===================== 串口发送数据 =====================
def send_to_esp32(data):
    if ser and ser.is_open:
        try:
            ser.write((str(data) + '\n').encode('utf-8'))
            print(f"📤 发送串口指令：{data}")
        except Exception as e:
            print(f"❌ 串口发送失败：{e}")

# 人脸检测器（绝对路径加载，避免空指针）
face_cascade = cv2.CascadeClassifier(
    cv2.data.haarcascades + "haarcascade_frontalface_default.xml"
)
# 人脸识别器
recognizer = cv2.face.LBPHFaceRecognizer_create()

names = []
face_data = []
labels = []

# ===================== 训练函数 =====================
def train_faces():
    global names, face_data, labels
    names.clear()
    face_data.clear()
    labels.clear()

    if not os.listdir(FACE_DIR):
        print("⚠️ No face data found")
        return False

    label_id = 0
    for name in os.listdir(FACE_DIR):
        user_dir = os.path.join(FACE_DIR, name)
        if not os.path.isdir(user_dir):
            continue
        names.append(name)
        for img_file in os.listdir(user_dir):
            img_path = os.path.join(user_dir, img_file)
            img = cv2.imread(img_path, cv2.IMREAD_GRAYSCALE)
            if img is not None:
                # 统一人脸尺寸
                img = cv2.resize(img, (100, 100))
                face_data.append(img)
                labels.append(label_id)
        label_id += 1

    if face_data:
        recognizer.train(face_data, np.array(labels))
        print(f"✅ Training complete: {len(names)} people")
        return True
    return False

# ===================== 主程序（核心修复） =====================
if __name__ == "__main__":
    # 初始化串口
    init_serial()

    # 强制指定摄像头索引0，加超时重试
    cap = cv2.VideoCapture(0, cv2.CAP_V4L2)
    if not cap.isOpened():
        print("❌ Camera open failed, retrying...")
        cap = cv2.VideoCapture(0)
        if not cap.isOpened():
            print("❌ Camera init failed, exit")
            exit(1)

    # Jetson Nano 最优分辨率，避免画面卡顿/黑屏
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
    cap.set(cv2.CAP_PROP_FPS, 30)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)  # 清空缓冲区，避免旧帧堆积

    # 预热摄像头，跳过前5帧（解决首帧黑屏）
    for _ in range(5):
        ret, _ = cap.read()
        time.sleep(0.05)

    # 初始化训练
    trained = train_faces()

    print("\n===== Hotkeys =====")
    print("S = Save face")
    print("T = Retrain model")
    print("Q = Quit")
    print("===================\n")

    last_send = None

    while True:
        # 强制检查帧有效性，空帧直接跳过
        ret, frame = cap.read()
        if not ret or frame is None:
            print("⚠️ Frame read failed, skip")
            continue

        # 镜像画面，使用更自然
        frame = cv2.flip(frame, 1)
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)

        # 人脸检测（Jetson优化参数）
        faces = face_cascade.detectMultiScale(gray, 1.1, 4, minSize=(50, 50))

        # 绘制人脸框 + 识别结果
        if trained and len(faces) > 0:
            for (x, y, w, h) in faces:
                cv2.rectangle(frame, (x, y), (x+w, y+h), (0, 255, 0), 2)
                roi_gray = gray[y:y+h, x:x+w]
                roi_gray = cv2.resize(roi_gray, (100, 100))

                try:
                    idx, conf = recognizer.predict(roi_gray)
                    if conf < 80 and idx < len(names):
                        name = names[idx]
                        cv2.putText(frame, f"{name} ({int(conf)})", 
                                    (x, y-10), cv2.FONT_HERSHEY_SIMPLEX, 
                                    0.7, (0, 255, 0), 2)
                        
                        # 根据识别的人脸向 ESP32 发送对应命令
                        if name == "ylk" and last_send != 1:
                            send_to_esp32(1)
                            last_send = 1
                        elif name == "zzc" and last_send != 2:
                            send_to_esp32(2)
                            last_send = 2
                        elif name not in ["ylk", "zzc"]:
                            last_send = None
                    else:
                        cv2.putText(frame, "Unknown", 
                                    (x, y-10), cv2.FONT_HERSHEY_SIMPLEX, 
                                    0.7, (0, 0, 255), 2)
                        last_send = None
                except Exception as e:
                    cv2.putText(frame, "Detecting", 
                                (x, y-10), cv2.FONT_HERSHEY_SIMPLEX, 
                                0.7, (255, 255, 0), 2)
                    last_send = None
        else:
            last_send = None

        # 绘制操作提示
        cv2.putText(frame, "S:Save  T:Train  Q:Quit", 
                    (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 
                    0.6, (255, 255, 255), 2)

        # 强制窗口刷新，避免黑屏
        cv2.imshow("Jetson Face Recognition", frame)
        key = cv2.waitKey(1) & 0xFF

        # 按键逻辑
        if key == ord('q'):
            break
        elif key == ord('s') and len(faces) > 0:
            x, y, w, h = faces[0]
            face_img = gray[y:y+h, x:x+w]
            name = input("Enter name: ").strip()
            if name:
                user_dir = os.path.join(FACE_DIR, name)
                os.makedirs(user_dir, exist_ok=True)
                save_path = os.path.join(user_dir, f"{int(time.time())}.jpg")
                cv2.imwrite(save_path, face_img)
                print(f"✅ Saved: {save_path}")
        elif key == ord('t'):
            trained = train_faces()
            last_send = None

    # 释放资源
    cap.release()
    cv2.destroyAllWindows()
    if ser and ser.is_open:
        ser.close()
