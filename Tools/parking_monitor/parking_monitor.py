#!/usr/bin/env python3
"""
ParkingLite Live Monitor
========================
Desktop app to view ESP32-CAM live feed + control classifier.

Features:
    • Live camera view (JPEG over serial, ~1-2 fps)
    • ROI overlay (8 slots with occupancy status)
    • 8-slot status grid (color-coded: green=free, red=occupied)
    • Control panel: CAL, METHOD, INTERVAL, RESET, FLASH
    • Serial log (timestamped, auto-scroll)
    • Auto-save captures to captures/ folder
    • Statistics: FPS, uptime, bitmap history

Requirements: PyQt5, pyserial, Pillow
Run: python3 parking_monitor.py [--port /dev/cu.usbserial-XXX]
"""

import sys, os, re, json, time, argparse
from datetime import datetime
from pathlib import Path
from io import BytesIO

import serial
import serial.tools.list_ports
from PIL import Image

from PyQt5.QtCore import (Qt, QThread, pyqtSignal, QTimer, QObject, pyqtSlot,
                           QSize, QRect)
from PyQt5.QtGui import (QPixmap, QImage, QPainter, QPen, QColor, QFont,
                          QPalette, QIcon, QBrush)
from PyQt5.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout,
                              QHBoxLayout, QGridLayout, QLabel, QPushButton,
                              QTextEdit, QComboBox, QSpinBox, QFrame,
                              QStatusBar, QAction, QFileDialog, QMessageBox,
                              QSplitter, QGroupBox, QSizePolicy, QLineEdit,
                              QCheckBox, QProgressBar)


# ═══════════════════════════════════════════════════════════════════════
# Configuration
# ═══════════════════════════════════════════════════════════════════════

APP_NAME = "ParkingLite Live Monitor"
APP_VERSION = "1.0"
CAPTURES_DIR = Path(__file__).parent / "captures"
CAPTURES_DIR.mkdir(exist_ok=True)

DEFAULT_BAUD = 115200
AUTO_SNAP_INTERVAL_MS = 2000  # Capture JPEG every 2s when auto-snap enabled

# UI Colors (dark theme)
C_BG = "#1e1e2e"
C_BG2 = "#2a2a3e"
C_ACCENT = "#7aa2f7"
C_SUCCESS = "#9ece6a"
C_WARN = "#e0af68"
C_ERROR = "#f7768e"
C_TEXT = "#c0caf5"
C_TEXT_DIM = "#7c809a"
C_OCC = "#f7768e"  # red - occupied
C_FREE = "#9ece6a"  # green - free
C_UNKNOWN = "#7c809a"  # gray - unknown

# Capture modes (matches firmware commands)
CAPTURE_MODES = [
    ("SNAP", "Grayscale 320×240 (nhanh)"),
    ("SNAP_COLOR", "Color SVGA 800×600"),
    ("SNAP_XGA", "Color XGA 1024×768"),
    ("SNAP_UXGA", "Color UXGA 1600×1200 (max)"),
]


# ═══════════════════════════════════════════════════════════════════════
# Serial Worker Thread
# ═══════════════════════════════════════════════════════════════════════

class SerialWorker(QThread):
    """Background thread handling serial I/O.

    Parses firmware output and emits structured signals for UI.
    """
    line_received = pyqtSignal(str)               # raw serial line
    bitmap_update = pyqtSignal(int, int, str)     # bitmap, n_occupied, method
    image_received = pyqtSignal(bytes, int, int)  # jpg_bytes, width, height
    slots_received = pyqtSignal(dict)              # parsed SLOTS_JSON
    rois_received = pyqtSignal(list)               # list of (x,y,w,h)
    connected = pyqtSignal(bool)
    error = pyqtSignal(str)

    def __init__(self, port, baud=DEFAULT_BAUD, parent=None):
        super().__init__(parent)
        self.port = port
        self.baud = baud
        self._running = False
        self._cmd_queue = []
        self._ser = None

    def stop(self):
        self._running = False

    def send_cmd(self, cmd):
        """Thread-safe: queue command to be sent from serial thread."""
        self._cmd_queue.append(cmd)

    def run(self):
        try:
            self._ser = serial.Serial(self.port, self.baud, timeout=0.1)
            self.connected.emit(True)
            self._running = True
        except Exception as e:
            self.error.emit(f"Cannot open {self.port}: {e}")
            return

        buf = b""
        snap_state = None  # None or {"size":N, "w":W, "h":H, "data":b"", "start":float}

        while self._running:
            # Send queued commands
            while self._cmd_queue:
                cmd = self._cmd_queue.pop(0)
                try:
                    self._ser.write((cmd + "\n").encode())
                    self._ser.flush()
                except Exception as e:
                    self.error.emit(f"Write failed: {e}")

            try:
                chunk = self._ser.read(8192)
            except Exception as e:
                self.error.emit(f"Read error: {e}")
                break

            if not chunk:
                # Check snap timeout
                if snap_state and (time.time() - snap_state["start"] > 30):
                    self.line_received.emit(f"[SNAP_TIMEOUT] Only got {len(snap_state['data'])}/{snap_state['size']} bytes")
                    snap_state = None
                continue

            buf += chunk

            # ── Binary SNAP mode: consume JPEG bytes ──
            if snap_state is not None:
                need = snap_state["size"] - len(snap_state["data"])
                consume = min(need, len(buf))
                snap_state["data"] += buf[:consume]
                buf = buf[consume:]

                if len(snap_state["data"]) >= snap_state["size"]:
                    jpg = snap_state["data"][:snap_state["size"]]
                    self.image_received.emit(jpg, snap_state["w"], snap_state["h"])
                    snap_state = None
                elif time.time() - snap_state["start"] > 30:
                    self.line_received.emit(f"[SNAP_TIMEOUT] Only got {len(snap_state['data'])}/{snap_state['size']} bytes")
                    snap_state = None
                continue

            # ── Normal text line processing ──
            while b"\n" in buf:
                line_bytes, buf = buf.split(b"\n", 1)
                try:
                    line = line_bytes.decode("utf-8", errors="replace").rstrip("\r")
                except Exception:
                    continue

                self.line_received.emit(line)

                # Check for SNAP_START → switch to binary mode
                m = re.search(r"\[SNAP_START\]\s+size=(\d+)\s+width=(\d+)\s+height=(\d+)", line)
                if m:
                    snap_state = {
                        "size": int(m.group(1)),
                        "w": int(m.group(2)),
                        "h": int(m.group(3)),
                        "data": b"",
                        "start": time.time(),
                    }
                    # Remaining buf is start of binary JPEG data
                    need = snap_state["size"]
                    consume = min(need, len(buf))
                    snap_state["data"] += buf[:consume]
                    buf = buf[consume:]

                    if len(snap_state["data"]) >= snap_state["size"]:
                        jpg = snap_state["data"][:snap_state["size"]]
                        self.image_received.emit(jpg, snap_state["w"], snap_state["h"])
                        snap_state = None
                    break  # Exit text parsing, continue outer loop in binary mode

                # Bitmap update
                m = re.search(r"Bitmap=0b(\d+)\s+\(0x([0-9a-fA-F]+)\)\s+\|\s+(\d+)ms\s+\|\s+M(\d+)\s+\|\s+(\d+)/(\d+)", line)
                if m:
                    bitmap = int(m.group(2), 16)
                    n_occ = int(m.group(5))
                    method = int(m.group(4))
                    self.bitmap_update.emit(bitmap, n_occ, f"M{method}")
                    continue

                # SLOTS_JSON
                m = re.search(r"\[SLOTS_JSON\]\s+(\{.*\})", line)
                if m:
                    try:
                        data = json.loads(m.group(1))
                        self.slots_received.emit(data)
                    except: pass
                    continue

                # ROI_JSON
                m = re.search(r"\[ROI_JSON\]\s+(\{.*\})", line)
                if m:
                    try:
                        data = json.loads(m.group(1))
                        rois = [(s["x"], s["y"], s["w"], s["h"]) for s in data.get("slots", [])]
                        self.rois_received.emit(rois)
                    except: pass

        try:
            if self._ser: self._ser.close()
        except: pass
        self.connected.emit(False)


# ═══════════════════════════════════════════════════════════════════════
# UI Widgets
# ═══════════════════════════════════════════════════════════════════════

class CameraView(QLabel):
    """Displays the live JPEG stream with ROI overlay."""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumSize(320, 240)
        self.setAlignment(Qt.AlignCenter)
        self.setStyleSheet(f"background-color: {C_BG2}; border: 2px solid {C_ACCENT}; border-radius: 6px;")
        self.setText("Chưa có ảnh\n(Gõ SNAP hoặc bật Auto)")
        self._pixmap = None
        self._rois = []  # list of (x, y, w, h)
        self._slot_states = []  # list of 0/1 per slot
        self._frame_count = 0
        self._last_size = (0, 0)

    def set_frame(self, jpg_bytes, width, height):
        img = QImage.fromData(jpg_bytes, "JPEG")
        if img.isNull(): return
        self._pixmap = QPixmap.fromImage(img)
        self._last_size = (width, height)
        self._frame_count += 1
        self._repaint_with_overlay()

    def set_rois(self, rois):
        self._rois = rois
        self._repaint_with_overlay()

    def set_slot_states(self, states):
        """states: list of 0/1 matching ROI order."""
        self._slot_states = states
        self._repaint_with_overlay()

    def _repaint_with_overlay(self):
        if self._pixmap is None: return
        px = self._pixmap.copy()
        # Scale to widget size while keeping aspect
        w = self.width(); h = self.height()
        scaled = px.scaled(w, h, Qt.KeepAspectRatio, Qt.SmoothTransformation)

        # Draw ROI overlay at scaled coordinates
        if self._rois and self._last_size[0] > 0:
            painter = QPainter(scaled)
            sx = scaled.width() / self._last_size[0]
            sy = scaled.height() / self._last_size[1]
            font = QFont(); font.setBold(True); font.setPointSize(10)
            painter.setFont(font)

            for i, (x, y, rw, rh) in enumerate(self._rois):
                occupied = (i < len(self._slot_states) and self._slot_states[i] == 1)
                color = QColor(C_OCC if occupied else C_FREE)
                pen = QPen(color); pen.setWidth(3)
                painter.setPen(pen)
                painter.drawRect(QRect(int(x*sx), int(y*sy), int(rw*sx), int(rh*sy)))
                # Label
                label = f"S{i} {'XE' if occupied else 'TR'}"
                painter.setPen(QPen(QColor('white')))
                painter.drawText(int(x*sx)+4, int(y*sy)+16, label)
            painter.end()

        self.setPixmap(scaled)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        if self._pixmap is not None:
            self._repaint_with_overlay()


class SlotGrid(QWidget):
    """8-slot status grid (2×4 layout)."""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumHeight(150)
        layout = QGridLayout(self)
        layout.setSpacing(6)
        self.labels = []
        for i in range(8):
            box = QLabel(f"S{i}\n---\n0%")
            box.setAlignment(Qt.AlignCenter)
            box.setMinimumSize(80, 60)
            box.setStyleSheet(self._style(None, 0))
            box.setFont(QFont("SF Mono", 10, QFont.Bold))
            layout.addWidget(box, i // 4, i % 4)
            self.labels.append(box)

    def _style(self, occupied, conf):
        if occupied is None:
            bg = C_BG2; color = C_TEXT_DIM
        elif occupied == 1:
            bg = C_OCC; color = "white"
        else:
            bg = C_SUCCESS; color = "white"
        return (f"background-color: {bg}; color: {color}; "
                f"border: 1px solid {C_ACCENT}; border-radius: 4px; padding: 4px;")

    def update_slots(self, slot_data):
        """slot_data: list of dict with i, pred, conf, raw."""
        for s in slot_data:
            i = s["i"]
            if i >= len(self.labels): continue
            pred = s["pred"]; conf = s["conf"]; raw = s["raw"]
            label = self.labels[i]
            text = f"S{i}\n{'XE' if pred else '---'}\n{conf}%"
            label.setText(text)
            label.setStyleSheet(self._style(pred, conf))

    def update_from_bitmap(self, bitmap):
        for i in range(8):
            bit = (bitmap >> i) & 1
            label = self.labels[i]
            text = f"S{i}\n{'XE' if bit else '---'}"
            label.setText(text)
            label.setStyleSheet(self._style(bit, 100))


class SerialLog(QTextEdit):
    """Read-only serial log with color highlighting."""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setReadOnly(True)
        self.setFont(QFont("SF Mono", 9))
        self.setStyleSheet(f"background-color: #0d0d15; color: {C_TEXT}; "
                          f"border: 1px solid {C_ACCENT}; border-radius: 4px;")
        self._max_lines = 500

    def append_line(self, line):
        ts = datetime.now().strftime("%H:%M:%S")
        # Color based on content
        color = C_TEXT
        if "[OK]" in line or "success" in line.lower(): color = C_SUCCESS
        elif "[WARN]" in line or "warning" in line.lower(): color = C_WARN
        elif "[ERROR]" in line or "fail" in line.lower() or "[FATAL]" in line: color = C_ERROR
        elif line.startswith("["): color = C_ACCENT

        # Skip binary-ish or empty
        if not line.strip(): return

        self.append(f'<span style="color:{C_TEXT_DIM}">{ts}</span> '
                   f'<span style="color:{color}">{self._escape(line)}</span>')

        # Trim
        doc = self.document()
        while doc.blockCount() > self._max_lines:
            cursor = self.textCursor()
            cursor.movePosition(cursor.Start)
            cursor.select(cursor.LineUnderCursor)
            cursor.removeSelectedText()
            cursor.deleteChar()

        self.verticalScrollBar().setValue(self.verticalScrollBar().maximum())

    @staticmethod
    def _escape(s):
        return (s.replace('&','&amp;').replace('<','&lt;').replace('>','&gt;'))


# ═══════════════════════════════════════════════════════════════════════
# Main Window
# ═══════════════════════════════════════════════════════════════════════

class MainWindow(QMainWindow):
    def __init__(self, port):
        super().__init__()
        self.setWindowTitle(f"{APP_NAME} v{APP_VERSION}")
        self.resize(1200, 780)
        self.port = port
        self.worker = None
        self.stats = {
            "frames": 0, "bitmaps": 0, "start_time": time.time(),
            "last_bitmap": None, "n_occupied": 0, "method": "?"
        }

        self._build_ui()
        self._apply_theme()
        self._connect()

        # Auto-snap timer
        self._capture_cmd = "SNAP"  # Default capture command
        self.snap_timer = QTimer()
        self.snap_timer.timeout.connect(lambda: self._send(self._capture_cmd))
        self.status_timer = QTimer()
        self.status_timer.timeout.connect(self._update_stats)
        self.status_timer.start(500)

    def _build_ui(self):
        central = QWidget(); self.setCentralWidget(central)
        main = QVBoxLayout(central); main.setContentsMargins(8,8,8,8); main.setSpacing(8)

        # Top bar
        top = QHBoxLayout()
        self.lbl_conn = QLabel("⚪ Disconnected")
        self.lbl_conn.setFont(QFont("SF Pro", 12, QFont.Bold))
        top.addWidget(self.lbl_conn)
        top.addStretch()

        self.lbl_fps = QLabel("0 fps")
        self.lbl_method = QLabel("M? • 0/8")
        self.lbl_uptime = QLabel("0s")
        for l in (self.lbl_fps, self.lbl_method, self.lbl_uptime):
            l.setFont(QFont("SF Mono", 11))
            l.setStyleSheet(f"color: {C_TEXT_DIM}; padding: 0 8px;")
            top.addWidget(l)
        main.addLayout(top)

        # Main split: left = camera + slots, right = controls + log
        splitter = QSplitter(Qt.Horizontal)

        # LEFT
        left = QWidget(); lv = QVBoxLayout(left); lv.setContentsMargins(0,0,0,0)
        self.camera_view = CameraView()
        lv.addWidget(self.camera_view, 1)

        gb_slots = QGroupBox("Trạng thái 8 ô đỗ")
        gb_slots.setStyleSheet(self._gb_style())
        gl = QVBoxLayout(gb_slots); gl.setContentsMargins(6,14,6,6)
        self.slot_grid = SlotGrid()
        gl.addWidget(self.slot_grid)
        lv.addWidget(gb_slots)

        # RIGHT
        right = QWidget(); rv = QVBoxLayout(right); rv.setContentsMargins(0,0,0,0); rv.setSpacing(8)

        # Controls
        gb_ctrl = QGroupBox("Điều khiển")
        gb_ctrl.setStyleSheet(self._gb_style())
        cl = QVBoxLayout(gb_ctrl); cl.setContentsMargins(8,18,8,8); cl.setSpacing(6)

        # Snapshot row
        h = QHBoxLayout()
        self.btn_snap = QPushButton("📷 Chụp ngay")
        self.btn_snap.clicked.connect(lambda: self._send(self._capture_cmd))
        h.addWidget(self.btn_snap)

        self.chk_auto = QCheckBox("Auto ({}s)".format(AUTO_SNAP_INTERVAL_MS // 1000))
        self.chk_auto.toggled.connect(self._toggle_auto_snap)
        h.addWidget(self.chk_auto)
        cl.addLayout(h)

        # Capture mode selector
        h = QHBoxLayout()
        h.addWidget(QLabel("Chế độ chụp:"))
        self.cb_capture_mode = QComboBox()
        for cmd, desc in CAPTURE_MODES:
            self.cb_capture_mode.addItem(f"{cmd}: {desc}", cmd)
        self.cb_capture_mode.currentIndexChanged.connect(self._on_capture_mode_change)
        h.addWidget(self.cb_capture_mode, 1)
        cl.addLayout(h)

        # Dedicated HD capture button
        self.btn_snap_hd = QPushButton("📸 Chụp HD (Color UXGA)")
        self.btn_snap_hd.clicked.connect(lambda: self._send("SNAP_UXGA"))
        self.btn_snap_hd.setStyleSheet(f"background-color:#3d5a80; color:white; font-weight:bold;")
        cl.addWidget(self.btn_snap_hd)

        # Calibrate
        self.btn_cal = QPushButton("🎯 CALIBRATE (bãi phải trống)")
        self.btn_cal.clicked.connect(lambda: self._send("CAL"))
        self.btn_cal.setStyleSheet(f"background-color:{C_WARN}; color:black; font-weight:bold;")
        cl.addWidget(self.btn_cal)

        self.btn_reset_cal = QPushButton("⟳ Reset Calibration")
        self.btn_reset_cal.clicked.connect(self._confirm_reset)
        cl.addWidget(self.btn_reset_cal)

        # Method
        h = QHBoxLayout()
        h.addWidget(QLabel("Phương pháp:"))
        self.cb_method = QComboBox()
        methods = ["0: edge_density", "1: bg_relative", "2: ref_frame (MAD)",
                   "3: hybrid ★", "4: gaussian_mad", "5: block_mad",
                   "6: percentile_mad", "7: max_block", "8: histogram",
                   "9: variance_ratio", "10: combined"]
        self.cb_method.addItems(methods)
        self.cb_method.setCurrentIndex(3)  # hybrid
        self.cb_method.currentIndexChanged.connect(self._on_method_change)
        h.addWidget(self.cb_method, 1)
        cl.addLayout(h)

        # Interval
        h = QHBoxLayout()
        h.addWidget(QLabel("Scan interval:"))
        self.sp_interval = QSpinBox()
        self.sp_interval.setRange(1000, 60000); self.sp_interval.setSingleStep(1000)
        self.sp_interval.setValue(5000); self.sp_interval.setSuffix(" ms")
        self.sp_interval.valueChanged.connect(lambda v: self._send(f"INTERVAL {v}"))
        h.addWidget(self.sp_interval, 1)
        cl.addLayout(h)

        # Flash LED
        h = QHBoxLayout()
        h.addWidget(QLabel("Flash LED:"))
        for text, cmd in [("OFF","FLASH 0"),("ON","FLASH 1"),("Blink","FLASH 2")]:
            b = QPushButton(text); b.clicked.connect(lambda _, c=cmd: self._send(c))
            h.addWidget(b)
        cl.addLayout(h)

        # Status refresh
        self.btn_status = QPushButton("ℹ️ Status")
        self.btn_status.clicked.connect(lambda: (self._send("STATUS"), self._send("SLOTS_GET")))
        cl.addWidget(self.btn_status)

        # Save image
        self.btn_save = QPushButton("💾 Lưu ảnh hiện tại")
        self.btn_save.clicked.connect(self._save_current)
        cl.addWidget(self.btn_save)
        cl.addStretch()

        rv.addWidget(gb_ctrl)

        # Custom command
        gb_cmd = QGroupBox("Lệnh tùy chỉnh")
        gb_cmd.setStyleSheet(self._gb_style())
        cmdl = QHBoxLayout(gb_cmd); cmdl.setContentsMargins(8,18,8,8)
        self.le_cmd = QLineEdit()
        self.le_cmd.setPlaceholderText("Ví dụ: PING, ROI 10 30 60 80 0 …")
        self.le_cmd.returnPressed.connect(self._send_custom)
        cmdl.addWidget(self.le_cmd, 1)
        b = QPushButton("Gửi"); b.clicked.connect(self._send_custom)
        cmdl.addWidget(b)
        rv.addWidget(gb_cmd)

        # Log
        gb_log = QGroupBox("Serial Log")
        gb_log.setStyleSheet(self._gb_style())
        ll = QVBoxLayout(gb_log); ll.setContentsMargins(4,14,4,4)
        self.log = SerialLog()
        ll.addWidget(self.log)
        rv.addWidget(gb_log, 1)

        splitter.addWidget(left)
        splitter.addWidget(right)
        splitter.setSizes([700, 500])
        main.addWidget(splitter, 1)

        # Menubar
        mb = self.menuBar()
        m_file = mb.addMenu("File")
        a = QAction("Lưu ảnh hiện tại...", self); a.setShortcut("Ctrl+S")
        a.triggered.connect(self._save_current); m_file.addAction(a)
        m_file.addSeparator()
        a = QAction("Mở thư mục captures", self); a.triggered.connect(self._open_captures)
        m_file.addAction(a)
        m_file.addSeparator()
        a = QAction("Thoát", self); a.setShortcut("Ctrl+Q")
        a.triggered.connect(self.close); m_file.addAction(a)

        m_dev = mb.addMenu("Device")
        for cmd in ["PING","STATUS","CAL","RESET","SNAP","SNAP_COLOR","SNAP_XGA","SNAP_UXGA","SLOTS_GET","ROI_GET"]:
            a = QAction(cmd, self); a.triggered.connect(lambda _, c=cmd: self._send(c))
            m_dev.addAction(a)

        self.setStatusBar(QStatusBar())
        self.statusBar().showMessage(f"Port: {self.port}")

    def _gb_style(self):
        return (f"QGroupBox {{ border: 1px solid {C_ACCENT}; border-radius: 4px; "
                f"margin-top: 10px; padding-top: 4px; color: {C_ACCENT}; "
                f"font-weight: bold; }}"
                f"QGroupBox::title {{ subcontrol-origin: margin; left: 10px; "
                f"padding: 0 4px; }}")

    def _apply_theme(self):
        self.setStyleSheet(f"""
            QMainWindow, QWidget {{ background-color: {C_BG}; color: {C_TEXT}; }}
            QPushButton {{ background-color: {C_BG2}; color: {C_TEXT};
                           border: 1px solid {C_ACCENT}; border-radius: 4px;
                           padding: 6px 12px; font-weight: 500; }}
            QPushButton:hover {{ background-color: {C_ACCENT}; color: {C_BG}; }}
            QPushButton:pressed {{ background-color: #5a82d7; }}
            QLabel {{ color: {C_TEXT}; }}
            QComboBox, QSpinBox, QLineEdit {{
                background-color: {C_BG2}; color: {C_TEXT};
                border: 1px solid {C_ACCENT}; border-radius: 4px; padding: 4px; }}
            QCheckBox {{ color: {C_TEXT}; }}
            QMenuBar, QMenuBar::item {{ background-color: {C_BG2}; color: {C_TEXT}; }}
            QMenuBar::item:selected {{ background-color: {C_ACCENT}; color: {C_BG}; }}
            QMenu {{ background-color: {C_BG2}; color: {C_TEXT};
                     border: 1px solid {C_ACCENT}; }}
            QMenu::item:selected {{ background-color: {C_ACCENT}; color: {C_BG}; }}
            QStatusBar {{ background-color: {C_BG2}; color: {C_TEXT_DIM}; }}
        """)

    # ─── Serial connection ─────────────────────────────────────────────
    def _connect(self):
        self.worker = SerialWorker(self.port)
        self.worker.connected.connect(self._on_connected)
        self.worker.line_received.connect(self.log.append_line)
        self.worker.bitmap_update.connect(self._on_bitmap)
        self.worker.image_received.connect(self._on_image)
        self.worker.slots_received.connect(self._on_slots)
        self.worker.rois_received.connect(self._on_rois)
        self.worker.error.connect(self._on_error)
        self.worker.start()

    def _on_connected(self, ok):
        if ok:
            self.lbl_conn.setText(f"🟢 Connected to {self.port}")
            # Request initial data
            QTimer.singleShot(3000, lambda: self._send("ROI_GET"))
            QTimer.singleShot(3500, lambda: self._send("SLOTS_GET"))
        else:
            self.lbl_conn.setText("⚪ Disconnected")

    def _on_error(self, msg):
        QMessageBox.warning(self, "Serial Error", msg)

    def _on_bitmap(self, bitmap, n_occ, method):
        self.stats["last_bitmap"] = bitmap
        self.stats["n_occupied"] = n_occ
        self.stats["method"] = method
        self.stats["bitmaps"] += 1
        self.slot_grid.update_from_bitmap(bitmap)
        # Update camera overlay
        states = [(bitmap >> i) & 1 for i in range(8)]
        self.camera_view.set_slot_states(states)

    def _on_image(self, jpg, w, h):
        self.camera_view.set_frame(jpg, w, h)
        self.stats["frames"] += 1
        # Auto-save
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        path = CAPTURES_DIR / f"snap_{ts}.jpg"
        path.write_bytes(jpg)
        self.statusBar().showMessage(f"Saved {path.name}", 2000)

    def _on_slots(self, data):
        slots = data.get("slots", [])
        self.slot_grid.update_slots(slots)

    def _on_rois(self, rois):
        self.camera_view.set_rois(rois)

    # ─── Actions ───────────────────────────────────────────────────────
    def _send(self, cmd):
        if self.worker:
            self.worker.send_cmd(cmd)
            self.log.append_line(f">>> {cmd}")

    def _send_custom(self):
        cmd = self.le_cmd.text().strip()
        if cmd:
            self._send(cmd)
            self.le_cmd.clear()

    def _on_method_change(self, idx):
        text = self.cb_method.currentText()
        m = text.split(":")[0]
        self._send(f"METHOD {m}")

    def _on_capture_mode_change(self, idx):
        cmd = self.cb_capture_mode.currentData()
        if cmd:
            self._capture_cmd = cmd
            self.statusBar().showMessage(f"Capture mode: {cmd}", 2000)

    def _confirm_reset(self):
        r = QMessageBox.question(self, "Reset Calibration",
                                 "Xóa calibration trong NVS? Cần CAL lại sau.",
                                 QMessageBox.Yes | QMessageBox.No)
        if r == QMessageBox.Yes:
            self._send("RESET")

    def _toggle_auto_snap(self, on):
        if on:
            self.snap_timer.start(AUTO_SNAP_INTERVAL_MS)
            self.statusBar().showMessage("Auto-snap ON")
        else:
            self.snap_timer.stop()
            self.statusBar().showMessage("Auto-snap OFF")

    def _save_current(self):
        if self.camera_view._pixmap is None:
            QMessageBox.information(self, "Lưu ảnh", "Chưa có ảnh để lưu.")
            return
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        default = str(CAPTURES_DIR / f"manual_{ts}.png")
        path, _ = QFileDialog.getSaveFileName(self, "Lưu ảnh", default,
                                              "PNG (*.png);;JPEG (*.jpg)")
        if path:
            self.camera_view._pixmap.save(path)
            self.statusBar().showMessage(f"Đã lưu: {path}", 3000)

    def _open_captures(self):
        import subprocess
        subprocess.Popen(["open", str(CAPTURES_DIR)])

    def _update_stats(self):
        up = int(time.time() - self.stats["start_time"])
        h, m, s = up // 3600, (up % 3600) // 60, up % 60
        self.lbl_uptime.setText(f"⏱ {h:02d}:{m:02d}:{s:02d}")
        # FPS: bitmaps per second over last period (approx)
        if up > 0:
            fps = self.stats["bitmaps"] / up
            self.lbl_fps.setText(f"{fps:.1f} bmp/s | {self.stats['frames']} imgs")
        bm = self.stats["last_bitmap"]
        n = self.stats["n_occupied"]; meth = self.stats["method"]
        if bm is not None:
            self.lbl_method.setText(f"{meth} • {n}/8 • 0x{bm:02X}")

    def closeEvent(self, e):
        if self.worker:
            self.worker.stop()
            self.worker.wait(2000)
        e.accept()


# ═══════════════════════════════════════════════════════════════════════
# Entry point
# ═══════════════════════════════════════════════════════════════════════

def find_esp32_port():
    """Auto-detect ESP32-CAM USB serial port."""
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if any(s in p.device.lower() for s in ["usbserial", "wchusbserial", "slab_usbto"]):
            return p.device
    return None


def main():
    ap = argparse.ArgumentParser(description=APP_NAME)
    ap.add_argument("--port", "-p", help="Serial port (auto-detect if omitted)")
    ap.add_argument("--baud", "-b", type=int, default=DEFAULT_BAUD)
    args = ap.parse_args()

    port = args.port or find_esp32_port()
    if not port:
        print("❌ Không tìm thấy ESP32-CAM serial port.")
        print("   Dùng: python3 parking_monitor.py --port /dev/cu.usbserial-XXX")
        print("   Hoặc cắm USB và thử lại.")
        sys.exit(1)

    print(f"→ Port: {port} @ {args.baud}")
    print(f"→ Captures: {CAPTURES_DIR}")

    app = QApplication(sys.argv)
    app.setApplicationName(APP_NAME)
    w = MainWindow(port)
    w.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
