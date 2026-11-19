# ESP32-C3 OLED RFID MQTT Simulator (JSON Tag List)

Firmware PlatformIO untuk **ESP32-C3 0.42" OLED (72×40 SSD1306)** yang membaca daftar EPC RFID dari file **`rfid_tags.json`** (tidak random, mudah diganti) dan mengirimkannya ke broker MQTT `mqtt.moko.ai.id` dengan kontrol `start`/`stop` melalui Serial Monitor (dan remote MQTT command).

## Mengganti daftar RFID (`rfid_tags.json`)

Edit langsung file `rfid_tags.json` di folder proyek ini. Mendukung **dua format JSON**:

1. **JSON Array langsung** (format unduhan dari server):
```json
[
  "E28069150000400F19922C9E",
  "E28069150000500F1992489E"
]
```

2. **JSON Object** (jika ingin kustom `device_id` dan `mode`):
```json
{
  "device_id": "PORTAL-MLG-01",
  "mode": "sequential",
  "tags": [
    "E28069150000400F19922C9E",
    "E28069150000500F1992489E"
  ]
}
```

- **`device_id`**: ID gateway pada payload MQTT (default `PORTAL-MLG-01`).
- **`mode`**:
  - `"sequential"` (default): kirim tag 1 per 1 secara berurutan lalu mengulang dari awal.
  - `"batch"`: kirim seluruh array `tags` sekaligus dalam 1 payload MQTT.
- **`tags`**: daftar EPC yang ingin dikirim (disimpan di Flash/PROGMEM sehingga tidak memakan RAM).

Setiap kali menjalankan `pio run` atau `pio run -t upload`, script `scripts/embed_rfid_json.py` otomatis memvalidasi dan menanamkan isi `rfid_tags.json` ke firmware (tidak perlu `uploadfs` terpisah).

## Konfigurasi aktif

| Parameter | Nilai |
|---|---|
| Board | ESP32-C3 DevKitM-1 (`0.42" OLED onboard`) |
| OLED | SSD1306 72×40, I2C `SDA=GPIO5`, `SCL=GPIO6` |
| MQTT broker | `mqtt.moko.ai.id` |
| MQTT port | `8883` (TLS, `WiFiClientSecure` + `ROOT_CA_CERT`) |
| MQTT topic | `/kolektibles2026` |
| MQTT client ID | `rfid_esp32_simulator` |
| Status awal saat boot | `STOP` |
| Interval default | `1000 ms` (1 detik) |
| Sumber tag RFID | `rfid_tags.json` |

## Payload MQTT

Mode `sequential`:
```json
{"device_id":"PORTAL-MLG-01","tags":["e28011700000020100000001"]}
```

Mode `batch`:
```json
{"device_id":"PORTAL-MLG-01","tags":["e28011700000020100000001","e28011700000020100000002"]}
```

## Kontrol via Serial Monitor (`115200` baud)

Pastikan line ending Serial Monitor diset ke **Newline (`\n`)** atau **Both NL & CR (`\r\n`)**.

| Perintah | Fungsi |
|---|---|
| `start` (atau `1`) | Mulai mengirim tag dari `rfid_tags.json` |
| `stop` (atau `0`) | Hentikan pengiriman |
| `once` (atau `send`) | Kirim 1× sekarang (tag berikutnya / batch) |
| `list` | Tampilkan semua tag yang dimuat dari `rfid_tags.json` |
| `reset` | Kembalikan urutan ke tag `#1` |
| `mode sequential` | Kirim 1 tag per interval secara berurutan |
| `mode batch` | Kirim semua tag sekaligus dalam 1 payload |
| `interval <ms>` | Ubah interval kirim (`100`–`60000` ms, default `1000`) |
| `scan` | Scan jaringan Wi-Fi 2.4 GHz sekitar |
| `status` | Tampilkan detail status koneksi & simulator |
| `help` | Tampilkan daftar perintah |

## Build, upload, dan monitor

```powershell
pio run
pio run -t upload
pio device monitor --baud 115200
```
