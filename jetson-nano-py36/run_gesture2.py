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

# 判断手指是否伸直
def is_finger_straight(landmark, tip_id, pip_id):
    return landmark[tip_id].y < landmark[pip_id].y - 0.02  # 阈值可微调

# 识别完整手势：握拳/摊手/1/2/3/4/胜利✌️
def get_gesture(landmark):
    # 拇指：判断x坐标（右手拇指尖在关节左边为伸直）
    thumb_straight = landmark[4].x < landmark[3].x
    # 其余四指：判断y坐标（指尖比关节高为伸直）
    index_straight = is_finger_straight(landmark, 8, 6)
    middle_straight = is_finger_straight(landmark, 12, 10)
    ring_straight = is_finger_straight(landmark, 16, 14)
    pinky_straight = is_finger_straight(landmark, 20, 18)

    fingers = [thumb_straight, index_straight, middle_straight, ring_straight, pinky_straight]
    finger_count = sum(fingers)

    # 胜利手势 ✌️：食指+中指伸直，其余弯曲
    if index_straight and middle_straight and not thumb_straight and not ring_straight and not pinky_straight:
        return 6
    # 握拳：所有手指弯曲
    elif finger_count == 0:
        return 7
    # 摊手：所有手指伸直
    elif finger_count == 5:
        return 5
    # 其他情况：返回伸直手指数量
    else:
        return finger_count

last_send = None

while True:
    ret, frame = cap.read()
    if not ret: break
    frame = cv2.flip(frame, 1)
    img_rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
    res = hands.process(img_rgb)

    gesture = None
    if res.multi_hand_landmarks:
        for h in res.multi_hand_landmarks:
            mp_draw.draw_landmarks(frame, h, mp_hands.HAND_CONNECTIONS)
            lm = h.landmark
            # 获取识别手势结果
            gesture = get_gesture(lm)

    # 串口发送逻辑
    if gesture is not None:
        cv2.putText(frame, f"GESTURE: {gesture}", (40, 60), cv2.FONT_HERSHEY_SIMPLEX, 1.3, (0, 255, 0), 2)
        if gesture != last_send:
            send_to_esp32(gesture)
            last_send = gesture
    else:
        last_send = None

    cv2.imshow("Gesture", frame)
    if cv2.waitKey(1) == 27: break

cap.release()
cv2.destroyAllWindows()
if ser and ser.is_open:
    ser.close()
