#!/usr/bin/env bash
# Install gc571sig as a DKMS-managed kernel module on Arch.
# Run from the directory containing gc571sig.c, Makefile, dkms.conf.
set -euo pipefail

PACKAGE_NAME=gc571sig
PACKAGE_VERSION=1.0
SRC_DIR="/usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}"
MODULES_LOAD_CONF="/etc/modules-load.d/${PACKAGE_NAME}.conf"

if [ "$(id -u)" -ne 0 ]; then
    exec sudo -E "$0" "$@"
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
for f in gc571sig.c Makefile dkms.conf; do
    [ -f "${HERE}/$f" ] || { echo "missing $f in ${HERE}"; exit 1; }
done

if ! command -v dkms >/dev/null; then
    echo ":: Installing dkms via pacman"
    pacman -S --needed --noconfirm dkms
fi

echo ":: Removing any prior DKMS install of ${PACKAGE_NAME}/${PACKAGE_VERSION}"
dkms remove "${PACKAGE_NAME}/${PACKAGE_VERSION}" --all 2>/dev/null || true

echo ":: Staging source at ${SRC_DIR}"
rm -rf "${SRC_DIR}"
mkdir -p "${SRC_DIR}"
install -m 0644 "${HERE}/gc571sig.c" "${HERE}/Makefile" "${HERE}/dkms.conf" "${SRC_DIR}/"

echo ":: dkms add / build / install"
dkms add "${PACKAGE_NAME}/${PACKAGE_VERSION}"
dkms build "${PACKAGE_NAME}/${PACKAGE_VERSION}"
dkms install "${PACKAGE_NAME}/${PACKAGE_VERSION}"

echo "${PACKAGE_NAME}" > "${MODULES_LOAD_CONF}"
echo ":: Wrote ${MODULES_LOAD_CONF} (auto-load at boot)"

if lsmod | awk '{print $1}' | grep -qx "${PACKAGE_NAME}"; then
    echo ":: Unloading currently-running ${PACKAGE_NAME}"
    rmmod "${PACKAGE_NAME}" || true
fi
echo ":: Loading ${PACKAGE_NAME}"
modprobe "${PACKAGE_NAME}"

echo
echo "=== DKMS status ==="
dkms status | grep "${PACKAGE_NAME}" || true
echo
echo "=== Module info ==="
modinfo "${PACKAGE_NAME}" | grep -E "^filename|^version|^vermagic"
echo
echo "=== debugfs files ==="
ls -l "/sys/kernel/debug/${PACKAGE_NAME}/" 2>/dev/null || echo "(no cards present, or debugfs not mounted)"
echo
echo ":: Done.  Read a card with:  sudo cat /sys/kernel/debug/${PACKAGE_NAME}/<serial>"
