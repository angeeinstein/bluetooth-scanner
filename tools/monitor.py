#!/usr/bin/env python3
"""Reconnecting serial monitor for the ESP32-S3 (USB-Serial-JTAG console).

PlatformIO's built-in monitor (pyserial's miniterm) drops -- and stays dead --
whenever the chip resets, because this board's console is the S3's *native*
USB-Serial-JTAG peripheral (see sdkconfig.defaults: no UART bridge). That USB
device re-enumerates on every reboot / crash / OTA update, so the port briefly
disappears and the monitor never reconnects.

This is a tiny raw monitor that just reopens the same port when it comes back,
like the Arduino IDE's monitor: you keep seeing boot logs and error output
across resets without touching anything.

    python tools/monitor.py            # auto-detect the ESP32-S3 port
    python tools/monitor.py COM5       # or name the port explicitly

Run it with a Python that has pyserial -- PlatformIO's always does:
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe tools/monitor.py
Ctrl+C to quit. (Raw stream, no exception decoding -- for decoded crash
backtraces, PlatformIO's own monitor or esp-idf-monitor is still the tool.)
"""
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial not found -- run with PlatformIO's python, e.g.:\n"
             r"  %USERPROFILE%\.platformio\penv\Scripts\python.exe tools/monitor.py")

BAUD = 115200
ESP_VID = 0x303A  # Espressif -- the S3's native USB-Serial-JTAG / USB-CDC


def find_port():
    for p in list_ports.comports():
        if p.vid == ESP_VID:
            return p.device
    return None


def stream(port):
    ser = serial.Serial(port, BAUD, timeout=0.2)
    print("[monitor] connected to %s @ %d" % (port, BAUD), flush=True)
    try:
        while True:
            data = ser.read(4096)
            if data:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
    finally:
        try:
            ser.close()
        except Exception:
            pass


def main():
    want = sys.argv[1] if len(sys.argv) > 1 else None
    print("Reconnecting monitor -- Ctrl+C to quit.", flush=True)
    waiting = False
    while True:
        port = want or find_port()
        if port is None:
            if not waiting:
                print("[monitor] waiting for the ESP32-S3 to appear...", flush=True)
                waiting = True
            time.sleep(0.5)
            continue
        waiting = False
        try:
            stream(port)
        except (serial.SerialException, OSError):
            # Port vanished (reset/re-enumerate) or wasn't ready yet -- just retry.
            print("\n[monitor] %s went away (reset?) -- reconnecting..." % port, flush=True)
        time.sleep(0.3)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[monitor] bye", flush=True)
