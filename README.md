# STM32F3 Custom USB Bootloader

A custom USB-CDC bootloader for the **STM32F303VCTx** (STM32F3 Discovery board).
Firmware updates are delivered over USB full-speed CDC (virtual COM port) using a
small framed, CRC-protected command protocol. A Python script (`flasher/flash.py`)
drives the whole update: it asks the running application to reboot into
bootloader mode, streams the new `.bin`, verifies it, and boots it.

## Flash memory map

```
0x08000000  ┌──────────────────────────┐
            │  Bootloader              │  64 KB  (linker script caps FLASH at 64 K)
0x08010000  ├──────────────────────────┤
            │  Application             │  ~190 KB writable
0x0803F800  ├──────────────────────────┤
            │  Metadata page           │  magic "BLDR" + image size + image CRC32
0x08040000  └──────────────────────────┘
```

`CCMRAM` byte 0 (`0x10000000`) holds a **magic word** used to signal "enter update
mode" across a warm `NVIC_SystemReset()`. CCM RAM contents survive a system
reset, and nothing in the linker script touches that address, so the flag is
preserved between the application and the bootloader.

## Boot decision logic (Bootloader `main.c`)

On every reset:

1. Pull **PA12 (USB D+)** low for 50 ms so Windows sees a clean USB disconnect.
2. Init clocks, GPIO, I2C1, SPI1, and the USB CDC device.
3. Check the magic word:
   - `== 0xDEADBEEF` → clear it, stay in **bootloader mode** (update requested).
   - Otherwise, if **B1 button (PA0)** is **not** pressed → validate the stored
     image (`BL_ImageIsValid()`: metadata magic + CRC32 over the whole image)
     and `JumpToApplication()` if intact.
   - B1 pressed, or image invalid → stay in **bootloader mode**.
4. In bootloader mode: start the **IWDG** (~9.6 s timeout) as a safety net — if
   an update stalls, the MCU resets and the normal boot path retries.
5. Main loop: feed IWDG, fast-blink the red LED (LD3 / PE9), and drain incoming
   protocol frames via `BL_ProtocolPoll()`.

The jump path de-initializes USB/clocks/interrupts before handing control to
the application's vector table at `0x08010000`.

## Wire protocol

Both directions use the same frame (little-endian):

```
[TYPE_OR_STATUS:1][LEN:4][PAYLOAD:LEN][CRC32:4]
```

CRC32 is zlib-compatible and covers `TYPE_OR_STATUS + LEN + PAYLOAD`.
Max payload: **1024 bytes** (`BL_MAX_PAYLOAD_SIZE`).

USB CDC FS delivers at most 64 bytes per packet, so `CDC_Receive_FS()` pushes
raw bytes into a 2 KB ring buffer (`BL_OnUsbRx()`, ISR context) and the main
loop reassembles complete frames.

### Commands (host → device)

| Cmd   | Value | Payload                       |
|-------|-------|-------------------------------|
| PING  | 0x01  | —                             |
| ERASE | 0x02  | u32 image size                |
| WRITE | 0x03  | ≤1024 bytes of image data     |
| VERIFY| 0x04  | u32 CRC32 of whole image      |
| JUMP  | 0x05  | —                             |

### Status codes (device → host)

`0x00` OK, `0x01` ERR_CRC, `0x02` ERR_BOUNDS, `0x03` ERR_FLASH,
`0x04` ERR_VERIFY, `0x05` ERR_STATE, `0x06` ERR_UNKNOWN_CMD, `0x07` ERR_PARAM.

### Update sequence

`PING` → `ERASE(size)` → `WRITE` × N → `VERIFY(crc32)` → `JUMP`

State machine on device: `IDLE → ERASED → WRITTEN → VERIFIED` (JUMP requires
VERIFIED). After VERIFY, a metadata record is written to the last flash page so
the image can be re-validated on every power-up.

## Triggering an update

The running **application** accepts the text command `UPDATE` on its USB CDC
interface. On receipt (handled directly in `CDC_Receive_FS()` interrupt
context) it writes `0xDEADBEEF` to `0x10000000` and calls `NVIC_SystemReset()`,
which lands the bootloader in update mode as described above.

If the application is not running (or has no USB trigger), hold **B1 (PA0)**
during reset, or just reset the board — if no valid image is present the
bootloader stays in update mode anyway.

## Host-side flasher

`flasher/flash.py` — Python 3 + `pyserial`.

```
python flash.py                          # defaults: COM7, 115200, Application.bin
python flash.py --port COM5 --file app.bin
```

Flow: sends `"UPDATE"` to the app port → waits for USB re-enumeration →
`PING` handshake → erase → stream writes → verify → jump.

## LEDs

| LED        | Meaning                                          |
|------------|--------------------------------------------------|
| LD3 red    | Bootloader mode — fast blink in main loop        |
| LD4 blue   | (debug) toggles when USB RX data reaches the ring buffer |
| LD6 green  | (debug) toggles on each CRC-valid command frame  |
| App LEDs   | Application-dependent (demo: `leds.c` cycles LD3–LD10) |

The LD4/LD6 toggles are temporary diagnostics in `bl_protocol.c` — remove them
for production.

## Repository layout

```
Bootloader/     CubeIDE project — bootloader firmware
  Core/Src/main.c            boot decision, jump, IWDG
  Core/Src/bl_protocol.c     frame reassembly, command handlers, flash writes
  USB_DEVICE/                USB CDC device stack (ST middleware)
Application/    CubeIDE project — application firmware (linked at 0x08010000)
  Core/Src/main.c            VTOR remap, app logic, watchdog feed
  USB_DEVICE/App/usbd_cdc_if.c   "UPDATE" trigger handling (in RX ISR)
flasher/flash.py             host update script
bootloader_doc.pdf           design documentation
```

## Building

Both projects are STM32CubeIDE projects (GCC toolchain, generated linker
scripts). Build each normally and program the Bootloader via ST-Link /
STM32CubeProgrammer. The Application is then flashed **only** over USB with
`flash.py`.

Notes:

- The Application sets `SCB->VTOR = 0x08010000` on entry — required for
  interrupts to work after the jump.
- Once the bootloader starts the IWDG it can never be stopped; the application
  must keep refreshing it (`IWDG->KR = 0xAAAA`) or the board resets.

## Troubleshooting

- **"timed out waiting for bootloader response"** — the device is not in
  bootloader mode, or is hung. Check whether the red LED is blinking. If it is
  solid ON, `main()` never reached the loop (watchdog init, clock config).
- **Red LED blinks but no response** — bytes are arriving but frames aren't
  completing; check the LD4/LD6 diagnostic toggles, or USB RX path.
- **COM port opens but never enumerates as bootloader** — ensure PA12 (D+)
  gets pulled low at boot and that no other terminal has the port open.
- **Watchdog resets (~9.6 s)** — an update stalled mid-way; just re-run
  `flash.py`, or hold B1 to force bootloader mode.
