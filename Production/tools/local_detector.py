#!/usr/bin/env python3
"""
ParkingLite Local Detector v1.0
=================================
Python port của Combined Ensemble (Method 10) — chạy hoàn toàn trên máy local,
không cần ESP32. Hỗ trợ ảnh chụp vuông góc và ảnh góc nghiêng (~45°).

MODES:
  Interactive (default):
      python3 local_detector.py [image.jpg]

  Batch (không GUI):
      python3 local_detector.py --batch \\
          -cal Images/parking_empty.png \\
          -test Images/Harsh_noon_sun/with_car_harsh_noon_sun.png \\
          -roi roi_config_local.json

INTERACTIVE CONTROLS:
  A        — Vẽ ROI mới (click+drag)
  D        — Xóa slot cuối
  C        — Calibrate với ảnh hiện tại (ảnh bãi trống)
  L        — Load ảnh calibration từ file khác
  SPACE/R  — Phân loại ảnh hiện tại
  O        — Mở ảnh mới để test
  W        — Kích hoạt perspective warp (click 4 góc: TL→TR→BR→BL)
             Press W lần 2 để reset warp
  E        — Export ROI config (JSON + lệnh ROI_LOAD cho ESP32)
  S        — Lưu ảnh kết quả (annotated)
  I        — Inspect ROI patches (so sánh current vs reference)
  0..9     — Chuyển method (1-9=method 1-9, 0=method 10/combined)
  H        — Help
  Q/ESC    — Thoát

REQUIREMENTS:
  pip install opencv-python numpy
  (tkinter có sẵn trong Python chuẩn)
"""

import cv2
import numpy as np
import json
import sys
import os
import tkinter as tk
from tkinter import filedialog
from pathlib import Path


# ═══════════════════════════════════════════════════════════════════════
#  CONSTANTS — Phải khớp chính xác với Production/sensor_node/roi_classifier.h
# ═══════════════════════════════════════════════════════════════════════

ROI_SIZE        = 32    # ROI luôn resize về 32×32
ROI_PIXELS      = 1024  # ROI_SIZE * ROI_SIZE
ROI_BLOCK_SIZE  = 8     # Block size cho block-based methods
ROI_N_BLOCKS    = 4     # ROI_SIZE / ROI_BLOCK_SIZE
ROI_TOTAL_BLOCKS= 16    # ROI_N_BLOCKS ** 2
HIST_N_BINS     = 16    # Histogram bins (256 / 16)
MAX_SLOTS       = 8

# Thresholds ×10 (fixed-point integer, khớp C firmware)
REF_DIFF_X10    = 77    # ref_frame MAD ×10 threshold  (7.7)
GAUSS_MAD_X10   = 77    # gaussian_mad threshold        (7.7)
BLOCK_MAD_X10   = 150   # per-block threshold ×10       (15.0)
BLOCK_VOTE_X100 = 40    # % blocks phải differ ×100     (40%)
PCTILE_P75_X10  = 150   # P75 threshold ×10             (15.0)
MAX_BLOCK_X10   = 200   # max block threshold ×10       (20.0)
HIST_INTER_X1000= 750   # histogram intersection ×1000  (0.75)
VAR_RATIO_X100  = 180   # variance ratio ×100           (1.80)
COMBINED_X100   = 50    # combined vote threshold ×100  (0.50)
OCC_RATIO_X1000 = 80    # edge density threshold ×1000  (0.08)
BG_RATIO_X100   = 140   # bg_relative ratio ×100        (1.40)
BG_BASELINE_FLOOR = 5   # min baseline ×1000            (0.005)
HYBRID_CONF_X100= 30    # hybrid confidence gate ×100   (0.30)

METHOD_NAMES = [
    "edge_density",   # 0 — không cần calibration
    "bg_relative",    # 1
    "ref_frame_mad",  # 2 — khuyên dùng (simple)
    "hybrid",         # 3
    "gaussian_mad",   # 4
    "block_mad",      # 5
    "percentile_mad", # 6
    "max_block",      # 7
    "histogram",      # 8
    "variance_ratio", # 9
    "combined",       # 10 ← default, robust nhất
]

# Gaussian weight table 32×32 — tính theo công thức của firmware C
# w[y][x] = int(128 * exp(-((x-15.5)² + (y-15.5)²) / (2 * 10.67²)))
def _make_gauss_weights():
    sigma  = 32.0 / 3.0   # ≈ 10.67, khớp firmware
    center = 15.5          # (ROI_SIZE - 1) / 2
    ys, xs = np.meshgrid(np.arange(32) - center,
                         np.arange(32) - center, indexing='ij')
    w = 128.0 * np.exp(-(xs**2 + ys**2) / (2.0 * sigma**2))
    return np.floor(w).astype(np.float32)  # truncate như C int cast

GAUSS_WEIGHTS    = _make_gauss_weights()
GAUSS_WEIGHT_SUM = float(GAUSS_WEIGHTS.sum())


# ═══════════════════════════════════════════════════════════════════════
#  ROI EXTRACTION
# ═══════════════════════════════════════════════════════════════════════

def roi_extract(gray_img, roi_rect):
    """Cắt và resize ROI→32×32 grayscale. Khớp roi_extract() trong C (bilinear)."""
    x, y, w, h = roi_rect
    ih, iw = gray_img.shape[:2]
    x = max(0, min(int(x), iw - 1))
    y = max(0, min(int(y), ih - 1))
    w = max(1, min(int(w), iw - x))
    h = max(1, min(int(h), ih - y))
    patch = gray_img[y:y+h, x:x+w]
    return cv2.resize(patch, (ROI_SIZE, ROI_SIZE), interpolation=cv2.INTER_LINEAR)


# ═══════════════════════════════════════════════════════════════════════
#  CORE HELPERS — khớp roi_classifier.cpp integer math
# ═══════════════════════════════════════════════════════════════════════

def _normalize_brightness(current, reference):
    """Per-ROI mean shift. Khớp normalize_brightness() trong C."""
    mean_cur = int(current.mean())
    mean_ref = int(reference.mean())
    shift = mean_ref - mean_cur
    return np.clip(current.astype(np.int16) + shift, 0, 255).astype(np.uint8)


def _mad_x10(current, reference):
    """Mean Absolute Difference × 10. Khớp compute_mean_diff_x10()."""
    total = int(np.abs(current.astype(np.int16) - reference.astype(np.int16)).sum())
    return total * 10 // ROI_PIXELS


def _mad_x10_normalized(current, reference):
    return _mad_x10(_normalize_brightness(current, reference), reference)


def _confidence(value, threshold):
    """Distance-based confidence [0-100]. Khớp compute_confidence() trong C."""
    if threshold == 0:
        return min(100, abs(value))
    return min(100, abs(value - threshold) * 100 // threshold)


def _sobel_edge_count(roi, threshold=42):
    """Integer Sobel edge count. Khớp compute_edge_count()."""
    roi_i = roi.astype(np.int16)
    gx = (- roi_i[:-2, :-2] + roi_i[:-2, 2:]
          - 2*roi_i[1:-1, :-2] + 2*roi_i[1:-1, 2:]
          - roi_i[2:, :-2] + roi_i[2:, 2:])
    gy = (- roi_i[:-2, :-2] - 2*roi_i[:-2, 1:-1] - roi_i[:-2, 2:]
          + roi_i[2:, :-2] + 2*roi_i[2:, 1:-1] + roi_i[2:, 2:])
    return int(((np.abs(gx) + np.abs(gy)) > threshold).sum())


# ═══════════════════════════════════════════════════════════════════════
#  CLASSIFICATION METHODS — Python port của roi_classifier.cpp
#  Tất cả trả về tuple (prediction: int, confidence: int, raw_metric: int)
# ═══════════════════════════════════════════════════════════════════════

def classify_edge_density(roi):
    """Method 0: Edge density, không cần calibration. Acc ≈ 68%."""
    count = _sobel_edge_count(roi, 42)
    ratio = count * 1000 // ROI_PIXELS
    pred  = 1 if ratio > OCC_RATIO_X1000 else 0
    return pred, _confidence(ratio, OCC_RATIO_X1000), ratio


def classify_bg_relative(roi, baseline_x1000):
    """Method 1: Background-relative edge density. Acc ≈ 96%."""
    cur   = _sobel_edge_count(roi, 35) * 1000 // ROI_PIXELS
    base  = max(int(baseline_x1000), BG_BASELINE_FLOOR)
    ratio = cur * 100 // base
    pred  = 1 if ratio > BG_RATIO_X100 else 0
    return pred, _confidence(ratio, BG_RATIO_X100), ratio


def classify_ref_frame(roi, ref_frame):
    """Method 2: Reference frame MAD. Acc = 100%."""
    diff = _mad_x10_normalized(roi, ref_frame)
    pred = 1 if diff > REF_DIFF_X10 else 0
    return pred, _confidence(diff, REF_DIFF_X10), diff


def classify_hybrid(roi, baseline_x1000, ref_frame):
    """Method 3: Hybrid bg_relative + ref_frame fallback."""
    r = classify_bg_relative(roi, baseline_x1000)
    if r[1] > HYBRID_CONF_X100:
        return r
    return classify_ref_frame(roi, ref_frame)


def classify_gaussian_mad(roi, ref_frame):
    """Method 4: Gaussian-weighted MAD. Acc = 100%."""
    cur_norm = _normalize_brightness(roi, ref_frame)
    diff = np.abs(cur_norm.astype(np.int16) - ref_frame.astype(np.int16)).astype(np.float32)
    weighted = float((GAUSS_WEIGHTS * diff).sum())
    gm = int(weighted * 10 / GAUSS_WEIGHT_SUM)
    pred = 1 if gm > GAUSS_MAD_X10 else 0
    return pred, _confidence(gm, GAUSS_MAD_X10), gm


def classify_block_mad(roi, ref_frame):
    """Method 5: Block MAD with voting (4×4 blocks). Acc = 100%."""
    cur_norm = _normalize_brightness(roi, ref_frame)
    n_occ = 0
    total_vote = 0
    for by in range(ROI_N_BLOCKS):
        for bx in range(ROI_N_BLOCKS):
            y0, y1 = by * ROI_BLOCK_SIZE, (by + 1) * ROI_BLOCK_SIZE
            x0, x1 = bx * ROI_BLOCK_SIZE, (bx + 1) * ROI_BLOCK_SIZE
            bdiff = int(np.abs(
                cur_norm[y0:y1, x0:x1].astype(np.int16) -
                ref_frame[y0:y1, x0:x1].astype(np.int16)
            ).sum())
            bm = bdiff * 10 // (ROI_BLOCK_SIZE * ROI_BLOCK_SIZE)
            if bm > BLOCK_MAD_X10:
                n_occ += 1
    vote = n_occ * 100 // ROI_TOTAL_BLOCKS
    pred = 1 if vote > BLOCK_VOTE_X100 else 0
    return pred, _confidence(vote, BLOCK_VOTE_X100), vote


def classify_percentile_mad(roi, ref_frame):
    """Method 6: Percentile MAD (P75). Acc = 100%."""
    cur_norm = _normalize_brightness(roi, ref_frame)
    diff = np.abs(cur_norm.astype(np.int16) - ref_frame.astype(np.int16)).flatten()
    diff_sorted = np.sort(diff.astype(np.uint8))
    target = ROI_PIXELS * 75 // 100
    p75 = int(diff_sorted[target - 1])
    p75x10 = p75 * 10
    pred = 1 if p75x10 > PCTILE_P75_X10 else 0
    return pred, _confidence(p75x10, PCTILE_P75_X10), p75x10


def classify_max_block(roi, ref_frame):
    """Method 7: Maximum block MAD. Acc = 100%."""
    cur_norm = _normalize_brightness(roi, ref_frame)
    max_bm = 0
    for by in range(ROI_N_BLOCKS):
        for bx in range(ROI_N_BLOCKS):
            y0, y1 = by * ROI_BLOCK_SIZE, (by + 1) * ROI_BLOCK_SIZE
            x0, x1 = bx * ROI_BLOCK_SIZE, (bx + 1) * ROI_BLOCK_SIZE
            bdiff = int(np.abs(
                cur_norm[y0:y1, x0:x1].astype(np.int16) -
                ref_frame[y0:y1, x0:x1].astype(np.int16)
            ).sum())
            bm = bdiff * 10 // (ROI_BLOCK_SIZE * ROI_BLOCK_SIZE)
            max_bm = max(max_bm, bm)
    pred = 1 if max_bm > MAX_BLOCK_X10 else 0
    return pred, _confidence(max_bm, MAX_BLOCK_X10), max_bm


def classify_histogram_inter(roi, ref_hist):
    """Method 8: Histogram intersection. Acc = 100%."""
    cur_hist, _ = np.histogram(roi.flatten(), bins=HIST_N_BINS, range=(0, 256))
    inter = int(np.minimum(ref_hist, cur_hist).sum())
    ix1000 = inter * 1000 // ROI_PIXELS
    # Inverted: thấp hơn = khác ref = có xe
    pred = 1 if ix1000 < HIST_INTER_X1000 else 0
    delta = abs(HIST_INTER_X1000 - ix1000)
    conf  = min(100, delta * 100 // HIST_INTER_X1000)
    return pred, conf, ix1000


def classify_variance_ratio(roi, ref_variance_x100):
    """Method 9: Variance ratio. Acc = 100%."""
    mean   = int(roi.mean())
    diff   = roi.astype(np.int16) - mean
    cur_var= int((diff * diff).sum() // ROI_PIXELS) * 100
    ref_var= max(int(ref_variance_x100), 100)
    ratio  = cur_var * 100 // ref_var
    pred   = 1 if ratio > VAR_RATIO_X100 else 0
    return pred, _confidence(ratio, VAR_RATIO_X100), ratio


def classify_combined(roi, cal):
    """Method 10: Combined weighted voting (7 sub-methods). Weights: 15/15/10/20/20/10/10.
    Khớp hoàn toàn classify_combined() trong C firmware. F1=0.985 trên 324K samples."""
    if cal is None:
        return classify_edge_density(roi)

    sub_results = [
        classify_ref_frame    (roi, cal['ref_frame']),        # weight 15
        classify_gaussian_mad (roi, cal['ref_frame']),        # weight 15
        classify_block_mad    (roi, cal['ref_frame']),        # weight 10
        classify_percentile_mad(roi, cal['ref_frame']),       # weight 20
        classify_max_block    (roi, cal['ref_frame']),        # weight 20
        classify_histogram_inter(roi, cal['ref_hist']),       # weight 10
        classify_variance_ratio(roi, cal['ref_var_x100']),    # weight 10
    ]
    weights = [15, 15, 10, 20, 20, 10, 10]

    # vote = weight × confidence_factor / 100
    # confidence_factor = 50 + conf/2  (range 50–100)
    score = 0
    for (pred, conf, _), w in zip(sub_results, weights):
        if pred:
            cf = 50 + conf // 2
            score += w * cf // 100

    pred = 1 if score > COMBINED_X100 else 0
    return pred, _confidence(score, COMBINED_X100), score


def classify_slot(roi, method, cal):
    """Dispatch classification method. Fallback to edge_density khi chưa calibrate."""
    if method == 0 or cal is None:
        return classify_edge_density(roi)
    if method == 1:
        return classify_bg_relative(roi, cal['baseline_x1000'])
    if method == 2:
        return classify_ref_frame(roi, cal['ref_frame'])
    if method == 3:
        return classify_hybrid(roi, cal['baseline_x1000'], cal['ref_frame'])
    if method == 4:
        return classify_gaussian_mad(roi, cal['ref_frame'])
    if method == 5:
        return classify_block_mad(roi, cal['ref_frame'])
    if method == 6:
        return classify_percentile_mad(roi, cal['ref_frame'])
    if method == 7:
        return classify_max_block(roi, cal['ref_frame'])
    if method == 8:
        return classify_histogram_inter(roi, cal['ref_hist'])
    if method == 9:
        return classify_variance_ratio(roi, cal['ref_var_x100'])
    if method == 10:
        return classify_combined(roi, cal)
    return classify_edge_density(roi)


# ═══════════════════════════════════════════════════════════════════════
#  CALIBRATION BUILDER — port của classifier_calibrate() trong C
# ═══════════════════════════════════════════════════════════════════════

def build_calibration(gray_img, rois):
    """Tạo calibration data từ ảnh bãi trống. Trả về list of cal_dict per slot."""
    cal_slots = []
    for roi_rect in rois:
        roi = roi_extract(gray_img, roi_rect)

        # 1. Baseline edge density ×1000 (for bg_relative)
        baseline = max(_sobel_edge_count(roi, 35) * 1000 // ROI_PIXELS, BG_BASELINE_FLOOR)

        # 2. Reference variance ×100 (two-pass, tránh cancellation)
        mean = int(roi.mean())
        diff = roi.astype(np.int16) - mean
        var  = int((diff * diff).sum() // ROI_PIXELS) * 100
        if var < 100:
            var = 100

        # 3. Reference histogram (16-bin)
        hist, _ = np.histogram(roi.flatten(), bins=HIST_N_BINS, range=(0, 256))

        cal_slots.append({
            'ref_frame':      roi.copy(),
            'baseline_x1000': baseline,
            'ref_var_x100':   var,
            'ref_hist':       hist,
        })
    return cal_slots


# ═══════════════════════════════════════════════════════════════════════
#  INTERACTIVE GUI — OpenCV window với mouse callbacks
# ═══════════════════════════════════════════════════════════════════════

class ParkingLiteLocalDetector:
    """Interactive tool for local testing of ROI MAD classification."""

    COLORS = {
        'occupied':     (0,  40, 220),   # đỏ → OCC
        'empty':        (0, 200,  50),   # xanh lá → FREE
        'uncalibrated': (0, 220, 220),   # vàng → chưa cal
        'warp_point':   (0,   0, 255),   # đỏ bright → warp corners
        'drawing':      (255, 255, 0),   # cyan → đang vẽ
    }

    def __init__(self):
        self.image_path   = None
        self.raw_image    = None    # Ảnh gốc BGR (chưa warp)
        self.gray_image   = None    # Grayscale (đã warp nếu có)
        self.display_img  = None    # Frame hiển thị (annotated BGR)

        self.rois         = []      # list of (x, y, w, h)
        self.cal_slots    = None    # list of cal_dict per slot
        self.results      = []      # list of (pred, conf, raw)
        self.method       = 10

        # Drawing state
        self.drawing      = False
        self.draw_start   = None
        self.draw_end     = None

        # Warp state
        self.warp_mode    = False
        self.warp_points  = []      # list of (x, y) — 4 corners TL,TR,BR,BL
        self.warp_matrix  = None    # cv2.getPerspectiveTransform result
        self.warp_size    = None    # (W, H) của warped image

        self.win = "ParkingLite Local Detector  [Q=quit H=help]"
        cv2.namedWindow(self.win, cv2.WINDOW_NORMAL)
        cv2.resizeWindow(self.win, 960, 720)
        cv2.setMouseCallback(self.win, self._on_mouse)

    # ── File dialog ──────────────────────────────────────────────────

    @staticmethod
    def _pick_file(title="Select image"):
        root = tk.Tk(); root.withdraw()
        path = filedialog.askopenfilename(
            title=title,
            filetypes=[("Images", "*.png *.jpg *.jpeg *.bmp *.tiff"), ("All", "*.*")]
        )
        root.destroy()
        return path or None

    # ── Image loading & warp ─────────────────────────────────────────

    def load_image(self, path):
        img = cv2.imread(path)
        if img is None:
            print(f"[ERROR] Cannot load: {path}")
            return False
        self.image_path  = path
        self.raw_image   = img
        self._recompute_gray()
        self.results     = []
        self.cal_slots   = None
        print(f"[OK] Loaded {os.path.basename(path)} ({img.shape[1]}×{img.shape[0]})")
        self._refresh()
        return True

    def _recompute_gray(self):
        """Convert raw→gray, optionally apply warp."""
        if self.raw_image is None:
            return
        gray = cv2.cvtColor(self.raw_image, cv2.COLOR_BGR2GRAY)
        if self.warp_matrix is not None:
            gray = cv2.warpPerspective(gray, self.warp_matrix, self.warp_size)
        self.gray_image = gray

    def _set_warp(self, pts):
        """Compute perspective matrix from 4 points (TL, TR, BR, BL)."""
        tl, tr, br, bl = pts
        W = int(max(np.linalg.norm(np.array(tr) - np.array(tl)),
                    np.linalg.norm(np.array(br) - np.array(bl))))
        H = int(max(np.linalg.norm(np.array(bl) - np.array(tl)),
                    np.linalg.norm(np.array(br) - np.array(tr))))
        src = np.float32([tl, tr, br, bl])
        dst = np.float32([[0,0], [W-1,0], [W-1,H-1], [0,H-1]])
        self.warp_matrix = cv2.getPerspectiveTransform(src, dst)
        self.warp_size   = (W, H)
        self._recompute_gray()
        self.rois      = []
        self.cal_slots = None
        self.results   = []
        print(f"[WARP] Applied: output size {W}×{H}. Vẽ lại ROI trên ảnh đã warp.")

    def reset_warp(self):
        self.warp_matrix = None
        self.warp_size   = None
        self._recompute_gray()
        self.rois      = []
        self.cal_slots = None
        self.results   = []
        print("[WARP] Reset — quay về ảnh gốc. Vẽ lại ROI.")

    # ── Calibration & Classification ─────────────────────────────────

    def calibrate_current(self):
        if self.gray_image is None or not self.rois:
            print("[WARN] Cần load ảnh và vẽ ROI trước (phím A)")
            return
        self.cal_slots = build_calibration(self.gray_image, self.rois)
        print(f"[CAL] OK — {len(self.cal_slots)} slots từ ảnh hiện tại")
        for i, c in enumerate(self.cal_slots):
            print(f"  Slot {i}: baseline={c['baseline_x1000']}‰  var={c['ref_var_x100']}×100")
        self.classify()

    def calibrate_from_file(self):
        path = self._pick_file("Chọn ảnh bãi TRỐNG để calibrate")
        if not path:
            return
        gray = cv2.imread(path, cv2.IMREAD_GRAYSCALE)
        if gray is None:
            print(f"[ERROR] Cannot load: {path}")
            return
        if self.warp_matrix is not None:
            gray = cv2.warpPerspective(gray, self.warp_matrix, self.warp_size)
        if not self.rois:
            print("[WARN] Vẽ ROI trước (phím A)")
            return
        self.cal_slots = build_calibration(gray, self.rois)
        print(f"[CAL] OK — {len(self.cal_slots)} slots từ {os.path.basename(path)}")
        self.classify()

    def classify(self):
        if self.gray_image is None or not self.rois:
            return
        self.results = []
        bitmap = 0
        for i, roi_rect in enumerate(self.rois):
            roi = roi_extract(self.gray_image, roi_rect)
            cal = self.cal_slots[i] if (self.cal_slots and i < len(self.cal_slots)) else None
            pred, conf, raw = classify_slot(roi, self.method, cal)
            self.results.append((pred, conf, raw))
            if pred:
                bitmap |= (1 << i)
        self._refresh()
        self._print_results(bitmap)

    def _print_results(self, bitmap):
        mn = METHOD_NAMES[self.method] if self.method < len(METHOD_NAMES) else "?"
        print(f"\n[RESULT] Method={self.method} ({mn})")
        print(f"  Bitmap: 0b{bitmap:08b}  (0x{bitmap:02X})  —  ESP32 ROI_LOAD ready")
        for i, (pred, conf, raw) in enumerate(self.results):
            marker = "■ OCC" if pred else "□ FREE"
            print(f"  Slot {i}: {marker}  conf={conf:3d}%  raw={raw}")

    # ── ROI inspection window ────────────────────────────────────────

    def inspect_rois(self):
        """Hiển thị cửa sổ phụ: ROI patch current vs reference side-by-side."""
        if self.gray_image is None or not self.rois:
            print("[INSPECT] Chưa có ROI")
            return
        cells = []
        scale = 6  # 32×32 → 192×192
        for i, roi_rect in enumerate(self.rois):
            roi = roi_extract(self.gray_image, roi_rect)
            cur_big = cv2.resize(roi, (ROI_SIZE*scale, ROI_SIZE*scale), interpolation=cv2.INTER_NEAREST)
            cur_bgr = cv2.cvtColor(cur_big, cv2.COLOR_GRAY2BGR)

            if self.cal_slots and i < len(self.cal_slots):
                ref = self.cal_slots[i]['ref_frame']
                ref_big = cv2.resize(ref, (ROI_SIZE*scale, ROI_SIZE*scale), interpolation=cv2.INTER_NEAREST)
                ref_bgr = cv2.cvtColor(ref_big, cv2.COLOR_GRAY2BGR)
                diff_big = cv2.absdiff(cur_big, ref_big)
                # colormap diff
                diff_color = cv2.applyColorMap(diff_big, cv2.COLORMAP_JET)
            else:
                blank = np.zeros((ROI_SIZE*scale, ROI_SIZE*scale, 3), dtype=np.uint8)
                ref_bgr = blank.copy(); diff_color = blank.copy()
                cv2.putText(ref_bgr, "No CAL", (20, ROI_SIZE*scale//2),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.8, (100,100,100), 2)

            label_h = 22
            def labeled(img, text, color=(220,220,220)):
                out = np.zeros((label_h + img.shape[0], img.shape[1], 3), np.uint8)
                out[label_h:] = img
                cv2.putText(out, text, (3, 15), cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 1)
                return out

            if self.results and i < len(self.results):
                pred, conf, raw = self.results[i]
                tag = f"S{i} {'OCC' if pred else 'FREE'} c={conf}% r={raw}"
                tc  = self.COLORS['occupied'] if pred else self.COLORS['empty']
            else:
                tag = f"Slot {i} (no result)"
                tc  = (200, 200, 0)

            row = np.hstack([
                labeled(cur_bgr,   f"Current (S{i})", (200,200,200)),
                labeled(ref_bgr,   "Reference",       (150,150,150)),
                labeled(diff_color,f"Diff  {tag}",    tc),
            ])
            cells.append(row)

        panel = np.vstack(cells) if len(cells) <= 4 else np.vstack(cells[:4])
        cv2.imshow("ROI Inspection  [any key to close]", panel)
        cv2.waitKey(0)
        cv2.destroyWindow("ROI Inspection  [any key to close]")

    # ── Display rendering ─────────────────────────────────────────────

    def _refresh(self):
        if self.raw_image is None:
            blank = np.zeros((480, 640, 3), np.uint8)
            cv2.putText(blank, "Press O to open image", (80, 240),
                        cv2.FONT_HERSHEY_SIMPLEX, 1.0, (180,180,180), 2)
            cv2.imshow(self.win, blank)
            return

        # Get the display source (warped if warp active)
        if self.warp_matrix is not None:
            src = cv2.warpPerspective(self.raw_image, self.warp_matrix, self.warp_size)
        else:
            src = self.raw_image.copy()
        disp = src.copy()

        # Draw ROI boxes
        for i, (x, y, w, h) in enumerate(self.rois):
            if self.results and i < len(self.results):
                pred, conf, _ = self.results[i]
                color = self.COLORS['occupied'] if pred else self.COLORS['empty']
                tag   = f"S{i}:{'OCC' if pred else 'FREE'} {conf}%"
            else:
                color = self.COLORS['uncalibrated']
                tag   = f"S{i}"
            cv2.rectangle(disp, (x, y), (x+w, y+h), color, 2)
            # Shadow + label
            cv2.putText(disp, tag, (x+1, y-4), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0,0,0), 2)
            cv2.putText(disp, tag, (x,   y-5), cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 1)

        # In-progress rectangle while drawing
        if self.drawing and self.draw_start and self.draw_end:
            x0, y0 = self.draw_start; x1, y1 = self.draw_end
            cv2.rectangle(disp, (min(x0,x1), min(y0,y1)),
                                 (max(x0,x1), max(y0,y1)), self.COLORS['drawing'], 1)

        # Warp corner markers
        for j, pt in enumerate(self.warp_points):
            cv2.circle(disp, pt, 8, self.COLORS['warp_point'], -1)
            labels = ['TL','TR','BR','BL']
            cv2.putText(disp, labels[j], (pt[0]+10, pt[1]+10),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0,0,255), 2)

        # Status bar
        mn     = METHOD_NAMES[self.method] if self.method < len(METHOD_NAMES) else "?"
        cal_s  = "CAL:YES" if self.cal_slots else "CAL:NO"
        warp_s = f"WARP:{self.warp_size[0]}x{self.warp_size[1]}" if self.warp_matrix is not None else ""
        mode_s = "WARP_MODE:click 4 corners" if self.warp_mode else ""
        status = (f" M{self.method}:{mn} | {cal_s} | {warp_s}{mode_s} | "
                  f"Slots:{len(self.rois)}  "
                  f"A=add D=del C=cal L=load_cal SPACE=run W=warp E=export I=inspect Q=quit")
        h, w = disp.shape[:2]
        cv2.rectangle(disp, (0, h-26), (w, h), (20,20,20), -1)
        cv2.putText(disp, status, (4, h-8), cv2.FONT_HERSHEY_SIMPLEX, 0.38, (190,190,190), 1)

        self.display_img = disp
        cv2.imshow(self.win, disp)

    # ── Mouse callback ────────────────────────────────────────────────

    def _on_mouse(self, event, x, y, flags, param):
        # Warp mode: collect 4 corner clicks
        if self.warp_mode:
            if event == cv2.EVENT_LBUTTONDOWN:
                self.warp_points.append((x, y))
                labels = ['TL','TR','BR','BL']
                print(f"[WARP] Point {len(self.warp_points)} ({labels[len(self.warp_points)-1]}): ({x},{y})")
                if len(self.warp_points) == 4:
                    self._set_warp(self.warp_points)
                    self.warp_points = []
                    self.warp_mode   = False
                self._refresh()
            return

        # Normal mode: click+drag to draw ROI rectangle
        if event == cv2.EVENT_LBUTTONDOWN:
            self.drawing    = True
            self.draw_start = (x, y)
            self.draw_end   = (x, y)
        elif event == cv2.EVENT_MOUSEMOVE and self.drawing:
            self.draw_end = (x, y)
            self._refresh()
        elif event == cv2.EVENT_LBUTTONUP and self.drawing:
            self.drawing = False
            x0, y0 = self.draw_start
            rx, ry = min(x0, x), min(y0, y)
            rw, rh = abs(x - x0), abs(y - y0)
            if rw > 10 and rh > 10:
                self.rois.append((rx, ry, rw, rh))
                print(f"[ROI] Added slot {len(self.rois)-1}: x={rx} y={ry} w={rw} h={rh}")
            self.draw_start = self.draw_end = None
            self._refresh()

    # ── Export ───────────────────────────────────────────────────────

    def export(self):
        if not self.rois:
            print("[WARN] Không có ROI để export")
            return

        mn  = METHOD_NAMES[self.method] if self.method < len(METHOD_NAMES) else "?"
        cfg = {
            "method":        self.method,
            "method_name":   mn,
            "n_slots":       len(self.rois),
            "image_source":  str(self.image_path),
            "warp_applied":  self.warp_matrix is not None,
            "slots": [
                {"i": i, "x": int(x), "y": int(y), "w": int(w), "h": int(h)}
                for i, (x, y, w, h) in enumerate(self.rois)
            ],
        }
        if self.warp_matrix is not None:
            cfg["warp_matrix"] = self.warp_matrix.tolist()
            cfg["warp_size"]   = list(self.warp_size)

        with open("roi_config_local.json", "w") as f:
            json.dump(cfg, f, indent=2)
        print("[EXPORT] Saved: roi_config_local.json")

        # ESP32 ROI_LOAD command (chỉ dùng khi KHÔNG có warp trong firmware)
        coords = " ".join(f"{x} {y} {w} {h}" for x, y, w, h in self.rois)
        cmd    = f"ROI_LOAD {len(self.rois)} {coords}"
        with open("roi_load_command.txt", "w") as f:
            f.write(cmd + "\n")
        print("[EXPORT] Saved: roi_load_command.txt")
        print(f"\n[ESP32 Serial Command]:\n  {cmd}\n")
        if self.warp_matrix is not None:
            print("  NOTE: ROI tọa độ trên ảnh đã warp — firmware cần thêm perspective warp để dùng.")

    def save_result(self):
        if self.display_img is None:
            return
        out = "result_annotated.jpg"
        cv2.imwrite(out, self.display_img)
        print(f"[SAVE] {out}")

    # ── Main event loop ───────────────────────────────────────────────

    def run(self, initial_path=None):
        if initial_path:
            self.load_image(initial_path)
        self._refresh()

        print("\n══════ ParkingLite Local Detector ══════")
        print(f"  Method: {self.method} ({METHOD_NAMES[self.method]})")
        print("  Keys: A=add_ROI  D=del_ROI  C=calibrate  L=load_cal_img")
        print("        SPACE=classify  O=open_image  W=warp  E=export")
        print("        I=inspect_ROIs  S=save_result  0-9=method  Q=quit")
        if not initial_path:
            print("  Chưa có ảnh — nhấn O để mở ảnh.")
        print()

        while True:
            key = cv2.waitKey(30) & 0xFF

            if key in (ord('q'), ord('Q'), 27):
                break
            elif key in (ord('a'), ord('A')):
                print("[MODE] Click+drag để vẽ ROI cho slot mới")
            elif key in (ord('d'), ord('D')):
                if self.rois:
                    removed = self.rois.pop()
                    self.results = []
                    print(f"[ROI] Đã xóa slot {len(self.rois)}: {removed}")
                    self._refresh()
            elif key in (ord('c'), ord('C')):
                self.calibrate_current()
            elif key in (ord('l'), ord('L')):
                self.calibrate_from_file()
            elif key in (ord(' '), ord('r'), ord('R')):
                self.classify()
            elif key in (ord('o'), ord('O')):
                path = self._pick_file()
                if path:
                    self.load_image(path)
            elif key in (ord('w'), ord('W')):
                if self.warp_matrix is not None:
                    self.reset_warp()
                    self._refresh()
                else:
                    self.warp_mode   = True
                    self.warp_points = []
                    print("[WARP] Click 4 góc theo thứ tự: TL → TR → BR → BL")
            elif key in (ord('e'), ord('E')):
                self.export()
            elif key in (ord('s'), ord('S')):
                self.save_result()
            elif key in (ord('i'), ord('I')):
                self.inspect_rois()
            elif key in (ord('h'), ord('H')):
                print(__doc__)
            elif ord('1') <= key <= ord('9'):
                self.method = key - ord('0')
                print(f"[METHOD] → {self.method} ({METHOD_NAMES[self.method]})")
                if not self.cal_slots:
                    print("         WARNING: method này cần CAL (nhấn C)")
                self.classify()
            elif key == ord('0'):
                self.method = 10
                print(f"[METHOD] → 10 (combined) — Combined Ensemble")
                self.classify()

        cv2.destroyAllWindows()


# ═══════════════════════════════════════════════════════════════════════
#  BATCH MODE — không GUI, chạy script hoàn toàn
# ═══════════════════════════════════════════════════════════════════════

def _load_rois_from_json(roi_json_path):
    """Load ROIs from JSON config. Supports multiple formats:
    - local_detector export: {'slots': [{'x','y','w','h'}]}
    - ROI calibration tool:  {'rois': [{'pts': [[x1,y1],[x2,y2],[x3,y3],[x4,y4]]}]}
    """
    with open(roi_json_path) as f:
        cfg = json.load(f)

    rois = []
    if 'slots' in cfg:
        # Format from local_detector export (E key)
        rois = [(s['x'], s['y'], s['w'], s['h']) for s in cfg['slots']]
    elif 'rois' in cfg:
        # Format from ROI calibration tool (pts = 4 corners)
        for r in cfg['rois']:
            pts = r['pts']
            xs = [p[0] for p in pts]
            ys = [p[1] for p in pts]
            x, y = min(xs), min(ys)
            w, h = max(xs) - x, max(ys) - y
            rois.append((x, y, w, h))
    return rois, cfg


def _auto_detect_rois(gray):
    """Auto-detect parking slot ROIs using the full image as single ROI,
    or grid-based detection for overhead parking lot images."""
    h, w = gray.shape[:2]
    # Use full image as single ROI (simplest — always works)
    return [(0, 0, w, h)]


def batch_classify(cal_path, test_paths, roi_json_path=None, method=10):
    """
    Calibrate từ 1 ảnh, phân loại nhiều ảnh test. Không cần GUI.

    Args:
        cal_path:      Đường dẫn ảnh bãi TRỐNG (empty lot) để calibrate
        test_paths:    List đường dẫn ảnh cần phân loại
        roi_json_path: Đường dẫn roi_config_local.json (export từ interactive mode)
                       Cũng hỗ trợ roi_config_parking_*.json (từ ROI calibration tool)
                       Nếu None → dùng toàn bộ ảnh làm 1 ROI
        method:        Method index (default 10 = combined)
    """
    # Load calibration image
    cal_gray = cv2.imread(cal_path, cv2.IMREAD_GRAYSCALE)
    if cal_gray is None:
        print(f"[ERROR] Cannot load calibration image: {cal_path}")
        return []

    cfg = {}
    # Load ROI config
    if roi_json_path and os.path.exists(roi_json_path):
        rois, cfg = _load_rois_from_json(roi_json_path)
        # Apply warp if present in config
        if cfg.get('warp_applied') and cfg.get('warp_matrix'):
            M    = np.float64(cfg['warp_matrix'])
            sz   = tuple(cfg['warp_size'])
            cal_gray = cv2.warpPerspective(cal_gray, M, sz)
        print(f"[OK] Loaded {len(rois)} ROIs from {roi_json_path}")
    else:
        rois = _auto_detect_rois(cal_gray)
        print(f"[AUTO] Using {len(rois)} auto-detected ROI(s) (full image)")

    # Build calibration
    cal_slots = build_calibration(cal_gray, rois)
    mn = METHOD_NAMES[method] if method < len(METHOD_NAMES) else "?"
    print(f"[CAL] Built for {len(cal_slots)} slots")
    print(f"[RUN] Method={method} ({mn})\n")

    all_results = []
    for img_path in test_paths:
        gray = cv2.imread(img_path, cv2.IMREAD_GRAYSCALE)
        if gray is None:
            print(f"[SKIP] Cannot load: {img_path}")
            continue

        # Apply warp if saved in config
        if cfg.get('warp_applied') and cfg.get('warp_matrix'):
            gray = cv2.warpPerspective(gray, np.float64(cfg['warp_matrix']),
                                       tuple(cfg['warp_size']))

        bitmap = 0
        slot_res = []
        for i, roi_rect in enumerate(rois):
            roi = roi_extract(gray, roi_rect)
            cal = cal_slots[i] if i < len(cal_slots) else None
            pred, conf, raw = classify_slot(roi, method, cal)
            slot_res.append({'slot': i, 'pred': pred, 'conf': conf, 'raw': raw})
            if pred:
                bitmap |= (1 << i)

        fname = os.path.basename(img_path)
        occ = sum(1 for s in slot_res if s['pred'])
        print(f"[{fname}]  bitmap=0b{bitmap:08b} (0x{bitmap:02X})  {occ}/{len(rois)} occupied")
        for s in slot_res:
            label = "■ OCC" if s['pred'] else "□ FREE"
            print(f"  Slot {s['slot']}: {label}  conf={s['conf']:3d}%  raw={s['raw']}")

        all_results.append({
            'image':  img_path,
            'bitmap': bitmap,
            'slots':  slot_res,
        })

    # Save JSON
    with open("batch_results.json", "w") as f:
        json.dump(all_results, f, indent=2, default=int)
    print(f"\n[DONE] {len(all_results)} images processed → batch_results.json")
    return all_results


# ═══════════════════════════════════════════════════════════════════════
#  ENTRY POINT
# ═══════════════════════════════════════════════════════════════════════

def _parse_batch_args(args):
    cal_path, test_paths, roi_path, method = None, [], None, 10
    i = 0
    while i < len(args):
        if args[i] == '-cal' and i + 1 < len(args):
            cal_path = args[i+1]; i += 2
        elif args[i] == '-test':
            i += 1
            while i < len(args) and not args[i].startswith('-'):
                test_paths.append(args[i]); i += 1
        elif args[i] == '-roi' and i + 1 < len(args):
            roi_path = args[i+1]; i += 2
        elif args[i] == '-method' and i + 1 < len(args):
            method = int(args[i+1]); i += 2
        else:
            i += 1
    return cal_path, test_paths, roi_path, method


def main():
    args = sys.argv[1:]

    if '--batch' in args:
        # Batch mode
        args.remove('--batch')
        cal, tests, roi, meth = _parse_batch_args(args)
        if not cal or not tests:
            print("Batch usage:\n"
                  "  local_detector.py --batch \\\n"
                  "    -cal Images/parking_empty.png \\\n"
                  "    -test Images/Harsh_noon_sun/with_car.png \\\n"
                  "    -roi roi_config_local.json \\\n"
                  "    -method 10")
            sys.exit(1)
        batch_classify(cal, tests, roi, meth)
    else:
        # Interactive mode
        img_path = args[0] if args else None
        app = ParkingLiteLocalDetector()
        app.run(img_path)


if __name__ == '__main__':
    main()
