# ParkingLite — Production Tools

Bộ công cụ Python để test và cải tiến thuật toán phát hiện xe trên máy local, không cần ESP32.

---

## Cấu trúc

```
Production/tools/
├── local_detector.py        # Tool chính: phân loại ảnh (interactive + batch)
├── generate_debug_images.py # Tạo ảnh debug overlay cho 7 scenario thời tiết
├── quick_snap.py            # Chụp ảnh nhanh từ ESP32-CAM
└── flash.sh                 # Flash firmware lên ESP32
```

---

## 1. `local_detector.py` — Phân loại ảnh (Interactive + Batch)

### Cài đặt
```bash
pip install opencv-python numpy
```

### Mode Interactive (GUI)
Dùng để **vẽ ROI mới** khi có ảnh góc nhìn khác:

```bash
python3 local_detector.py Images/parking_empty.png
```

**Các phím tắt quan trọng:**

| Phím | Chức năng |
|------|-----------|
| `A` | Vẽ ROI slot mới (click + kéo) |
| `D` | Xóa slot cuối |
| `C` | Calibrate với ảnh hiện tại (ảnh bãi TRỐNG) |
| `L` | Load ảnh calibration từ file khác |
| `SPACE` / `R` | Phân loại ảnh hiện tại |
| `O` | Mở ảnh test mới |
| `W` | Bật/tắt Perspective Warp (click 4 góc: TL→TR→BR→BL) |
| `E` | **Export ROI config** → `roi_config_local.json` |
| `S` | Lưu ảnh kết quả |
| `I` | Inspect ROI patches (so sánh current vs reference) |
| `0`–`9` | Chuyển method (0=edge, 1-9=các method, 0=combined) |
| `H` | Help |
| `Q` / `ESC` | Thoát |

### Mode Batch (không GUI)
```bash
python3 local_detector.py --batch \
    -cal Images/parking_empty.png \
    -test Images/with_car.png \
    -roi roi_config_local.json \
    -method 10
```

---

## 2. `generate_debug_images.py` — Tạo ảnh debug 7 scenarios

Tạo ảnh overlay detection (như ảnh chuẩn của đề tài) cho tất cả 7 điều kiện thời tiết.

```bash
python3 generate_debug_images.py
```

Output sẽ được lưu tại `Simulation/output/real_photo_results/final_7scenarios/`:
- `detect_Trua_nang.png` — Trưa nắng
- `detect_Am_u.png` — Âm u
- `detect_Mua_nhe.png` — Mưa nhẹ
- `detect_Mua_to.png` — Mưa to
- `detect_Suong_mu.png` — Sương mù
- `detect_Chieu_muon.png` — Chiều muộn
- `detect_Dem_mua.png` — Đêm mưa
- `GRAND_DEBUG_ALL_7.png` — Tất cả 7 scenario ghép lại

**Tùy chọn:**
```bash
python3 generate_debug_images.py \
    --roi Simulation/roi_config_parking_empty.json \
    --output output/my_test/ \
    --method 10
```

---

## Quy trình cho ảnh góc nhìn khác (Camera mới / góc nghiêng)

Khi nhóm dev muốn test với ảnh từ camera khác (góc khác, độ cao khác, zoom khác):

### Bước 1 — Chuẩn bị ảnh
Cần 2 ảnh từ cùng 1 camera:
- `empty_new_angle.png` — bãi đỗ **trống hoàn toàn**
- `with_car_new_angle.png` — bãi đỗ **có xe** (để test)

> ⚠️ Hai ảnh phải chụp **đúng cùng vị trí camera**, không di chuyển.

### Bước 2 — Tạo ROI config bằng Interactive mode

```bash
python3 local_detector.py empty_new_angle.png
```

1. **Nếu ảnh nhìn thẳng từ trên xuống (overhead):**
   - Nhấn `A` → click kéo để vẽ từng ô đỗ
   - Vẽ hết tất cả slots (8 top + 8 bottom = 16 slots)
   - Nhấn `C` để calibrate
   - Nhấn `E` để export → `roi_config_local.json`

2. **Nếu ảnh nhìn xiên (góc ~30-60°):**
   - Nhấn `W` để bật Perspective Warp
   - Click 4 góc của bãi đỗ theo thứ tự: **Trên-trái → Trên-phải → Dưới-phải → Dưới-trái**
   - Tool tự động "kéo thẳng" ảnh về overhead view
   - Sau đó nhấn `A` để vẽ ROI như bình thường
   - Nhấn `E` → export config (bao gồm warp matrix)

### Bước 3 — Kiểm tra calibration
```bash
python3 local_detector.py empty_new_angle.png
# Load ROI config: L → chọn roi_config_local.json
# Nhấn SPACE → tất cả slot phải hiện FREE (cyan)
# Nhấn O → mở with_car_new_angle.png
# Nhấn SPACE → slot có xe = GREEN (OCC)
```

### Bước 4 — Chạy batch test
```bash
python3 local_detector.py --batch \
    -cal empty_new_angle.png \
    -test with_car_new_angle.png \
    -roi roi_config_local.json \
    -method 10
```

### Bước 5 — Tạo debug overlay image
Thêm scenario mới vào `generate_debug_images.py`, phần `SCENARIOS`:

```python
SCENARIOS = [
    # ... existing scenarios ...
    ("Ten scenario moi", "folder_images", "empty_new.png", "with_car_new.png"),
]
```

Rồi chạy lại:
```bash
python3 generate_debug_images.py
```

---

## Method Index (Method 10 = Combined Ensemble là tốt nhất)

| Index | Tên | Mô tả |
|-------|-----|-------|
| 0 | edge_density | Đếm edge pixel (không cần calibrate) |
| 1 | bg_relative | So sánh edge với baseline |
| 2 | ref_frame_mad | MAD so với ảnh reference |
| 3 | hybrid | Kết hợp bg_relative + ref_frame_mad |
| 4 | gaussian_mad | MAD với Gaussian weight |
| 5 | block_mad | MAD theo 4 block |
| 6 | percentile_mad | MAD theo percentile 75th |
| 7 | max_block_mad | MAD block tối đa |
| 8 | histogram_inter | Histogram intersection |
| 9 | variance_ratio | Tỷ lệ variance |
| 10 | **combined** | **Ensemble of methods 1-9 (khuyến nghị)** |

---

## Kết quả trên 7 scenarios thực tế

| Scenario | Điều kiện | Accuracy |
|----------|-----------|----------|
| Trưa nắng | Nắng gắt 12h | **100%** |
| Âm u | Không nắng | **100%** |
| Sương mù | Fog sáng sớm | **100%** |
| Mưa nhẹ | Light rain | 81% |
| Mưa to | Heavy rain | 75% |
| Đêm mưa | Night + rain | 75% |
| Chiều muộn | Bóng dài 17h | 50% |

> **Note:** Chiều muộn accuracy thấp do bóng xe dài ~3× lấn sang ô trống — đây là thách thức thực tế. Cải thiện: calibrate bằng ảnh empty cùng điều kiện chiều muộn (bóng dài).
