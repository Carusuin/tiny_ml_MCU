# Testing dengan Serial Port (Real Hardware atau Virtual COM)

## Jika ingin test dengan real hardware K-Line (Honda CB150R)

### Hardware Requirements
- L9636 atau L9637 IC (K-Line interface, 12V → 3.3V converter)
- K-Line cable dari Honda CB150R ECU
- ESP32 board

### Wiring
```
Honda CB150R ECU
    |
    ├─ K-Line (12V)
    |
    └─ GND

        ↓ (via L9637 IC)

ESP32
    ├─ GPIO16 (RX)
    ├─ GPIO17 (TX)
    └─ GND
```

### Catatan
- **GPIO 3.3V ↔ 12V K-Line**: Perlu IC interface (L9637)
- **Direct connect tidak boleh** - akan rusak GPIO!
- L9637 bisa didapat dari Aliexpress ~$2-5

---

## Testing dengan Virtual Serial Port (Windows)

Jika tidak ada hardware, bisa pakai virtual COM port pair untuk simulasi.

### Setup Virtual COM (Windows)

**Option 1: com0com (Free, Open Source) - RECOMMENDED**

1. Download dari: https://sourceforge.net/projects/com0com/
2. Install
3. Jalankan "Setup" (administrator)
4. Klik "Add pair"
5. Create: `COM3 ↔ COM4`
6. OK

**Verifikasi:**
```cmd
mode COM3
mode COM4
```
Kedua harus "ready"

**Option 2: Virtual Serial Port Emulator (Berbayar)**
- https://www.eterlogic.com/Products.VSPE.html
- Setup port pair dari GUI

---

## Testing Setup: Serial Port Version

### File yang dipakai:
- `ECU_Emulator_KWP2000.py` - Emulator (10400 baud)

### Step-by-Step

#### 1. Setup Virtual COM (jika tidak ada hardware)

```cmd
# Windows - setup com0com untuk create COM3 ↔ COM4
# Atau gunakan hardware real untuk direct K-Line
```

#### 2. Terminal 1 - ECU Emulator

Edit `ECU_Emulator_KWP2000.py` line 28:

```python
default_port = 'COM3'  # Ubah ke port Anda
```

Jalankan:
```bash
python ECU_Emulator_KWP2000.py
```

Output:
```
============================================================
Honda CB150R K15 ECU Emulator - KWP2000 Protocol
============================================================
Configuration:
  Port: COM3
  Baudrate: 10400
  ...
============================================================

[WAITING] Listening for ESP32 connection...
```

#### 3. Terminal 2 - Build & Flash

Edit `main/main.c` - pastikan GPIO sesuai (16, 17 sudah benar).

Jika testing dengan virtual COM:
- Ubah main.c untuk connect ke `COM4` (pasangan dari COM3)
- Atau ubah UART pin ke GPIO yang connect ke COM4

Untuk testing real hardware:
- Biarkan GPIO 16, 17 (ke K-Line IC)

Build & Flash:
```bash
. C:\esp\v6.0.2\esp-idf\export.ps1
cd d:\tinyml_classificator\tinyml
idf.py build
idf.py -p COM5 flash       # Flash ke ESP32 USB port
idf.py -p COM5 monitor
```

#### 4. Lihat Output

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
```

---

## Troubleshooting Serial Port

| Issue | Solusi |
|-------|--------|
| "Port already in use" | Close aplikasi lain yang pakai port |
| "Timeout waiting" | Cek baud rate, GPIO connection, cable |
| "Wrong checksum" | Verify XOR calculation |
| Virtual COM tidak muncul | Restart com0com, jalankan sebagai admin |
| "Permission denied" | Run as Administrator |
| UART data corrupt | Reduce cable length, add shielding |

---

## Port Assignment

```
PC Serial Ports:
  COM1 - System (reserved)
  COM2 - Available
  COM3 - Virtual COM (ECU Emulator)  ← ECU side
  COM4 - Virtual COM (ESP32 emulated) ← ESP32 side
  COM5 - ESP32 USB/UART connection ← Actual ESP32 Flash/Monitor

Jika pakai real hardware:
  COM5 - ESP32 USB
  K-Line - Direct connect via L9637 IC
```

---

## Command Summary

```bash
# Terminal 1 - ECU Emulator (Serial)
python ECU_Emulator_KWP2000.py

# Terminal 1 - ECU Emulator (TCP - RECOMMENDED)
python ECU_Emulator_TCP.py

# Terminal 2 - Setup IDF
. C:\esp\v6.0.2\esp-idf\export.ps1

# Terminal 2 - Build
idf.py build

# Terminal 2 - Flash (ubah COM5 ke port Anda)
idf.py -p COM5 flash

# Terminal 2 - Monitor (ubah COM5 ke port Anda)
idf.py -p COM5 monitor

# Check available serial ports
idf.py serial-ports
```

---

## Recommendation

**Untuk testing pertama kali:**
1. Gunakan `ECU_Emulator_TCP.py` (paling mudah, tidak perlu virtual COM)
2. Jika ingin test serial port version, setup virtual COM terlebih dahulu
3. Kalau sudah confirm Tester Present works, lanjut ke real hardware

**Untuk production (real Honda CB150R):**
1. Setup hardware K-Line + L9637 IC
2. Gunakan `ECU_Emulator_KWP2000.py` atau langsung test dengan real ECU
3. Monitor serial output untuk debug
