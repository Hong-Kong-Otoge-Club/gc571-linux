# gc571sig

A small Linux kernel module that reads the HDMI input signal of an AVerMedia GC571 (Live Gamer
Ultra 2.1, USB `07ca:0571`) and reports it in debugfs:

```
$ sudo cat /sys/kernel/debug/gc571sig/<serial>
1920x1080 @ 60.00 Hz  Htotal=2200 Vtotal=1125  checksum=ok
```

or `no signal`. One file per card, named by the card's USB serial number.

## Why

The GC571 is a UVC device, so `uvcvideo` streams video from it without a vendor driver. The
input signal timing is not in the UVC stream. The card exposes it through a vendor command
mailbox on UVC Extension Unit 4 of the VideoControl interface. `uvcvideo` does not register
that extension unit, because it hangs off the Output Terminal and the chain scanner drops it.

This module talks to endpoint 0 directly. It claims no interface and changes nothing in
`uvcvideo`, so it is safe to use while the card streams.

## Mailbox protocol

Reverse engineered from the vendor's Windows tool.

- Selector 1 is the command and response buffer, variable length.
- Selector 2 is a 16-bit register. SET resizes selector 1. GET reports the pending response length.
- To read register R: SET_CUR(selector 2) = 9, SET_CUR(selector 1) = the 9-byte command frame,
  poll GET_CUR(selector 2) until it reports a length L, then GET_CUR(selector 1) for L bytes.
- Command frame: `[toggle] 06 00 00 [reg] 00 00 00 [checksum]`. The toggle byte must differ from
  the previous command or the device ignores it. The response echoes the toggle. The checksum
  makes the whole frame sum to 0 modulo 256.
- Register `0x37` holds the raw HDMI timing block: Vtotal, Htotal, height, width and the refresh
  rate in 0.01 Hz, each 16-bit little endian.

Only register `0x37` is read. The module sends no other command.

## Install

Build and load once, for the running kernel:

```
make
sudo insmod gc571sig.ko
```

Or install with DKMS so it rebuilds on every kernel update and loads at boot. `install.sh` does
that on Arch Linux: it installs `dkms` with pacman, stages the source under `/usr/src`, runs
`dkms add`, `build` and `install`, writes `/etc/modules-load.d/gc571sig.conf` and loads the
module. On other distributions, run the same `dkms` commands after you install `dkms` and the
kernel headers.

Plug the cards in before you load the module. It finds the cards at load time, up to 8, and
does not watch for hot-plug. Reload it after you plug a card in.

## Notes

- The module needs `debugfs` mounted at `/sys/kernel/debug`, which is the default.
- Each read sends one mailbox command and waits up to about one second for the answer, so do not
  poll the file many times per second.
- Written for and used on Arch Linux. The module uses `usb_control_msg_send` and `_recv`, so it needs kernel 5.10 or newer.
