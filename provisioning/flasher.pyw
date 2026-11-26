"""VibraShock Flasher -- first-time USB flash of a fresh board, with a small GUI.

Windows: double-click this file (or "Start Flasher.bat" next to it, which also
shows any startup error). Elsewhere: run `python3 flasher.pyw`.
Pick the board's port, click Flash. It writes the *-full-flash.bin next to this
file at offset 0x0; after that the board updates itself over the air.

Needs Python 3 with Tkinter (included in the python.org installer) and esptool,
which it offers to install on first start. This resets the board's saved
settings -- it's for new boards, not for updating a running one.
"""

import glob
import os
import queue
import re
import subprocess
import sys
import threading
import traceback
import webbrowser
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

HERE = os.path.dirname(os.path.abspath(__file__))
# USB vendor IDs of the ESP32-S3's native USB and common USB-UART bridges.
ESP_VIDS = {0x303A: "Espressif", 0x10C4: "CP210x", 0x1A86: "CH34x", 0x0403: "FTDI"}
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)  # keep child consoles hidden under pythonw
SETUP_URL = "http://192.168.4.1"
AP_PASSWORD = "recorder123"  # the board's own setup Wi-Fi (AP_PASSWORD in wifi_manager.c)

# esptool prints the chip's factory MAC while connecting; the board's Wi-Fi name
# is "VibraShock-" + its last three bytes (device_id.c / wifi_manager.c).
MAC_RE = re.compile(r"MAC:\s*([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})")
# "Writing at 0x... (37 %)" (esptool 4) and "Writing at 0x... [===> ] 98.3%" (esptool 5).
PROGRESS_RE = re.compile(r"Writing at .*?(\d+(?:\.\d+)?)\s*%")

OK_BG, OK_FG = "#e7f5ea", "#1b6b32"
ERR_BG, ERR_FG = "#fdecec", "#a4262c"


def find_image():
    imgs = glob.glob(os.path.join(HERE, "*-full-flash.bin"))
    return max(imgs, key=os.path.getmtime) if imgs else ""


def esptool_major():
    """Installed esptool's major version, or None if it isn't installed."""
    r = subprocess.run([sys.executable, "-m", "esptool", "version"],
                       capture_output=True, text=True, creationflags=NO_WINDOW)
    if r.returncode != 0:
        return None
    m = re.search(r"(\d+)\.\d+", r.stdout + r.stderr)
    return int(m.group(1)) if m else 4


def wrap_to_width(label, pad=4):
    """Wrap a label's text to the width its grid cell actually gets (DPI-safe),
    instead of letting a long line widen the whole window."""
    label.configure(wraplength=300)  # small initial request; widened to the real width below
    label.bind("<Configure>", lambda e: label.configure(wraplength=max(e.width - pad, 100)))
    return label


def list_ports():
    """[(device, label, is_esp)] -- ESP-looking ports first."""
    try:
        from serial.tools import list_ports as lp
    except ImportError:
        return []
    out = []
    for p in lp.comports():
        chip = ESP_VIDS.get(p.vid)
        desc = p.description if p.description and p.description != "n/a" else "serial port"
        desc = desc.replace("(%s)" % p.device, "").strip()  # Windows repeats "(COM5)" in the description
        label = "%s  -  %s%s" % (p.device, desc, "  (%s)" % chip if chip else "")
        out.append((p.device, label, chip is not None))
    out.sort(key=lambda t: (not t[2], t[0]))
    return out


class App:
    def __init__(self, root):
        self.root = root
        self.q = queue.Queue()
        self.busy = False
        self.ports = []
        self.mac = None

        root.title("VibraShock Flasher")
        root.minsize(620, 640)
        f = ttk.Frame(root, padding=14)
        f.pack(fill="both", expand=True)
        f.columnconfigure(1, weight=1)
        self.frame = f

        ttk.Label(f, text="VibraShock Flasher", font=("Segoe UI", 14, "bold")).grid(
            row=0, column=0, columnspan=3, sticky="w")
        ttk.Label(f, text="First-time USB setup of a new board. Later updates run over the air.",
                  foreground="#555").grid(row=1, column=0, columnspan=3, sticky="w", pady=(0, 12))

        ttk.Label(f, text="Firmware").grid(row=2, column=0, sticky="w", padx=(0, 10))
        self.image = tk.StringVar(value=find_image())
        ttk.Entry(f, textvariable=self.image).grid(row=2, column=1, sticky="ew")
        ttk.Button(f, text="Browse...", command=self.browse).grid(row=2, column=2, padx=(6, 0))

        ttk.Label(f, text="Port").grid(row=3, column=0, sticky="w", padx=(0, 10), pady=(8, 0))
        self.port = ttk.Combobox(f, state="readonly")
        self.port.grid(row=3, column=1, sticky="ew", pady=(8, 0))
        self.port.bind("<<ComboboxSelected>>", lambda e: self.update_port_hint())
        ttk.Button(f, text="Refresh", command=self.refresh).grid(row=3, column=2, padx=(6, 0), pady=(8, 0))
        self.port_hint = wrap_to_width(ttk.Label(f, text="", foreground="#666"))
        self.port_hint.grid(row=4, column=1, columnspan=2, sticky="ew", pady=(2, 6))

        self.erase = tk.BooleanVar(value=False)
        ttk.Checkbutton(f, text="Erase the entire chip first (slower; only needed if a board misbehaves)",
                        variable=self.erase).grid(row=5, column=0, columnspan=3, sticky="w")

        self.flash_btn = ttk.Button(f, text="Flash", command=self.flash)
        self.flash_btn.grid(row=6, column=0, columnspan=3, sticky="ew", pady=(12, 6), ipady=4)
        self.progress = ttk.Progressbar(f, maximum=100)
        self.progress.grid(row=7, column=0, columnspan=3, sticky="ew")
        self.status = ttk.Label(f, text="")
        self.status.grid(row=8, column=0, columnspan=3, sticky="w", pady=(4, 6))

        self.result = None  # the success / failure panel (row 9), rebuilt per outcome

        ttk.Label(f, text="Log", foreground="#666").grid(row=10, column=0, columnspan=3, sticky="w")
        self.log = tk.Text(f, height=7, wrap="word", font=("Consolas", 9), state="disabled")
        self.log.grid(row=11, column=0, columnspan=3, sticky="nsew")
        f.rowconfigure(11, weight=1)

        self.refresh()
        root.after(100, self.pump)
        root.after(2000, self.watch_ports)
        if esptool_major() is None:
            root.after(300, self.offer_install)

    # --- helpers ------------------------------------------------------------
    def say(self, text):
        self.log.configure(state="normal")
        self.log.insert("end", text)
        self.log.see("end")
        self.log.configure(state="disabled")

    def set_status(self, text):
        self.status.configure(text=text)

    def browse(self):
        p = filedialog.askopenfilename(initialdir=HERE, title="Select the full-flash image",
                                       filetypes=[("Firmware image", "*.bin"), ("All files", "*.*")])
        if p:
            self.image.set(p)

    def selected_port(self):
        i = self.port.current()
        return self.ports[i] if 0 <= i < len(self.ports) else None

    def update_port_hint(self):
        sel = self.selected_port()
        if not self.ports:
            text = "No serial ports found -- plug the board in via USB (the list updates by itself)."
        elif sel and sel[2]:
            text = "Board recognized."
        else:
            text = ("Not recognized as an ESP board. If yours doesn't show up marked "
                    "(Espressif), try another USB port or cable.")
        self.port_hint.configure(text=text)

    def refresh(self, prefer=None):
        """Re-list ports, keeping the selection if it's still there (or picking `prefer`)."""
        keep = prefer or (self.selected_port() or (None,))[0]
        self.ports = list_ports()
        self.port["values"] = [label for _, label, _ in self.ports]
        devices = [d for d, _, _ in self.ports]
        if keep in devices:
            self.port.current(devices.index(keep))
        elif self.ports:
            self.port.current(0)  # ESP ports sort first
        else:
            self.port.set("")
        self.update_port_hint()

    def watch_ports(self):
        """Poll for plugged / unplugged boards so the user never has to click Refresh."""
        if not self.busy:
            before = {d for d, _, _ in self.ports}
            now = list_ports()
            if {d for d, _, _ in now} != before:
                new_esp = [d for d, _, esp in now if esp and d not in before]
                self.refresh(prefer=new_esp[0] if new_esp else None)
        self.root.after(2000, self.watch_ports)

    def run_async(self, cmd, done):
        """Run `cmd`, streaming its output into the log; call done(returncode) on the UI thread."""
        def worker():
            try:
                p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     creationflags=NO_WINDOW)
                buf = b""
                while True:
                    ch = p.stdout.read(1)
                    if not ch:
                        break
                    buf += ch
                    if ch in (b"\n", b"\r"):
                        self.q.put(("line", buf.decode(errors="replace")))
                        buf = b""
                if buf:
                    self.q.put(("line", buf.decode(errors="replace")))
                self.q.put(("done", (done, p.wait())))
            except Exception as e:
                self.q.put(("line", "\n%s\n" % e))
                self.q.put(("done", (done, 1)))
        threading.Thread(target=worker, daemon=True).start()

    def pump(self):
        try:
            while True:
                kind, val = self.q.get_nowait()
                if kind == "line":
                    m = PROGRESS_RE.search(val)
                    if m:
                        pct = float(m.group(1))
                        self.progress["value"] = pct
                        self.set_status("Writing... %d %%" % pct)
                    mac = MAC_RE.search(val)
                    if mac and self.mac is None:
                        self.mac = mac.group(1).upper()
                    if val.strip():
                        self.say(val.rstrip("\r\n") + "\n")
                else:
                    done, rc = val
                    done(rc)
        except queue.Empty:
            pass
        self.root.after(50, self.pump)

    def set_busy(self, busy):
        self.busy = busy
        self.flash_btn.configure(state="disabled" if busy else "normal")

    # --- result panel -------------------------------------------------------
    def clear_result(self):
        if self.result is not None:
            self.result.destroy()
            self.result = None

    def new_panel(self, bg, fg, title):
        self.clear_result()
        p = tk.Frame(self.frame, bg=bg, highlightthickness=1, highlightbackground=fg, padx=14, pady=10)
        p.grid(row=9, column=0, columnspan=3, sticky="ew", pady=(2, 10))
        p.columnconfigure(1, weight=1)
        tk.Label(p, text=title, bg=bg, fg=fg, font=("Segoe UI", 12, "bold")).grid(
            row=0, column=0, columnspan=3, sticky="w", pady=(0, 6))
        self.result = p
        return p

    def copy_field(self, parent, value, bg, row, col):
        """A read-only field the user can select and copy (or copy with the button)."""
        e = tk.Entry(parent, font=("Consolas", 12, "bold"), relief="flat", readonlybackground="white",
                     width=19)  # same for every field, so the Copy buttons line up
        e.insert(0, value)
        e.configure(state="readonly")
        e.grid(row=row, column=col, sticky="w", padx=(0, 6), pady=2, ipady=2)
        ttk.Button(parent, text="Copy", width=6,
                   command=lambda: (self.root.clipboard_clear(), self.root.clipboard_append(value))
                   ).grid(row=row, column=col + 1, sticky="w", pady=2)

    def show_success(self):
        bg = OK_BG
        p = self.new_panel(bg, OK_FG, "✔  Flashed successfully -- now connect to the board")
        body = ("Segoe UI", 10)

        def step(row, num, text):
            tk.Label(p, text=num, bg=bg, fg=OK_FG, font=("Segoe UI", 11, "bold")).grid(
                row=row, column=0, sticky="nw", padx=(0, 8), pady=(6, 0))
            wrap_to_width(tk.Label(p, text=text, bg=bg, font=body, justify="left", anchor="w")).grid(
                row=row, column=1, sticky="ew", pady=(6, 0))

        step(1, "1", "Wait about 10 seconds while the board starts.")
        step(2, "2", "On your phone or laptop, join the board's Wi-Fi network:")
        fields = tk.Frame(p, bg=bg)
        fields.grid(row=3, column=1, sticky="w", pady=(2, 0))
        ssid = "VibraShock-" + "".join(self.mac.split(":")[3:]) if self.mac else "VibraShock-XXXXXX"
        tk.Label(fields, text="Network", bg=bg, font=body).grid(row=0, column=0, sticky="w", padx=(0, 10))
        self.copy_field(fields, ssid, bg, 0, 1)
        tk.Label(fields, text="Password", bg=bg, font=body).grid(row=1, column=0, sticky="w", padx=(0, 10))
        self.copy_field(fields, AP_PASSWORD, bg, 1, 1)
        if not self.mac:
            tk.Label(fields, text="(XXXXXX differs per board)", bg=bg, fg="#555", font=("Segoe UI", 9)).grid(
                row=2, column=1, columnspan=2, sticky="w")

        step(4, "3", "Open the setup page, then go to Settings → Wi-Fi → Add a network "
                     "to connect the board to your own Wi-Fi.")
        link = tk.Frame(p, bg=bg)
        link.grid(row=5, column=1, sticky="w", pady=(4, 0))
        tk.Label(link, text=SETUP_URL, bg=bg, fg="#0b5cad", font=("Consolas", 12, "bold")).pack(side="left")
        ttk.Button(link, text="Open in browser", command=lambda: webbrowser.open(SETUP_URL)).pack(
            side="left", padx=(10, 0))

        wrap_to_width(tk.Label(p, text="Later updates run over the air (Settings → Firmware). "
                                       "Flashing another board? Plug it in and click Flash.",
                               bg=bg, fg="#555", font=("Segoe UI", 9), justify="left", anchor="w")).grid(
            row=6, column=0, columnspan=3, sticky="ew", pady=(10, 0))

    def show_failure(self):
        bg = ERR_BG
        p = self.new_panel(bg, ERR_FG, "✖  Flashing failed -- the board wasn't changed")
        tips = [
            "Try a different USB port or cable -- some cables only charge and carry no data.",
            "Pick the port marked (Espressif). The list updates by itself when you plug the board in.",
            "Still no luck? Hold the BOOT button while plugging in the cable, then click Flash again.",
        ]
        for i, t in enumerate(tips, start=1):
            tk.Label(p, text="•", bg=bg, fg=ERR_FG, font=("Segoe UI", 10, "bold")).grid(
                row=i, column=0, sticky="nw", padx=(0, 8))
            wrap_to_width(tk.Label(p, text=t, bg=bg, font=("Segoe UI", 10), justify="left",
                                   anchor="w")).grid(row=i, column=1, sticky="ew", pady=1)
        tk.Label(p, text="The exact error is in the log below.", bg=bg, fg="#555",
                 font=("Segoe UI", 9)).grid(row=len(tips) + 1, column=0, columnspan=2, sticky="w", pady=(8, 0))

    # --- esptool install ----------------------------------------------------
    def offer_install(self):
        if not messagebox.askyesno("esptool needed",
                                   "The flasher needs the 'esptool' Python package, which isn't "
                                   "installed yet.\n\nInstall it now? (runs pip, needs internet)"):
            self.set_status("esptool is not installed -- flashing is unavailable.")
            return
        self.set_busy(True)
        self.set_status("Installing esptool...")
        self.run_async([sys.executable, "-m", "pip", "install", "esptool"], self.install_done)

    def install_done(self, rc):
        self.set_busy(False)
        if rc == 0:
            self.set_status("esptool installed -- ready to flash.")
            self.refresh()
        else:
            self.set_status("Installing esptool failed -- see the log.")

    # --- flash ----------------------------------------------------------------
    def flash(self):
        if self.busy:
            return
        image = self.image.get().strip()
        if not os.path.isfile(image):
            messagebox.showerror("No firmware", "Select the *-full-flash.bin image first.")
            return
        if not image.endswith("-full-flash.bin") and not messagebox.askyesno(
                "Unusual image", "This doesn't look like a *-full-flash.bin. A plain vibrashock-v*.bin "
                "(the over-the-air update file) will NOT boot when flashed here.\n\nFlash it anyway?"):
            return
        sel = self.selected_port()
        if sel is None:
            messagebox.showerror("No port", "Plug the board in via USB and pick its port.")
            return
        port, label, is_esp = sel
        if not is_esp and not messagebox.askyesno(
                "Port not recognized", "%s isn't recognized as an ESP board, so this will probably "
                "fail.\n\nIf the board is plugged in but doesn't appear marked (Espressif), try another "
                "USB port or cable first.\n\nTry this port anyway?" % label):
            return
        major = esptool_major()
        if major is None:
            self.offer_install()
            return
        if not messagebox.askyesno("Flash board",
                                   "Flash %s to the board on %s?\n\nThis resets the board's saved "
                                   "settings (Wi-Fi networks, toggles)." % (os.path.basename(image), port)):
            return

        write = "write-flash" if major >= 5 else "write_flash"  # esptool 5 renamed the command
        cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
               "--baud", "921600", write]
        if self.erase.get():
            cmd.append("--erase-all")
        cmd += ["0x0", image]

        self.clear_result()
        self.mac = None
        self.progress["value"] = 0
        self.set_busy(True)
        self.set_status("Connecting to %s..." % port)
        self.say("\n$ esptool %s 0x0 %s (%s)\n" % (write, os.path.basename(image), port))
        self.run_async(cmd, self.flash_done)

    def flash_done(self, rc):
        self.set_busy(False)
        if rc == 0:
            self.progress["value"] = 100
            self.set_status("Done -- the board is restarting.")
            self.show_success()
        else:
            self.progress["value"] = 0
            self.set_status("Flashing failed.")
            self.show_failure()


def report_crash(exc_text, root=None):
    """No console under pythonw -- log the error next to this file and show it."""
    log = os.path.join(HERE, "flasher-error.log")
    try:
        with open(log, "w", encoding="utf-8") as f:
            f.write(exc_text)
    except OSError:
        log = "(could not write flasher-error.log)"
    sys.stderr and sys.stderr.write(exc_text)  # visible when started from a console / the .bat
    try:
        if root is None:
            root = tk.Tk()
            root.withdraw()
        messagebox.showerror("VibraShock Flasher -- error",
                             "The flasher hit an error:\n\n%s\n\nDetails: %s"
                             % (exc_text.strip().splitlines()[-1], log))
    except Exception:
        pass


def main():
    root = None
    try:
        root = tk.Tk()
        root.report_callback_exception = lambda *exc: report_crash(
            "".join(traceback.format_exception(*exc)), root)
        App(root)
        root.mainloop()
    except Exception:
        report_crash(traceback.format_exc(), root)
        sys.exit(1)


if __name__ == "__main__":
    main()
