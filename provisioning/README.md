# Set up a new board (first flash)

A brand-new board needs **one** USB flash. After that it updates itself over the air
(dashboard → Settings → Firmware update), so this is the only time you need a cable.

What's in this folder:

| File | What it is |
|---|---|
| `btscanner-v<version>-full-flash.bin` | The complete image (bootloader + partition table + app) — the only file you flash |
| `flasher.pyw` | GUI flasher: double-click, pick the port, click **Flash** |
| `Start Flasher.bat` | Fallback launcher if `flasher.pyw` closes right away (shows the error) |
| `make_image.py` | Rebuilds the image from the last `pio run` (developers only) |

## Option A — in the browser (nothing to install)

1. Plug the board in via USB-C.
2. Open **<https://espressif.github.io/esptool-js/>** in **Chrome or Edge** (Firefox/Safari
   don't support Web Serial).
3. Baud rate `921600` → **Connect** → choose the board's port.
4. Under *Program*: flash address **`0x0`**, choose the `*-full-flash.bin` from this folder →
   **Program**.
5. When it finishes, press the board's reset/power button or re-plug it.

## Option B — the GUI flasher (needs Python)

1. Install Python 3 from <https://www.python.org/downloads/> if you don't have it.
2. Double-click **`flasher.pyw`** (macOS/Linux: `python3 flasher.pyw`). If it closes right away,
   use **`Start Flasher.bat`** instead, which shows what went wrong. On first start the flasher
   offers to install `esptool` — say yes.
3. Plug the board in and pick the port marked **(Espressif)** — the list updates by itself. Click
   **Flash**. When it's done, the window shows the board's exact Wi-Fi name and the next steps.

## After flashing

The board restarts and opens its own Wi-Fi, **`BTScanner-XXXXXX`** (password
**`recorder123`**). Join it and open <http://192.168.4.1> to add your Wi-Fi network — from then on use the dashboard, including for
firmware updates.

## Good to know

- **Flashing resets the board's saved settings** (Wi-Fi networks, toggles). It's meant for new
  boards; update a running board through its dashboard instead.
- **Connection fails / no port shows up?** Hold the **BOOT** button while plugging in the USB
  cable, then try again. Use a USB cable that carries data (some are charge-only).
- The `-full-flash.bin` is **USB only** — never upload it through the dashboard's firmware update;
  that takes the plain `btscanner-v<version>.bin`.
- The image doesn't have to be the newest version: flash any image once, then update over the air.
  To make a new one after a build: `python provisioning/make_image.py`.
