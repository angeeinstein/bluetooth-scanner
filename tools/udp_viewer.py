#!/usr/bin/env python3
"""Live viewer for the BTScanner recorder's UDP accelerometer stream.

Receives the full-rate (26.7kHz, undecimated) packet stream described in
README.md ("UDP stream packet format"), and shows it as a scrolling
time-domain plot plus a live FFT with an adjustable frequency band.

Usage:
    python tools/udp_viewer.py --port 9000

Then start streaming to this machine's IP from the dashboard's UDP card.

Requires: numpy, pyqtgraph, and a Qt binding (PyQt5, PySide6, PyQt6 or
PySide2 -- pyqtgraph will use whichever is installed). Missing packages are
detected and installed automatically on startup (see _bootstrap_dependencies
below); if that fails for some reason, install manually with:
    python -m pip install numpy pyqtgraph PyQt5
"""

import importlib
import os
import subprocess
import sys

# --- Dependency bootstrap ---------------------------------------------------
# Runs before numpy/pyqtgraph are imported, so it works even on a completely
# fresh Python install. Always targets `sys.executable -m pip`, not whatever
# `python`/`pip` happen to resolve to on PATH -- that's what makes this
# robust across different Python installs/virtualenvs/versions: pip always
# ends up installing into the exact interpreter that's about to run this
# script, never a different one.

QT_BINDING_CANDIDATES = ("PyQt5", "PySide6", "PyQt6", "PySide2")
QT_BINDING_TO_INSTALL = "PyQt5"  # only used if none of the above are already present
_BOOTSTRAP_GUARD_ENV = "_BTSCANNER_UDP_VIEWER_BOOTSTRAPPED"


def _importable(module_name):
    try:
        importlib.import_module(module_name)
        return True
    except ImportError:
        return False


def _ensure_pip():
    """Returns True once `sys.executable -m pip` works, bootstrapping pip via
    the standard library's ensurepip module if it isn't available yet (e.g.
    some minimal/embeddable Python installs don't ship pip by default)."""
    check = subprocess.run([sys.executable, "-m", "pip", "--version"], capture_output=True)
    if check.returncode == 0:
        return True
    print("pip isn't available for this Python (%s) -- bootstrapping it via ensurepip..." %
          sys.executable)
    result = subprocess.run([sys.executable, "-m", "ensurepip", "--upgrade"])
    return result.returncode == 0


def _pip_install(packages):
    print("Installing missing package(s): " + " ".join(packages))
    cmd = [sys.executable, "-m", "pip", "install", "--upgrade"] + packages
    if subprocess.run(cmd).returncode == 0:
        return True
    # A common failure mode is not having write access to the interpreter's
    # site-packages (e.g. a system-wide Python without admin rights) --
    # retry into the per-user site-packages instead of just giving up.
    print("Install failed, retrying with --user...")
    cmd_user = [sys.executable, "-m", "pip", "install", "--upgrade", "--user"] + packages
    return subprocess.run(cmd_user).returncode == 0


def _bootstrap_dependencies():
    if os.environ.get(_BOOTSTRAP_GUARD_ENV) == "1":
        return  # already attempted once in this process tree -- don't loop forever

    missing = []
    if not _importable("numpy"):
        missing.append("numpy")
    if not _importable("pyqtgraph"):
        missing.append("pyqtgraph")
    if not any(_importable(binding) for binding in QT_BINDING_CANDIDATES):
        missing.append(QT_BINDING_TO_INSTALL)

    if not missing:
        return

    print("udp_viewer.py: missing required package(s): " + ", ".join(missing))
    if not _ensure_pip():
        print("Could not find or bootstrap pip for %s. Install pip manually, then run:\n"
              "    %s -m pip install %s" % (sys.executable, sys.executable, " ".join(missing)))
        sys.exit(1)

    if not _pip_install(missing):
        print("Automatic install failed. Install manually with:\n"
              "    %s -m pip install %s" % (sys.executable, " ".join(missing)))
        sys.exit(1)

    print("Install succeeded -- restarting...")
    os.environ[_BOOTSTRAP_GUARD_ENV] = "1"
    os.execv(sys.executable, [sys.executable] + sys.argv)


_bootstrap_dependencies()

# --- Everything below only imports cleanly once the bootstrap above has
# ensured these are installed. ------------------------------------------

import argparse
import csv
import math
import socket
import struct
import threading
import time

import numpy as np
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore, QtGui, QtWidgets

# --- Wire format (must match src/udp_stream.c) ------------------------------

HEADER = struct.Struct("<BBHIQ")  # magic, flags, sample_count, seq, sample_index
MAGIC = 0xAC
SAMPLE_RATE_HZ = 26700.0  # IIS3DWB's only supported ODR -- fixed, not configurable
FS_TO_MG_PER_LSB = {0: 0.061, 1: 0.488, 2: 0.122, 3: 0.244}  # iis3dwb_sensitivity_mg_per_lsb()
FS_TO_G = {0: 2, 1: 16, 2: 4, 3: 8}

AXIS_COLORS = ("#f85149", "#3fb950", "#58a6ff")  # same X/Y/Z palette as the dashboard
AXIS_NAMES = ("X", "Y", "Z")


# --- Ring buffer -------------------------------------------------------------


class RingBuffer:
    """Fixed-capacity circular buffer of (N, 3) float32 samples, thread-safe.

    Two independent things read from this (the time-domain plot and the FFT
    window) at different, changing window sizes, so a simple "latest N"
    accessor is more useful here than a consuming read.
    """

    def __init__(self, capacity):
        self.capacity = capacity
        self.data = np.zeros((capacity, 3), dtype=np.float32)
        self.write_pos = 0
        self.count = 0
        self.lock = threading.Lock()

    def append(self, samples):
        with self.lock:
            n = len(samples)
            if n == 0:
                return
            if n >= self.capacity:
                self.data[:] = samples[-self.capacity:]
                self.write_pos = 0
                self.count = self.capacity
                return
            end = self.write_pos + n
            if end <= self.capacity:
                self.data[self.write_pos:end] = samples
            else:
                first = self.capacity - self.write_pos
                self.data[self.write_pos:] = samples[:first]
                self.data[: end - self.capacity] = samples[first:]
            self.write_pos = end % self.capacity
            self.count = min(self.capacity, self.count + n)

    def get_latest(self, n):
        with self.lock:
            n = min(n, self.count)
            if n == 0:
                return np.zeros((0, 3), dtype=np.float32)
            idx = (self.write_pos - n) % self.capacity
            if idx + n <= self.capacity:
                return self.data[idx : idx + n].copy()
            first = self.capacity - idx
            return np.concatenate([self.data[idx:], self.data[: n - first]])


# --- UDP receiver thread ------------------------------------------------------


class Stats:
    """Counters updated by the receiver thread, read by the GUI timer."""

    def __init__(self):
        self.lock = threading.Lock()
        self.packets = 0
        self.bytes = 0
        self.drops = 0
        self.malformed = 0
        self.last_packet_time = 0.0
        self.fs_code = 0
        self.lpf2 = False


class UdpReceiver(threading.Thread):
    def __init__(self, host, port, ring, stats):
        super().__init__(daemon=True)
        self.ring = ring
        self.stats = stats
        self.stop_event = threading.Event()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))
        self.sock.settimeout(0.5)
        self.expected_sample_index = None

    def run(self):
        while not self.stop_event.is_set():
            try:
                data, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            self._handle_packet(data)
        self.sock.close()

    def _handle_packet(self, data):
        if len(data) < HEADER.size:
            with self.stats.lock:
                self.stats.malformed += 1
            return
        magic, flags, sample_count, seq, sample_index = HEADER.unpack_from(data)
        if magic != MAGIC or len(data) < HEADER.size + sample_count * 6:
            with self.stats.lock:
                self.stats.malformed += 1
            return

        with self.stats.lock:
            # sample_index directly reports the sample count, unlike seq
            # (which would need multiplying by sample_count to mean the
            # same thing) -- diff that for gap detection.
            if self.expected_sample_index is not None:
                if sample_index > self.expected_sample_index:
                    self.stats.drops += sample_index - self.expected_sample_index
                elif sample_index < self.expected_sample_index:
                    pass  # sender restarted (index resets on /api/udp/start) -- not a loss
            self.expected_sample_index = sample_index + sample_count
            self.stats.packets += 1
            self.stats.bytes += len(data)
            self.stats.last_packet_time = time.time()
            self.stats.fs_code = flags & 0x03
            self.stats.lpf2 = bool(flags & 0x04)

        sens_g = FS_TO_MG_PER_LSB[flags & 0x03] / 1000.0
        raw = np.frombuffer(data, dtype="<i2", count=sample_count * 3, offset=HEADER.size)
        self.ring.append(raw.reshape(-1, 3).astype(np.float32) * sens_g)

    def stop(self):
        self.stop_event.set()


# --- Small reusable widgets ---------------------------------------------------


def make_axis_checkboxes(on_changed):
    boxes = []
    row = QtWidgets.QHBoxLayout()
    for name, color in zip(AXIS_NAMES, AXIS_COLORS):
        cb = QtWidgets.QCheckBox(name)
        cb.setChecked(True)
        cb.setStyleSheet(f"QCheckBox {{ color: {color}; font-weight: bold; }}")
        cb.stateChanged.connect(on_changed)
        row.addWidget(cb)
        boxes.append(cb)
    return row, boxes


# --- Time-domain tab -----------------------------------------------------------


class TimeDomainTab(QtWidgets.QWidget):
    def __init__(self, ring):
        super().__init__()
        self.ring = ring

        layout = QtWidgets.QVBoxLayout(self)

        controls = QtWidgets.QHBoxLayout()
        controls.addWidget(QtWidgets.QLabel("Window (s):"))
        self.window_spin = QtWidgets.QDoubleSpinBox()
        self.window_spin.setRange(0.05, 30.0)
        self.window_spin.setSingleStep(0.5)
        self.window_spin.setValue(2.0)
        controls.addWidget(self.window_spin)

        axis_row, self.axis_boxes = make_axis_checkboxes(lambda _: None)
        controls.addLayout(axis_row)

        self.autoscale_box = QtWidgets.QCheckBox("Autoscale Y")
        self.autoscale_box.setChecked(True)
        controls.addWidget(self.autoscale_box)
        controls.addStretch(1)
        layout.addLayout(controls)

        self.plot = pg.PlotWidget()
        self.plot.setLabel("bottom", "Time", units="s")
        self.plot.setLabel("left", "Acceleration", units="g")
        self.plot.showGrid(x=True, y=True, alpha=0.2)
        self.plot.addLegend(offset=(10, 10))
        layout.addWidget(self.plot)

        self.curves = []
        for name, color in zip(AXIS_NAMES, AXIS_COLORS):
            curve = self.plot.plot(pen=pg.mkPen(color, width=1), name=name)
            # 'peak' downsampling preserves spikes instead of averaging them
            # away -- important for vibration data, unlike the default mean.
            curve.setDownsampling(auto=True, method="peak")
            curve.setClipToView(True)
            self.curves.append(curve)

        self.value_labels = []
        values_row = QtWidgets.QHBoxLayout()
        for name, color in zip(AXIS_NAMES, AXIS_COLORS):
            lbl = QtWidgets.QLabel(f"{name}: 0.000 g")
            lbl.setStyleSheet(f"color: {color}; font-weight: bold; font-size: 13px;")
            values_row.addWidget(lbl)
            self.value_labels.append(lbl)
        values_row.addStretch(1)
        layout.addLayout(values_row)

    def refresh(self):
        window_s = self.window_spin.value()
        n = max(2, int(window_s * SAMPLE_RATE_HZ))
        samples = self.ring.get_latest(n)
        if len(samples) < 2:
            return
        t = (np.arange(len(samples)) - len(samples)) / SAMPLE_RATE_HZ
        for i, (curve, box, lbl) in enumerate(zip(self.curves, self.axis_boxes, self.value_labels)):
            visible = box.isChecked()
            curve.setVisible(visible)
            if visible:
                curve.setData(t, samples[:, i])
            lbl.setText(f"{AXIS_NAMES[i]}: {samples[-1, i]:+.4f} g")
        if not self.autoscale_box.isChecked():
            self.plot.setYRange(-2, 2)
        else:
            self.plot.enableAutoRange(axis="y")


# --- FFT tab ---------------------------------------------------------------

WINDOW_FUNCS = {
    "Hann": np.hanning,
    "Hamming": np.hamming,
    "Blackman": np.blackman,
    "Rectangular": lambda n: np.ones(n),
}


class FftTab(QtWidgets.QWidget):
    def __init__(self, ring):
        super().__init__()
        self.ring = ring
        self.peak_hold = None
        self._syncing_region = False

        layout = QtWidgets.QVBoxLayout(self)

        row1 = QtWidgets.QHBoxLayout()
        row1.addWidget(QtWidgets.QLabel("FFT size:"))
        self.size_combo = QtWidgets.QComboBox()
        self.size_combo.addItems(["1024", "2048", "4096", "8192", "16384", "32768"])
        self.size_combo.setCurrentText("8192")
        row1.addWidget(self.size_combo)

        row1.addWidget(QtWidgets.QLabel("Window:"))
        self.window_combo = QtWidgets.QComboBox()
        self.window_combo.addItems(list(WINDOW_FUNCS.keys()))
        row1.addWidget(self.window_combo)

        self.db_box = QtWidgets.QCheckBox("dB scale")
        self.db_box.setChecked(True)
        row1.addWidget(self.db_box)

        self.log_x_box = QtWidgets.QCheckBox("Log frequency axis")
        row1.addWidget(self.log_x_box)

        self.peak_hold_box = QtWidgets.QCheckBox("Peak hold")
        row1.addWidget(self.peak_hold_box)
        reset_peak_btn = QtWidgets.QPushButton("Reset peak hold")
        reset_peak_btn.clicked.connect(self._reset_peak_hold)
        row1.addWidget(reset_peak_btn)
        row1.addStretch(1)
        layout.addLayout(row1)

        row2 = QtWidgets.QHBoxLayout()
        axis_row, self.axis_boxes = make_axis_checkboxes(lambda _: None)
        row2.addLayout(axis_row)

        row2.addWidget(QtWidgets.QLabel("Stats axis:"))
        self.stats_combo = QtWidgets.QComboBox()
        self.stats_combo.addItems(["X", "Y", "Z", "Combined magnitude"])
        row2.addWidget(self.stats_combo)

        row2.addWidget(QtWidgets.QLabel("Band (Hz):"))
        self.lo_spin = QtWidgets.QDoubleSpinBox()
        self.lo_spin.setRange(0, SAMPLE_RATE_HZ / 2)
        self.lo_spin.setValue(10)
        self.lo_spin.setSuffix(" Hz")
        row2.addWidget(self.lo_spin)
        row2.addWidget(QtWidgets.QLabel("to"))
        self.hi_spin = QtWidgets.QDoubleSpinBox()
        self.hi_spin.setRange(0, SAMPLE_RATE_HZ / 2)
        self.hi_spin.setValue(1000)
        self.hi_spin.setSuffix(" Hz")
        row2.addWidget(self.hi_spin)
        reset_zoom_btn = QtWidgets.QPushButton("Full spectrum")
        reset_zoom_btn.setToolTip("Zoom out to the full 0 Hz - Nyquist view without changing the band")
        reset_zoom_btn.clicked.connect(self._reset_zoom)
        row2.addWidget(reset_zoom_btn)
        row2.addStretch(1)
        layout.addLayout(row2)

        self.plot = pg.PlotWidget()
        self.plot.setLabel("bottom", "Frequency", units="Hz")
        self.plot.setLabel("left", "Magnitude")
        self.plot.showGrid(x=True, y=True, alpha=0.2)
        self.plot.addLegend(offset=(10, 10))
        layout.addWidget(self.plot)

        self.curves = []
        for name, color in zip(AXIS_NAMES, AXIS_COLORS):
            curve = self.plot.plot(pen=pg.mkPen(color, width=1), name=name)
            self.curves.append(curve)
        self.peak_curve = self.plot.plot(pen=pg.mkPen("#d2a8ff", width=1, style=QtCore.Qt.DashLine),
                                          name="peak hold")

        self.region = pg.LinearRegionItem(values=(self.lo_spin.value(), self.hi_spin.value()),
                                           brush=pg.mkBrush(255, 255, 255, 20))
        self.plot.addItem(self.region)
        self.region.sigRegionChanged.connect(self._region_to_spinboxes)
        self.lo_spin.valueChanged.connect(self._spinboxes_to_region)
        self.hi_spin.valueChanged.connect(self._spinboxes_to_region)
        self.log_x_box.stateChanged.connect(self._on_log_toggle)

        stats_row = QtWidgets.QHBoxLayout()
        self.peak_freq_label = QtWidgets.QLabel("Peak: -")
        self.rms_label = QtWidgets.QLabel("RMS in band: -")
        for lbl in (self.peak_freq_label, self.rms_label):
            lbl.setStyleSheet("font-weight: bold; font-size: 13px;")
            stats_row.addWidget(lbl)
        stats_row.addStretch(1)
        layout.addLayout(stats_row)

    def _region_to_spinboxes(self):
        if self._syncing_region:
            return
        self._syncing_region = True
        lo, hi = self.region.getRegion()
        self.lo_spin.setValue(lo)
        self.hi_spin.setValue(hi)
        self._syncing_region = False
        self._apply_zoom(lo, hi)

    def _spinboxes_to_region(self):
        if self._syncing_region:
            return
        self._syncing_region = True
        lo, hi = self.lo_spin.value(), self.hi_spin.value()
        self.region.setRegion((lo, hi))
        self._syncing_region = False
        self._apply_zoom(lo, hi)

    def _apply_zoom(self, lo, hi):
        # The band control doubles as the plot's zoom -- the whole point of
        # setting bounds is to look closely at that range, not just see it
        # highlighted inside the full 0-Nyquist view.
        if hi <= lo:
            return
        if self.log_x_box.isChecked():
            # In log-x mode pyqtgraph's ViewBox works in log10-transformed
            # coordinates, so the range passed here must be too. Clamp lo
            # away from 0 -- log10(0) is undefined and 0 Hz isn't a
            # meaningful lower bound on a log axis anyway.
            lo = max(lo, 1.0)
            self.plot.setXRange(math.log10(lo), math.log10(hi), padding=0.02)
        else:
            self.plot.setXRange(lo, hi, padding=0.02)

    def _on_log_toggle(self):
        self.plot.setLogMode(x=self.log_x_box.isChecked(), y=False)
        self._apply_zoom(*self.region.getRegion())

    def _reset_zoom(self):
        # Zoom out to the full spectrum without touching the band values
        # (which still drive the stats and stay marked on the plot).
        if self.log_x_box.isChecked():
            self.plot.setXRange(math.log10(1.0), math.log10(SAMPLE_RATE_HZ / 2), padding=0.02)
        else:
            self.plot.setXRange(0, SAMPLE_RATE_HZ / 2, padding=0.02)

    def _reset_peak_hold(self):
        self.peak_hold = None

    def refresh(self):
        n = int(self.size_combo.currentText())
        samples = self.ring.get_latest(n)
        if len(samples) < n:
            return  # not enough buffered yet for a full window at this size

        win = WINDOW_FUNCS[self.window_combo.currentText()](n)
        win_gain = win.sum()  # coherent gain compensation so magnitude is comparable across windows
        freqs = np.fft.rfftfreq(n, d=1.0 / SAMPLE_RATE_HZ)[1:]  # drop DC bin (also avoids log(0))

        mags = np.zeros((3, len(freqs)))
        for i in range(3):
            spec = np.fft.rfft(samples[:, i] * win)[1:]
            mags[i] = np.abs(spec) / (win_gain / 2)  # single-sided amplitude in g

        combined = None
        if self.stats_combo.currentText() == "Combined magnitude":
            mag_signal = np.sqrt(np.sum(samples.astype(np.float64) ** 2, axis=1))
            combined = np.abs(np.fft.rfft(mag_signal * win)[1:]) / (win_gain / 2)

        use_db = self.db_box.isChecked()

        def to_display(m):
            return 20 * np.log10(np.maximum(m, 1e-9)) if use_db else m

        if self.peak_hold_box.isChecked():
            stack = mags if combined is None else np.vstack([mags, combined])
            if self.peak_hold is None or self.peak_hold.shape != stack.shape:
                self.peak_hold = stack.copy()
            else:
                np.maximum(self.peak_hold, stack, out=self.peak_hold)
        else:
            self.peak_hold = None

        for i, (curve, box) in enumerate(zip(self.curves, self.axis_boxes)):
            visible = box.isChecked()
            curve.setVisible(visible)
            if visible:
                curve.setData(freqs, to_display(mags[i]))

        if self.peak_hold is not None and self.stats_combo.currentText() != "Combined magnitude":
            idx = AXIS_NAMES.index(self.stats_combo.currentText())
            self.peak_curve.setVisible(True)
            self.peak_curve.setData(freqs, to_display(self.peak_hold[idx]))
        else:
            self.peak_curve.setVisible(False)

        self.plot.setLogMode(x=self.log_x_box.isChecked(), y=False)

        # Stats over the selected band, from whichever spectrum the user picked.
        choice = self.stats_combo.currentText()
        if choice == "Combined magnitude":
            spectrum = combined
        else:
            spectrum = mags[AXIS_NAMES.index(choice)]
        lo, hi = sorted(self.region.getRegion())
        band = (freqs >= lo) & (freqs <= hi)
        if np.any(band):
            band_mags = spectrum[band]
            band_freqs = freqs[band]
            peak_i = np.argmax(band_mags)
            self.peak_freq_label.setText(
                f"Peak in band: {band_freqs[peak_i]:.1f} Hz @ {band_mags[peak_i]:.4g} g")
            rms = np.sqrt(np.mean(band_mags ** 2))
            self.rms_label.setText(f"RMS in band: {rms:.4g} g")
        else:
            self.peak_freq_label.setText("Peak in band: -")
            self.rms_label.setText("RMS in band: -")


# --- Main window ---------------------------------------------------------------


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self, host, port, buffer_seconds):
        super().__init__()
        self.setWindowTitle(f"BTScanner UDP viewer -- listening on {host}:{port}")
        self.resize(1200, 800)

        capacity = int(buffer_seconds * SAMPLE_RATE_HZ)
        self.ring = RingBuffer(capacity)
        self.stats = Stats()
        self.receiver = UdpReceiver(host, port, self.ring, self.stats)
        self.receiver.start()

        self.paused = False
        self._last_stat_snapshot = (0, 0.0)  # (packets, time) for rate calc

        central = QtWidgets.QWidget()
        self.setCentralWidget(central)
        outer = QtWidgets.QVBoxLayout(central)

        outer.addLayout(self._build_status_bar())

        self.tabs = QtWidgets.QTabWidget()
        self.time_tab = TimeDomainTab(self.ring)
        self.fft_tab = FftTab(self.ring)
        self.tabs.addTab(self.time_tab, "Time Domain")
        self.tabs.addTab(self.fft_tab, "FFT")
        outer.addWidget(self.tabs)

        self.timer = QtCore.QTimer()
        self.timer.timeout.connect(self._tick)
        self.timer.start(33)  # ~30Hz UI refresh

    def _build_status_bar(self):
        row = QtWidgets.QHBoxLayout()
        self.status_dot = QtWidgets.QLabel("●")
        self.status_dot.setStyleSheet("color: #888; font-size: 16px;")
        row.addWidget(self.status_dot)
        self.status_label = QtWidgets.QLabel("waiting for data...")
        row.addWidget(self.status_label)

        self.rate_label = QtWidgets.QLabel("")
        row.addWidget(self.rate_label)
        self.drops_label = QtWidgets.QLabel("")
        row.addWidget(self.drops_label)
        self.fs_label = QtWidgets.QLabel("")
        row.addWidget(self.fs_label)
        row.addStretch(1)

        self.pause_btn = QtWidgets.QPushButton("Pause")
        self.pause_btn.setCheckable(True)
        self.pause_btn.toggled.connect(self._toggle_pause)
        row.addWidget(self.pause_btn)

        save_btn = QtWidgets.QPushButton("Save buffer to CSV")
        save_btn.clicked.connect(self._save_csv)
        row.addWidget(save_btn)
        return row

    def _toggle_pause(self, checked):
        self.paused = checked
        self.pause_btn.setText("Resume" if checked else "Pause")

    def _save_csv(self):
        path, _ = QtWidgets.QFileDialog.getSaveFileName(self, "Save buffer to CSV", "capture.csv",
                                                          "CSV files (*.csv)")
        if not path:
            return
        samples = self.ring.get_latest(self.ring.capacity)
        with open(path, "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["sample_index", "time_s", "x_g", "y_g", "z_g"])  # matches the firmware's own CSV export
            for i, row in enumerate(samples):
                writer.writerow([i, i / SAMPLE_RATE_HZ, f"{row[0]:.4f}", f"{row[1]:.4f}", f"{row[2]:.4f}"])
        self.status_label.setText(f"saved {len(samples)} samples to {path}")

    def _tick(self):
        with self.stats.lock:
            packets = self.stats.packets
            drops = self.stats.drops
            malformed = self.stats.malformed
            last_time = self.stats.last_packet_time
            fs_code = self.stats.fs_code
            lpf2 = self.stats.lpf2

        now = time.time()
        have_data = last_time > 0 and (now - last_time) < 2.0
        self.status_dot.setStyleSheet(f"color: {'#3fb950' if have_data else '#888'}; font-size: 16px;")
        self.status_label.setText("receiving" if have_data else "no data")

        prev_packets, prev_time = self._last_stat_snapshot
        dt = now - prev_time
        if dt >= 0.5:
            pps = (packets - prev_packets) / dt if prev_time > 0 else 0
            self.rate_label.setText(f"| {pps:.0f} pkt/s (~{pps * 242:.0f} samples/s)")
            self._last_stat_snapshot = (packets, now)
        self.drops_label.setText(f"| drops: {drops}" + (f", malformed: {malformed}" if malformed else ""))
        self.fs_label.setText(f"| ±{FS_TO_G.get(fs_code, '?')}g{' LPF2' if lpf2 else ''}")

        if self.paused:
            return
        if self.tabs.currentWidget() is self.time_tab:
            self.time_tab.refresh()
        else:
            self.fft_tab.refresh()

    def closeEvent(self, event):
        self.receiver.stop()
        self.receiver.join(timeout=1.0)
        super().closeEvent(event)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="0.0.0.0", help="address to bind (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=9000, help="UDP port to listen on (default: 9000)")
    parser.add_argument("--buffer-seconds", type=float, default=15.0,
                         help="how much history to keep in RAM for the time-domain/FFT windows")
    args = parser.parse_args()

    app = QtWidgets.QApplication(sys.argv)
    pg.setConfigOptions(antialias=False, background="#0d1420", foreground="#c9d1d9")
    win = MainWindow(args.host, args.port, args.buffer_seconds)
    win.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
