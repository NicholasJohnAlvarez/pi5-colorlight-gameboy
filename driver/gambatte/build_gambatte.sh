#!/bin/bash
# Build the Gambatte core with network link support for Pokémon trading.
#
# Prebuilt Gambatte cores are compiled with HAVE_NETWORK=0, which leaves the
# network link cable out entirely. This builds it from source with the link
# turned on and the 1-second connect timeout patch applied, then copies it
# to driver/cores/.
#
# Use the same Gambatte commit on both machines: the option names for the
# server address changed between versions.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${SRC:-$HOME/src/gambatte-libretro}"
COMMIT="${COMMIT:-d9d6cd0}"

mkdir -p "$(dirname "$SRC")"
if [ ! -d "$SRC" ]; then
    git clone https://github.com/libretro/gambatte-libretro "$SRC"
fi
cd "$SRC"
git fetch --quiet origin
git checkout --quiet "$COMMIT"

python3 "$HERE/patch_connect_timeout.py" "$SRC"
make -j4 HAVE_NETWORK=1

mkdir -p "$HERE/../cores"
cp gambatte_libretro.so "$HERE/../cores/"
echo "Installed $HERE/../cores/gambatte_libretro.so"
