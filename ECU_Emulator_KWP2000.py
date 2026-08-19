# -*- coding: utf-8 -*-
"""
Modified ECU Emulator for Honda CB150R K15 testing with KWP2000 protocol
Based on original by Vincenzo G.

Modifications:
- Changed baud rate from 115200 to 10400
- Updated for standard KWP2000 protocol
- Added Tester Present response support
- Added Read Data By ID support

Module needed:
    pyserial
    
Usage:
    python ECU_Emulator_KWP2000.py COM3
    (or auto-detect COM port)
"""

import serial
import time
import sys
from random import randint

# Configuration
default_port = 'COM3'  # Change this to your COM port (COM1, COM2, COM3, etc)
TOOL_ADDRESS = 0xF1
ECU_ADDRESS = 0x11  # Honda CB150R ECU address
BAUDRATE = 10400
PARITY = serial.PARITY_ODD
STOPBITS = serial.STOPBITS_1
BYTESIZE = serial.EIGHTBITS

# KWP2000 Timing parameters (milliseconds)
P2m = 40      # Time between bytes
P2M_ms = 1000  # Max time for response
P3m = 50      # Time between frames
P3M_ms = 1500  # Max time to complete frame
P4m = 10      # Inter-byte time

# Calculate P2M and P3M hex values
if P2M_ms <= 6000:
    P2M_hex = int(P2M_ms / 25)
elif P2M_ms > 6000 and P2M_ms <= 89600:
    P2M_hex = int(P2M_ms / 256 / 25) | 0xf0
else:
    print("P2M time too long")
    sys.exit()

if P3M_ms <= 6000:
    P3M_hex = int(P3M_ms / 25)
elif P3M_ms > 6000 and P3M_ms <= 89600:
    P3M_hex = int(P3M_ms / 256 / 25) | 0xf0
else:
    print("P3M time too long")
    sys.exit()

# KWP2000 Service IDs
SERVICE_START_DIAGNOSTIC = 0x10
SERVICE_TESTER_PRESENT = 0x3E
SERVICE_READ_DATA_BY_ID = 0x22
SERVICE_READ_DTC = 0x18

# Frame definitions - KWP2000 format: [length, target, sender, service, data..., checksum]

# Start diagnostic session (initialization)
start_diagnostic = [0x02, ECU_ADDRESS, TOOL_ADDRESS, SERVICE_START_DIAGNOSTIC, 0x01]
start_diagnostic_ok = [0x06, TOOL_ADDRESS, ECU_ADDRESS, 0x50, 0x01, P2m, P2M_hex, P3m, P3M_hex, P4m]

# Tester Present (keep-alive)
tester_present = [0x02, ECU_ADDRESS, TOOL_ADDRESS, SERVICE_TESTER_PRESENT, 0x00]
tester_present_ok = [0x02, TOOL_ADDRESS, ECU_ADDRESS, 0x7E, 0x00]

# Read Data By ID responses
read_data_ok = [0x05, TOOL_ADDRESS, ECU_ADDRESS, 0x62, 0x00, 0x00]  # Will add PID data

# Dummy sensor data (Honda CB150R-like values)
dummy_sensor_data = {
    0x0001: [50, 25],           # Engine Speed (RPM) - 50 hex = 5000 RPM
    0x0002: [20],               # Throttle Position
    0x0003: [35, 64],           # Vehicle Speed
    0x0004: [45],               # Fuel Consumption
    0x0005: [85, 50],           # Engine Temperature
}

# Global variables
request = []
ECU_connected = False
last_byte = 0
last_data_received = time.time()
ser = None


def calculate_checksum(arr):
    """Calculate XOR checksum for KWP2000 frame"""
    checksum = 0
    for byte in arr:
        checksum ^= byte
    return checksum


def send_frame(frame_data):
    """Send KWP2000 frame via serial"""
    frame = frame_data.copy()
    
    # Calculate and append checksum
    checksum = calculate_checksum(frame)
    frame.append(checksum)
    
    print(f"[SEND] {' '.join([f'0x{b:02X}' for b in frame])}")
    
    # Send byte by byte with P4m timing
    for byte in frame:
        ser.write(bytes([byte]))
        time.sleep(P4m / 1000.0)
    
    time.sleep(P2m / 1000.0)


def compare_frame(received, expected):
    """Compare received frame with expected frame"""
    if len(received) < len(expected):
        return False
    
    for i, byte in enumerate(expected):
        try:
            if received[i] != byte:
                return False
        except IndexError:
            return False
    
    return True


def listen():
    """Listen for incoming data from serial port"""
    global last_data_received, last_byte, request
    
    if ser.in_waiting == 0:
        return False
    
    request.clear()
    byte_count = 0
    last_byte = -1
    start_time = time.time()
    
    try:
        while time.time() - start_time < (P3M_ms / 1000.0):
            if ser.in_waiting > 0:
                in_byte = ser.read(1)[0]
                request.append(in_byte)
                
                print(f"[RECV] Byte {byte_count}: 0x{in_byte:02X}")
                
                # Parse frame length
                if byte_count == 0:
                    # First byte is length
                    last_byte = in_byte + 2  # Add for header byte + checksum
                
                # Check if we have complete frame
                if byte_count == last_byte and last_byte > 0:
                    # Verify checksum
                    received_checksum = request[-1]
                    calculated_checksum = calculate_checksum(request[:-1])
                    
                    print(f"[RECV] Checksum: received 0x{received_checksum:02X}, calculated 0x{calculated_checksum:02X}")
                    
                    if received_checksum == calculated_checksum:
                        last_data_received = time.time()
                        print(f"[RECV] Valid frame: {' '.join([f'0x{b:02X}' for b in request])}\n")
                        return True
                    else:
                        print("[ERROR] Checksum mismatch!\n")
                        return False
                
                byte_count += 1
                time.sleep(P4m / 1000.0)
            else:
                time.sleep(1)
    
    except Exception as e:
        print(f"[ERROR] Listen exception: {e}")
        return False
    
    return False


def handle_tester_present():
    """Handle Tester Present (0x3E) - keep-alive"""
    print("[KWP2000] Tester Present received")
    response = tester_present_ok.copy()
    send_frame(response)


def handle_read_data_by_id(data_id_high, data_id_low):
    """Handle Read Data By ID (0x22)"""
    data_id = (data_id_high << 8) | data_id_low
    print(f"[KWP2000] Read Data By ID: 0x{data_id:04X}")
    
    response = [0x03, TOOL_ADDRESS, ECU_ADDRESS, 0x62]  # Response to 0x22
    response.append(data_id_high)
    response.append(data_id_low)
    
    # Add dummy data if available
    if data_id in dummy_sensor_data:
        response[0] += len(dummy_sensor_data[data_id])  # Update length
        response.extend(dummy_sensor_data[data_id])
    else:
        # Return some default data
        response.extend([0x00, 0x00])
    
    send_frame(response)


def handle_start_diagnostic():
    """Handle Start Diagnostic Session (0x10)"""
    print("[KWP2000] Start Diagnostic Session received")
    response = start_diagnostic_ok.copy()
    send_frame(response)


def handle_read_dtc():
    """Handle Read DTC (0x18)"""
    print("[KWP2000] Read DTC received")
    response = [0x02, TOOL_ADDRESS, ECU_ADDRESS, 0x58]  # Response to 0x18
    response.append(0x00)  # No errors
    send_frame(response)


def process_frame(frame):
    """Process received KWP2000 frame"""
    global ECU_connected
    
    if len(frame) < 4:
        print("[ERROR] Frame too short")
        return
    
    length = frame[0]
    target = frame[1]
    sender = frame[2]
    service_id = frame[3]
    
    print(f"[KWP2000] Service: 0x{service_id:02X}, Length: {length}, Data: {' '.join([f'0x{b:02X}' for b in frame[4:-1]])}")
    
    if service_id == SERVICE_START_DIAGNOSTIC:
        ECU_connected = True
        handle_start_diagnostic()
    
    elif service_id == SERVICE_TESTER_PRESENT:
        handle_tester_present()
    
    elif service_id == SERVICE_READ_DATA_BY_ID:
        if len(frame) >= 6:
            data_id_high = frame[4]
            data_id_low = frame[5]
            handle_read_data_by_id(data_id_high, data_id_low)
    
    elif service_id == SERVICE_READ_DTC:
        handle_read_dtc()
    
    else:
        print(f"[WARNING] Unknown service ID: 0x{service_id:02X}")


def main():
    """Main emulator loop"""
    global ser, last_data_received
    
    print("=" * 60)
    print("Honda CB150R K15 ECU Emulator - KWP2000 Protocol")
    print("=" * 60)
    print(f"Configuration:")
    print(f"  Port: {default_port}")
    print(f"  Baudrate: {BAUDRATE}")
    print(f"  Parity: ODD")
    print(f"  Stop Bits: 1")
    print(f"  Byte Size: 8")
    print(f"  ECU Address: 0x{ECU_ADDRESS:02X}")
    print(f"  Tool Address: 0x{TOOL_ADDRESS:02X}")
    print("=" * 60 + "\n")
    
    try:
        # Open serial port
        ser = serial.Serial(
            port=default_port,
            baudrate=BAUDRATE,
            parity=PARITY,
            stopbits=STOPBITS,
            bytesize=BYTESIZE,
            timeout=2
        )
        print(f"[SUCCESS] Serial port opened: {ser.name}")
        print(f"{ser}\n")
        
    except Exception as e:
        print(f"[ERROR] Could not open serial port {default_port}: {e}")
        print("[INFO] Available COM ports:")
        import platform
        if platform.system() == "Windows":
            print("  Check Device Manager -> Ports (COM & LPT)")
        else:
            print("  ls /dev/tty* or ls /dev/ttyUSB*")
        sys.exit(1)
    
    last_data_received = time.time()
    
    try:
        print("[WAITING] Listening for ESP32 connection...\n")
        
        while True:
            # Check if connection timed out
            if time.time() - last_data_received > (P3M_ms / 1000.0):
                if ECU_connected:
                    print(f"\n[INFO] Connection timeout after {P3M_ms/1000.0}s")
                    ECU_connected = False
            
            # Listen for incoming frame
            if listen():
                process_frame(request)
            else:
                time.sleep(0.01)
    
    except KeyboardInterrupt:
        print("\n[INFO] Shutting down...")
    
    finally:
        if ser and ser.is_open:
            ser.close()
            print("[INFO] Serial port closed")


if __name__ == "__main__":
    # Allow command line argument for COM port
    if len(sys.argv) > 1:
        default_port = sys.argv[1]
    
    main()
