#!/bin/bash
# Launch the next game on the Colorlight LED wall.
# Every power-on rotates to the next ROM in $ROM_DIR (alphabetical, wraps around).
# Hold the Menu button (GP7) while switching on to stay on the current game.
#
# Edit the variables below to match your setup.

INSTALL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROM_DIR="${ROM_DIR:-$HOME/roms/gbc}"
CORE="${CORE:-/usr/lib/aarch64-linux-gnu/libretro/mgba_libretro.so}"
IFACE="${IFACE:-eth0}"
STATE_FILE="$INSTALL_DIR/.game_index"
LOCK_FILE="$INSTALL_DIR/.game_lock"

# .gb and .gbc only, skip BIOS files
mapfile -t ROMS < <(find "$ROM_DIR" -maxdepth 1 \( -name "*.gbc" -o -name "*.gb" \) ! -iname "*bios*" | sort)
if [ ${#ROMS[@]} -eq 0 ]; then
    echo "No ROMs found in $ROM_DIR"
    exit 1
fi

INDEX=0
[ -f "$STATE_FILE" ] && INDEX=$(cat "$STATE_FILE")
if [ ! -f "$LOCK_FILE" ]; then
    INDEX=$((INDEX + 1))
fi
[ "$INDEX" -ge ${#ROMS[@]} ] && INDEX=0
echo "$INDEX" > "$STATE_FILE"

ROM="${ROMS[$INDEX]}"
echo "Game $((INDEX + 1))/${#ROMS[@]}: $(basename "$ROM")"

exec "$INSTALL_DIR/colorlight_libretro" \
    -i "$IFACE" \
    -c "$CORE" \
    -r "$ROM" \
    --dim 0.5
