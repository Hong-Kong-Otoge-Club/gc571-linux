#!/usr/bin/env python3
"""
Fix OBS V4L2 source settings for GC550 (/dev/video10) so OBS does not fail with:
  error: v4l2-helpers: unable to start stream

Why this happens:
  When you select a resolution or framerate in the OBS GUI for a v4l2_input source,
  OBS stores `"resolution"` and `"framerate"` in the source settings JSON and calls
  VIDIOC_S_FMT / VIDIOC_S_PARM when opening `/dev/video10`.
  Because `lgx2d` is already actively writing 1920x1080 YUYV frames to the v4l2loopback
  device, v4l2loopback rejects format renegotiation (EBUSY), causing OBS to show a black screen.

How this script fixes it:
  Strips `"resolution"` and `"framerate"` from the v4l2_input source settings and sets
  `pixelformat` to YUYV (1448695129) with `buffering: False`, either directly in an OBS
  scene collection JSON file or live over obs-websocket.
"""
import argparse
import json
import shutil
import struct

YUYV_FOURCC = struct.unpack("<I", b"YUYV")[0]  # 1448695129


def clean_v4l2_settings(device_id: str) -> dict:
    return {
        "device_id": device_id,
        "input": 0,
        "pixelformat": YUYV_FOURCC,
        "buffering": False,
    }


def fix_scene_json(json_path: str, device_id: str) -> int:
    with open(json_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    fixed = 0
    for src in data.get("sources", []):
        if src.get("id") == "v4l2_input":
            settings = src.get("settings", {})
            if settings.get("device_id") == device_id:
                src["settings"] = clean_v4l2_settings(device_id)
                fixed += 1
                print(f"Fixed v4l2_input source {src.get('name')!r} ({device_id}) in {json_path}")

    if fixed:
        shutil.copy2(json_path, json_path + ".bak")
        with open(json_path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=4, ensure_ascii=False)
    else:
        print(f"No matching v4l2_input source for {device_id} found in {json_path}")
    return fixed


def main():
    parser = argparse.ArgumentParser(description="Fix OBS V4L2 source for GC550 v4l2loopback")
    parser.add_argument("--device", default="/dev/video10", help="v4l2loopback device path (default: /dev/video10)")
    parser.add_argument(
        "--scene-json",
        default="/home/streamer/.var/app/com.obsproject.Studio/config/obs-studio/basic/scenes/Untitled.json",
        help="Path to OBS scene collection JSON file",
    )
    args = parser.parse_args()
    fix_scene_json(args.scene_json, args.device)


if __name__ == "__main__":
    main()
