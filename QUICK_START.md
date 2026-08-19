# Quick Start Guide - Testing KWP2000

## File yang Sudah Siap

✅ **Component KWP2000** (sudah compile)
- `components/kwp2000/include/kwp2000.h` - API header
- `components/kwp2000/src/kwp2000.c` - Implementasi protocol
- `components/kwp2000/CMakeLists.txt` - Build config

✅ **Main Application** (sudah update)
- `main/main.c` - Test application
- `main/CMakeLists.txt` - Updated dengan KWP2000 dependency

✅ **ECU Emulator untuk Testing** (2 pilihan)
- `ECU_Emulator_KWP2000.py` - Serial port version (perlu virtual COM)
- `ECU_Emulator_TCP.py` - TCP Loopback version (RECOMMENDED - paling mudah!)

✅ **Documentation**
- `TESTING_KWP2000.md` - Setup guide lengkap

---

## 🚀 QUICK START - Testing dengan TCP Emulator (Paling Mudah)

### 1. Install Python Dependencies
```bash
pip install pyserial
```

### 2. Terminal 1 - Jalankan ECU Emulator
```bash
python d:\tinyml_classificator\tinyml\ECU_Emulator_TCP.py
```

Harusnya muncul:
```
============================================================
Honda CB150R K15 ECU Emulator - TCP Loopback
============================================================
Listening on 127.0.0.1:5555
ECU Address: 0x11
Tool Address: 0xF1
============================================================

[WAITING] Untuk ESP32 connect...
```

### 3. Terminal 2 - Build & Flash ESP32
```bash
# Setup IDF
. C:\esp\v6.0.2\esp-idf\export.ps1

# Go to project
cd d:\tinyml_classificator\tinyml

# Build
idf.py build

# Flash (adjust COM port!)
idf.py -p COM3 flash

# Monitor
idf.py -p COM3 monitor
```

### 4. Lihat Output

**Di Terminal 1 (ECU Emulator):**
```
[CLIENT] Connected from ('127.0.0.1', 12345)

[CLIENT from ('127.0.0.1', 12345)]
  [RECV] Service 0x3E, Length 2, Data: 0x00
  [ECU] Tester Present -> keep-alive OK
  [SEND] 0x02 0xF1 0x11 0x7E 0x00 0x6C

[CLIENT from ('127.0.0.1', 12345)]
  [RECV] Service 0x3E, Length 2, Data: 0x00
  [ECU] Tester Present -> keep-alive OK
  [SEND] 0x02 0xF1 0x11 0x7E 0x00 0x6C
```

**Di Terminal 2 (ESP32 Monitor):**
```
ESP-ROM:esp32-20220511
Build:May 11 2022
rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_BOOT)
configsip: 0, SPIWP:0xee
clk_drv:0x00,q_drv:0x00,d_drv:0x00,cs0_drv:0x00,d_drv:0x00,cs1_drv:0x00
...

[APP_MAIN] Starting TinyML Classificator with KWP2000
[KWP2000] UART initialized on pins RX=16, TX=17, Baudrate=10400
[KWP2000] KWP2000 initialized
[KWP2000] KWP2000 task started on core 1
[APP_MAIN] Test KWP2000 task started on core 0
[APP_MAIN] Connected to Honda CB150R K15 ECU!
[APP_MAIN] Keep-alive OK
[APP_MAIN] Keep-alive OK
[APP_MAIN] Keep-alive OK
...
```

---

## 📝 Penjelasan Architecture

### **Execution Flow:**

```
ESP32 (Core 0) ←→ KWP2000 Component ←→ UART (GPIO 16, 17) ←→ TCP Loopback ←→ ECU Emulator Python
     Main Task         Library                Driver              Socket              Server
```

### **Task Distribution:**

- **Core 0** (main/app_test_kwp2000):
  - Mengirim Tester Present setiap 3 detik
  - Log hasil komunikasi
  - Nanti: TinyML inference + Web server

- **Core 1** (kwp2000_task):
  - Listen UART K-Line
  - Parse frame KWP2000
  - Push ke response queue

### **Timing (KWP2000):**
- P2m: 40ms (inter-byte delay)
- P2M: 1000ms (max response time)
- P3m: 50ms (inter-frame delay)
- P3M: 1500ms (max frame timeout)
- Baudrate: 10400 baud

---

## 🔧 Konfigurasi ESP32 untuk Real Hardware

Kalau nanti mau test dengan hardware real (K-Line IC):

1. **Hardware Setup:**
   - L9636 atau L9637 IC untuk K-Line interface
   - K-Line 12V → L9637 → GPIO 16 (RX)
   - K-Line 12V → L9637 → GPIO 17 (TX)
   - Ubah main.c:
     ```c
     #define KLINE_RX_GPIO  16
     #define KLINE_TX_GPIO  17
     #define KLINE_BAUDRATE 10400
     ```

2. **ECU Koneksi:**
   - Hubungkan K-Line ke Honda CB150R
   - Biarkan logic sama

---

## 📚 API KWP2000 yang Tersedia

```c
// Initialize KWP2000
void kwp2000_init(int rx_gpio, int tx_gpio, int baudrate);

// Send/Receive frame
bool kwp2000_send_frame(kwp2000_frame_t *frame);
bool kwp2000_receive_frame(kwp2000_frame_t *frame, uint32_t timeout_ms);

// High-level operations
bool kwp2000_tester_present(void);
bool kwp2000_read_data_by_id(uint16_t data_id, uint8_t *response, uint8_t *response_len);
bool kwp2000_write_data_by_id(uint16_t data_id, uint8_t *data, uint8_t data_len);
bool kwp2000_read_dtc(uint8_t *dtc_buffer, uint8_t *dtc_count);
bool kwp2000_clear_dtc(void);
bool kwp2000_start_diagnostic_session(uint8_t session_type);
QueueHandle_t kwp2000_get_response_queue(void);
```

---

## 🧪 Next Steps setelah Tester Present Berhasil

1. **Read RPM dari ECU:**
   ```c
   uint8_t response[10];
   uint8_t response_len;
   if (kwp2000_read_data_by_id(0x0001, response, &response_len)) {
       uint16_t rpm = (response[0] << 8) | response[1];
       printf("RPM: %d\n", rpm * 25);  // Honda: value * 25
   }
   ```

2. **Read DTC (Error Codes):**
   ```c
   uint8_t dtc_buffer[32];
   uint8_t dtc_count;
   if (kwp2000_read_dtc(dtc_buffer, &dtc_count)) {
       printf("DTC Count: %d\n", dtc_count);
   }
   ```

3. **Integrate dengan TinyML:**
   - Buat task di core 0 untuk inference
   - Read sensor data dari queue
   - Predict state kendaraan
   - Stream hasil ke web server

---

## 🚨 Troubleshooting

| Issue | Solusi |
|-------|--------|
| "Connection refused" di Terminal 2 | Pastikan ECU Emulator jalan di Terminal 1 |
| "No response" dari ECU | Cek baud rate 10400, cek checksum |
| Checksum mismatch di ECU | Verifikasi XOR algorithm di kwp2000.c |
| Timeout di ESP32 | Tambah timeout di kwp2000_receive_frame |
| Serial port error | Flash ke port yang benar, cek `idf.py serial-ports` |

---

## 📞 Summary

✅ **Build Status:** SUCCESS
- Component KWP2000 compiled
- Main application ready
- ECU Emulator ready (2 versions)

✅ **Testing Ready:**
- TCP Loopback (no virtual COM needed) - RECOMMENDED
- Serial port version (for real hardware)

✅ **Next Phase:**
- Confirm Tester Present works
- Add Read Data By ID
- Integrate TinyML
- Add web server on core 0

Good luck! 🎉
