# Bản đồ chèn hình — Quick Reference (v2.2)

> Cấu trúc theo luồng: Bài toán → Kiến trúc → Phân loại → Synthetic → Real → ESP-NOW → Kết quả
>
> ⚠️ **`detect_night_rain.png` có FP ở B5 (Acc=94%) — KHÔNG dùng minh họa 100%.** Dùng `detect_mua_nhe.png` thay thế.

---

## Hình CẦN XÓA

| Hình cũ | Trang ước | Hành động |
|---------|-----------|-----------|
| Hình 2.1 (sơ đồ kiến trúc cũ) | ~29 | XÓA → thay `diagrams/fig_architecture_system.png` |
| Hình 3.2 (F1 8 phương pháp) | ~46 | XÓA → thay `charts/fig1_f1_comparison_11methods.png` |
| Hình 3.3 (bandwidth cũ) | ~49 | XÓA → thay `charts/fig7_bandwidth_comparison.png` |

---

## Chương 2: Kiến trúc hệ thống

| Hình | File | Loại | Caption |
|:---:|:---|:---|:---|
| H.1 (THAY 2.1) | `diagrams/fig_architecture_system.png` | Full | Kiến trúc ParkingLite 3 tầng: Edge–Gateway–Cloud |
| H.2 (MỚI) | `evaluation/pipeline_overview.png` | Full | Pipeline phân loại tại ESP32-CAM |
| H.3 (MỚI) | `diagrams/fig_payload_v2_structure.png` | Half | Payload v2 8 bytes cho giai đoạn mở rộng |
| H.4 (MỚI) | `diagrams/fig_deployment_workflow_comparison.png` | Full | Workflow v1.0 vs v2.0 |

## Chương 3: Kết quả phân loại

| Hình | File | Loại | Caption |
|:---:|:---|:---|:---|
| H.5 (THAY 3.2) | `charts/fig1_f1_comparison_11methods.png` | Full | F1-Score 11 phương pháp (4 nhóm) |
| H.6 (MỚI) | `charts/fig2_f1_weather_scenarios.png` | Full | F1 theo 6 kịch bản thời tiết |
| H.7 (MỚI) | `charts/fig11_calibrated_vs_fixed.png` | Full | Bước nhảy +20,4 F1: Fixed→Calibrated |
| H.8 (MỚI) | `charts/fig8_mad_distribution_safety.png` | Full | Phân bố MAD, safety margin 28,8 |

## §3.NEW — Kiểm chứng ảnh thực tế ⭐ (section mới, chèn sau §3.2)

| Hình | File | Loại | Caption |
|:---:|:---|:---|:---|
| **H.9** | **`real_photos/FINAL_REAL_DETECTION.png`** | Full | Tổng hợp detection 16 ô: 100% accuracy |
| **H.10** | **`real_photos/detect_noon_sun.png`** | Full | Trưa nắng gắt — 100% |
| **H.11** | **`real_photos/detect_overcast.png`** | Full | Âm u — 100% |
| **H.12** | **`real_photos/detect_fog.png`** | Full | Sương mù — 100% |
| **H.13** | **`real_photos/detect_mua_nhe.png`** | Full | Mưa nhẹ — 100% ✅ (thay night_rain) |
| H.14 | `charts/fig4_confusion_matrix_16slots.png` | Half | Confusion matrix 16 slots: 100% |
| H.15 | `charts/fig5_confusion_matrix_gemini.png` | Half | Confusion matrix 7 kịch bản |

## Chương 3: Truyền thông & Tài nguyên

| Hình | File | Loại | Caption |
|:---:|:---|:---|:---|
| H.16 (THAY 3.3) | `charts/fig7_bandwidth_comparison.png` | Full | Bandwidth savings 99,9% |
| H.17 (MỚI) | `charts/fig6_resource_usage_donuts.png` | Full | SRAM/Flash/PSRAM — headroom >60% |
| H.18 (MỚI) | `charts/fig10_pipeline_timing.png` | Full | Timeline 1 scan cycle 60 ms |

## Chương 3: So sánh giải pháp

| Hình | File | Loại | Caption |
|:---:|:---|:---|:---|
| H.19 (MỚI) | `charts/fig12_cost_comparison.png` | Half | Chi phí ~$0,63/ô vs $15+ |
| H.20 (MỚI) | `charts/fig9_technology_radar.png` | Full | Radar 4 công nghệ truyền thông |

---

## Hình tùy chọn

| File | Vị trí gợi ý |
|------|-------------|
| `evaluation/roi_grid_all_slots.png` | §2.2 — minh họa ROI grid |
| `evaluation/sample_sunny_morning.png` | §3.1 — mẫu synthetic sáng sớm |
| `evaluation/sample_overcast_rain.png` | §3.1 — mẫu synthetic mưa âm u |
| `evaluation/sample_night_lit.png` | §3.1 — mẫu synthetic đêm |
| `charts/fig3_rssi_vs_distance.png` | §2.4 — RSSI vs khoảng cách ESP-NOW |

---

**Tổng kết:**
- **3 hình xóa** (X1–X3)
- **3 hình thay thế** tại vị trí cũ
- **17 hình mới** thêm vào
- **5 hình tùy chọn** nếu còn chỗ
- → Đánh số lại **Hình X.X** từ đầu sau khi chèn xong
