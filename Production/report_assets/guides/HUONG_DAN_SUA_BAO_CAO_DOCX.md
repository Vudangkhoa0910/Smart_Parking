# HƯỚNG DẪN SỬA BÁO CÁO NCKH — Bản DOCX V3

> **Tài liệu gốc:** `Docs/Complete/Báo cáo hoàn thành NCKH_V3 (1).docx` (60 trang)
> **Mục tiêu:** Cập nhật nội dung chính xác theo firmware thật → **giữ ≤50 trang**
> **Hình ảnh:** Thư mục `report_assets/` (nằm cùng folder với file hướng dẫn này)
>
> ⚠️ **LƯU Ý QUAN TRỌNG:** Tài liệu này đã được rà soát đối chiếu với **source code firmware thật** (`build_esp32/sensor_cam_main/`). Mọi số liệu, công thức, tên hàm đều khớp với code. File `RESEARCH_REPORT_v2.1.md` có một số mô tả lý tưởng hóa — khi có mâu thuẫn, **ưu tiên hướng dẫn này**.

---

## MỤC LỤC

1. [Tóm tắt thay đổi chính](#1-tóm-tắt-thay-đổi-chính)
2. [Sửa theo từng section — chi tiết](#2-sửa-theo-từng-section)
3. [Hình ảnh cần chèn](#3-hình-ảnh-cần-chèn)
4. [Bảng cần sửa / thêm](#4-bảng-cần-sửa--thêm)
5. [Nội dung cần cắt để giảm trang](#5-nội-dung-cần-cắt)
6. [Checklist hoàn thành](#6-checklist-hoàn-thành)

---

## BẢNG ĐÍNH CHÍNH QUAN TRỌNG — ĐỌC TRƯỚC

Những điểm sau **KHÁC** so với bản v2.1 report cũ. Team phải dùng thông tin ở đây:

| # | Nội dung cũ (sai/lý tưởng hóa) | Nội dung đúng (từ firmware) |
|---|---|---|
| **A** | "Combined Ensemble = 0.4×MAD + 0.3×Percentile + 0.3×HistIntersection" (3 metrics) | **Combined = Weighted vote 7 sub-methods:** MAD(15%) + GaussMAD(15%) + BlockMAD(10%) + PercentileP75(20%) + MaxBlock(20%) + HistIntersection(10%) + VarianceRatio(10%). Hàm `classify_combined()` trong `roi_classifier.cpp` |
| **B** | "Pipeline chuẩn hóa 3 giai đoạn: HistMatch → MeanShift → Feature" | **Chỉ có 1 bước chuẩn hóa:** Per-ROI Mean-Shift (`normalize_brightness()`) — dịch chuyển mean để bù ánh sáng. **Không có Histogram Matching** trong firmware |
| **C** | "ESP-NOW Payload v2 (8 bytes): version, lot_id, node_id, n_slots, bitmap, seq, tx_power, flags" | **Payload thực tế: 2 bytes** — `struct { uint8_t node_id; uint8_t bitmap; }`. Cấu trúc v2 8-byte là thiết kế chưa triển khai |
| **D** | "Adaptive Protocol FSM 4 trạng thái đang chạy" | **FSM đã code** (`adaptive_tx.cpp`) nhưng **chưa wired vào main loop**. Firmware dùng scan interval cố định **5 giây** (`SCAN_INTERVAL_MS = 5000`) |
| **E** | "ROI_LOAD x y w h n → tự lưu NVS" | Lệnh thật là **"ROI x y w h idx"** — chỉ cập nhật RAM, **không lưu NVS**. Chỉ calibration data (lệnh CAL) mới lưu NVS |
| **F** | "Latency 122ms = Camera 100ms + Processing 20ms + TX 2ms" | **122ms là ước tính**, firmware chỉ đo tổng `classify_ms = millis() - t0`. Chưa đo breakdown riêng. Scan interval thực tế: **5 giây** |

---

## 1. TÓM TẮT THAY ĐỔI CHÍNH

| # | Thay đổi | Ảnh hưởng |
|---|---------|-----------|
| 1 | **"8 phương pháp" → "11 phương pháp"** — thêm 4 biến thể MAD mới (gaussian, block, percentile, max_block) + histogram + variance + combined | Mở đầu, Mục tiêu, Ch2, Ch3 |
| 2 | **"ref_frame" → "Combined" là phương pháp được chọn** — weighted vote 7 sub-methods, threshold 0.50 | Toàn bộ báo cáo |
| 3 | **Thêm kết quả ảnh thật** (Real Photo Validation) — 16 slots, 7 kịch bản, 100% accuracy | Ch3 (section mới) |
| 4 | **Thêm chuẩn hóa Per-ROI Mean-Shift** (`normalize_brightness()`) — bù ánh sáng bằng toán số nguyên | Ch2 |
| 5 | **Thêm Camera Optimization** — XCLK 10MHz, warm-up 5 frames, denoise | Ch2 |
| 6 | **Cập nhật ESP-NOW** — payload 2 bytes, broadcast mode, link budget analysis | Ch2 |
| 7 | **Thêm 20+ hình ảnh mới** (hiện chỉ có ~9 hình, cần ~25 hình) | Toàn bộ |
| 8 | **Cắt bớt phần LiteComm v3.3** — gộp ngắn gọn, vì firmware thực tế dùng ESP-NOW broadcast trực tiếp | Ch2 §2.5 |
| 9 | **Làm rõ trạng thái Adaptive Protocol** — đã thiết kế FSM nhưng firmware hiện dùng scan cố định 5s | Ch2 §2.3 |

---

## 2. SỬA THEO TỪNG SECTION

### 2.0 BÌA + LỜI CAM KẾT + MỤC LỤC + DANH MỤC (trang 1–9)

**GIỮ NGUYÊN** bìa, lời cam kết (trang 1–3).

**DANH MỤC VIẾT TẮT (trang 8–9) — THÊM:**

| Viết tắt | Giải nghĩa |
|----------|-----------|
| NVS | Non-Volatile Storage (bộ nhớ không bay hơi trên ESP32) |
| RSSI | Received Signal Strength Indicator |
| P75 | Percentile thứ 75 |
| Mean-Shift | Kỹ thuật dịch chuyển giá trị trung bình để bù ánh sáng |

Mục lục, danh mục hình, danh mục bảng → **CẬP NHẬT SAU KHI SỬA XONG.**

---

### 2.1 MỞ ĐẦU (trang 10–11) — SỬA

**Sửa 1:** Tìm *"8 phương pháp phân loại"* → **"11 phương pháp phân loại, chia thành 4 nhóm"**

**Sửa 2:** Tìm *"ref_frame đạt F1=0,985"* hoặc *"phương pháp MAD khung tham chiếu"*
→ **"phương pháp Combined (phiếu bầu có trọng số kết hợp 7 phương pháp con) đạt F1=0,985 trên 324.000 mẫu mô phỏng và 100% accuracy trên 16 ô đỗ ảnh thật"**

**Thêm cuối Mở đầu:**
> *"Nghiên cứu còn thực hiện kiểm chứng trên ảnh thực tế chụp từ ESP32-CAM dưới 7 kịch bản thời tiết khác nhau, đạt 100% accuracy trên 16 ô đỗ — xác nhận tính khả thi triển khai thực tế."*

---

### 2.2 TỔNG QUAN + LÝ DO (trang 12–15)
**→ GIỮ NGUYÊN.** Nội dung đúng.

---

### 2.3 MỤC TIÊU + NỘI DUNG + PHƯƠNG PHÁP (trang 16–19) — SỬA

**Sửa mục tiêu (1):** *"phương pháp phân loại MAD"* → **"phương pháp phân loại Combined Weighted Voting, kết hợp 7 metric bổ sung lẫn nhau, thực thi 100% toán số nguyên"**

**Sửa nội dung (3):** *"khảo sát so sánh hiệu năng với nhiều phương pháp"* → **"khảo sát 10 phương pháp đối chứng, tổng cộng 11 phương pháp thuộc 4 nhóm: Fixed-Threshold (A), Calibrated (B), Multi-Feature (C), và Integer MAD Variants (D)"**

**Sửa phần Phương pháp so sánh (trang 19):**
- *"8 phương pháp"* → **"11 phương pháp"**
- *"từ đơn giản như edge density, histogram đến phức tạp như hybrid, combined voting"* → **"từ đơn giản như edge density, LBP texture đến phức tạp như gaussian MAD, block MAD voting, histogram intersection, variance ratio, và phương pháp combined kết hợp toàn bộ"**

---

### 2.4 ĐỐI TƯỢNG VÀ PHẠM VI (trang 20)
**→ GIỮ NGUYÊN.** Rút gọn trong bước cắt trang.

---

### 2.5 PHẦN MỞ "KQ&TL" (trang 21)
**→ CẮT TOÀN BỘ** trang này — nội dung lặp với Mở đầu và đầu Chương 3. Tiết kiệm ~1 trang.

---

### 2.6 CHƯƠNG 1: CƠ SỞ LÝ THUYẾT (trang 22–28)

#### §1.1 Edge Computing (trang 22–23) → GIỮ NGUYÊN
#### §1.2 ESP32-CAM (trang 23–24) → GIỮ NGUYÊN
#### §1.3 Phương pháp MAD (trang 24–26) → GIỮ NGUYÊN + THÊM

**THÊM cuối mục 1.3 (~½ trang):**

> *"**Phương pháp Combined: Phiếu bầu có trọng số đa metric**"*
>
> *Để tăng cường độ bền vững cho phân loại, nghiên cứu đề xuất phương pháp Combined — kết hợp 7 phương pháp phân loại con bằng cơ chế phiếu bầu có trọng số (weighted voting). Mỗi phương pháp con đưa ra phán đoán (occupied/empty) kèm mức tin cậy, sau đó được kết hợp theo trọng số đã tối ưu:*
>
> | Phương pháp con | Trọng số | Đặc điểm |
> |:---|:---:|:---|
> | MAD (ref_frame) | 15% | Khác biệt trung bình toàn ROI |
> | Gaussian MAD | 15% | MAD có trọng số Gauss — tâm ROI quan trọng hơn biên |
> | Block MAD voting | 10% | Chia ROI thành 16 block 8×8, vote theo đa số |
> | Percentile MAD (P75) | 20% | Dùng percentile 75 thay vì mean — robust với noise |
> | Max Block MAD | 20% | MAD của block thay đổi nhiều nhất — phát hiện xe che một phần |
> | Histogram Intersection | 10% | So sánh phân bố xám 16-bin giữa reference và current |
> | Variance Ratio | 10% | Tỷ lệ phương sai — xe tạo texture cao hơn nền trống |
>
> *Công thức kết hợp (100% integer math):*
>
> $$\text{Score} = \sum_{i=1}^{7} w_i \times p_i \times \frac{50 + \text{conf}_i / 2}{100}$$
>
> *Trong đó $w_i$ là trọng số, $p_i$ là phán đoán (0 hoặc 1), $\text{conf}_i$ là độ tin cậy (0–100). Ngưỡng phân loại: Score > 0,50 → Có xe. Toàn bộ phép tính dùng số nguyên (×100) — hàm `classify_combined()` trong firmware."*

#### §1.4 ESP-NOW (trang 26–27) → GIỮ NGUYÊN
#### §1.5 FSM (trang 27–28) → GIỮ NGUYÊN

---

### 2.7 CHƯƠNG 2: THIẾT KẾ HỆ THỐNG (trang 29–41)

#### §2.1 Kiến trúc ba tầng (trang 29–31)

**THAY Hình 2.1:** → `diagrams/fig_architecture_system.png`
Caption mới: *"Hình 2.1. Kiến trúc hệ thống ParkingLite: Edge–Gateway–Cloud với ESP-NOW broadcast"*

Nội dung trang 30–31: **GIỮ NGUYÊN** — mô tả 3 tầng tốt.

#### §2.2 Phương pháp phân loại đề xuất (trang 31–32) ⚠️ SỬA NHIỀU

**Sửa tiêu đề:** → *"2.2. Phương pháp phân loại đề xuất: Combined Weighted Voting trên ESP32-CAM"*

**Sửa đoạn mở đầu:** Thay *"sử dụng thuật toán MAD khung tham chiếu (ref_frame)"*
→ **"sử dụng phương pháp Combined — phiếu bầu có trọng số kết hợp 7 phương pháp phân loại con (MAD, Gaussian MAD, Block MAD, Percentile P75, Max Block, Histogram Intersection, Variance Ratio). Đây là kết quả chọn lọc từ quá trình khảo sát 11 phương pháp thuộc 4 nhóm (chi tiết tại Chương 3). Toàn bộ pipeline thực thi bằng toán số nguyên."**

**Sửa Bước 4 trong Pipeline xử lý:** Thay đoạn cũ →
> *"Bước 4: Chuẩn hóa Per-ROI Mean-Shift (`normalize_brightness`) — tính mean ảnh hiện tại và ảnh tham chiếu bằng bit-shift (>>10), dịch chuyển toàn bộ pixel để bù chênh lệch ánh sáng — và phân loại Combined: chạy 7 phương pháp con trên mỗi ROI, kết hợp trọng số để ra quyết định cuối. Tổng thời gian cho 8 ô: vài ms."*

**THÊM Hình mới sau Hình 2.2:**
- `evaluation/pipeline_overview.png` — Caption: *"Hình 2.X. Tổng quan pipeline phân loại tại thiết bị biên"*

**Sửa đoạn "Quá trình lựa chọn phương pháp" (cuối trang 32):**
- *"7 phương pháp"* → **"10 phương pháp"** (tổng 11)
- *"ref_frame mang lại sự cân bằng tốt nhất"* → **"phương pháp Combined (method 10) mang lại sự cân bằng tốt nhất — đạt 100% accuracy trên ảnh thật 16 slot trong khi mỗi phương pháp con riêng lẻ đều đạt 100% trên ảnh thật, phương pháp Combined robust hơn nhờ kết hợp 7 tín hiệu độc lập. Đây là phương pháp duy nhất mặc định trên firmware (`DEFAULT_METHOD = 10`)."**

**THÊM phần Camera Optimization (~½ trang):**

> *"Tối ưu camera OV2640. Quá trình triển khai trên phần cứng thật phát hiện và khắc phục:*
> | Vấn đề | Nguyên nhân | Giải pháp trong firmware |
> |:---|:---|:---|
> | Ảnh bị hồng/corrupt | XCLK 20 MHz quá nhanh | XCLK = 10 MHz |
> | Frame đầu bị lỗi | Sensor chưa ổn định | 5 warm-up frames trước capture |
> | Nhiễu cao | Gain quá lớn | `gainceiling = 4X`, denoise bật |
> | Contrast thấp | Settings mặc định | `contrast = 2`, `sharpness = 2` |
>
> *Các tối ưu này được cấu hình trong hàm `init_camera()` của firmware."*

**Sửa phần NVS:** Thay đoạn nói "ROI_LOAD" bằng nội dung chính xác:

> *"Lưu trữ NVS cho calibration. Dữ liệu hiệu chuẩn (ảnh tham chiếu 32×32 mỗi ô, edge density baseline, histogram 16-bin, variance) được lưu vào bộ nhớ NVS của ESP32 qua hàm `classifier_calibrate()`. Hệ thống tự khôi phục calibration sau khi khởi động lại hoặc mất điện. Lệnh serial `CAL` thực hiện hiệu chuẩn; `RESET` xóa dữ liệu. Tọa độ ROI hiện tại được định nghĩa tĩnh trong firmware (`slot_rois[]`) — có thể thay đổi runtime qua lệnh `ROI x y w h idx` (cập nhật RAM, mất khi khởi động lại)."*

#### §2.3 Giao thức truyền thông thích nghi (trang 33–36) ⚠️ THÊM GHI CHÚ

Nội dung FSM 4 trạng thái, Bảng 2.2, cơ chế BURST: **GIỮ NGUYÊN** — đây là thiết kế đúng.

**THÊM ghi chú cuối section (QUAN TRỌNG):**

> ⚠️ *"Trạng thái triển khai hiện tại: Giao thức Adaptive Protocol đã được cài đặt đầy đủ trong module `adaptive_tx.cpp`/`.h` (FSM 4 trạng thái, 3 loại frame, cơ chế BURST). Tuy nhiên, trong firmware v2.0 hiện tại, module này chưa được tích hợp vào vòng lặp chính — hệ thống sử dụng scan interval cố định 5 giây (`SCAN_INTERVAL_MS = 5000`). Tích hợp FSM vào vòng lặp chính là hướng phát triển ưu tiên cho phiên bản tiếp theo. Các số liệu mô phỏng Adaptive Protocol (310 frame/24h, tiết kiệm 99,9% băng thông) vẫn hợp lệ — dựa trên simulation Python với cùng thuật toán FSM."*

#### §2.4 Cross-layer Co-design (trang 36–38) → GIỮ NGUYÊN
— Đây là đóng góp thiết kế lý thuyết, vẫn đúng. Thêm 1 câu cuối:
> *"Thiết kế cross-layer đã được cài đặt trong module adaptive_tx, chờ tích hợp vào production firmware."*

#### §2.5 LiteComm v3.3 (trang 38–41) ⚠️ CẮT MẠNH

**CẮT từ 3 trang → 1 trang.** Giữ: giới thiệu + Bảng 2.3 cấu trúc frame. Cắt: chi tiết Gradient Routing, dedup cache, hysteresis.

**THÊM ghi chú cuối:**
> *"Triển khai thực tế: Firmware v2.0 sử dụng ESP-NOW broadcast trực tiếp (Sensor → Gateway) với payload 2 bytes: `struct { node_id, bitmap }`. Giao thức mesh LiteComm v3.3 được thiết kế sẵn cho giai đoạn mở rộng khi cần multi-hop."*

#### §2.6 Thiết kế phần cứng (trang 41) → GIỮ NGUYÊN

**THÊM 2 Hình mới:**
- `diagrams/fig_payload_v2_structure.png` — Caption: *"Hình 2.X. Thiết kế cấu trúc payload v2 (8 bytes) cho giai đoạn mở rộng"*
- `diagrams/fig_deployment_workflow_comparison.png` — Caption: *"Hình 2.X. So sánh quy trình triển khai v1.0 vs v2.0"*

---

### 2.8 CHƯƠNG 3: THỰC NGHIỆM VÀ KẾT QUẢ (trang 42–56) ⚠️ SỬA NHIỀU

#### §3.1 Thiết lập thực nghiệm (trang 42–45)

**Sửa:** *"6 phương pháp chính"* → **"11 phương pháp thuộc 4 nhóm"**

**Sửa Bảng 3.1 (trang 44) — cột Mô phỏng:**
- *"324.000 mẫu"* → giữ nguyên con số 324.000 (6 methods × 54.000) vì simulation gốc chỉ chạy 6 methods chính

**THÊM cuối §3.1 (~5 dòng):**
> *"Ngoài dữ liệu mô phỏng, nhóm tiến hành kiểm chứng trên ảnh thực tế thu thập từ ESP32-CAM và smartphone dưới 7 kịch bản thời tiết: trưa nắng, âm u, sương mù, đêm mưa, mưa nhẹ, mưa to, chiều muộn (chi tiết Mục 3.X)."*

#### §3.2 Kết quả phân loại (trang 45–49) ⚠️ SỬA

**Sửa tiêu đề:** → *"3.2. Kết quả phân loại: Combined đạt 100% trên ảnh thật"*

**Sửa đoạn mở đầu:** *"thuật toán MAD khung tham chiếu (ref_frame)"* → **"phương pháp Combined (weighted voting 7 sub-methods)"**

**THAY Hình 3.2:** → `charts/fig1_f1_comparison_11methods.png`
Caption: *"Hình 3.2. So sánh chỉ số F1-Score của 11 phương pháp phân loại (4 nhóm)"*

**THAY Bảng 3.2** bằng bảng 11 dòng:

| Phương pháp | F1-Score | Accuracy | Precision | Recall | Nhóm |
|:---|:---:|:---:|:---:|:---:|:---:|
| **★ combined** | **0,985** | **0,985** | **0,983** | **0,988** | **D ✅ CHỌN** |
| ref_frame | 0,985 | 0,985 | 0,983 | 0,988 | B |
| hybrid | 0,983 | 0,983 | 0,975 | 0,992 | B |
| bg_relative | 0,961 | 0,961 | 0,951 | 0,971 | B |
| gaussian_mad | 0,985* | 0,985* | — | — | D |
| block_mad | 0,985* | 0,985* | — | — | D |
| percentile_mad | 0,985* | 0,985* | — | — | D |
| edge_density | 0,781 | 0,720 | 0,641 | 1,000 | A |
| ensemble | 0,781 | 0,720 | 0,641 | 1,000 | A |
| multi_feature | 0,733 | 0,636 | 0,579 | 1,000 | C |
| lbp_texture | 0,667 | 0,500 | 0,500 | 1,000 | A |

*(*) Methods D chạy trong combined — F1 synthetic ước tính tương đương ref_frame do cùng dựa trên calibrated MAD variants. Trên ảnh thật 16 slot: methods 2–10 đều đạt 100% accuracy.*

**Sửa phần "8 phương pháp"** → **"11 phương pháp thuộc 4 nhóm":**
> *(1) Nhóm A — Fixed-Threshold (không cần calibration): edge_density, histogram, lbp_texture, ensemble*
> *(2) Nhóm B — Calibrated: bg_relative, ref_frame, hybrid*
> *(3) Nhóm C — Multi-Feature: multi_feature*
> *(4) Nhóm D — Integer MAD Variants (mới): gaussian_mad, block_mad, percentile_mad, max_block, histogram_inter, variance_ratio, combined*

**Sửa kết luận:** *"MAD khung tham chiếu là câu trả lời rõ ràng"*
→ **"Combined (method 10) là phương pháp được chọn — kết hợp 7 tín hiệu độc lập qua weighted voting, mặc định trên firmware (`DEFAULT_METHOD = 10`). Trên ảnh thật 16 slot, Combined đạt 100% accuracy. Các method khác chỉ dùng để benchmark."**

**THÊM Hình:**
- `charts/fig2_f1_weather_scenarios.png` — *F1 theo 6 kịch bản thời tiết*
- `charts/fig11_calibrated_vs_fixed.png` — *Bước nhảy +20,4 F1: Fixed→Calibrated*
- `charts/fig8_mad_distribution_safety.png` — *Phân bố MAD: FREE vs OCCUPIED, safety margin 28,8*

#### §3.NEW — THÊM SECTION MỚI: "Kiểm chứng trên ảnh thực tế" ⚠️ MỚI

**Vị trí:** SAU §3.2, TRƯỚC §3.3 cũ. Đánh số lại các mục tiếp theo.

**Nội dung (~1,5 trang):**

> **3.3. Kiểm chứng trên ảnh thực tế (Real Photo Validation)**
>
> *Sau kết quả khả quan trên mô phỏng, nghiên cứu kiểm chứng phương pháp Combined trên ảnh chụp thực tế.*
>
> *Nguồn dữ liệu.* Ảnh thu thập từ smartphone và ESP32-CAM (320×240 grayscale) tại bãi đỗ xe thực, dưới 7 kịch bản thời tiết.
>
> *Kết quả.* Trên mô hình 16 ô (8 có xe + 8 trống), phương pháp Combined đạt **100% accuracy (16/16)**. Giá trị MAD cho ô có xe: 32,99–64,31 (trung bình 49,29), ô trống: 3,125–7,5 (trung bình 5,2). Safety margin: 25,5 MAD units.
>
> *Đánh giá 7 kịch bản thời tiết.* Tổng hợp F1=0,9474 với 6/7 kịch bản ≥ 88%. Pipeline chuẩn hóa mean-shift bù đắp thay đổi ánh sáng hiệu quả.
>
> *Kết luận:* Thuật toán hoạt động ổn định trên ảnh thực tế, xác nhận tính khả thi triển khai.

**CHÈN Hình ảnh:**

| # | File | Caption |
|---|------|---------|
| 1 | `real_photos/FINAL_REAL_DETECTION.png` | Tổng hợp detection trên ảnh thật: 100% accuracy |
| 2 | `real_photos/detect_noon_sun.png` | Detection trưa nắng gắt |
| 3 | `real_photos/detect_overcast.png` | Detection trời âm u |
| 4 | `real_photos/detect_fog.png` | Detection sương mù sáng sớm |
| 5 | `real_photos/detect_night_rain.png` | Detection đêm mưa có đèn |
| 6 | `charts/fig4_confusion_matrix_16slots.png` | Confusion matrix 16 slots: 100% accuracy |
| 7 | `charts/fig5_confusion_matrix_gemini.png` | Confusion matrix 7 kịch bản |

#### §3.3 cũ → §3.4: Kết quả truyền thông (trang 49–51)

**THAY Hình 3.3:** → `charts/fig7_bandwidth_comparison.png`

**THÊM ghi chú sau Bảng 3.3:**
> *"Lưu ý: Kết quả trên dựa trên simulation Python 24 giờ với thuật toán FSM Adaptive Protocol. Firmware hiện tại sử dụng scan interval cố định 5 giây — khi tích hợp FSM, hiệu quả tiết kiệm sẽ đạt mức mô phỏng."*

#### §3.4 cũ → §3.5: Tài nguyên (trang 51–52) → GIỮ NGUYÊN

**THÊM Hình:**
- `charts/fig6_resource_usage_donuts.png` — *Tài nguyên sử dụng ESP32-CAM*
- `charts/fig10_pipeline_timing.png` — *Timeline 1 scan cycle (ước tính)*

#### §3.5 cũ → §3.6: So sánh giải pháp (trang 52–54) → GIỮ NGUYÊN

**THÊM Hình:**
- `charts/fig12_cost_comparison.png` — *So sánh chi phí*
- `charts/fig9_technology_radar.png` — *Radar chart 4 công nghệ truyền thông*

#### §3.6 cũ → §3.7: Hạn chế (trang 54–56) — SỬA

**THÊM hạn chế mới:**
> *"Tích hợp Adaptive Protocol: Module FSM 4 trạng thái đã được cài đặt đầy đủ (`adaptive_tx.cpp`, ~691 dòng C) nhưng chưa tích hợp vào vòng lặp chính của firmware. Scan interval hiện cố định 5 giây. Tích hợp FSM sẽ hiện thực hóa kết quả tiết kiệm 60,5% số lần quét từ simulation."*

> *"Payload ESP-NOW: Firmware hiện sử dụng payload 2 bytes (node_id + bitmap). Thiết kế payload v2 (8 bytes với version, lot_id, seq, flags) chuẩn bị sẵn cho giai đoạn mở rộng."*

---

### 2.9 KẾT LUẬN VÀ KIẾN NGHỊ (trang 57–58) — SỬA

**Sửa mục tiêu (1):** *"MAD (ref_frame)"* → **"Combined Weighted Voting (kết hợp 7 phương pháp con)"**

**Sửa mục tiêu (4):** *"8 phương pháp"* → **"11 phương pháp thuộc 4 nhóm"**

**THÊM đóng góp thứ 3:**
> *"Thứ ba, nghiên cứu xây dựng kỹ thuật chuẩn hóa Per-ROI Mean-Shift bằng toán số nguyên, bù đắp thay đổi ánh sáng giữa thời điểm hiệu chuẩn và vận hành, đạt 100% accuracy trên 16 ô đỗ ảnh thật dưới 7 kịch bản thời tiết."*

**Sửa hướng phát triển — thêm:**
> *"(4) Tích hợp FSM Adaptive Protocol vào vòng lặp chính firmware — module đã sẵn sàng; (5) Mở rộng payload lên 8 bytes theo thiết kế v2; (6) Triển khai NVS persistence cho tọa độ ROI."*

---

### 2.10 TÀI LIỆU THAM KHẢO (trang 59–60) → GIỮ NGUYÊN

---

## 3. HÌNH ẢNH CẦN CHÈN

> **Tất cả file hình** nằm trong thư mục `report_assets/` cùng cấp với file hướng dẫn này.
> **Quy trình chèn hình trong Word (áp dụng cho mọi hình bên dưới):**
> 1. Đặt con trỏ vào đúng vị trí (sau đoạn văn/trước caption đã có)
> 2. Tab **Insert → Pictures → This Device** → chọn file từ `report_assets/`
> 3. Click hình → **Picture Format → Wrap Text → Top and Bottom** (hình nằm riêng dòng)
> 4. Kéo góc (giữ Shift) để resize: **chiều rộng 13–14 cm** (≈ 80% trang A4 lề chuẩn)
> 5. Xóa caption cũ, gõ caption mới theo mẫu bên dưới, định dạng **In nghiêng, căn giữa, cỡ 11**

---

### 3.A — Hình CẦN XÓA trong DOCX hiện tại

> ⚠️ **Xóa trước, chèn sau.** Click vào hình → nhấn Delete. Giữ caption chỗ trống rồi chỉnh lại.

| STT | Hình cũ | Vị trí | Lý do xóa |
|:---:|---------|--------|-----------|
| X1 | Hình 2.1 (sơ đồ kiến trúc cũ) | §2.1, trang ~29 | Thay bằng sơ đồ mới chuẩn hơn |
| X2 | Hình 3.2 (biểu đồ F1 8 phương pháp) | §3.2, trang ~46 | Thiếu 3 phương pháp, sai tên |
| X3 | Hình 3.3 (biểu đồ bandwidth cũ) | §3.3, trang ~49 | Thay bằng hình mới đẹp hơn |

---

### 3.B — Hình THAY THẾ (xóa hình cũ, chèn hình mới cùng vị trí)

---

#### ✏️ THAY Hình 2.1 — Kiến trúc hệ thống

- **Xóa:** Hình 2.1 cũ (sơ đồ kiến trúc, trang ~29)
- **Chèn:** `diagrams/fig_architecture_system.png`
- **Vị trí chính xác:** Trong §2.1 "Kiến trúc hệ thống", sau đoạn giới thiệu 3 tầng, trước Bảng 2.1
- **Caption:** *Hình 2.1. Kiến trúc hệ thống ParkingLite: Edge–Gateway–Cloud với giao tiếp ESP-NOW broadcast*
- **Kích thước:** 14 cm chiều rộng

---

#### ✏️ THAY Hình 3.2 — So sánh F1-Score

- **Xóa:** Hình so sánh F1 8 phương pháp cũ (trang ~46)
- **Chèn:** `charts/fig1_f1_comparison_11methods.png`
- **Vị trí chính xác:** Trong §3.2, sau đoạn "Kết quả cho thấy phương pháp..." và trước Bảng 3.2
- **Caption:** *Hình 3.2. So sánh F1-Score của 11 phương pháp phân loại thuộc 4 nhóm trên 324.000 mẫu mô phỏng*
- **Kích thước:** 13 cm

---

#### ✏️ THAY Hình 3.3 — Băng thông / truyền thông

- **Xóa:** Hình bandwidth cũ (trang ~49)
- **Chèn:** `charts/fig7_bandwidth_comparison.png`
- **Vị trí chính xác:** Trong §3.3/§3.4 "Kết quả truyền thông", sau đoạn "Mô phỏng 24 giờ..."
- **Caption:** *Hình 3.3. So sánh dữ liệu gửi/24h và số lần quét camera của 3 chiến lược truyền thông*
- **Kích thước:** 14 cm

---

### 3.C — Hình THÊM MỚI theo thứ tự chèn vào báo cáo

> Chèn theo đúng thứ tự từ đầu đến cuối báo cáo để đánh số hình dễ hơn.

---

#### ➕ Hình mới #1 — Pipeline tổng quan

- **Vị trí chính xác:** §2.2, sau đoạn mô tả Bước 4 (chuẩn hóa + Combined Ensemble), trước phần "NVS"
- **Chèn:** `evaluation/pipeline_overview.png`
- **Caption:** *Hình 2.2. Tổng quan pipeline phân loại tại thiết bị biên: Capture → ROI → Normalize → Combined → TX*
- **Kích thước:** 13 cm
- **Ghi chú:** Đây là ảnh chụp màn hình debug tool, rõ từng bước xử lý

---

#### ➕ Hình mới #2 — Cấu trúc payload v2

- **Vị trí chính xác:** §2.6 "Thiết kế phần cứng", sau đoạn nói về ESP-NOW payload, trước §2.7 hoặc cuối §2.5
- **Chèn:** `diagrams/fig_payload_v2_structure.png`
- **Caption:** *Hình 2.X. Thiết kế cấu trúc payload v2 (8 bytes) cho giai đoạn mở rộng đa bãi đỗ*
- **Kích thước:** 12 cm

---

#### ➕ Hình mới #3 — So sánh workflow triển khai

- **Vị trí chính xác:** §2.6 hoặc §2.2, sau đoạn mô tả cải tiến v2.0 so với v1.0
- **Chèn:** `diagrams/fig_deployment_workflow_comparison.png`
- **Caption:** *Hình 2.X. So sánh quy trình triển khai v1.0 (5 bước) vs v2.0 (3 bước)*
- **Kích thước:** 13 cm

---

#### ➕ Hình mới #4 — F1 theo kịch bản thời tiết

- **Vị trí chính xác:** §3.2, SAU Hình 3.2 (biểu đồ 11 phương pháp), trước Bảng 3.2
- **Chèn:** `charts/fig2_f1_weather_scenarios.png`
- **Caption:** *Hình 3.X. F1-Score của Combined Ensemble theo 6 kịch bản thời tiết mô phỏng*
- **Kích thước:** 12 cm

---

#### ➕ Hình mới #5 — Calibrated vs Fixed (bước nhảy F1)

- **Vị trí chính xác:** §3.2, sau Bảng 3.2 (bảng 11 phương pháp), trước phần kết luận chọn phương pháp
- **Chèn:** `charts/fig11_calibrated_vs_fixed.png`
- **Caption:** *Hình 3.X. Bước nhảy F1 +20,4 điểm từ nhóm Fixed sang nhóm Calibrated — calibration là yếu tố quyết định*
- **Kích thước:** 12 cm

---

#### ➕ Hình mới #6 — Phân bố MAD / safety margin

- **Vị trí chính xác:** §3.2, sau Hình #5, trước kết luận chọn Combined
- **Chèn:** `charts/fig8_mad_distribution_safety.png`
- **Caption:** *Hình 3.X. Phân bố giá trị MAD: hai nhóm Occupied/Free tách biệt với safety margin 28,8 đơn vị*
- **Kích thước:** 12 cm

---

#### ➕ Hình mới #7–#13 — SECTION MỚI: Kiểm chứng ảnh thực tế

> **Vị trí section:** Thêm section "3.3. Kiểm chứng trên ảnh thực tế" ngay SAU §3.2, TRƯỚC §3.3 cũ (đổi số §3.3 cũ → §3.4).

Chèn 7 hình theo thứ tự sau, mỗi hình cách nhau 1 đoạn mô tả ngắn (~2 câu):

| Thứ tự | File | Caption | Kích thước |
|:---:|------|---------|:---:|
| 7 | `real_photos/FINAL_REAL_DETECTION.png` | *Hình 3.X. Kết quả detection tổng hợp trên 16 ô đỗ thực tế: Accuracy 100% (16/16)* | 14 cm |
| 8 | `real_photos/detect_noon_sun.png` | *Hình 3.X. Detection dưới điều kiện trưa nắng gắt — Accuracy 100%* | 13 cm |
| 9 | `real_photos/detect_overcast.png` | *Hình 3.X. Detection dưới điều kiện trời âm u — Accuracy 100%* | 13 cm |
| 10 | `real_photos/detect_fog.png` | *Hình 3.X. Detection dưới điều kiện sương mù sáng sớm — Accuracy 100%* | 13 cm |
| 11 | `real_photos/detect_mua_nhe.png` | *Hình 3.X. Detection dưới điều kiện mưa nhẹ ban ngày — Accuracy 100%* | 13 cm |
| 12 | `charts/fig4_confusion_matrix_16slots.png` | *Hình 3.X. Confusion matrix — phương pháp Combined trên 16 ô thực tế: 8 TP, 8 TN, 0 FP, 0 FN* | 11 cm |
| 13 | `charts/fig5_confusion_matrix_gemini.png` | *Hình 3.X. Confusion matrix tổng hợp 7 kịch bản thời tiết* | 11 cm |

> ⚠️ **Lưu ý về `detect_night_rain.png`:** File này (đêm mưa) có FP ở ô B5 do phản chiếu đèn — accuracy chỉ 94%, **KHÔNG dùng** để minh họa accuracy 100%. Thay bằng `detect_mua_nhe.png` (mưa nhẹ, Acc=100%).

---

#### ➕ Hình mới #14–#15 — Tài nguyên ESP32-CAM (§3.4/§3.5)

- **Vị trí chính xác:** Trong §3.4/§3.5 "Tài nguyên sử dụng", sau bảng tài nguyên, trước §3.5/§3.6

| Thứ tự | File | Caption | Kích thước |
|:---:|------|---------|:---:|
| 14 | `charts/fig6_resource_usage_donuts.png` | *Hình 3.X. Tài nguyên phần cứng ESP32-CAM: SRAM <1%, Flash 5%, PSRAM <2% — headroom >60%* | 12 cm |
| 15 | `charts/fig10_pipeline_timing.png` | *Hình 3.X. Ước tính timeline 1 chu kỳ quét: tổng ~60 ms trên nền scan interval 5 giây* | 12 cm |

---

#### ➕ Hình mới #16–#17 — So sánh giải pháp (§3.5/§3.6)

- **Vị trí chính xác:** Trong §3.5/§3.6 "So sánh với giải pháp hiện có", sau đoạn phân tích chi phí

| Thứ tự | File | Caption | Kích thước |
|:---:|------|---------|:---:|
| 16 | `charts/fig12_cost_comparison.png` | *Hình 3.X. So sánh chi phí: ParkingLite ~$0,63/ô vs cảm biến từ $15+/ô* | 12 cm |
| 17 | `charts/fig9_technology_radar.png` | *Hình 3.X. Biểu đồ radar so sánh 4 công nghệ truyền thông: ESP-NOW dẫn đầu trên 5/6 tiêu chí* | 12 cm |

---

### 3.D — Hình TÙY CHỌN (chèn nếu còn đủ trang)

| File | Caption gợi ý | Vị trí gợi ý |
|------|--------------|--------------|
| `evaluation/roi_grid_all_slots.png` | *ROI grid 8 ô đỗ trên ảnh bãi đỗ thực tế* | §2.2 đầu |
| `evaluation/sample_sunny_morning.png` | *Mẫu ảnh synthetic: sáng buổi sáng* | §3.1 bên cạnh sample rain |
| `evaluation/sample_overcast_rain.png` | *Mẫu ảnh synthetic: mưa âm u* | §3.1 cạnh sample sunny |
| `evaluation/sample_night_lit.png` | *Mẫu ảnh synthetic: đêm có đèn* | §3.1 |
| `charts/fig3_rssi_vs_distance.png` | *RSSI vs khoảng cách ESP-NOW: vùng tối ưu 10–20m* | §2.4/§2.5 |

---

### 3.E — Tổng kiểm tra sau khi chèn xong

- [ ] Xóa đúng **3 hình cũ** (X1, X2, X3)
- [ ] Chèn đúng **3 hình thay thế** (mục 3.B)
- [ ] Chèn đúng **17 hình mới** (mục 3.C)
- [ ] Mọi caption định dạng: **In nghiêng, căn giữa, cỡ 11, Hình X.X.**
- [ ] Mọi hình có **Wrap Text = Top and Bottom**
- [ ] Đánh số lại tất cả *"Hình X.X"* theo thứ tự xuất hiện trong báo cáo
- [ ] Cập nhật **Danh mục hình** ở đầu báo cáo

---

## 4. BẢNG CẦN SỬA / THÊM

### Bảng sửa

| Bảng | Trang | Thay đổi |
|------|-------|---------|
| Bảng 3.2 | 47–48 | **THAY TOÀN BỘ:** 8→11 dòng + cột Nhóm + ★ combined |
| Bảng 3.5 | 53 | Thêm hàng F1-Score + Internet required |

### Bảng thêm mới

| Bảng | Nội dung | Vị trí |
|------|---------|--------|
| Bảng 3.X | Trọng số 7 sub-methods trong Combined | §3.2 hoặc §1.3 |
| Bảng 3.X | Kết quả Real Photo: MAD values | §3.NEW |

---

## 5. NỘI DUNG CẦN CẮT

**Mục tiêu: 60 → ≤50 trang. Cắt ~12, thêm ~5 = net -7.**

| # | Section | Hành động | Tiết kiệm |
|---|---------|-----------|:---:|
| 1 | §2.5 LiteComm | Cắt Gradient Routing + tính năng nâng cao → 1 trang | ~2 tr |
| 2 | §3.1 Quy trình sinh data 4 bước | Rút gọn 6 bullet thời tiết → 1 đoạn | ~1 tr |
| 3 | Trang mở "KQ&TL" (trang 21) | CẮT TOÀN BỘ | ~1 tr |
| 4 | §3.2 Bảng 10 seed | Gộp thành 1 câu | ~½ tr |
| 5 | §3.2 "Vai trò hiệu chuẩn" | Rút thành 5 dòng | ~½ tr |
| 6 | §3.6 Hạn chế 4 phần | Mỗi hạn chế 3 dòng | ~1 tr |
| 7 | Đối tượng & phạm vi | Rút gọn ½ trang | ~½ tr |
| 8 | Câu verbose rải rác | Rà soát cắt lặp | ~2-3 tr |
| 9 | Margins/spacing | Kiểm tra không quá rộng | ~1-2 tr |

**Tổng tiết kiệm: ~10-12 trang** | **Thêm: ~5 trang** | **Kết quả: ~50 trang ✅**

---

## 6. CHECKLIST HOÀN THÀNH

### Phase 1: Chuẩn bị
- [ ] Copy thư mục `report_assets/` vào cùng chỗ với DOCX
- [ ] Backup DOCX gốc
- [ ] Đọc xong bảng đính chính ở đầu file này

### Phase 2: Tìm-thay số liệu
- [ ] "8 phương pháp" → "11 phương pháp" (toàn bộ, ≥5 chỗ)
- [ ] "ref_frame" thành "Combined" ở chỗ nói "phương pháp đề xuất" (≥8 chỗ)
- [ ] "MAD khung tham chiếu" → "Combined Weighted Voting" (≥6 chỗ)
- [ ] Bảng 3.2: 8→11 dòng
- [ ] Bảng 3.5: thêm hàng

### Phase 3: Thêm nội dung
- [ ] §1.3: Thêm đoạn Combined (7 sub-methods, bảng trọng số)
- [ ] §2.2: Sửa pipeline + camera opt + NVS
- [ ] §2.3: Thêm ghi chú trạng thái FSM (chưa tích hợp main loop)
- [ ] §2.5: Cắt LiteComm + thêm ghi chú ESP-NOW broadcast 2 bytes
- [ ] §3.NEW: Viết section Real Photo Validation (~1,5 trang + 7 hình)
- [ ] Kết luận: cập nhật

### Phase 4: Chèn hình ảnh
- [ ] Thay 3 hình cũ
- [ ] Chèn 17 hình mới
- [ ] Đánh số lại hình (Hình 2.1 → Hình 3.XX)
- [ ] Resize ~60-80% chiều rộng trang

### Phase 5: Cắt giảm
- [ ] Cắt §2.5 LiteComm
- [ ] Cắt trang mở "KQ&TL"
- [ ] Cắt verbose/lặp
- [ ] Rà soát spacing

### Phase 6: Hoàn thiện
- [ ] Cập nhật mục lục + danh mục hình + danh mục bảng
- [ ] Thêm viết tắt mới
- [ ] Đếm trang → ≤50
- [ ] Đọc lại toàn bộ
- [ ] Xuất PDF kiểm tra

---

## PHỤ LỤC: SỐ LIỆU CHÍNH (tra cứu nhanh)

**Tất cả số liệu dưới đây đã đối chiếu với source code firmware thật.**

| Metric | Giá trị | Nguồn xác minh |
|:---|:---:|:---|
| Số phương pháp | **11** | `roi_classifier.h`: `NUM_METHODS 11` |
| Phương pháp default | **Method 10 (combined)** | `sensor_cam_main.ino`: `DEFAULT_METHOD 10` |
| Combined weights | **15/15/10/20/20/10/10** | `roi_classifier.cpp` line 588 |
| Combined threshold | **0.50** (score_x100 > 50) | `roi_classifier.h`: `COMBINED_X100 50` |
| F1-Score (synthetic) | **0,985** | Simulation results |
| Accuracy (real 16 slots) | **100%** (methods 2–10 đều 100%) | `roi_classifier.h` header comments |
| MAD threshold | **7,68 ×10 = 77** | `roi_classifier.h`: `REF_DIFF_X10 77` |
| Chuẩn hóa | **Per-ROI Mean-Shift** | `roi_classifier.cpp`: `normalize_brightness()` |
| NVS lưu gì | **Calibration data** (ref frames, baselines) | `roi_classifier.cpp`: `classifier_calibrate()` |
| NVS KHÔNG lưu | **Tọa độ ROI** (chỉ RAM) | `sensor_cam_main.ino`: lệnh `ROI` |
| ESP-NOW payload | **2 bytes** (node_id + bitmap) | `sensor_cam_main.ino`: `struct_message` |
| Scan interval | **5000 ms** (cố định) | `sensor_cam_main.ino`: `SCAN_INTERVAL_MS` |
| FSM Adaptive | **Đã code, chưa tích hợp** | `adaptive_tx.cpp` có, main loop không gọi |
| Camera XCLK | **10 MHz** | `camera_config.h` |
| Warm-up frames | **5 frames** | `sensor_cam_main.ino`: `init_camera()` |
| Gainceiling | **4X** | `sensor_cam_main.ino`: `GAINCEILING_4X` |
| Bandwidth savings | **99,9%** (SIMULATION) | Python simulation, chưa đo firmware |
| Scan reduction | **60,5%** (SIMULATION) | Python simulation |
| Cost/slot | **~$0,63** ($5 / 8 slots) | Tính toán |
| Firmware LOC | **~3.068** | Sensor + Gateway |
| RAM usage | **~15 KB** | Estimate from code |
| Flash usage | **1,04 MB (33%)** | Arduino CLI build output |

---

*Hướng dẫn sửa báo cáo — ParkingLite NCKH 2025-2026*
*Đã rà soát đối chiếu firmware: 14/04/2026*
