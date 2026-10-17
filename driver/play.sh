#!/bin/bash
# Launch the next game on the Colorlight LED wall.
# Every power-on rotates to the next ROM in $ROM_DIR (alphabetical, wraps around).
# Hold the Menu button (GP7) while switching on to lock/unlock the current game.
#
# Uses the network-link Gambatte core from gambatte/build_gambatte.sh if it has
# been built (for Pokémon trading), otherwise the stock mGBA core.

INSTALL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROM_DIR="${ROM_DIR:-$HOME/roms/gbc}"
IFACE="${IFACE:-eth0}"
BRIGHTNESS="${BRIGHTNESS:-80}"
STATE_FILE="$INSTALL_DIR/.game_index"
LOCK_FILE="$INSTALL_DIR/.game_lock"

if [ -z "$CORE" ]; then
    if [ -f "$INSTALL_DIR/cores/gambatte_libretro.so" ]; then
        CORE="$INSTALL_DIR/cores/gambatte_libretro.so"
    else
        CORE="/usr/lib/aarch64-linux-gnu/libretro/mgba_libretro.so"
    fi
fi
OPTS=()
case "$CORE" in
    *gambatte*) OPTS=(--opts-file "$INSTALL_DIR/gambatte/gambatte.opt") ;;
esac

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
echo "Game $((INDEX + 1))/${#ROMS[@]}: $(basename "$ROM")  [$(basename "$CORE")]"

exec "$INSTALL_DIR/colorlight_libretro" \
    -i "$IFACE" \
    -c "$CORE" \
    -r "$ROM" \
    -b "$BRIGHTNESS" \
    "${OPTS[@]}"
