# Panduan Wiring Hardware & Pengujian ESP32 K-Line (L9637D ke ECU Honda)

Dokumen ini memuat konfigurasi pinout hardware mandiri (*standalone*), skema wiring **ESP32 $\leftrightarrow$ L9637D $\leftrightarrow$ ECU**, cara build/flash via ESP-IDF v6.0.2, serta prosedur diagnosa real-time melalui Web Dashboard bawaan firmware.

> [!NOTE]
> Sistem ini bekerja **100% mandiri (tanpa aplikasi PC / Android)**. ESP32 memancarkan WiFi AP sendiri dan menyajikan Web Dashboard langsung ke browser pengguna.

---

## 1. Spesifikasi Hardware & Pinout ESP32

Komunikasi K-Line menggunakan **UART1** pada ESP32 yang terhubung langsung ke IC transceiver **L9637D** untuk konversi level tegangan antara **3.3V TTL** (ESP32) dengan **12V K-Line** (ECU Motor).

| Pin ESP32 | Terhubung Ke (Pin L9637D) | Keterangan Teknis |
| :--- | :--- | :--- |
| **`GPIO 16` (RX)** | Pin **RX** (Pin 1 L9637D) | Menerima data serial dari ECU (Pull-up internal ESP32 aktif) |
| **`GPIO 17` (TX)** | Pin **TX** (Pin 4 L9637D) | Mengirim pulsa Fast Init (Bit-bang) dan frame UART ke ECU |
| **`3.3V` / `5V`** | Pin **Vs** (Pin 8 L9637D) | Sumber daya logika IC L9637D |
| **`GND`** | Pin **GND** (Pin 3 L9637D) & **GND Motor** | Ground bersama (*common ground*) wajib tersambung |

### Koneksi dari L9637D ke Soket DLC Motor (Honda 4-Pin):
* **Pin K (Pin 7 L9637D)** $\longrightarrow$ Terhubung ke kabel **K-Line DLC Motor** (12V)
* **Pin Vb (Pin 6 L9637D)** $\longrightarrow$ Terhubung ke tegangan **12V Baterai Motor / Kontak**
* **Pin GND L9637D** $\longrightarrow$ Terhubung ke **GND Motor**

---

## 2. Skema Pengkabelan Lengkap (Wiring Diagram)

```text
               ┌─────────────────────────────────┐
               │    Soket DLC Motor Honda        │
               │   (K-Line 12V, GND, Baterai 12V)│
               └──────────────┬──────────────────┘
                              │
          ┌───────────────────┴───────────────────┐
          │                                       │
     K-Line (12V)                              GND Motor
          │                                       │
          ▼                                       ▼
    ┌───────────┐                           ┌───────────┐
    │  Pin 7(K) │                           │ Pin 3(GND)│
    │           │                           │           │
    │           │         L9637D IC         │           │
    │           │   (K-Line Transceiver)    │           │
    │  Pin 1    │                           │   Pin 4   │
    │  (RX Out) │                           │  (TX In)  │
    └─────┬─────┘                           └─────▲─────┘
          │                                       │
          │ 3.3V Serial                           │ 3.3V Serial
          ▼                                       │
    ┌───────────┐                                 │
    │  GPIO 16  │                                 │
    │ (UART1 RX)│                                 │
    │           │                                 │
    │  GPIO 17 ─┴─────────────────────────────────┘ (UART1 TX)
    │           │
    │    GND ───┴─────────────────────────────── Common GND (ESP32 + L9637D + Motor)
    │           │
    │           │   I2C Bus (Persiapan TinyML Sensor Inersia):
    │  GPIO 21 ─┼───> ADXL345 SDA
    │  GPIO 22 ─┼───> ADXL345 SCL
    │   3.3V   ─┴───> ADXL345 VCC
    └───────────┘
     ESP32 Board
```

---

## 3. Kompilasi & Flash Firmware (ESP-IDF v6.0.2)

1. Buka PowerShell dan muat environment ESP-IDF:
   ```powershell
   & "C:\esp\v6.0.2\esp-idf\export.ps1"
   cd "D:\tinyml_classificator\tinyml"
   ```
2. Kompilasi:
   ```powershell
   idf.py build
   ```
3. Flash ke ESP32 (ganti `COM_PORT` sesuai port USB Anda, misal `COM3`):
   ```powershell
   idf.py -p COM_PORT flash
   ```

---

## 4. Prosedur Pengujian Live (Melalui Web Dashboard)

1. **Hubungkan Hardware:** Pasang ESP32 dan modul L9637D ke soket DLC motor. Nyalakan kunci kontak motor (**Kontak ON**).
2. **Sambungkan WiFi:** Hubungkan smartphone atau laptop ke WiFi Access Point ESP32:
   * **SSID:** `Honda_KWP2000`
   * **Password:** `12345678`
3. **Buka Web:** Akses alamat **`http://192.168.4.1`** pada browser.
4. **Inisialisasi ECU:** Tekan tombol **Connect ECU**. ESP32 akan melakukan 5-Step Handshake otomatis hingga status menjadi **`ECU session ready`**.
5. **Start Diagnosis:** Tekan tombol **Start Diagnosis** untuk melihat parameter live sensor (RPM, TPS, Coolant, MAP, Speed).

---

## 5. Troubleshooting Cepat (Menggunakan Panel Monitor)

Jika koneksi gagal, periksa panel monitor di bagian bawah dashboard:
* **Panel Raw RX kosong (0 byte):** Cek apakah kontak motor sudah ON dan pastikan jalur kabel TX/RX antara ESP32 dan L9637D tidak terbalik.
* **STEP 1 NO RESPONSE:** Pulsa Fast Init tidak dibalas ECU. Cek sambungan kawat K-Line dari pin 7 L9637D ke pin DLC motor.
* **Data Sensor Tidak Bergerak:** Lakukan refresh pada browser (`Ctrl+F5` atau buka ulang tab browser).
