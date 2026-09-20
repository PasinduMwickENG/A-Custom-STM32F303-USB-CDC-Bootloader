import serial
import time
import sys
import os

COM_PORT = 'COM7'
BAUD_RATE = 115200
FILE_PATH = r'D:\STM32_Discovery_Projects\Custom_Bootloader\Application\Debug\Application.bin'
CHUNK_SIZE = 64

def wait_for_ack(ser, timeout=5.0):
    start = time.time()
    buf = b''
    while time.time() - start < timeout:
        if ser.in_waiting:
            buf += ser.read(ser.in_waiting)
            if b'OK' in buf:          # or if b'\x06' in buf
                return True
        time.sleep(0.001)
    return False

def upload_firmware():
    if not os.path.exists(FILE_PATH):
        print(f"Error: Could not find {FILE_PATH}")
        sys.exit(1)

    file_size = os.path.getsize(FILE_PATH)
    print(f"Target File: {FILE_PATH} ({file_size} bytes)")
    print(f"Connecting to {COM_PORT}...")

    try:
        with serial.Serial(COM_PORT, BAUD_RATE, timeout=1) as ser:
            # Optional: clear any pending data
            ser.reset_input_buffer()
            ser.reset_output_buffer()
            time.sleep(0.1)

            print("Connected. Starting upload...")
            with open(FILE_PATH, 'rb') as f:
                bytes_sent = 0
                while True:
                    chunk = f.read(CHUNK_SIZE)
                    if not chunk:
                        break

                    ser.write(chunk)
                    ser.flush()

                    # Wait for ACK (the first one will take longer because of the erase)
                    if not wait_for_ack(ser, timeout=5.0):
                        print(f"\nTimeout waiting for ACK after {bytes_sent} bytes")
                        sys.exit(1)

                    bytes_sent += len(chunk)
                    progress = (bytes_sent / file_size) * 100
                    sys.stdout.write(f"\rProgress: [{bytes_sent}/{file_size} bytes] {progress:.1f}%")
                    sys.stdout.flush()

            print("\n\nUpload complete! Press the black RESET button (button released).")

    except serial.SerialException as e:
        print(f"\nSerial error: {e}")
    except Exception as e:
        print(f"\nUnexpected error: {e}")

if __name__ == '__main__':
    upload_firmware()