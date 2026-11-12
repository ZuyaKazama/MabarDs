EmuNDS untuk Linux
==================

Isi folder ini adalah melonDS versi desktop yang dibangun dari core yang SAMA PERSIS
dengan aplikasi EmuNDS Android. Karena itu multiplayer LAN-nya bisa saling terhubung:
PC Linux <-> HP Android, PC <-> PC, HP <-> HP.

Menjalankan
-----------
  ./emunds-linux.sh

Jalankan lewat skrip ini, bukan langsung ./melonDS. Saat pertama kali dijalankan, skrip
membuat pengaturan dengan alamat MAC acak. Setiap pemain wajib punya MAC berbeda.
Pengaturan, save, dan savestate disimpan di folder portable/ di samping program.

Buka ROM: File -> Open ROM. Tidak perlu BIOS (memakai FreeBIOS + firmware buatan).
Atur tombol: Config -> Input and hotkeys.

Firewall (wajib sekali saja)
----------------------------
Multiplayer memakai UDP 7063 (mencari room) dan UDP 7064 (data game).
Di Fedora/Nobara:
  sudo firewall-cmd --permanent --add-port=7063-7064/udp
  sudo firewall-cmd --reload
Di Ubuntu (ufw):
  sudo ufw allow 7063:7064/udp

Main bareng (mabar)
-------------------
1. Semua perangkat di Wi-Fi/hotspot yang sama, atau semua memakai Tailscale/ZeroTier
   kalau beda tempat.
2. Satu orang jadi host:
   - Linux:   System -> Multiplayer -> Host LAN game
   - Android: Multiplayer -> Buat room
3. Yang lain bergabung:
   - Linux:   System -> Multiplayer -> Join LAN game (pilih room, atau isi IP host)
   - Android: ketuk room yang muncul, atau gabung lewat IP host
4. Setelah semua terhubung, tiap pemain membuka game yang SAMA, lalu masuk ke menu
   wireless / DS Download Play di dalam game.

Catatan:
- Kalau room dari HP tidak muncul di PC (atau sebaliknya), cek firewall di atas,
  atau gabung langsung pakai IP host.
- Fast-forward dan savestate jangan dipakai saat mabar.
- Versi Linux dan Android harus dari rilis EmuNDS yang sama.

Lisensi: GPL-3.0 (melonDS). Lihat LICENSE.
