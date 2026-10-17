# Colorlight receiver card packets (5A-75B / E80)

These are notes on the "PC / netcard" frames the driver sends. They come from community reverse-engineering and our own testing with E80 and 5A-75B cards on firmware v13.x. They aren't official documentation.

All frames are raw Ethernet (no IP) with fixed addresses:

| Field | Value |
|---|---|
| Destination MAC | `11:22:33:44:55:66` |
| Source MAC | `22:22:33:44:55:66` |
| Byte 12 | Packet type |
| Byte 13 onward | Payload: `data[0]` is at byte 13, `data[N]` at byte 13+N |

## Pixel row: type `0x55`

| Bytes | Meaning |
|---|---|
| 13–14 | Row number (big-endian) |
| 15–16 | First pixel offset within the row |
| 17–18 | Pixel count |
| 19–20 | `0x08 0x88` |
| 21… | Pixels, 3 bytes each in **B, G, R** order |

One 192-pixel row is 14 + 7 + 576 = 597 bytes. The driver pre-builds one packet per row and only copies new pixel data in each frame.

## Display/sync: type `0x01`

Sent after all rows of a frame. It tells the card to show the frame it has buffered. The payload is 98 bytes:

| Byte | Value |
|---|---|
| 13 (`data[0]`) | `0x07` |
| 35 (`data[22]`) | Brightness |
| 36 (`data[23]`) | `0x05` |
| 38–40 (`data[25..27]`) | Brightness R, G, B |

v13 firmware is happiest when the sync packet is sent twice.

## Brightness: type `0x0A`

Payload of 63 bytes: `data[0..2]` = brightness for R, G, B, `data[3]` = `0xFF`. Sent at start-up. Brightness stored in the card's flash by LEDVision can override this, which is why the driver also has a software `--dim`.

## Timing lessons

- Sending rows back-to-back and the sync immediately afterwards made the **bottom row of panels flicker**. A 250–1,000 µs pause before the sync fixed it. The default is 500 µs.
- Batching rows into `sendmmsg()` calls of 32 (the panels' 1/32 scan) was smooth. One huge batch or one call per row both worked but weren't better.
- Configuring the card's refresh to 1,920 Hz (LEDVision Segments ×32) removed flicker on camera.
