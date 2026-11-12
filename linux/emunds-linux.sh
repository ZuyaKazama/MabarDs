#!/usr/bin/env bash
# EmuNDS for Linux launcher (melonDS desktop built from the same core as the Android app).
# On first run it writes a starter config with a random MAC address: every console in a
# multiplayer session needs a different MAC, and melonDS' generated firmware always uses
# 00:09:BF:11:22:33.
set -euo pipefail
HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
CFGDIR="$HERE/portable"
CFG="$CFGDIR/melonDS.toml"
mkdir -p "$CFGDIR"

if [ ! -f "$CFG" ]; then
    # 00:09:BF is Nintendo's OUI, same prefix the Android app uses; last 3 bytes random
    MAC="$(od -An -N3 -tx1 /dev/urandom | awk '{printf "00:09:BF:%s:%s:%s", toupper($1), toupper($2), toupper($3)}')"
    NAME="$(printf '%s' "${USER:-Player}" | tr -cd '[:alnum:]_ -' | cut -c1-10)"
    [ -n "$NAME" ] || NAME=Player
    cat > "$CFG" <<EOF
[Emu]
DirectBoot = true
ExternalBIOSEnable = false

[LAN]
PlayerName = "$NAME"

# Instance0 must be an explicit table: melonDS adds plain keys to it later
[Instance0]

[Instance0.Firmware]
OverrideSettings = true
Username = "$NAME"
Language = 1
MAC = "$MAC"
EOF
    echo "EmuNDS: config baru dibuat ($CFG), MAC $MAC, nama $NAME"
fi

exec "$HERE/melonDS" "$@"
