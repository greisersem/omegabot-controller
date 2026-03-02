from ultralytics import YOLO
import cv2

input_pipeline = (
    'udpsrc port=12346 caps="application/x-rtp,media=video,encoding-name=H264,payload=96" '
    '! rtph264depay '
    '! avdec_h264 '
    '! videoconvert '
    '! video/x-raw,format=BGR '
    '! appsink sync=false'
)

output_pipeline = (
    "appsrc ! videoconvert ! video/x-raw,format=I420 ! "
    "x264enc tune=zerolatency bitrate=1000 speed-preset=ultrafast ! "
    "rtph264pay config-interval=1 pt=96 ! "
    "udpsink host=127.0.0.1 port=12349 sync=false"
)

cap = cv2.VideoCapture(input_pipeline, cv2.CAP_GSTREAMER)
if not cap.isOpened():
    print("Не удалось открыть видеопоток")
    exit()

# Читаем первый кадр
ret, frame = cap.read()
if not ret:
    print("Не удалось получить кадр")
    exit()

height, width = frame.shape[:2]
fps = cap.get(cv2.CAP_PROP_FPS) or 30  # если 0, ставим 30

writer = cv2.VideoWriter(output_pipeline, cv2.CAP_GSTREAMER, 0, fps, (width, height), True)

model = YOLO("yolov8n.pt")

while True:
    ret, frame = cap.read()
    if not ret or frame is None:
        break

    results = model(frame)[0]
    annotated_frame = results.plot()
    writer.write(annotated_frame)