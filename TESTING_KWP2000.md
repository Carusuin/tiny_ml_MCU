# Testing KWP2000 dengan ECU Emulator

## Overview
Ini adalah setup untuk test komunikasi KWP2000 antara ESP32 dan ECU emulator (Python) **tanpa hardware real**.

## Profil timing dan pemetaan sensor (uji AGY)

Firmware saat ini memakai Fast Init LOW 25 ms + HIGH 25 ms, W4 0 ms, UART
10400 baud 8N1 inverted, timeout respons 120 ms, dan jeda TX 1 ms di antara
byte. ACK wake-up standar `0E 04 72 7C` tetap diterima; dua varian yang
dilaporkan AGY (`0E 04 72` tanpa checksum dan `72 8C`) juga diterima untuk uji.
Untuk tabel data ID `0x10`, pemetaan uji diterapkan langsung ke indeks response
yang dikembalikan (byte ID berada di indeks 0):

| Sensor | Indeks response | Konversi |
|--------|-----------------|----------|
| TPS | `0x05` | raw × 0,5% |
| RPM | `0x06`–`0x07` | unsigned 16-bit, byte besar lebih dulu |
| ECT/EOT | `0x08` | raw − 40 °C |
| MAP | `0x0B` | raw × 0,75 kPa |

Ini masih profil uji dari hasil analisis AGY, belum terkonfirmasi terhadap
respons ECU nyata. Verifikasi timing, format ACK, indeks, dan skala dengan data
respons aktual sebelum menganggap komunikasi atau nilai sensor tervalidasi.

## Requirements
1. **Python 3.6+** dengan library `pyserial`
2. **Virtual Serial Port Emulator** (untuk membuat COM port pair)
3. **ESP32 dengan KWP2000 component** (sudah dibuat)

## Setup Step-by-Step

### Step 1: Install Python Dependencies
```bash
pip install pyserial
```

### Step 2: Setup Virtual Serial Port Pair
Virtual serial port memungkinkan 2 aplikasi berkomunikasi via "serial port" yang sama tanpa hardware real.

**Opsi A: Windows - Pakai com0com (Recommended)**
- Download: https://sourceforge.net/projects/com0com/
- Install com0com
- Buka "Setup" → Create port pair, contoh:
  - `COM3 ↔ COM4`
- Test dengan: `mode COM3` (harus exist dan ready)

**Opsi B: Windows - Pakai Virtual Serial Port Emulator**
- Download: https://www.eterlogic.com/Products.VSPE.html
- Buat port pair
- Catatan: Perlu lisensi untuk versi penuh

**Opsi C: Linux - Pakai socat (Paling mudah)**
```bash
socat -d -d pty,raw,echo=0 pty,raw,echo=0
```
Output contoh:
```
2020/01/01 00:00:00 socat[12345] N PTY is /dev/pts/5
2020/01/01 00:00:00 socat[12345] N PTY is /dev/pts/6
```
Gunakan `/dev/pts/5` dan `/dev/pts/6`

### Step 3: Verifikasi Virtual Port
**Windows:**
```cmd
mode COM3
mode COM4
```
Kedua harus menunjukkan status "ready"

**Linux:**
```bash
ls -la /dev/pts/5 /dev/pts/6
```

### Step 4: Update Configuration

**File: `ECU_Emulator_KWP2000.py` (Line 28)**
```python
default_port = 'COM3'  # Ubah sesuai virtual port Anda
```

**File: `main/main.c` (Line 12-13)**
```c
#define KLINE_RX_GPIO  16  // GPIO connected to virtual port
#define KLINE_TX_GPIO  17  // GPIO connected to virtual port
```

**UNTUK TESTING (tanpa hardware):**
Karena ini virtual port, kita modifikasi slightly. Mari gunakan socket TCP loopback instead...

## SOLUSI LEBIH MUDAH: TCP Loopback (Recommended!)

Daripada pakai virtual port, kita buat wrapper yang:
1. ECU Emulator mendengar di TCP port 5555
2. ESP32 connect ke TCP 127.0.0.1:5555
3. Protocol sama, tapi via TCP socket, bukan serial

Ini lebih mudah dan tidak perlu virtual port!

### Buat ECU Emulator versi TCP

[See ECU_Emulator_TCP.py - coming next]

## Step 5: Build & Flash ESP32

### Terminal 1: ECU Emulator
```bash
python ECU_Emulator_KWP2000.py COM3
```
Output:
```
============================================================
Honda CB150R K15 ECU Emulator - KWP2000 Protocol
============================================================
[SUCCESS] Serial port opened: COM3
[WAITING] Listening for ESP32 connection...
```

### Terminal 2: Build & Flash
```bash
cd d:\tinyml_classificator\tinyml

# Setup IDF
. C:\esp\v6.0.2\esp-idf\export.ps1

# Build
idf.py build

# Flash
idf.py -p COM4 flash

# Monitor
idf.py -p COM4 monitor
```

### Terminal 3: Monitor ECU Emulator
Lihat output yang muncul di Terminal 1.

## Expected Output

**Terminal 2 (ESP32 Monitor):**
```
[APP_MAIN] Starting TinyML Classificator with KWP2000
[KWP2000] UART initialized on pins RX=16, TX=17, Baudrate=10400
[KWP2000] KWP2000 initialized
[KWP2000] KWP2000 task started on core 1
[APP_MAIN] Test KWP2000 task started on core 0
[APP_MAIN] Connected to Honda CB150R K15 ECU!
[APP_MAIN] Keep-alive OK
[APP_MAIN] Keep-alive OK
...
```

**Terminal 1 (ECU Emulator):**
```
[RECV] Byte 0: 0x02
[RECV] Byte 1: 0x11
[RECV] Byte 2: 0xF1
[RECV] Byte 3: 0x3E
[RECV] Byte 4: 0x00
[RECV] Byte 5: 0xCC
[RECV] Valid frame: 0x02 0x11 0xF1 0x3E 0x00 0xCC

[KWP2000] Tester Present received
[SEND] 0x02 0xF1 0x11 0x7E 0x00 0x6C
```

## Troubleshooting

### ECU Emulator says "Wrong checksum"
- Periksa KWP2000 checksum calculation
- Default: XOR semua byte sebelum checksum

### ESP32 tidak terkoneksi (timeout)
- Periksa baud rate: harus 10400
- Periksa GPIO pins: RX=16, TX=17
- Periksa frame format
- Lihat log di ESP32 monitor

### Virtual port tidak muncul
- Restart com0com/aplikasi
- Cek Device Manager pada Windows
- Linux: Pastikan socat running

### Permission denied pada COM port
- Windows: Jalankan command prompt sebagai Administrator
- Linux: `sudo chmod 666 /dev/pts/5`

## Next Steps

Setelah berhasil Tester Present, lanjutkan dengan:
1. Read Data By ID (sensor values)
2. Read DTC (diagnostic codes)
3. Write Data By ID (parameter adjustment)
4. Integrate dengan TinyML inference

## Files

- `ECU_Emulator_KWP2000.py` - Emulator Python (10400 baud)
- `main/main.c` - ESP32 KWP2000 client
- `components/kwp2000/` - KWP2000 library

Good luck! 🚀
