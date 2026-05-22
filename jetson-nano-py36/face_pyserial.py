import cv2
import os
import time
import pickle
import numpy as np
import serial

# ===================== 配置 =====================
FACE_DIR = "faces"
DATA_FILE = "face_data.pkl"

# 使用 J41 杜邦线引脚通信，固定为 /dev/ttyTHS1
SERIAL_PORT = "/dev/ttyTHS1"
BAUDRATE = 115200  # 需和ESP32的串口波特率一致
TIMEOUT = 1

if not os.path.exists(FACE_DIR):
    os.mkdir(FACE_DIR)

# 人脸检测器
face_cascade = cv2.CascadeClassifier(
    cv2.data.haarcascades + "haarcascade_frontalface_default.xml"
)

# LBPH 识别器（OpenCV 自带，最适合 Jetson）
recognizer = cv2.face.LBPHFaceRecognizer_create()

# 全局数据
names = []
face_data = []
labels = []
# 初始化串口对象
ser = None
try:
    ser = serial.Serial(SERIAL_PORT, BAUDRATE, timeout=TIMEOUT)
    time.sleep(2)  # 给串口初始化时间
    print(f"✅ 串口已连接：{SERIAL_PORT}")
except Exception as e:
    print(f"⚠️  串口初始化失败：{e}")
    ser = None

# ===================== 保存人脸数据 =====================
def save_face(name, face_img):
    user_dir = os.path.join(FACE_DIR, name)
    if not os.path.exists(user_dir):
        os.mkdir(user_dir)
    fname = f"{int(time.time())}.jpg"
    cv2.imwrite(os.path.join(user_dir, fname), face_img)
    print(f"✅ 已保存：{name}")

# ===================== 加载所有人脸并训练 =====================
def train_faces():
    global names, face_data, labels
    names = []
    face_data = []
    labels = []

    if not os.listdir(FACE_DIR):
        print("⚠️  未找到任何人脸数据")
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
            face_data.append(img)
            labels.append(label_id)
        label_id += 1

    recognizer.train(face_data, np.array(labels))
    print(f"🎯 训练完成：{len(names)} 人")
    return True

# ===================== 串口发送数据 =====================
def send_to_esp32(data):
    """向ESP32发送字节数据"""
    if ser and ser.is_open:
        try:
            ser.write((str(data) + '\n').encode('utf-8'))  # 发送字符串并带上换行符 \n
            # 可选：等待ESP32回复（如果需要）
            # resp = ser.readline().decode('utf-8').strip()
            # print(f"ESP32回复：{resp}")
            print(f"📤 串口发送：{data}")
        except Exception as e:
            print(f"⚠️  串口发送失败：{e}")
    else:
        print("⚠️  串口未打开，无法发送")

# ===================== 主程序 =====================
cap = cv2.VideoCapture(0)
cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)

# 先训练
train_faces()

print("\n==== 使用说明 ====")
print("S = 录入人脸")
print("Q = 退出")
print("T = 重新训练")
print("==================\n")

# 记录上一次发送的标识，避免重复发送
last_send = None

while True:
    ret, frame = cap.read()
    if not ret:
        break

    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    faces = face_cascade.detectMultiScale(gray, 1.1, 4)
    current_name = None  # 记录当前识别到的姓名
    if len(faces) == 0:
        last_send = None

    for (x, y, w, h) in faces:
        roi_gray = gray[y:y+h, x:x+w]
        cv2.rectangle(frame, (x, y), (x+w, y+h), (0,255,0), 2)

        # 尝试识别
        try:
            idx, conf = recognizer.predict(roi_gray)
            if conf < 80:
                current_name = names[idx]
                cv2.putText(frame, f"{current_name} {int(conf)}",
                            (x, y-5), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0,255,0), 2)
                # 根据姓名发送对应标识
                if current_name == "ylk" and last_send != 1:
                    send_to_esp32(1)
                    last_send = 1
                elif current_name == "zzc" and last_send != 2:
                    send_to_esp32(2)
                    last_send = 2
                elif current_name not in ["ylk", "zzc"] and last_send is not None:
                    # 识别到其他人脸，重置发送状态（可选）
                    last_send = None
            else:
                cv2.putText(frame, "Unknown",
                            (x, y-5), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0,0,255), 2)
                last_send = None  # 未知人脸，重置发送状态
        except:
            cv2.putText(frame, "Detecting", (x, y-5),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255,255,0), 2)
            last_send = None  # 检测中，重置发送状态

    cv2.putText(frame, "S:Save  T:Train  Q:Quit",
                (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255,255,255), 2)
    cv2.imshow("Jetson Face System", frame)

    key = cv2.waitKey(1) & 0xFF
    if key == ord('q'):
        break

    # 录入人脸
    if key == ord('s') and len(faces) > 0:
        name = input("输入姓名：").strip()
        if name:
            x, y, w, h = faces[0]
            save_face(name, gray[y:y+h, x:x+w])

    # 重新训练
    if key == ord('t'):
        train_faces()
        last_send = None  # 重新训练后重置发送状态

# 释放资源
cap.release()
cv2.destroyAllWindows()
if ser and ser.is_open:
    ser.close()
    print("🔌 串口已关闭")
