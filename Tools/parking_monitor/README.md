# ParkingLite Live Monitor

Desktop app để xem camera ESP32-CAM + điều khiển classifier qua serial.

## Tính năng

- 📷 **Live camera view** — xem ảnh JPEG từ ESP32-CAM (1-2 fps)
- 🔲 **ROI overlay** — 8 ô đỗ hiển thị màu (xanh = trống, đỏ = có xe)
- 📊 **8-slot status grid** — confidence + raw MAD value từng ô
- 🎛️ **Control panel** — CAL / METHOD / INTERVAL / RESET / FLASH
- 📝 **Serial log** — color-coded, auto-scroll, 500 dòng
- 💾 **Auto-save captures** — lưu vào `captures/snap_YYYYMMDD_HHMMSS.jpg`
- 📈 **Stats** — FPS, uptime, bitmap history

## Cài đặt

```bash
pip3 install PyQt5 pyserial Pillow
```

## Chạy

```bash
# Auto-detect port
python3 parking_monitor.py

# Hoặc chỉ định port
python3 parking_monitor.py --port /dev/cu.usbserial-110
```

## Quy trình sử dụng

1. **Mở app** → kết nối tự động với ESP32-CAM qua serial
2. **Đợi firmware READY** (xem log góc phải)
3. **Click "📷 Chụp ngay"** hoặc bật ☑ Auto (2s/lần) để xem camera
4. **Hướng camera vào bãi trống** → click **"🎯 CALIBRATE"**
5. **Chọn Method 3 (hybrid)** cho kết quả tốt nhất
6. **Đặt xe vào ô** → xem ROI overlay đổi màu đỏ real-time

## Lệnh firmware support

Firmware ESP32-CAM phải có các lệnh serial:
- `CAL` — calibrate với ảnh hiện tại
- `STATUS` — trạng thái chi tiết
- `METHOD X` — chuyển method (0-10)
- `INTERVAL ms` — đặt chu kỳ scan
- `SNAP` — chụp JPEG và gửi qua serial
- `SLOTS_GET` — lấy trạng thái 8 ô dạng JSON
- `ROI_GET` — lấy ROI configuration dạng JSON
- `PING` — test kết nối

## Thư mục

- `parking_monitor.py` — app chính
- `captures/` — ảnh tự động lưu
- `assets/` — icons, logos (nếu có)

## Shortcut

- `Ctrl+S` — lưu ảnh hiện tại
- `Ctrl+Q` — thoát
- Menu Device → gửi lệnh nhanh

## Giao diện

- **Bên trái:** Live camera + 8-slot grid
- **Bên phải:** Control + Custom command + Serial log
- **Top bar:** Connection status, FPS, method, uptime, bitmap
- **Theme:** Dark tokyo-night inspired
