#!/usr/bin/env python3
"""
Power switch daemon for the giant Game Boy.

The Pico controller reports the bistable power switch as BTN_SOUTH (bit 0).
Switch ON  -> start play.sh (the emulator)
Switch OFF -> stop it cleanly (SIGINT, so the save file is written)

Holding Menu (GP7, BTN_TR) at the moment the switch goes ON toggles
"lock" mode, which keeps the same game instead of rotating to the next one.
"""
import glob
import os
import signal
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
LAUNCH = os.path.join(HERE, "play.sh")
LOCK_FILE = os.path.join(HERE, ".game_lock")
DEVICE_NAME = "Custom Gamepad"          # product string from the Pico firmware

EV_KEY = 1
BTN_SOUTH = 0x130                       # power switch
BTN_TR = 0x137                          # menu button
EVENT = struct.Struct("llHHi")          # struct input_event on 64-bit Linux

game = None
menu_held = False


def find_pico():
    """Find the Pico's event device by name, so event numbers can change."""
    for path in sorted(glob.glob("/sys/class/input/event*/device/name")):
        with open(path) as f:
            if DEVICE_NAME in f.read():
                return "/dev/input/" + path.split("/")[4]
    return None


def start_game():
    global game
    if game and game.poll() is None:
        return
    if menu_held:
        if os.path.exists(LOCK_FILE):
            os.remove(LOCK_FILE)
            print("Game lock OFF: rotating games again")
        else:
            open(LOCK_FILE, "w").close()
            print("Game lock ON: staying on this game")
    print("POWER ON")
    game = subprocess.Popen([LAUNCH], start_new_session=True)


def stop_game():
    global game
    if game and game.poll() is None:
        print("POWER OFF")
        try:
            os.killpg(os.getpgid(game.pid), signal.SIGINT)
            game.wait(timeout=5)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            os.killpg(os.getpgid(game.pid), signal.SIGKILL)
    game = None


def main():
    global menu_held
    while True:
        dev = find_pico()
        if not dev:
            print("Waiting for Pico controller...")
            time.sleep(3)
            continue
        print(f"Listening on {dev}")
        try:
            with open(dev, "rb") as f:
                while True:
                    data = f.read(EVENT.size)
                    if len(data) < EVENT.size:
                        break
                    _, _, ev_type, code, value = EVENT.unpack(data)
                    if ev_type != EV_KEY:
                        continue
                    if code == BTN_TR:
                        menu_held = bool(value)
                    elif code == BTN_SOUTH:
                        start_game() if value else stop_game()
        except OSError:
            print("Controller disconnected")
            stop_game()
            time.sleep(1)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: (stop_game(), sys.exit(0)))
    try:
        main()
    except KeyboardInterrupt:
        stop_game()
