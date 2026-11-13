# AVerMedia GC550 (Live Gamer Extreme) Linux + OBS Setup

This branch packages the complete setup and OBS fix for running the **AVerMedia GC550 (`07ca:4710`)** on Linux stream PCs while keeping the card **100% safe for the official Windows driver**.

## 1. Driver Repository (`lgx2d`)

The guarded C++ userspace driver lives in [`Hong-Kong-Otoge-Club/lgx2userspace` (branch `gc550`)](https://github.com/Hong-Kong-Otoge-Club/lgx2userspace/tree/gc550):
- **`Gc550Policy` fail-closed filter**: Blocks all `AT88SC` CryptoMemory opcodes (`05/06/07/09/B8/BA`), board flash writes (`0D/15`), and CDCE913 EEPROM/lock register writes so the anti-clone chip counter is never touched and **the card continues to work on Windows**.
- **Zero-copy 1080p60 pipeline**: Uses `memchr` in-place marker scanning and a 32-slot ring buffer, writing `1920x1080` `YUYV` frames into a `v4l2loopback` device (`/dev/video10`) and publishing audio to PipeWire (`LGX2 Audio`).

## 2. Quick Install on Another Linux PC

Run [`install-gc550.sh`](./install-gc550.sh) on the target Linux PC:

```bash
chmod +x install-gc550.sh
./install-gc550.sh
```

What it does:
1. Installs dependencies (`g++`, `pkg-config`, `libusb-1.0-0-dev`, `libpipewire-0.3-dev`, `v4l2loopback-dkms`).
2. Clones [`Hong-Kong-Otoge-Club/lgx2userspace` (`gc550` branch)](https://github.com/Hong-Kong-Otoge-Club/lgx2userspace/tree/gc550) and runs `scripts/build-gc550.sh`.
3. Installs `/etc/udev/rules.d/70-avermedia-gc550.rules` (`07ca:4710`) and `/etc/modprobe.d/v4l2loopback.conf` (`video_nr=10 card_label="gc550" exclusive_caps=0`).
4. Installs and enables `~/.config/systemd/user/gc550.service` (`lgx2d -x -d /dev/video10`).

---

## 3. Fixing the OBS Black Screen (`v4l2-helpers: unable to start stream`)

### Why OBS fails when configured via the OBS GUI
When you open the properties of a **Video Capture Device (V4L2)** (`v4l2_input`) in the OBS GUI, OBS writes explicit `"resolution"` (e.g. `8246337209400` for `1920x1080`) and `"framerate"` (`-1` or packed fraction) keys into the source settings.

When OBS opens `/dev/video10` with those keys present, it calls `VIDIOC_S_FMT` and `VIDIOC_S_PARM` to renegotiate the V4L2 format. Because `lgx2d` is **already actively writing frames** to `/dev/video10`, `v4l2loopback` returns `EBUSY`, and OBS fails with:
```text
error: v4l2-helpers: unable to start stream
```

### How to fix it
Do **not** include `"resolution"` or `"framerate"` in the OBS `v4l2_input` source settings for `/dev/video10`. Only pass:

```json
{
    "device_id": "/dev/video10",
    "input": 0,
    "pixelformat": 1448695129,
    "buffering": false
}
```
*(where `1448695129` is FourCC `YUYV`).*

When `"resolution"` and `"framerate"` are omitted:
- OBS skips `VIDIOC_S_FMT` / `VIDIOC_S_PARM` renegotiation.
- OBS immediately adopts the active `1920x1080 YUYV` format from `lgx2d` and starts streaming without error.

### Automated Fix Script
- **Offline (while OBS is closed):**
  ```bash
  python3 fix-obs-gc550.py --device /dev/video10 \
    --scene-json ~/.var/app/com.obsproject.Studio/config/obs-studio/basic/scenes/Untitled.json
  ```
- **Live via `obs-websocket` (in Python layout builder scripts):**
  ```python
  import struct
  YUYV = struct.unpack("<I", b"YUYV")[0]

  obs.req("CreateInput", {
      "sceneName": "Main",
      "inputName": "1P Capture",
      "inputKind": "v4l2_input",
      "inputSettings": {
          "device_id": "/dev/video10",
          "input": 0,
          "pixelformat": YUYV,
          "buffering": False,
      },
      "sceneItemEnabled": True,
  })
  ```
