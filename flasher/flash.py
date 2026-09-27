#!/usr/bin/env python3
"""STM32F3 USB-CDC bootloader flasher.

Wire protocol (both directions):
    [TYPE_OR_STATUS:1][LEN:4 LE][PAYLOAD:LEN][CRC32:4 LE]
CRC32 is zlib-compatible and covers TYPE_OR_STATUS + LEN + PAYLOAD.

Usage:
    python flash.py                      # flash default binary to COM7
    python flash.py --port COM5 --file path/to/app.bin
"""

import argparse
import os
import struct
import sys
import time
import zlib

import serial

# --- Commands (host -> device) ---
CMD_PING   = 0x01
CMD_ERASE  = 0x02   # payload: u32 LE image size
CMD_WRITE  = 0x03   # payload: up to MAX_PAYLOAD bytes
CMD_VERIFY = 0x04   # payload: u32 LE crc32 of whole image
CMD_JUMP   = 0x05

# --- Status codes (device -> host) ---
STATUS_NAMES = {
    0x00: "OK",
    0x01: "ERR_CRC",
    0x02: "ERR_BOUNDS",
    0x03: "ERR_FLASH",
    0x04: "ERR_VERIFY",
    0x05: "ERR_STATE",
    0x06: "ERR_UNKNOWN_CMD",
    0x07: "ERR_PARAM",
}
STATUS_OK = 0x00

MAX_PAYLOAD = 1024          # must match BL_MAX_PAYLOAD_SIZE on the device
DEFAULT_FILE = r"D:\STM32_Discovery_Projects\Custom_Bootloader\Application\Debug\Application.bin"


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    header = bytes([cmd]) + struct.pack("<I", len(payload))
    return header + payload + struct.pack("<I", crc32(header + payload))


def read_response(ser: serial.Serial, timeout: float = 5.0):
    """Read one framed response. Returns (status, payload_bytes)."""
    buf = bytearray()
    start = time.time()
    while time.time() - start < timeout:
        if ser.in_waiting:
            buf += ser.read(ser.in_waiting)
            if len(buf) >= 5:
                (length,) = struct.unpack("<I", bytes(buf[1:5]))
                if len(buf) >= 5 + length + 4:
                    frame = bytes(buf[: 5 + length + 4])
                    expected = struct.unpack("<I", frame[-4:])[0]
                    if crc32(frame[:-4]) != expected:
                        raise RuntimeError("corrupt response frame (bad CRC)")
                    return frame[0], frame[5 : 5 + length]
        else:
            time.sleep(0.001)
    raise RuntimeError("timed out waiting for bootloader response")


def send_cmd(ser: serial.Serial, cmd: int, payload: bytes = b"", timeout: float = 5.0) -> bytes:
    ser.write(build_frame(cmd, payload))
    ser.flush()
    status, data = read_response(ser, timeout)
    if status != STATUS_OK:
        name = STATUS_NAMES.get(status, f"0x{status:02X}")
        raise RuntimeError(f"command 0x{cmd:02X} failed with status {name}")
    return data


def trigger_bootloader(port: str, baud: int):
    print(f"Connecting to Application on {port} to trigger update...")
    try:
        with serial.Serial(port, baud, timeout=1) as ser:
            ser.write(b"UPDATE")
            ser.flush()
            print("Trigger sent. Device is rebooting into Bootloader mode...")
    except serial.SerialException:
        print("Could not connect. Assuming device is already in Bootloader mode.")


def wait_for_reconnect(port: str, baud: int) -> bool:
    print("Waiting for USB to re-enumerate", end="")
    for _ in range(10):
        try:
            ser = serial.Serial(port, baud, timeout=1)
            ser.close()
            print("\nReconnected to Bootloader!")
            return True
        except serial.SerialException:
            print(".", end="")
            sys.stdout.flush()
            time.sleep(1)
    print("\nTimeout: Device did not return.")
    return False


def ping_bootloader(ser: serial.Serial):
    """Handshake: confirm we are really talking to the bootloader."""
    for attempt in range(5):
        try:
            data = send_cmd(ser, CMD_PING, timeout=2.0)
            if len(data) >= 2:
                version = struct.unpack("<H", data[:2])[0]
                print(f"Bootloader v{version >> 8}.{version & 0xFF} ready.")
                return
            raise RuntimeError("unexpected PING payload")
        except RuntimeError:
            if attempt == 4:
                raise
            time.sleep(0.5)


def upload_firmware(port: str, baud: int, file_path: str):
    if not os.path.exists(file_path):
        print(f"Error: Could not find {file_path}")
        sys.exit(1)

    with open(file_path, "rb") as f:
        image = f.read()
    file_size = len(image)
    image_crc = crc32(image)

    # --- STAGE 1: Wake up the Bootloader ---
    trigger_bootloader(port, baud)

    # Wait for the Windows USB driver to catch up
    time.sleep(1.5)
    if not wait_for_reconnect(port, baud):
        sys.exit(1)

    # --- STAGE 2: Flash the firmware ---
    print(f"Target File: {file_path} ({file_size} bytes), CRC32=0x{image_crc:08X}")
    try:
        with serial.Serial(port, baud, timeout=1) as ser:
            ser.reset_input_buffer()
            ser.reset_output_buffer()
            time.sleep(0.1)

            ping_bootloader(ser)

            print("Erasing application space...")
            send_cmd(ser, CMD_ERASE, struct.pack("<I", file_size), timeout=30.0)

            print("Starting upload...")
            bytes_sent = 0
            while bytes_sent < file_size:
                chunk = image[bytes_sent : bytes_sent + MAX_PAYLOAD]
                send_cmd(ser, CMD_WRITE, chunk)
                bytes_sent += len(chunk)
                progress = (bytes_sent / file_size) * 100
                sys.stdout.write(f"\rProgress: [{bytes_sent}/{file_size} bytes] {progress:.1f}%")
                sys.stdout.flush()

            print("\nVerifying image...")
            send_cmd(ser, CMD_VERIFY, struct.pack("<I", image_crc))
            print("Image verified.")

            # --- STAGE 3: Execute the jump ---
            print("Sending boot command...")
            send_cmd(ser, CMD_JUMP)

            print("\nApplication should now be running seamlessly!")

    except RuntimeError as e:
        print(f"\nUpdate failed: {e}")
        sys.exit(1)
    except serial.SerialException as e:
        print(f"\nSerial error: {e}")
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description="STM32F3 CDC bootloader flasher")
    parser.add_argument("--port", default="COM7", help="COM port (default: COM7)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--file", default=DEFAULT_FILE, help="Path to the .bin file to flash")
    args = parser.parse_args()

    upload_firmware(args.port, args.baud, args.file)


if __name__ == "__main__":
    main()
