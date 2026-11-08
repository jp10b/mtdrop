#!/usr/bin/env bash
# M0: collect environment facts into a report and copy all results to a
# PRIVATE folder on the Windows partition (outside the git repo).
#
# Usage:  bash m0-collect.sh <path-to-mounted-Windows-C-drive>
#   e.g.  bash m0-collect.sh /media/ubuntu/Windows
#
# The report masks MAC addresses. pcaps, logs and keylogs are NOT redacted and
# contain device names / MACs / Apple pseudonyms: they stay private and must
# never be committed or shared.
set -euo pipefail

WORK="${WORK:-$HOME/m0}"
WIN="${1:-}"
REPORT="$WORK/m0-report.txt"
mask() { sed -E 's/([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}/xx:xx:xx:xx:xx:xx/g'; }

{
  echo "# M0 report  $(date -Iseconds)"
  echo
  echo "## OS / kernel";        lsb_release -a 2>/dev/null; uname -a
  echo; echo "## PCI";          lspci -nnk -d 14c3: 2>/dev/null
  echo; echo "## active monitor"; iw list 2>/dev/null | grep -i "active monitor" || echo "NOT SUPPORTED"
  echo; echo "## interface modes"; iw list 2>/dev/null | sed -n '/Supported interface modes/,/Band 1/p' | head -15
  echo; echo "## regulatory";   iw reg get 2>/dev/null | head -5
  echo; echo "## driver / firmware (ethtool)"
  for i in /sys/class/net/*; do
    n=$(basename "$i")
    if readlink -f "$i/device/driver" 2>/dev/null | grep -q mt7921; then ethtool -i "$n" 2>/dev/null; fi
  done
  echo; echo "## module";       modinfo mt7921e 2>/dev/null | grep -E '^(filename|version|firmware|vermagic)'
  echo; echo "## firmware files on disk (sha256)"
  sha256sum /lib/firmware/mediatek/*7961* 2>/dev/null || ls -la /lib/firmware/mediatek/ 2>/dev/null | grep -i 7961
  echo; echo "## dmesg (mt7921/mt792x)"
  sudo dmesg 2>/dev/null | grep -iE 'mt7921|mt792x|mediatek' | tail -40
  echo; echo "## airdrop-mt7921 commit"; cat "$WORK/airdrop-mt7921.commit" 2>/dev/null
  echo; echo "## owl / opendrop"
  ls -la "$WORK/owl/build/daemon/owl" 2>/dev/null
  git -C "$WORK/owl" rev-parse HEAD 2>/dev/null
  echo; echo "## runs"
  ls -la "$WORK/airdrop-mt7921/runs" 2>/dev/null
  echo; echo "## received files (names only)"
  ls -la "$HOME/Downloads" 2>/dev/null
} 2>&1 | mask > "$REPORT"

echo "Report written: $REPORT"

if [ -z "$WIN" ]; then
  echo "No Windows path given - results stay in $WORK (lost on reboot without persistence!)."
  exit 0
fi

DEST="$WIN/Users/desktop/mtdrop-private/m0-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$DEST"
cp "$REPORT" "$DEST/"
[ -d "$WORK/airdrop-mt7921/runs" ] && sudo cp -r "$WORK/airdrop-mt7921/runs" "$DEST/"
for f in sslkeys.log awdl-raw.pcap notes.txt; do
  [ -e "$WORK/$f" ] && sudo cp "$WORK/$f" "$DEST/"
done
sudo chown -R "$(id -u):$(id -g)" "$DEST" 2>/dev/null || true
sync
echo "Copied to: $DEST"
ls -la "$DEST"
