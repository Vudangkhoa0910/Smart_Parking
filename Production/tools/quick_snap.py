#!/usr/bin/env python3
import serial
import serial.tools.list_ports
import time
import re
import sys
import os
from datetime import datetime

def find_esp32_port():
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if any(s in p.device.lower() for s in ["usbserial", "wchusbserial", "slab_usbto"]):
            return p.device
    return None

def main():
    print("=== CÔNG CỤ CHỤP ẢNH TRỰC TIẾP TỪ ESP32-CAM ===")
    port = find_esp32_port()
    if not port:
        print("❌ Không tìm thấy cổng của ESP32-CAM. Hãy kiểm tra kết nối USB.")
        sys.exit(1)
        
    baud = 115200
    print(f"🔗 Đang kết nối tới {port} (Baudrate: {baud})...")
    
    try:
        ser = serial.Serial(port, baud, timeout=1)
        # Chờ 2s để ESP32 khởi động nếu cắm lại cáp
        time.sleep(2)
        # Dọn rác trong bộ đệm
        ser.reset_input_buffer()
        
        print("📸 Đang gửi lệnh SNAP_COLOR tới ESP32-CAM...")
        ser.write(b"SNAP_COLOR\n")
        ser.flush()
        
        img_size = 0
        img_data = bytearray()
        receiving = False
        
        start_time = time.time()
        timeout = 15 # chờ tối đa 15s
        
        while time.time() - start_time < timeout:
            if not receiving:
                line = ser.readline()
                if line:
                    try:
                        line_str = line.decode('utf-8', errors='ignore').strip()
                        print(f"ESP32: {line_str}")
                        
                        m = re.search(r"\[SNAP_START\]\s+size=(\d+)", line_str)
                        if m:
                            img_size = int(m.group(1))
                            print(f"🚀 Bắt đầu nhận ảnh nhị phân: {img_size} bytes...")
                            receiving = True
                    except:
                        pass
            else:
                # Đang nhận nội dung JPEG
                need_bytes = img_size - len(img_data)
                if need_bytes > 0:
                    chunk = ser.read(min(4096, need_bytes))
                    if chunk:
                        img_data.extend(chunk)
                
                if len(img_data) >= img_size:
                    print(f"✅ Đã nhận đủ {len(img_data)} bytes dữ liệu JPEG.")
                    break
                    
        if len(img_data) == 0:
            print("\n❌ LỖI: Không nhận được dữ liệu (Timeout).")
            print("→ Hãy kiểm tra xem mã nguồn mới () đã được flash vào ESP32 chưa.")
        elif len(img_data) < img_size:
            print(f"\n⚠️ LỖI MẤT DỮ LIỆU: Chỉ nhận được {len(img_data)}/{img_size} bytes.")
            print("→ Đường truyền bị rớt do cáp USB lỏng hoặc mạch thiếu điện.")
        else:
            filename = datetime.now().strftime("capture_direct_%H%M%S.jpg")
            with open(filename, "wb") as f:
                f.write(img_data)
            print(f"🎉 THÀNH CÔNG: Đã lưu ảnh thành '{filename}'")
            print(f"🔍 Đang mở ảnh...")
            os.system(f"open {filename}")
            
        ser.close()

    except Exception as e:
        print(f"❌ Xảy ra lỗi: {e}")

if __name__ == '__main__':
    main()
