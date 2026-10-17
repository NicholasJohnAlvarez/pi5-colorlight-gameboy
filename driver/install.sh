#!/bin/bash
# Build the driver and set it up to start from the power switch.
# Run on the Raspberry Pi 5 from inside the driver/ folder.
set -e
cd "$(dirname "$0")"

echo "Installing build dependencies..."
sudo apt install -y build-essential libasound2-dev libretro-mgba evtest

echo "Building colorlight_libretro..."
gcc -O2 -o colorlight_libretro colorlight_libretro.c -ldl -lasound -lrt -lm -lpthread

# Raw Ethernet + real-time priority without running as root
sudo setcap cap_net_raw,cap_sys_nice,cap_ipc_lock+ep colorlight_libretro

# Let the gameboy user read input devices
sudo usermod -aG input "$USER"

chmod +x play.sh power_switch.py

echo "Installing power switch service..."
sed -e "s|/home/gameboy/pi5-colorlight-gameboy/driver|$(pwd)|g" \
    -e "s|User=gameboy|User=$USER|" \
    -e "s|/run/user/1000|/run/user/$(id -u)|" \
    systemd/gameboy-power.service | sudo tee /etc/systemd/system/gameboy-power.service > /dev/null
sudo systemctl daemon-reload
sudo systemctl enable --now gameboy-power

echo
echo "Done. Put ROMs in ~/roms/gbc and flip the power switch."
echo "Logs: journalctl -u gameboy-power -f"
