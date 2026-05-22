import cv2
import os
import time
import numpy as np
import serial

# ===================== 核心配置（根据你的硬件修改） =====================
FACE_DIR = "faces"
# 串口配置（统一115200波特率，和ESP32完全一致）
SERIAL_PORT = "/dev/ttyTHS1"  # Jetson 40-Pin 串口引脚对应的设备节点
BAUDRATE = 115200
TIMEOUT = 1
# 无屏幕模式开关（必须开启才能开机自启）
HEADLESS_MODE = True
# 人脸识别置信度阈值（数值越小越严格）
CONFIDENCE_THRESHOLD = 95

# ===================== 初始化 =====================
if not os.path.exists(FACE_DIR):
    os.mkdir(FACE_DIR)

# 人脸检测器
face_cascade = cv2.CascadeClassifier(
    cv2.data.haarcascades + "haarcascade_frontalface_default.xml"
)

# LBPH 识别器（Jetson性能最优）
recognizer = cv2.face.LBPHFaceRecognizer_create()

# 全局数据
names = []
face_data = []
labels = []
ser = None
trained = False  # 标记是否训练成功

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
        print("⚠️  请检查接线和权限，执行：sudo chmod 666 /dev/ttyTHS1")
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

# ===================== 加载并训练人脸 =====================
def train_faces():
    global names, face_data, labels, trained
    names = []
    face_data = []
    labels = []
    trained = False

    if not os.listdir(FACE_DIR):
        print("⚠️  未找到任何人脸数据，请先录入ylk和zzc的人脸")
        return False

    label_id = 0
    for name in os.listdir(FACE_DIR):
        path = os.path.join(FACE_DIR, name)
        if not os.path.isdir(path):
            continue
        names.append(name)
        for f in os.listdir(path):
            img = cv2.imread(os.path.join(path, f), cv2.IMREAD_GRAYSCALE)
            if img is None:
                continue
            # 统一人脸尺寸，提升识别稳定性
            img = cv2.resize(img, (100, 100))
            face_data.append(img)
            labels.append(label_id)
        label_id += 1

    recognizer.train(face_data, np.array(labels))
    trained = True
    print(f"🎯 训练完成：{len(names)} 人 ({', '.join(names)})")
    return True

# ===================== 主程序 =====================
if __name__ == "__main__":
    # 初始化串口
    init_serial()

    # 训练人脸模型
    train_faces()

    # 打开摄像头
    cap = cv2.VideoCapture(0, cv2.CAP_V4L2)
    if not cap.isOpened():
        print("❌ 摄像头打开失败")
        exit(1)
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)  # 减少延迟

    # 记录上一次发送的指令，避免重复发送
    last_send = None

    print("\n✅ 人脸识别系统已启动")
    print("📌 识别规则：ylk→发1(红灯) | zzc→发2(绿灯) | 陌生人→无动作")
    print("📌 无屏幕模式已开启，可直接开机自启\n")

    while True:
        ret, frame = cap.read()
        if not ret:
            break

        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        faces = face_cascade.detectMultiScale(gray, 1.1, 4)

        # 只有训练成功才进行识别
        if trained and len(faces) > 0:
            for (x, y, w, h) in faces:
                roi_gray = gray[y:y+h, x:x+w]
                roi_gray = cv2.resize(roi_gray, (100, 100))

                try:
                    idx, conf = recognizer.predict(roi_gray)
                    if conf < CONFIDENCE_THRESHOLD:
                        name = names[idx]
                        print(f"👤 识别到：{name} (置信度：{int(conf)})")

                        # 严格匹配ylk和zzc，发送对应指令
                        if name == "ylk" and last_send != 1:
                            send_to_esp32(1)
                            last_send = 1
                        elif name == "zzc" and last_send != 2:
                            send_to_esp32(2)
                            last_send = 2
                        # 其他已知人脸：不发送任何指令
                        elif name not in ["ylk", "zzc"]:
                            last_send = None
                    else:
                        # 陌生人：不发送任何指令
                        print("👤 识别到：陌生人")
                        last_send = None
                except cv2.error as e:
                    print(f"⚠️  识别出错：{e}")
                    last_send = None
        else:
            # 无人脸/未训练：重置发送状态
            last_send = None

        # 非无屏幕模式才显示窗口
        if not HEADLESS_MODE:
            cv2.imshow("Face Recognition", frame)
            if cv2.waitKey(1) & 0xFF == ord('q'):
                break

        time.sleep(0.05)  # 控制帧率，降低CPU占用

    # 释放资源
    cap.release()
    if not HEADLESS_MODE:
        cv2.destroyAllWindows()
    if ser and ser.is_open:
        ser.close()
    print("✅ 系统已退出")
