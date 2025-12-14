#!/usr/bin/env python3
"""
Smart Farm Serial Server
สื่อสารกับ ESP32 ผ่าน Serial และให้บริการ Web Interface
"""

from flask import Flask, jsonify, request, Response
from flask_cors import CORS
import serial
import serial.tools.list_ports
import cv2
import json
import threading
import time
import argparse

app = Flask(__name__)
CORS(app)  # เปิด CORS สำหรับทุก routes

# ========================================
# Configuration
# ========================================
ser = None
camera = None
camera_index = 0

# Cache data from ESP32
cached_status = {
    "run": False,
    "soil": [0, 0, 0],
    "pWater": False,
    "pFert": False
}

# ========================================
# Serial Communication
# ========================================
def find_esp32_port():
    """ค้นหาพอร์ต ESP32 อัตโนมัติ"""
    ports = serial.tools.list_ports.comports()
    for port in ports:
        if 'USB' in port.description or 'UART' in port.description or 'CP210' in port.description or 'CH340' in port.description:
            return port.device
    return None

def init_serial(port=None, baudrate=115200):
    """เปิดการเชื่อมต่อ Serial"""
    global ser
    try:
        if port is None:
            port = find_esp32_port()
            if port is None:
                print("❌ ไม่พบพอร์ต ESP32")
                return False
        
        ser = serial.Serial(port, baudrate, timeout=1)
        time.sleep(2)  # รอ ESP32 รีเซ็ต
        print(f"✅ เชื่อมต่อ Serial: {port} @ {baudrate} baud")
        return True
    except Exception as e:
        print(f"❌ Serial Error: {e}")
        return False

def send_command(cmd_dict):
    """ส่งคำสั่ง JSON ไปยัง ESP32"""
    if ser is None or not ser.is_open:
        print("⚠️  Serial not connected")
        return {"error": "Serial not connected"}
    
    try:
        cmd_str = json.dumps(cmd_dict) + '\n'
        print(f"→ Sending: {cmd_str.strip()}")
        ser.write(cmd_str.encode())
        ser.flush()
        
        # รอรับ response
        time.sleep(0.2)  # เพิ่มเวลารอ
        response_lines = []
        while ser.in_waiting > 0:
            line = ser.readline().decode().strip()
            if line:
                response_lines.append(line)
                print(f"← Received: {line}")
        
        # หา JSON response
        for line in response_lines:
            if line.startswith('{'):
                try:
                    return json.loads(line)
                except:
                    pass
        
        # ถ้าไม่มี JSON ส่งข้อความกลับ
        if response_lines:
            return {"raw": " ".join(response_lines)}
        
        return {"status": "no_response"}
    except Exception as e:
        print(f"❌ Serial error: {e}")
        return {"error": str(e)}

def serial_reader_thread():
    """Thread สำหรับอ่านข้อมูลจาก Serial ตลอดเวลา"""
    global cached_status
    while True:
        if ser and ser.is_open and ser.in_waiting > 0:
            try:
                line = ser.readline().decode().strip()
                if line.startswith('{'):
                    data = json.loads(line)
                    # ถ้าเป็น status update ให้ cache ไว้
                    if 'soil' in data:
                        cached_status = data
                else:
                    print(f"ESP32: {line}")
            except Exception as e:
                pass
        time.sleep(0.05)

# ========================================
# Camera Functions
# ========================================
def get_camera():
    """เปิดกล้อง"""
    global camera
    if camera is None:
        try:
            camera = cv2.VideoCapture(camera_index)
            if camera.isOpened():
                camera.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
                camera.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
                print(f"✅ Camera {camera_index} opened")
            else:
                camera = None
        except Exception as e:
            print(f"❌ Camera error: {e}")
            camera = None
    return camera

def generate_frames():
    """สร้าง MJPEG stream"""
    import numpy as np
    
    cam = get_camera()
    use_dummy = False
    if cam is None or not cam.isOpened():
        print("⚠️  Using dummy camera")
        use_dummy = True
    
    frame_count = 0
    while True:
        if use_dummy:
            # ภาพจำลอง
            frame = np.zeros((480, 640, 3), dtype=np.uint8)
            frame[:] = (40, 80, 40)
            
            for i in range(0, 640, 40):
                cv2.line(frame, (i, 0), (i, 480), (60, 100, 60), 1)
            for i in range(0, 480, 40):
                cv2.line(frame, (0, i), (640, i), (60, 100, 60), 1)
            
            cv2.putText(frame, 'DUMMY CAMERA', (180, 200), 
                        cv2.FONT_HERSHEY_SIMPLEX, 1.5, (100, 255, 100), 3)
            cv2.putText(frame, f'Frame: {frame_count}', (250, 300), 
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2)
            
            frame_count += 1
            time.sleep(0.033)
        else:
            success, frame = cam.read()
            if not success:
                use_dummy = True
                continue
            
            cv2.putText(frame, 'Smart Farm Camera', (10, 30), 
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
        
        ret, buffer = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 80])
        frame_bytes = buffer.tobytes()
        
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame_bytes + b'\r\n')

# ========================================
# Web Routes
# ========================================
@app.route('/')
def index():
    """หน้าเว็บหลัก"""
    return open('sketch_nov29e/data/index.html', 'r', encoding='utf-8').read()

@app.route('/video')
def video_feed():
    """Video streaming"""
    return Response(generate_frames(),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

@app.route('/status')
def status():
    """ดึงสถานะจาก ESP32"""
    result = send_command({"cmd": "status"})
    # ถ้าได้ response กลับมา ให้อัปเดต cache
    if result and 'soil' in result:
        global cached_status
        cached_status = result
        # แปลง soil เป็น moisture สำหรับหน้าเว็บ
        response = {
            "run": result.get("run", False),
            "moisture": result.get("soil", [0, 0, 0]),
            "pWater": result.get("pWater", False),
            "pFert": result.get("pFert", False)
        }
        return jsonify(response)
    # ถ้าไม่ได้ response ส่ง cache เก่า
    response = {
        "run": cached_status.get("run", False),
        "moisture": cached_status.get("soil", [0, 0, 0]),
        "pWater": cached_status.get("pWater", False),
        "pFert": cached_status.get("pFert", False)
    }
    return jsonify(response)

@app.route('/tree')
def tree():
    """สั่งไปยังต้นไม้"""
    tree_id = request.args.get('id', type=int)
    if tree_id is not None:
        # แปลง 0-8 เป็น 1-9 สำหรับ ESP32
        result = send_command({"cmd": "tree", "id": tree_id + 1})
        return jsonify(result)
    return jsonify({"error": "Missing id"}), 400

@app.route('/pump')
def pump():
    """เปิด/ปิดปั๊ม"""
    pump_type = request.args.get('type')
    if pump_type:
        result = send_command({"cmd": "pump", "type": pump_type})
        return jsonify(result)
    return jsonify({"error": "Missing type"}), 400

@app.route('/home')
def home():
    """กลับบ้าน"""
    result = send_command({"cmd": "home"})
    return jsonify(result)

@app.route('/recalibrate')
def recalibrate():
    """Calibrate ใหม่"""
    result = send_command({"cmd": "recalibrate"})
    return jsonify(result)

@app.route('/move')
def move():
    """Jog manual - แปลง x,y (mm) เป็น revolutions"""
    x = request.args.get('x', 0, type=int)
    y = request.args.get('y', 0, type=int)
    
    # แปลง 100 pixels -> 0.0625 revolutions (~1cm ต่อกด)
    # ปรับได้ตามต้องการ
    revsX = x / 1600.0  # 1600 steps = 1 rev
    revsY = y / 1600.0
    
    result = send_command({"cmd": "move", "revsX": revsX, "revsY": revsY})
    return jsonify(result)

@app.route('/serial/send')
def serial_send():
    """ส่งคำสั่ง JSON แบบ raw"""
    cmd = request.args.get('cmd')
    if cmd:
        try:
            cmd_dict = json.loads(cmd)
            result = send_command(cmd_dict)
            return jsonify(result)
        except:
            return jsonify({"error": "Invalid JSON"}), 400
    return jsonify({"error": "Missing cmd"}), 400

# ========================================
# Main Function
# ========================================
def main():
    global camera_index
    
    parser = argparse.ArgumentParser(description='Smart Farm Serial Server')
    parser.add_argument('--port', type=str, help='Serial port (auto-detect if not specified)')
    parser.add_argument('--baud', type=int, default=115200, help='Baud rate (default: 115200)')
    parser.add_argument('--camera', type=int, default=0, help='Camera index (default: 0)')
    parser.add_argument('--web-port', type=int, default=8000, help='Web server port (default: 8000)')
    parser.add_argument('--host', type=str, default='0.0.0.0', help='Host address (default: 0.0.0.0)')
    args = parser.parse_args()
    
    camera_index = args.camera
    
    print("\n" + "="*60)
    print("🌱 Smart Farm Serial Server")
    print("="*60)
    
    # เชื่อมต่อ Serial
    if not init_serial(args.port, args.baud):
        print("\n⚠️  Warning: Serial not connected")
        print("   Server will run without ESP32 communication")
    
    # เริ่ม Serial reader thread
    if ser:
        reader = threading.Thread(target=serial_reader_thread, daemon=True)
        reader.start()
        print("✅ Serial reader thread started")
    
    print(f"📷 Camera Index: {args.camera}")
    print(f"🌐 Web Server: http://{args.host}:{args.web_port}")
    print("="*60)
    print(f"\n📌 เปิดเบราว์เซอร์: http://localhost:{args.web_port}")
    print("\nกดCtrl+C เพื่อหยุด\n")
    
    try:
        app.run(host=args.host, port=args.web_port, threaded=True, debug=False)
    except KeyboardInterrupt:
        print("\n\n🛑 Shutting down...")
        if ser:
            ser.close()
            print("📡 Serial closed")
        if camera:
            camera.release()
            print("📷 Camera released")

if __name__ == '__main__':
    main()
