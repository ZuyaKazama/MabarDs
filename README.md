# EmuNDS

Emulator Nintendo DS untuk Android, dengan fokus pada **multiplayer lokal (offline) dan online**.
EmuNDS adalah frontend Android baru di atas core [melonDS](https://github.com/melonDS-emu/melonDS) (GPL-3.0).

> Tidak ada ROM atau BIOS yang disertakan. EmuNDS memakai FreeBIOS bawaan melonDS dan firmware
> yang dibangkitkan otomatis, jadi tidak perlu file BIOS/firmware. Pakai dump game milikmu sendiri.

## Fitur

| Area | Detail |
|---|---|
| Performa | JIT ARM → ARM64 (fastmem), render 3D multi-thread, `-O3` + LTO, audio AAudio low-latency, frame pacing presisi, emu thread prioritas tinggi, upload frame zero-copy (swizzle BGRA di shader) |
| Multiplayer offline | Wireless DS lewat Wi-Fi / hotspot yang sama. Room ditemukan otomatis (UDP broadcast 7063), data game lewat ENet UDP 7064. Sampai 16 pemain. |
| Multiplayer online | Gabung lewat IP/hostname: host buka port UDP 7064, atau semua pemain pakai VPN mesh (Tailscale, ZeroTier, Radmin). |
| Nintendo WFC | Access point internet teremulasi (slirp). Isi DNS kustom (mis. Wiimmfi) di Pengaturan untuk game yang didukung server pengganti. Setelan WFC disimpan permanen. |
| Kontrol | Tombol on-screen multi-touch (D-pad 8 arah, geser antar tombol), layar sentuh, gamepad Bluetooth/USB (stik analog, hat, R2 = fast-forward, L2 = tiup mic), keyboard |
| Lainnya | 5 slot savestate per game, auto-save `.sav`, 6 tata letak layar + tukar layar, fast-forward 2×/3×/4×/tanpa batas, library dengan ikon & judul dari banner ROM |

## Cara build (GitHub Actions)

1. Buat repo baru di GitHub, lalu push folder ini:
   ```bash
   git init && git add . && git commit -m "EmuNDS"
   git branch -M main
   git remote add origin https://github.com/<user>/<repo>.git
   git push -u origin main
   ```
2. Buka tab **Actions** → workflow **Build APK** jalan otomatis (±10–15 menit pertama kali).
3. Unduh artifact **EmuNDS-apk** → install `app-release.apk` di HP.
4. Untuk rilis: `git tag v0.1.0 && git push --tags` → APK dilampirkan di GitHub Releases.

### Tanda tangan APK yang stabil (disarankan)

Tanpa secret, CI membuat kunci sementara setiap build, sehingga update harus uninstall dulu.
Agar bisa update langsung, buat keystore sekali lalu simpan sebagai secret repo:

```bash
keytool -genkeypair -v -keystore emunds.jks -alias emunds -keyalg RSA -keysize 2048 -validity 10000
base64 -w0 emunds.jks   # salin hasilnya
```

Secret: `KEYSTORE_BASE64`, `KEYSTORE_PASSWORD`, `KEY_ALIAS`, `KEY_PASSWORD`.

### Build lokal (Android Studio / CLI)

```bash
./scripts/fetch_deps.sh          # mengunduh melonDS (commit dipin) + ENet 1.3.18
gradle assembleRelease           # butuh JDK 17, Android SDK 35, NDK 27.2.12479018, CMake 3.22.1
```

## Cara main multiplayer

**Offline (satu ruangan)**
1. Semua HP tersambung ke Wi-Fi yang sama, atau satu HP menyalakan hotspot dan yang lain menyambung.
2. Pemain 1: *Multiplayer → Buat room*. Pemain lain: room muncul di daftar → ketuk.
3. Setelah semua masuk, tiap pemain ketuk *Pilih game & mulai* dan memuat **game yang sama**.
4. Di dalam game, masuk ke menu wireless / DS Download Play seperti di DS asli.

**Online (beda tempat)**
- Paling mudah: semua pemain pasang **Tailscale** atau **ZeroTier** dan masuk ke jaringan yang sama,
  lalu gabung lewat IP Tailscale host (`100.x.x.x`) yang tampil di lobby.
- Alternatif: host membuka **port forwarding UDP 7064** di router, pemain lain gabung lewat IP publik host.
- Latensi penting: wireless DS sangat sensitif waktu. Ping < 30–40 ms biasanya lancar; di atas itu
  beberapa game bisa putus. Fast-forward otomatis dimatikan selama sesi multiplayer.

**Nintendo WFC** (Mario Kart DS, Pokémon, dll. lewat server pengganti)
- Isi *Pengaturan → DNS kustom Nintendo WFC* dengan DNS dari layanan pengganti (cek situs resminya untuk alamat terbaru),
  lalu atur koneksi di menu Nintendo WFC dalam game.

## Struktur

```
app/src/main/cpp/
  EmuCore.*            thread emulasi, loop frame, save/savestate, firmware, multiplayer
  Platform_android.cpp implementasi melonDS::Platform (file, thread, MP, net, mic)
  AudioOut.*           AAudio, menarik sampel langsung dari ring buffer SPU
  VideoOut.*           presenter OpenGL ES 3 (texture array, 1 upload / frame)
  jni_bridge.cpp       API JNI untuk Kotlin
app/src/main/java/com/emunds/nds/
  MainActivity         library ROM (SAF)
  EmulatorActivity     layar game, gamepad, menu dalam game
  ControlsView         kontrol sentuh
  MultiplayerActivity  lobby host / LAN / IP
  SettingsActivity     pengaturan
scripts/fetch_deps.sh  mengunduh dependensi
.github/workflows/     CI build APK
```

## Batasan saat ini

- Hanya renderer 3D software (resolusi asli 256×192). Upscaling butuh port renderer OpenGL melonDS ke GLES.
- Mode DSi, kamera, dan mic asli belum didukung (tombol MIC mengirim suara "tiup").
- Belum ada server relay: online butuh port forwarding atau VPN mesh.

## Lisensi

GPL-3.0-or-later, mengikuti melonDS.
