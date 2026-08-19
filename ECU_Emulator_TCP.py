# -*- coding: utf-8 -*-
"""
KWP2000 ECU Emulator - TCP Loopback Version (No Virtual COM needed)

This version emulates ECU via TCP socket instead of serial port.
Much easier for testing without hardware!

Usage:
    python ECU_Emulator_TCP.py

Then connect ESP32 to: 127.0.0.1:5555
"""

import socket
import time
import threading
import sys
from random import randint

# Configuration
LISTEN_HOST = '127.0.0.1'
LISTEN_PORT = 5555
TOOL_ADDRESS = 0xF1
ECU_ADDRESS = 0x11
BAUDRATE = 10400

# KWP2000 Timing (milliseconds)
P2m = 40
P2M_ms = 1000
P3m = 50
P3M_ms = 1500
P4m = 10

# KWP2000 Service IDs
SERVICE_START_DIAGNOSTIC = 0x10
SERVICE_TESTER_PRESENT = 0x3E
SERVICE_READ_DATA_BY_ID = 0x22
SERVICE_READ_DTC = 0x18

# Dummy sensor data
dummy_sensor_data = {
    0x0001: [50, 25],           # Engine Speed (RPM)
    0x0002: [20],               # Throttle Position
    0x0003: [35, 64],           # Vehicle Speed
    0x0004: [45],               # Fuel Consumption
    0x0005: [85, 50],           # Engine Temperature
}

# Global state
clients = []
ECU_connected = False
clients_lock = threading.Lock()


def calculate_checksum(data):
    """Calculate XOR checksum"""
    checksum = 0
    for byte in data:
        checksum ^= byte
    return checksum


def send_frame(sock, frame_data):
    """Send KWP2000 frame"""
    frame = frame_data.copy()
    checksum = calculate_checksum(frame)
    frame.append(checksum)
    
    print(f"  [SEND] {' '.join([f'0x{b:02X}' for b in frame])}")
    sock.sendall(bytes(frame))
    time.sleep(P2m / 1000.0)


def handle_tester_present(sock):
    """Handle Tester Present (0x3E)"""
    print("  [ECU] Tester Present -> keep-alive OK")
    response = [0x02, TOOL_ADDRESS, ECU_ADDRESS, 0x7E, 0x00]
    send_frame(sock, response)


def handle_read_data_by_id(sock, data_id_high, data_id_low):
    """Handle Read Data By ID (0x22)"""
    data_id = (data_id_high << 8) | data_id_low
    print(f"  [ECU] Read Data By ID: 0x{data_id:04X}")
    
    response = [0x03, TOOL_ADDRESS, ECU_ADDRESS, 0x62, data_id_high, data_id_low]
    
    if data_id in dummy_sensor_data:
        response[0] += len(dummy_sensor_data[data_id])
        response.extend(dummy_sensor_data[data_id])
    else:
        response.extend([0x00, 0x00])
    
    send_frame(sock, response)


def handle_start_diagnostic(sock):
    """Handle Start Diagnostic (0x10)"""
    print("  [ECU] Start Diagnostic -> session opened")
    response = [0x06, TOOL_ADDRESS, ECU_ADDRESS, 0x50, 0x01, P2m, 
                int(P2M_ms / 25), P3m, int(P3M_ms / 25), P4m]
    send_frame(sock, response)


def handle_read_dtc(sock):
    """Handle Read DTC (0x18)"""
    print("  [ECU] Read DTC -> no errors")
    response = [0x02, TOOL_ADDRESS, ECU_ADDRESS, 0x58, 0x00]
    send_frame(sock, response)


def process_frame(sock, frame):
    """Process KWP2000 frame"""
    if len(frame) < 4:
        print(f"  [ERROR] Frame too short: {len(frame)} bytes")
        return
    
    length = frame[0]
    target = frame[1]
    sender = frame[2]
    service_id = frame[3]
    
    print(f"  [RECV] Service 0x{service_id:02X}, Length {length}, Data: " + 
          ' '.join([f'0x{b:02X}' for b in frame[4:-1]]))
    
    if service_id == SERVICE_START_DIAGNOSTIC:
        handle_start_diagnostic(sock)
    elif service_id == SERVICE_TESTER_PRESENT:
        handle_tester_present(sock)
    elif service_id == SERVICE_READ_DATA_BY_ID:
        if len(frame) >= 6:
            handle_read_data_by_id(sock, frame[4], frame[5])
    elif service_id == SERVICE_READ_DTC:
        handle_read_dtc(sock)
    else:
        print(f"  [WARNING] Unknown service: 0x{service_id:02X}")


def handle_client(client_sock, addr):
    """Handle single client connection"""
    global ECU_connected
    
    print(f"\n[CLIENT] Connected from {addr}")
    ECU_connected = True
    last_data = time.time()
    
    try:
        while True:
            # Timeout check
            if time.time() - last_data > (P3M_ms / 1000.0):
                print(f"[CLIENT] Timeout from {addr}")
                break
            
            try:
                # Receive frame length
                data = client_sock.recv(1)
                if not data:
                    break
                
                length = data[0]
                frame = [length]
                
                # Receive rest of frame (+ checksum)
                for i in range(length + 2):
                    data = client_sock.recv(1)
                    if not data:
                        break
                    frame.append(data[0])
                
                # Verify checksum
                received_cs = frame[-1]
                calculated_cs = calculate_checksum(frame[:-1])
                
                if received_cs != calculated_cs:
                    print(f"  [ERROR] Checksum mismatch: 0x{received_cs:02X} != 0x{calculated_cs:02X}")
                    continue
                
                print(f"\n[CLIENT from {addr}]")
                process_frame(client_sock, frame)
                last_data = time.time()
            
            except socket.timeout:
                pass
            except Exception as e:
                print(f"  [ERROR] Exception: {e}")
                break
    
    finally:
        ECU_connected = False
        client_sock.close()
        print(f"[CLIENT] Disconnected from {addr}\n")


def main():
    """Main server loop"""
    print("=" * 60)
    print("Honda CB150R K15 ECU Emulator - TCP Loopback")
    print("=" * 60)
    print(f"Listening on {LISTEN_HOST}:{LISTEN_PORT}")
    print(f"ECU Address: 0x{ECU_ADDRESS:02X}")
    print(f"Tool Address: 0x{TOOL_ADDRESS:02X}")
    print("=" * 60)
    print("\nConfigurasi ESP32:")
    print(f"  - Host: {LISTEN_HOST}")
    print(f"  - Port: {LISTEN_PORT}")
    print("\n[WAITING] Untuk ESP32 connect...\n")
    
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        server_sock.bind((LISTEN_HOST, LISTEN_PORT))
        server_sock.listen(5)
        
        while True:
            try:
                client_sock, addr = server_sock.accept()
                client_sock.settimeout(0.1)
                
                # Handle client in separate thread
                client_thread = threading.Thread(target=handle_client, args=(client_sock, addr))
                client_thread.daemon = True
                client_thread.start()
            
            except KeyboardInterrupt:
                break
            except Exception as e:
                print(f"[ERROR] Accept exception: {e}")
    
    except Exception as e:
        print(f"[ERROR] Server error: {e}")
    
    finally:
        server_sock.close()
        print("[INFO] Server shutdown")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[INFO] Shutting down...")
        sys.exit(0)
