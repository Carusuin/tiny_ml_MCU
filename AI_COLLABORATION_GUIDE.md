# Panduan Kolaborasi AI & Konteks Teknis (Claude Code, Gemini, Antigravity)

Dokumen ini adalah **catatan resmi konteks teknis, keputusan desain, dan improvisasi** pada repositori ini. Semua AI yang memodifikasi kode repositori ini **WAJIB membaca dan menghormati keputusan desain ini** agar tidak terjadi konflik logika atau regresi kode.

---

## 1. Visi Utama Proyek: Standalone TinyML Classificator

* **Topologi Fisik:**
  $$\text{ESP32 (GPIO 16 RX / GPIO 17 TX)} \iff \text{IC Transceiver L9637D} \iff \text{ECU Keihin (DLC Motor Honda)}$$
  $$\text{ESP32 (GPIO 21 SDA / GPIO 22 SCL)} \iff \text{Sensor Akselerometer ADXL345 (I2C)}$$
* **Sifat Sistem:** **100% Standalone Embedded System**. Tidak ada aplikasi PC, Android, atau server eksternal dalam operasional.
* **Tujuan Akhir:**
  1. Mengumpulkan dataset sinkron: Data ECU (RPM, TPS, MAP, Coolant, Speed) + Data Inersia (Akselerasi 3-sumbu $X, Y, Z$).
  2. Melakukan inferensi model **TinyML** langsung di dalam ESP32 (*edge intelligence*).
  3. Menampilkan status dan hasil prediksi secara real-time melalui Web Dashboard bawaan WiFi ESP32 (`http://192.168.4.1`).

---

## 2. Improvisasi K-Line & Alasan Desain (PENTING)

### A. Mengapa Polling Hanya Meminta Table 0x10 (Bukan Bergantian 11, 10, 13, 16, 17)?
* **Latar Belakang:** Protokol lama Analist Pro meminta Table 11, lalu 10, 13, 16, 17 serta frame housekeeping (`0x20` dan `0xD1`).
* **Keputusan Desain:** Polling live data **disederhanakan hanya meminta Table 0x10**.
* **Alasannya:**
  1. Kecepatan K-Line adalah **10.400 baud** (lambat secara fisik).
  2. Jika harus bergantian membaca DTC (Table 13, 16, 17) dan frame housekeeping, waktu per siklus memakan 600–1000 ms (hanya 1 frame per detik).
  3. Dengan membatasi ke Table 0x10 pada interval `80 ms`, data live sensor (RPM, TPS, MAP, Speed) didapatkan pada kecepatan **~10 Hz (10 FPS real-time)** yang responsif untuk pengendara dan inferensi TinyML.
  4. Pembacaan riwayat DTC nantinya dilakukan secara *on-demand* (melalui tombol terpisah di web), bukan di dalam loop polling sensor real-time.

### B. Pemetaan Offset Payload Sensor Table 0x10 (Telah Diverifikasi)
Dalam fungsi `kwp2000_parse_frame()`, `table_data[i] = frame[i + 3]`.
Format frame ECU: `[02 1A 71 10 ... Checksum]` (total 26 byte).
Maka `table_data[0]` = `frame[3]` (Table ID: `0x10`).

Pemetaan yang benar sesuai dekompilasi GUI Analist Pro (`aGauge2` = RPM, `aGauge4` = TPS):
* **TPS %:** `table_data[2]` (yaitu `frame[5]`), formula: $\text{byte} \times 0.5\%$.
* **Engine RPM:** `((table_data[3] << 8) | table_data[4])` (yaitu `frame[6..7]`).
* **Coolant (ECT):** `table_data[5] - 40` (yaitu `frame[8]`).
* **MAP kPa:** `table_data[9] * 0.75` (yaitu `frame[12]`).
* **Speed (km/h):** `table_data[14]` (yaitu `frame[17]`).

*(Catatan: Versi kode awal sempat tertukar karena salah mengira offset Table 0x10 sama dengan standar generic OBD-2 PID. Offset di atas adalah yang resmi dan benar).*

### C. Toleransi Checksum Wake-up ACK (`0x7E` vs `0x7C`)
* Secara hitungan 2's complement matematis: `0x100 - (0x0E + 0x04 + 0x72) = 0x7C`.
* Di lapangan, beberapa unit ECU Keihin Honda asli mengirimkan byte penutup `0x7E` pada frame `[0E 04 72 7E]`.
* Fungsi `kwp2000_parse_frame()` memberikan pengecualian khusus untuk frame wake-up ACK ini agar proses inisialisasi awal ke motor asli tidak gagal (*handshake tolerant*).

### D. Standby Keep-Alive Heartbeat (1.5 Detik)
* ECU Keihin memiliki timeout internal: jika bus idle tanpa aktivitas selama ~2–3 detik, ECU akan tidur (*sleep*).
* Firmware mengirimkan frame keep-alive `72 05 00 F0 99` setiap **1.5 detik** saat dalam mode standby (setelah Connect ECU berhasil, sebelum Start Diagnosis ditekan).

### E. Fast Init Fisik (70 ms LOW + 130 ms HIGH) Telah Terbukti Berhasil
* **Status:** **SUDAH TERBUKTI BERHASIL (Verified)** pada motor asli Honda CB150R Old (K15 / ECU Keihin `38770-K15-903`).
* **Implementasi:** Pada `kwp2000.c` fungsi `kwp2000_wake_up()`, pin TX (GPIO 17) dilepas sementara dari driver UART dan diatur sebagai output push-pull biasa (bit-bang):
  1. `LOW (0V)` selama **70 ms** (`low_ms = 70`).
  2. `HIGH (12V)` selama **130 ms** (`high_ms = 130`).
  3. UART1 di-restore ke **10.400 baud 8N1**, buffer di-purge, lalu frame wake-up `[FE 04 72 8C]` dikirim.
* **PENTING UNTUK SEMUA AI:** Jangan pernah mengubah durasi pulsa ini kembali ke standar generik ISO 14230 (25ms/25ms) atau menghapus bit-bang GPIO ini, karena ECU Keihin tidak akan bangun jika pulsa LOW di bawah 70 ms.

---

## 3. Arsitektur Multi-Core & Pembagian Task ESP32

```text
┌──────────────────────────────────────────────┐  ┌──────────────────────────────────────────────┐
│                   CORE 0                     │  │                   CORE 1                     │
│  - WiFi SoftAP (Honda_KWP2000 / 12345678)    │  │  - kwp2000_task (UART1 10400 bps K-Line)     │
│  - HTTP Web Server (Port 80 REST API)        │  │  - app_test_kwp2000 (State Machine & Polling)│
│  - adxl345_task (I2C SDA=21, SCL=22, 20 Hz)  │  │  - queue_consumer_task (SD Logger)          │
└──────────────────────────────────────────────┘  └──────────────────────────────────────────────┘
```

* **Isolasi Hardware:** Task K-Line ditempatkan di **Core 1** agar timing serial 10.400 bps dan pulsa bit-bang Fast Init tidak terganggu oleh latensi interrupt WiFi / TCP stack di **Core 0**.
* **Keamanan Sensor ADXL345:** Task ADXL345 dirancang *fault-tolerant*. Jika modul sensor belum terpasang fisik, task akan tidur hemat daya dan tidak memblokir sistem maupun membanjiri log.

---

## 4. Format Dataset SD Card (Persiapan TinyML Bulan Desember)

Saat kartu microSD terpasang, data dicatat ke `/sdcard/tinyml_dataset.csv` dengan header:
```csv
timestamp_ms,ecu_connected,rpm,tps_pct,map_kpa,coolant_c,speed_kph,accel_x,accel_y,accel_z
```
Format ini siap digunakan langsung sebagai input pelatihan model TinyML (anomali mesin, deteksi perpindahan gigi, atau analisis gaya berkendara).
