import serial
import time
import sys
import os

# --- Configuration ---
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
            if b'OK' in buf:
                return True
        time.sleep(0.001)
    return False

def trigger_bootloader():
    print(f"Connecting to Application on {COM_PORT} to trigger update...")
    try:
        with serial.Serial(COM_PORT, BAUD_RATE, timeout=1) as ser:
            ser.write(b'UPDATE')
            ser.flush()
            print("Trigger sent. Device is rebooting into Bootloader mode...")
    except serial.SerialException:
        print("Could not connect. Assuming device is already in Bootloader mode.")

def wait_for_reconnect():
    print("Waiting for USB to re-enumerate", end="")
    # Try to connect repeatedly for up to 10 seconds
    for _ in range(10):
        try:
            ser = serial.Serial(COM_PORT, BAUD_RATE, timeout=1)
            ser.close()
            print("\nReconnected to Bootloader!")
            return True
        except serial.SerialException:
            print(".", end="")
            sys.stdout.flush()
            time.sleep(1)
    
    print("\nTimeout: Device did not return.")
    return False

def upload_firmware():
    if not os.path.exists(FILE_PATH):
        print(f"Error: Could not find {FILE_PATH}")
        sys.exit(1)

    file_size = os.path.getsize(FILE_PATH)
    
    # --- STAGE 1: Wake up the Bootloader ---
    trigger_bootloader()
    
    # Wait for the Windows USB driver to catch up
    time.sleep(1.5) 
    if not wait_for_reconnect():
        sys.exit(1)

    # --- STAGE 2: Flash the Firmware ---
    print(f"Target File: {FILE_PATH} ({file_size} bytes)")
    try:
        with serial.Serial(COM_PORT, BAUD_RATE, timeout=1) as ser:
            ser.reset_input_buffer()
            ser.reset_output_buffer()
            time.sleep(0.1)

            print("Starting upload...")
            with open(FILE_PATH, 'rb') as f:
                bytes_sent = 0
                while True:
                    chunk = f.read(CHUNK_SIZE)
                    if not chunk:
                        break

                    ser.write(chunk)
                    ser.flush()

                    if not wait_for_ack(ser, timeout=5.0):
                        print(f"\nTimeout waiting for ACK after {bytes_sent} bytes")
                        sys.exit(1)

                    bytes_sent += len(chunk)
                    progress = (bytes_sent / file_size) * 100
                    sys.stdout.write(f"\rProgress: [{bytes_sent}/{file_size} bytes] {progress:.1f}%")
                    sys.stdout.flush()

            # --- STAGE 3: Execute the Jump ---
            print("\n\nUpload complete! Sending boot command...")
            ser.write(b'JUMP')
            ser.flush()
            
            # Force Python to keep the port open until the STM32 processes the command
            print("Waiting for boot acknowledgment...", end="")
            start_time = time.time()
            boot_ack = b''
            while time.time() - start_time < 3.0:
                if ser.in_waiting:
                    boot_ack += ser.read(ser.in_waiting)
                    if b'BOOT' in boot_ack:
                        print(" Acknowledged!")
                        break
                time.sleep(0.01)

            print("\nApplication should now be running seamlessly!")

    except Exception as e:
        print(f"\nUnexpected error: {e}")

if __name__ == '__main__':
    upload_firmware()