#!/usr/bin/env python3
"""
Generate debug detection overlay images for all 7 weather scenarios.
Output matches the format: colored ROI boxes on real image with labels.

Usage:
    python3 generate_debug_images.py [--output DIR] [--roi ROI_CONFIG.json]

Output per scenario:
    detect_<name>.png — annotated detection overlay (OCC/FREE boxes on test image)
    detect_<name>_empty.png — annotated empty reference

Also generates:
    GRAND_DEBUG_ALL_7.png — all 7 scenarios stitched side by side
"""

import os
import sys
import json
import cv2
import numpy as np

# Add parent dir for local_detector imports
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from local_detector import (
    roi_extract, build_calibration, classify_slot, classify_combined,
    METHOD_NAMES, ROI_SIZE,
    REF_DIFF_X10,
    _load_rois_from_json,
)

# ─── Scenarios ───────────────────────────────────────────────────────
BASE = os.path.join(os.path.dirname(__file__), '..', '..')

SCENARIOS = [
    ("Trua nang",    "Harsh_noon_sun",             "empty_harsh_noon_sun.png",        "with_car_harsh_noon_sun.png"),
    ("Am u",         "Overcast_cloudy",             "empty_overcast_cloudy.png",       "with_car_overcast_cloudy.png"),
    ("Mua nhe",      "Light_rain",                  "empty_light_rain.png",            "with_car_light_rain.png"),
    ("Mua to",       "Heavy_rain",                  "empty_heavy_rain.png",            "with_car_heavy_rain.png"),
    ("Suong mu",     "Early Morning Fog",           "empty_car_early_mor_fog.png",     "with_car_early_mor_fog.png"),
    ("Chieu muon",   "Late_afternoon",              "empty_late_afternoon.png",        "with_car_late_afternoon.png"),
    ("Dem mua",      "Night Rain with Lights",      "empty_car_night_rain_lights.png", "with_car_night_rain_lights.png"),
]

# Ground truth is computed dynamically: n//2 top = OCC, rest = FREE

# Colors (BGR)
GREEN    = (0, 220, 0)
CYAN     = (220, 220, 0)
RED      = (0, 0, 220)
WHITE    = (255, 255, 255)
BLACK    = (0, 0, 0)
HEADER_BG = (50, 50, 140)  # dark brown-red


def load_roi_config(roi_path):
    """Load ROI config and return list of (x,y,w,h) + labels."""
    rois, cfg = _load_rois_from_json(roi_path)
    labels = []
    if 'rois' in cfg:
        labels = [r.get('label', f'S{i}') for i, r in enumerate(cfg['rois'])]
    elif 'slots' in cfg:
        labels = [s.get('label', f'S{i}') for i, s in enumerate(cfg['slots'])]
    return rois, labels, cfg


def scale_rois(rois, cfg_size, img_size):
    """Scale ROIs from config image size to actual image size."""
    cfg_w, cfg_h = cfg_size
    img_h, img_w = img_size[:2]
    sx = img_w / cfg_w
    sy = img_h / cfg_h
    scaled = []
    for (x, y, w, h) in rois:
        scaled.append((int(x*sx), int(y*sy), int(w*sx), int(h*sy)))
    return scaled


def draw_detection_overlay(img_bgr, rois, labels, slot_results, scenario_name,
                           ground_truth=None):
    """Draw colored ROI boxes with labels on image. Returns annotated copy."""
    vis = img_bgr.copy()
    h_img, w_img = vis.shape[:2]

    # Compute accuracy if ground truth available
    tp = tn = fp = fn = 0
    if ground_truth and len(ground_truth) == len(slot_results):
        for gt, sr in zip(ground_truth, slot_results):
            pred = sr['pred']
            if gt == 1 and pred == 1: tp += 1
            elif gt == 0 and pred == 0: tn += 1
            elif gt == 0 and pred == 1: fp += 1
            elif gt == 1 and pred == 0: fn += 1
        total = tp + tn + fp + fn
        acc = (tp + tn) / max(total, 1) * 100

    # Header bar
    header_h = max(40, int(h_img * 0.03))
    header = np.full((header_h, w_img, 3), HEADER_BG, dtype=np.uint8)
    font = cv2.FONT_HERSHEY_SIMPLEX
    font_scale = header_h / 50.0
    thickness = max(1, int(font_scale * 2))

    if ground_truth:
        header_text = (f"{scenario_name} | Acc={acc:.0f}% "
                       f"TP={tp} TN={tn} FP={fp} FN={fn} | "
                       f"MAD>={REF_DIFF_X10/10:.0f} OR Edge>=5")
    else:
        occ = sum(1 for s in slot_results if s['pred'])
        header_text = f"{scenario_name} | {occ}/{len(slot_results)} occupied"

    cv2.putText(header, header_text, (10, int(header_h * 0.75)),
                font, font_scale, GREEN, thickness, cv2.LINE_AA)

    # Combine header + image
    vis = np.vstack([header, vis])

    # Draw ROI boxes
    for i, ((x, y, w, h), sr) in enumerate(zip(rois, slot_results)):
        y_off = y + header_h  # offset for header

        pred = sr['pred']
        conf = sr['conf']
        raw = sr['raw']

        # Determine row label
        lbl = labels[i] if i < len(labels) else f"S{i}"
        # Map to T/B naming: first half = Top row, second half = Bottom row
        n = len(rois)
        if i < n // 2:
            row_label = f"T{i}"
        else:
            row_label = f"B{i - n // 2}"

        if pred == 1:
            color = GREEN
            status_text = "XE"
        else:
            color = CYAN
            status_text = "----"

        # Draw rectangle
        cv2.rectangle(vis, (x, y_off), (x + w, y_off + h), color, 3)

        # Slot label (top-left corner)
        fs = max(0.5, font_scale * 0.6)
        th = max(1, int(thickness * 0.7))
        cv2.putText(vis, row_label, (x + 4, y_off + int(20 * font_scale)),
                    font, fs, GREEN, th, cv2.LINE_AA)

        # Method + metric info (below slot label)
        method_text = f"M{sr.get('method', 10)} E={raw}"
        cv2.putText(vis, method_text, (x + 4, y_off + int(38 * font_scale)),
                    font, fs * 0.75, GREEN, max(1, th - 1), cv2.LINE_AA)

        # Status text (center of ROI)
        cx = x + w // 2
        cy = y_off + h // 2
        (tw, th_t), _ = cv2.getTextSize(status_text, font, fs, th)
        cv2.putText(vis, status_text, (cx - tw // 2, cy + th_t // 2),
                    font, fs, WHITE, th, cv2.LINE_AA)

    return vis


def process_scenario(scenario_name, folder, empty_file, test_file,
                     rois, labels, cfg, output_dir, method=10):
    """Process one scenario: calibrate + classify + draw debug overlay."""
    empty_path = os.path.join(BASE, 'Images', folder, empty_file)
    test_path = os.path.join(BASE, 'Images', folder, test_file)

    # Load images
    empty_bgr = cv2.imread(empty_path)
    test_bgr = cv2.imread(test_path)
    if empty_bgr is None:
        print(f"  [SKIP] Cannot load: {empty_path}")
        return None
    if test_bgr is None:
        print(f"  [SKIP] Cannot load: {test_path}")
        return None

    empty_gray = cv2.cvtColor(empty_bgr, cv2.COLOR_BGR2GRAY)
    test_gray = cv2.cvtColor(test_bgr, cv2.COLOR_BGR2GRAY)

    # Scale ROIs to match actual image size
    cfg_size = cfg.get('image_size', [empty_gray.shape[1], empty_gray.shape[0]])
    scaled_rois = scale_rois(rois, cfg_size, empty_gray.shape)

    # Sort ROIs into rows: top row (lower y) sorted left-to-right,
    # bottom row (higher y) sorted left-to-right
    img_h = empty_gray.shape[0]
    mid_y = img_h // 2
    top_indices = sorted([i for i in range(len(scaled_rois)) if scaled_rois[i][1] < mid_y],
                         key=lambda i: scaled_rois[i][0])
    bot_indices = sorted([i for i in range(len(scaled_rois)) if scaled_rois[i][1] >= mid_y],
                         key=lambda i: scaled_rois[i][0])
    sorted_order = top_indices + bot_indices
    scaled_rois = [scaled_rois[i] for i in sorted_order]
    labels = [labels[i] if i < len(labels) else f"S{i}" for i in sorted_order]

    # Calibrate
    cal_slots = build_calibration(empty_gray, scaled_rois)

    # Classify each slot on test image (with car)
    slot_results_test = []
    for i, roi_rect in enumerate(scaled_rois):
        roi = roi_extract(test_gray, roi_rect)
        cal = cal_slots[i] if i < len(cal_slots) else None
        pred, conf, raw = classify_slot(roi, method, cal)
        slot_results_test.append({
            'slot': i, 'pred': pred, 'conf': conf, 'raw': raw, 'method': method
        })

    # Also classify empty image (should be all FREE)
    slot_results_empty = []
    for i, roi_rect in enumerate(scaled_rois):
        roi = roi_extract(empty_gray, roi_rect)
        cal = cal_slots[i] if i < len(cal_slots) else None
        pred, conf, raw = classify_slot(roi, method, cal)
        slot_results_empty.append({
            'slot': i, 'pred': pred, 'conf': conf, 'raw': raw, 'method': method
        })

    # Ground truth
    n = len(scaled_rois)
    gt_test = [1] * (n // 2) + [0] * (n - n // 2)   # top=OCC, bottom=FREE
    gt_empty = [0] * n                                # all FREE

    # Draw overlays
    detect_test = draw_detection_overlay(
        test_bgr, scaled_rois, labels, slot_results_test,
        scenario_name, ground_truth=gt_test)

    detect_empty = draw_detection_overlay(
        empty_bgr, scaled_rois, labels, slot_results_empty,
        f"{scenario_name} (empty)", ground_truth=gt_empty)

    # Resize to max 1280 width for reasonable file size
    for img_ref in [detect_test, detect_empty]:
        pass  # just a marker
    def _resize_max(img, max_w=1280):
        h, w = img.shape[:2]
        if w > max_w:
            scale = max_w / w
            return cv2.resize(img, (max_w, int(h * scale)))
        return img
    detect_test = _resize_max(detect_test)
    detect_empty = _resize_max(detect_empty)

    # Save
    safe_name = scenario_name.replace(' ', '_')
    test_path_out = os.path.join(output_dir, f"detect_{safe_name}.png")
    empty_path_out = os.path.join(output_dir, f"detect_{safe_name}_empty.png")
    cv2.imwrite(test_path_out, detect_test)
    cv2.imwrite(empty_path_out, detect_empty)

    # Print summary
    tp = sum(1 for g, s in zip(gt_test, slot_results_test) if g == 1 and s['pred'] == 1)
    tn = sum(1 for g, s in zip(gt_test, slot_results_test) if g == 0 and s['pred'] == 0)
    fp = sum(1 for g, s in zip(gt_test, slot_results_test) if g == 0 and s['pred'] == 1)
    fn = sum(1 for g, s in zip(gt_test, slot_results_test) if g == 1 and s['pred'] == 0)
    acc = (tp + tn) / max(tp + tn + fp + fn, 1) * 100
    print(f"  {scenario_name}: Acc={acc:.0f}% TP={tp} TN={tn} FP={fp} FN={fn}")

    # Return results for grand image
    return {
        'name': scenario_name,
        'detect_img': detect_test,
        'empty_img': detect_empty,
        'acc': acc, 'tp': tp, 'tn': tn, 'fp': fp, 'fn': fn,
        'slot_results_test': slot_results_test,
        'slot_results_empty': slot_results_empty,
    }


def make_grand_image(results, output_dir):
    """Stitch all scenario detect images into one grand image."""
    imgs = [r['detect_img'] for r in results if r is not None]
    if not imgs:
        return

    # Resize all to same height
    target_h = min(img.shape[0] for img in imgs)
    resized = []
    for img in imgs:
        scale = target_h / img.shape[0]
        new_w = int(img.shape[1] * scale)
        resized.append(cv2.resize(img, (new_w, target_h)))

    # Stack horizontally (may be very wide)
    grand = np.hstack(resized)

    # If too wide, arrange in grid (2 rows)
    if len(resized) > 4:
        row1 = np.hstack(resized[:4])
        row2_imgs = resized[4:]
        # Pad row2 to match row1 width
        row2 = np.hstack(row2_imgs)
        if row2.shape[1] < row1.shape[1]:
            pad = np.zeros((row2.shape[0], row1.shape[1] - row2.shape[1], 3), dtype=np.uint8)
            row2 = np.hstack([row2, pad])
        elif row2.shape[1] > row1.shape[1]:
            row2 = row2[:, :row1.shape[1]]
        grand = np.vstack([row1, row2])

    out_path = os.path.join(output_dir, 'GRAND_DEBUG_ALL_7.png')
    cv2.imwrite(out_path, grand)
    print(f"\n[GRAND] Saved {grand.shape[1]}x{grand.shape[0]} → {out_path}")


def main():
    # Default paths
    roi_path = os.path.join(BASE, 'Simulation', 'roi_config_parking_empty.json')
    output_dir = os.path.join(BASE, 'Simulation', 'output', 'real_photo_results', 'final_7scenarios')
    method = 10  # combined ensemble

    # Parse simple args
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == '--output' and i + 1 < len(args):
            output_dir = args[i + 1]; i += 2
        elif args[i] == '--roi' and i + 1 < len(args):
            roi_path = args[i + 1]; i += 2
        elif args[i] == '--method' and i + 1 < len(args):
            method = int(args[i + 1]); i += 2
        else:
            i += 1

    os.makedirs(output_dir, exist_ok=True)

    print(f"=== ParkingLite Debug Image Generator ===")
    print(f"ROI config: {roi_path}")
    print(f"Output dir: {output_dir}")
    print(f"Method:     {method} ({METHOD_NAMES[method]})")
    print()

    # Load ROI config
    rois, labels, cfg = load_roi_config(roi_path)
    print(f"Loaded {len(rois)} ROIs")
    print()

    # Process each scenario
    results = []
    for name, folder, empty_file, test_file in SCENARIOS:
        result = process_scenario(
            name, folder, empty_file, test_file,
            rois, labels, cfg, output_dir, method)
        results.append(result)

    # Grand combined image
    make_grand_image(results, output_dir)

    # Summary
    print(f"\n{'='*50}")
    print(f"SUMMARY: {len([r for r in results if r])} / {len(SCENARIOS)} scenarios processed")
    total_acc = np.mean([r['acc'] for r in results if r])
    print(f"Average accuracy: {total_acc:.1f}%")


if __name__ == '__main__':
    main()
