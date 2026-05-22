import cv2
import mediapipe as mp
import serial
import time

# ===================== 核心配置 =====================
# 串口配置（统一 115200 波特率，与 ESP32 保持一致）
SERIAL_PORT = "/dev/ttyTHS1"
BAUDRATE = 115200
TIMEOUT = 1

ser = None
try:
    ser = serial.Serial(SERIAL_PORT, BAUDRATE, timeout=TIMEOUT)
    time.sleep(2)  # 给串口初始化时间
    print(f"✅ 串口连接成功：{SERIAL_PORT} @ {BAUDRATE}")
except Exception as e:
    print(f"❌ 串口连接失败：{e}")
    ser = None

# ===================== 串口发送函数 =====================
def send_to_esp32(data):
    if ser and ser.is_open:
        try:
            ser.write((str(data) + '\n').encode('utf-8'))
            print(f"📤 发送串口指令：{data}")
        except Exception as e:
            print(f"❌ 串口发送失败：{e}")

mp_hands = mp.solutions.hands
mp_draw = mp.solutions.drawing_utils
cap = cv2.VideoCapture(0)
hands = mp_hands.Hands(max_num_hands=1, min_detection_confidence=0.7)

last_send = None

while True:
    ret, frame = cap.read()
    if not ret: break
    frame = cv2.flip(frame, 1)
    img_rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
    res = hands.process(img_rgb)

    finger_count = None
    if res.multi_hand_landmarks:
        for h in res.multi_hand_landmarks:
            mp_draw.draw_landmarks(frame, h, mp_hands.HAND_CONNECTIONS)
            lm = h.landmark
            fingers = []
            if lm[4].x < lm[3].x : fingers.append(1)
            else: fingers.append(0)
            for tip in [8,12,16,20]:
                if lm[tip].y < lm[tip-2].y: fingers.append(1)
                else: fingers.append(0)
            finger_count = sum(fingers)

    if finger_count is not None:
        cv2.putText(frame, f"FINGERS: {finger_count}", (40,60), cv2.FONT_HERSHEY_SIMPLEX, 1.3, (0,255,0), 2)
        # 如果指尖数量变化，发送新状态
        if finger_count != last_send:
            send_to_esp32(finger_count)
            last_send = finger_count
    else:
        last_send = None

    cv2.imshow("Gesture", frame)
    if cv2.waitKey(1) == 27: break

cap.release()
cv2.destroyAllWindows()
if ser and ser.is_open:
    ser.close()
