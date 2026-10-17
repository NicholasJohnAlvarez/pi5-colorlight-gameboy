# pi5-colorlight-gameboy

**A Raspberry Pi 5 that talks straight to Colorlight LED receiver cards over Ethernet. No sending card, no HDMI, no X server.**

This is the software behind the 5ft 9in Pokémon Yellow edition of the *World's Largest Game Boy Color*, a giant, fully playable Game Boy Color built by Nick Alvarez and Paige Powell at Inventopia in Davis, California.

```
emulator core ──► colorlight_libretro (Pi 5) ──raw Ethernet──► Colorlight 5A-75B / E80 ──HUB75──► 3×3 LED panels
                         ▲
Pico USB gamepad ────────┘   (giant buttons, power switch, volume knob)
```

## Why this exists

LED video walls normally need two boxes: a **sending card** (such as a Colorlight X2S) that takes HDMI from a computer, and **receiver cards** behind the panels. Most hobby projects either use a sending card or drive HUB75 panels directly from GPIO with a HAT.

This project removes the sending card. The Pi 5 builds the receiver card's Ethernet frames itself and sends them from its gigabit port. The libretro frontend loads an emulator core with `dlopen()`, takes every frame straight from the core's video callback and streams it to the LED wall. There's no screen capture and no intermediate display, just pixels to packets.

**What you get:**

- 192×192 output at the Game Boy's native ~59.7 fps on a 3×3 grid of 64×64 P3 panels
- Real-time pacing: `SCHED_FIFO`, CPU pinning, `mlockall()` and absolute-deadline `clock_nanosleep()`
- Rows sent in 32-row `sendmmsg()` batches, matching the panels' 1/32 scan, plus a short pause before the sync packet that fixes bottom-row flicker
- Works with any libretro core that outputs XRGB8888, RGB565 or 0RGB1555. Tested with mGBA and Gambatte.
- Battery saves (`.srm`) written every 5 seconds and on power-off, using an atomic write that survives the plug being pulled
- Core options from the command line, e.g. Gambatte's network link for Pokémon trading between two machines
- A matching **Raspberry Pi Pico** controller: an 11-button USB gamepad, a bistable power switch, an absolute volume knob and a power LED

## Repository layout

| Path | What it is |
|---|---|
| `driver/colorlight_libretro.c` | The libretro frontend and Colorlight Ethernet driver (single C file) |
| `driver/play.sh` | Picks the next ROM on each power-on and launches the frontend |
| `driver/power_switch.py` | Watches the Pico's power switch: starts the game on ON and saves and stops it on OFF |
| `driver/systemd/gameboy-power.service` | Starts the power switch daemon at boot |
| `driver/install.sh` | Builds and installs everything on the Pi |
| `controller/firmware/` | Pico firmware (C, Pico SDK + TinyUSB) |
| `controller/usb_gamepad.uf2` | Prebuilt firmware: drag onto the Pico in BOOTSEL mode |
| `controller/volume_daemon.py` | Turns the volume knob into the Pi's system volume (PipeWire) |
| `docs/colorlight-protocol.md` | Notes on the receiver card packet format |

## Hardware

| Part | Notes |
|---|---|
| Raspberry Pi 5 | Raspberry Pi OS 64-bit (Bookworm or newer) |
| Colorlight receiver card | E80 or 5A-75B, firmware v13.x |
| 9× 64×64 P3 HUB75 panels, 1/32 scan | 3×3 grid = 192×192 pixels, 576 mm square |
| 5 V supply | Ours is 60 A; nine panels typically draw 18–27 A |
| Ethernet cable | Pi 5 straight to the receiver card's input port, no switch |
| Raspberry Pi Pico | The controller |
| 10 momentary buttons + 1 latching switch | We use long-arm limit switches behind custom giant buttons |
| 47 kΩ potentiometer + 47 kΩ resistor | Volume knob (the pot with a pull-up to 3V3) |
| Red LED, 150 Ω, 2N2222, 1 kΩ | Power indicator |
| Audio output and speaker | Anything PipeWire can see |

### Receiver card setup (one time)

Configure the panels on the receiver card once with Colorlight's LEDVision/LEDSetting software on Windows: module type, scan, cabinet size 192×192 and data group order. Save it to the card. For our P3 1/32 panels, setting *Segments* to ×32 gave a 1,920 Hz refresh and removed camera flicker. After that the card remembers everything and the Pi just sends pixels.

### Controller wiring (Pico)

| GPIO | Function | GPIO | Function |
|---|---|---|---|
| GP0 | Power switch (latching) | GP6 | B |
| GP1 | D-pad left | GP7 | Menu |
| GP2 | Select | GP8 | A |
| GP3 | D-pad down | GP9 | D-pad up |
| GP4 | D-pad right | GP10 | Right bumper |
| GP5 | Start | GP15 | Power LED (to 2N2222 base via 1 kΩ) |
| GP27 / ADC1 | Pot wiper | 3V3 / AGND | Pot ends (47 kΩ pull-up to 3V3) |

All buttons connect the GPIO to GND (internal pull-ups, active low).

## Install

On the Pi 5:

```bash
git clone https://github.com/NicholasJohnAlvarez/pi5-colorlight-gameboy.git
cd pi5-colorlight-gameboy/driver
./install.sh
mkdir -p ~/roms/gbc        # put your own .gb / .gbc files here
```

Flash the controller: hold BOOTSEL on the Pico, plug it in, and copy `controller/usb_gamepad.uf2` onto the `RPI-RP2` drive. To build it yourself, see `controller/firmware/` (standard Pico SDK CMake build).

Volume knob service (runs as your user):

```bash
mkdir -p ~/.config/systemd/user
cp ../controller/systemd/volume-daemon.service ~/.config/systemd/user/
systemctl --user daemon-reload && systemctl --user enable --now volume-daemon
```

If the game sounds quieter than other apps, run `amixer sset PCM 100%` once.

Now flip the power switch. Each power-on loads the next game in `~/roms/gbc`. Hold **Menu** while switching on to lock the current game (and again to unlock).

## Running by hand

```bash
./colorlight_libretro -i eth0 \
    -c /usr/lib/aarch64-linux-gnu/libretro/mgba_libretro.so \
    -r ~/roms/gbc/game.gbc --dim 0.5
```

Useful options:

| Option | Default | |
|---|---|---|
| `-i` | `eth0` | Interface wired to the receiver card |
| `-W` / `-H` | 192 / 192 | Output size (match your cabinet) |
| `--integer` | off | 1:1 pixels instead of aspect-fit (160×144 → 192×172) |
| `-y` / `-x` | 0 | Nudge the image to line up with the bezel |
| `-d`, `--dim` | 1.0 | Software brightness; the card may ignore `-b` if brightness is stored in its flash |
| `--batch` | 32 | Rows per `sendmmsg()` call |
| `--sync-delay` | 500 µs | Raise it if the last rows flicker |
| `--cpu` | 3 | CPU core to pin to (`-1` disables) |
| `--opt KEY=VALUE` | | Core option, repeatable |
| `--pot-volume` | off | Scale game audio by the knob directly instead of using `volume_daemon.py` |

### Pokémon trading between two Game Boys

Gambatte has a TCP "network link" that emulates the link cable. Build Gambatte with `HAVE_NETWORK=1`, then pass its options. For example, on the machine acting as the client:

```bash
./colorlight_libretro -i eth0 -c ./gambatte_libretro.so -r "Pokemon Yellow.gbc" \
    --opt gambatte_gb_hwmode=GBC \
    --opt gambatte_gb_bootloader=enabled \
    --opt gambatte_gb_link_mode="Network Client" \
    --opt gambatte_gb_link_network_port=56400
```

Option names and values vary between Gambatte builds. Check your core's options list.

## Notes and limitations

- The Game Boy Color screen is 160×144. HUB75 panels come in 32×32 and 64×64 blocks, so we used a 192×192 grid and scale the image to fit. It's close, but not pixel-perfect.
- Brightness: if the receiver card has brightness saved in flash, the brightness packet may be ignored. Use `--dim` instead.
- Bring your own legally owned ROMs or homebrew. No games or BIOS files are included.
- The 9ft 7in World's Largest Game Boy Color uses a Windows mini PC and a Colorlight X2S sending card instead. It uses the same Pico controller.

## Credits

- Built by **Nick Alvarez** (electronics, firmware, software) and **Paige Powell** (CAD, fabrication, art), with sound design by **Fabrizio D'Amelio**, button mechanisms by UC Davis volunteers, and the support of **Tim Keller** and Inventopia, Davis CA.
- The Colorlight packet format comes from the community's reverse-engineering of the 5A-75B/E80 receiver cards. Thanks to everyone who documented it.
- [libretro](https://www.libretro.com/), [mGBA](https://mgba.io/), [Gambatte](https://github.com/libretro/gambatte-libretro), [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) and [TinyUSB](https://github.com/hathach/tinyusb).
- Follow the builds: Instagram [@NickBuildStuff](https://instagram.com/NickBuildStuff) and [@ppow_creative](https://instagram.com/ppow_creative).

## License

MIT. See [LICENSE](LICENSE). Emulator cores are separate projects with their own licences; this frontend only loads them at runtime.
