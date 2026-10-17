#!/usr/bin/env python3
"""
Volume knob daemon: turns the Pico's Rz axis into the Pi's system volume.

The pot is absolute (it has a real position), so it is sent as a gamepad
axis instead of HID volume up/down keys. This daemon reads that axis with
evdev and sets the PipeWire default sink with wpctl.

Run as the desktop user (not root) so it can reach the PipeWire session.
"""
import glob
import struct
import subprocess
import time

DEVICE_NAME = "Custom Gamepad"
EV_ABS = 3
ABS_RZ = 0x05
EVENT = struct.Struct("llHHi")
AXIS_MIN, AXIS_MAX = -127, 127


def find_pico():
    for path in sorted(glob.glob("/sys/class/input/event*/device/name")):
        with open(path) as f:
            if DEVICE_NAME in f.read():
                return "/dev/input/" + path.split("/")[4]
    return None


def set_volume(percent):
    subprocess.run(["wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", f"{percent}%"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    last = None
    while True:
        dev = find_pico()
        if not dev:
            time.sleep(3)
            continue
        try:
            with open(dev, "rb") as f:
                while True:
                    data = f.read(EVENT.size)
                    if len(data) < EVENT.size:
                        break
                    _, _, ev_type, code, value = EVENT.unpack(data)
                    if ev_type == EV_ABS and code == ABS_RZ:
                        pct = round((value - AXIS_MIN) * 100 / (AXIS_MAX - AXIS_MIN))
                        pct = max(0, min(100, pct))
                        if pct != last:
                            set_volume(pct)
                            last = pct
        except OSError:
            time.sleep(3)


if __name__ == "__main__":
    main()
