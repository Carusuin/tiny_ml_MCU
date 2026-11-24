# TinyML Classificator: ESP32 Standalone Honda ECU & ADXL345 System

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-6.0.2-blue)
![Target ECU](https://img.shields.io/badge/Target%20ECU-Honda%20Keihin%20K15-orange)
![Interface](https://img.shields.io/badge/Interface-L9637D%20Transceiver-green)
![Status](https://img.shields.io/badge/System-Standalone%20Embedded%20Web-brightgreen)

Sistem mikrokontroler mandiri (*100% Standalone Embedded System*) berbasis **ESP32** untuk akuisisi data mesin motor Honda (**Keihin PGM-FI / CB150R K15 - 38770-K15-903**) dan sensor accelerometer (**ADXL345**) guna keperluan data logging serta inferensi **TinyML** langsung di perangkat (*on-device intelligence*).

> [!IMPORTANT]
> **Arsitektur Sistem (Tanpa Aplikasi Luar):**
> Sistem ini **TIDAK menggunakan aplikasi Android, desktop, atau software PC sama sekali**. Seluruh sistem berjalan secara mandiri (*standalone*) di dalam firmware ESP32:
> * ESP32 bertindak sebagai WiFi Access Point dan Web Server mandiri.
> * Pengguna cukup membuka browser (HP/Laptop) di alamat `http://192.168.4.1` untuk mengakses dashboard interaktif, kontrol diagnostik, dan monitoring data real-time.

---

## 🔌 Topologi Perangkat Keras (Hardware Topology)

Komunikasi dua arah berlangsung melalui IC transceiver khusus otomotif **L9637D**:

```text
┌─────────────────┐           ┌───────────────┐           ┌─────────────────┐
│                 │  3.3V TTL │               │  12V K-Line│                 │
│  ESP32 Board    │ ────────> │   L9637D IC   │ ────────> │   Honda Keihin  │
│  (GPIO 16 RX    │ <──────── │ (Transceiver) │ <──────── │   PGM-FI ECU    │
│   GPIO 17 TX)   │           │               │           │   (Soket DLC)   │
└────────┬────────┘           └───────────────┘           └─────────────────┘
         │
         │ I2C Bus (GPIO 21 SDA, GPIO 22 SCL)
         ▼
┌─────────────────┐
│  ADXL345 Sensor │
│  (Akselerasi &  │
│   Getaran Mesin)│
└─────────────────┘
```

---

## 🎯 Visi & Alur Kerja TinyML

Proyek ini dirancang sebagai platform klasifikasi cerdas (*edge classification*):
1. **Akuisisi Data Multi-Sensor:**
   * **Data ECU (via K-Line L9637D):** RPM, Throttle (TPS), Tekanan Manifold (MAP), Suhu Mesin (Coolant), Kecepatan (Speed).
   * **Data Inersia (via I2C ADXL345):** Getaran mesin dan gaya akselerasi 3-sumbu ($X, Y, Z$).
2. **Data Logging (microSD):** Penyimpanan dataset terpadu secara sinkron untuk data training machine learning.
3. **On-Device TinyML Inference:** Menjalankan model klasifikasi cerdas (misalnya: deteksi anomali/kerusakan mesin, klasifikasi perilaku berkendara, atau prediksi posisi gigi) langsung pada CPU ESP32 tanpa koneksi internet.
4. **Hasil Prediksi:** Ditampilkan secara instan di Web Dashboard internal.

---

## 🚀 Arsitektur Multi-Core Firmware (ESP32)

Firmware memanfaatkan kedua core CPU ESP32 secara optimal untuk menjamin performa real-time bebas gangguan:

* **Core 0 (Networking & User Interface):**
  * WiFi SoftAP (`SSID: Honda_KWP2000`, `Pass: 12345678`).
  * Embedded HTTP REST Web Server (`port 80`).
  * Dashboard interaktif responsif (HTML/CSS/JS) dengan anti-cache header.
* **Core 1 (Real-Time Physical Communication & Sensor Loop):**
  * `kwp2000_task`: Membaca UART 10.400 baud, merakit frame byte-by-byte, dan memfilter pantulan *self-echo*.
  * `app_test_kwp2000`: Fast Init (70ms LOW, 130ms HIGH), 5-Step Handshake, keep-alive heartbeat (1.5 detik), dan live polling Table 0x10 (~10 Hz).
  * `queue_consumer_task`: Sinkronisasi status terproteksi Mutex dan logging ke microSD.

---

## 📖 Indeks Dokumen Repositori

Untuk pengembang dan agen AI (**Claude Code, Gemini, Antigravity**), gunakan dokumen berikut sebagai referensi resmi:

| Dokumen | Deskripsi |
| :--- | :--- |
| **[AI_COLLABORATION_GUIDE.md](AI_COLLABORATION_GUIDE.md)** | **Panduan Kolaborasi AI & Konteks Desain:** Penjelasan resmi keputusan improvisasi polling 10 Hz, koreksi offset Table 10, toleransi checksum 0x7E, dan arsitektur TinyML untuk AI lain (Claude Code, Gemini, Antigravity). |
| **[TESTING_GUIDE.md](TESTING_GUIDE.md)** | **Panduan Wiring & Pengujian:** Skema pinout detail ESP32 $\leftrightarrow$ L9637D $\leftrightarrow$ DLC Motor, cara compile/flash via ESP-IDF, dan panduan troubleshooting. |
| **[HONDA_K15_KLINE_PROTOCOL.md](HONDA_K15_KLINE_PROTOCOL.md)** | **Spesifikasi Protokol Honda K-Line:** Dokumentasi biner mendalam hasil reverse engineering kabel Analist Pro, struktur frame hex, timing elektrik, dan rumus checksum. |

---

## ⚡ Cara Kompilasi & Flash

```powershell
& "C:\esp\v6.0.2\esp-idf\export.ps1"
cd "D:\tinyml_classificator\tinyml"
idf.py build
idf.py -p COM_PORT flash
```
Setelah flash selesai, hidupkan kontak motor, hubungkan WiFi ke **`Honda_KWP2000`**, lalu buka **`http://192.168.4.1`** di browser.
