#!/bin/sh
# One-step installer for AVerMedia GC550 (LGX, 07ca:4710) on Debian/Ubuntu/Arch Linux.
# Uses the guarded Windows-safe driver from Hong-Kong-Otoge-Club/lgx2userspace (branch: gc550).
set -eu

STREAM_USER="${STREAM_USER:-streamer}"
VIDEO_NR="${VIDEO_NR:-10}"
CARD_LABEL="${CARD_LABEL:-gc550}"
REPO_URL="https://github.com/Hong-Kong-Otoge-Club/lgx2userspace.git"
BRANCH="gc550"
WORKDIR="${WORKDIR:-/tmp/lgx2userspace-gc550}"

echo "[1/5] Installing build and runtime dependencies..."
if command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends \
        git ca-certificates g++ pkg-config \
        libusb-1.0-0-dev libpipewire-0.3-dev \
        v4l2loopback-dkms v4l2loopback-utils v4l-utils
elif command -v pacman >/dev/null 2>&1; then
    sudo pacman -S --needed --noconfirm \
        git gcc pkgconf libusb pipewire v4l2loopback-dkms v4l-utils
fi

echo "[2/5] Cloning and building guarded lgx2d (branch: $BRANCH)..."
rm -rf "$WORKDIR"
git clone --depth 1 -b "$BRANCH" "$REPO_URL" "$WORKDIR"
sh "$WORKDIR/scripts/build-gc550.sh"

STREAM_HOME=$(eval echo "~$STREAM_USER")
sudo install -D -o "$STREAM_USER" -g "$STREAM_USER" -m 0755 \
    "$WORKDIR/lgx2d" "$STREAM_HOME/.local/bin/lgx2d"

echo "[3/5] Installing udev rule and v4l2loopback config (/dev/video${VIDEO_NR})..."
sudo tee /etc/udev/rules.d/70-avermedia-gc550.rules >/dev/null <<'EOF'
# AVerMedia Live Gamer Extreme (GC550 / LGX)
SUBSYSTEM=="usb", ATTR{idVendor}=="07ca", ATTR{idProduct}=="4710", MODE="0660", GROUP="video", TAG+="uaccess"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger

echo "v4l2loopback" | sudo tee /etc/modules-load.d/v4l2loopback.conf >/dev/null
sudo tee /etc/modprobe.d/v4l2loopback.conf >/dev/null <<EOF
options v4l2loopback video_nr=${VIDEO_NR} card_label="${CARD_LABEL}" exclusive_caps=0
EOF

if ! [ -e "/dev/video${VIDEO_NR}" ]; then
    sudo modprobe -r v4l2loopback 2>/dev/null || true
    sudo modprobe v4l2loopback video_nr="${VIDEO_NR}" card_label="${CARD_LABEL}" exclusive_caps=0
fi

echo "[4/5] Installing systemd user unit gc550.service for ${STREAM_USER}..."
UNIT_DIR="$STREAM_HOME/.config/systemd/user"
sudo install -d -o "$STREAM_USER" -g "$STREAM_USER" "$UNIT_DIR"
sudo tee "$UNIT_DIR/gc550.service" >/dev/null <<EOF
[Unit]
Description=AVerMedia GC550 (LGX) userspace capture daemon
After=pipewire.service

[Service]
Type=simple
ExecStart=/usr/bin/stdbuf -oL ${STREAM_HOME}/.local/bin/lgx2d -x -d /dev/video${VIDEO_NR}
Restart=always
RestartSec=2

[Install]
WantedBy=default.target
EOF
sudo chown "$STREAM_USER:$STREAM_USER" "$UNIT_DIR/gc550.service"

echo "[5/5] Enabling and starting gc550.service..."
STREAM_UID=$(id -u "$STREAM_USER")
sudo -u "$STREAM_USER" XDG_RUNTIME_DIR="/run/user/$STREAM_UID" systemctl --user daemon-reload
sudo -u "$STREAM_USER" XDG_RUNTIME_DIR="/run/user/$STREAM_UID" systemctl --user enable --now gc550.service

echo "Done! Verify with: journalctl --user-unit=gc550.service -n 30 --no-pager"
