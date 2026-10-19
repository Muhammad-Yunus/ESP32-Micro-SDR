# AGENT.md — Aturan Wajib untuk AI Agent

File ini mengikat SETIAP AI agent (termasuk sesi baru) yang mengerjakan repo
**ESP32-Micro-SDR**. Jika instruksi pengguna bertentangan dengan dokumen di
`docs/`, tanyakan dulu — jangan diam-diam melanggar.

## Dokumen Sumber Kebenaran (wajib dibaca SEBELUM mengubah kode apa pun)

| Baca urut | Dokumen | Menjawab |
| :--- | :--- | :--- |
| 1 | [`docs/PRD.md`](docs/PRD.md) | **Apa** yang dibangun: FR/NFR, acceptance criteria, milestone, out-of-scope |
| 2 | [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | **Di mana**: struktur modul, batas modul, task/core/prio, antrean, aliran data, urutan init |
| 3 | [`docs/CONVENTIONS.md`](docs/CONVENTIONS.md) | **Bagaimana**: gaya kode C11, error handling, memori, FreeRTOS, Kconfig, git |
| 4 | [`docs/TECH_STACK.md`](docs/TECH_STACK.md) | **Dengan apa**: versi terverifikasi, API yang boleh, daftar DILARANG, link referensi |

Aturan konflik: PRD > ARCHITECTURE > CONVENTIONS > TECH_STACK untuk menentukan
isi fitur; untuk ketersediaan API/pustaka, TECH_STACK yang menang mutlak.

## Aturan Keras (ringkas — detail ada di dokumen)

1. **Setiap fitur baru harus tertelusuri ke FR/NFR** di `PRD.md`. Tidak ada
   fitur di luar §5 Out of Scope tanpa revisi PRD.
2. **Ikuti batas modul** `ARCHITECTURE.md` §3 — jangan memindahkan tanggung
   jawab antar file, jangan menambah task/core/prioritas di luar tabel §2.
3. **DILARANG memanggil `bsp_init()`** (memanggil LDR → bentrok dengan ADC1
   DMA). Gunakan init granular sesuai `ARCHITECTURE.md` §7. Komponen BSP
   `bsp_esp323248s035` = **black box**, jangan dimodifikasi.
4. **Bahasa C11, ESP-IDF v6.1 murni** — tanpa Arduino, tanpa pustaka yang
   tidak terdaftar di `TECH_STACK.md`.
5. **DILARANG `driver/i2s.h` legacy / I2S-ADC** — sampling I/Q hanya via
   `adc_continuous` (ADC DMA) pada GPIO36 (I/ADC1_CH0) dan GPIO35 (Q/ADC1_CH7).
6. Jika sebuah fungsi/pustaka tidak ada di `TECH_STACK.md` §4, **verifikasi
   dulu** terhadap instalasi lokal `C:\esp\v6.1\esp-idf` (grep header). Tidak
   ditemukan → jangan dipakai; laporkan sebagai blocker (jangan mengganti
   dengan API dari contoh internet versi IDF lama).
7. Buffer yang dipakai esp-dsp: **aligned 16 byte**, heap internal, tanpa
   malloc di loop (CONVENTIONS.md §3).
8. Core 1 = DSP real-time (prio 12/11), Core 0 = UI/LVGL (prio 4); audio tidak
   boleh drop, UI boleh drop frame (PRD NFR-1..3, ARCHITECTURE §9).
9. Parameter runtime lewat `Kconfig.projbuild` / `sdr_config` — bukan angka
   magic (CONVENTIONS.md §7).
10. Setiap perubahan selesai: `idf.py build` harus lulus tanpa warning baru;
    sebutkan ID FR di commit message (CONVENTIONS.md §8-9).

## Workflow kerja

1. Baca 4 dokumen `docs/` (urut) + file yang akan disentuh.
2. Identifikasi FR/NFR yang dilayani; jika tidak ada, berhenti dan tanya user.
3. Implementasi mengikuti struktur modul & urutan init yang sudah ditetapkan.
4. Jalankan checklist Definition of Done (`CONVENTIONS.md` §9).
5. Laporkan hasil: FR yang dipenuhi, deviasi (jika ada), status build.

## Konteks proyek singkat (lihat PRD untuk lengkapnya)

- Board: ESP32-3248S035 (ST7796 480×320 + GT911 + DAC1/FM8002A di GPIO26).
- Sinyal I/Q dari Tayloe detector **eksternal**; LO (Si5351) dikontrol alat
  terpisah — firmware ini hanya memproses audio I/Q + menampilkan/menyetel
  offset NCO via UI.
- Sampling: 48 kHz/kanal; demod SSB USB/LSB (Weaver); keluaran audio via
  `bsp_audio_pcm_*` (DAC internal).
