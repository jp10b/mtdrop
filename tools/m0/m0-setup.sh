#!/usr/bin/env bash
# M0 setup for an Ubuntu live (or persistent) session on the target laptop.
# Installs build dependencies, clones jedbillyb/airdrop-mt7921 and runs its
# installer (builds owl + patched OpenDrop). Touches nothing outside the live
# session: no internal-disk changes, no driver changes.
#
# Usage:  bash m0-setup.sh
set -euo pipefail

WORK="${WORK:-$HOME/m0}"

echo "==> Checking kernel support for active monitor (decides whether M0 can work)"
if ! command -v iw >/dev/null 2>&1; then sudo apt-get install -y iw; fi
if iw list 2>/dev/null | grep -qi "active monitor"; then
  echo "    OK: $(iw list | grep -i 'active monitor' | head -1 | sed 's/^\s*//')"
else
  echo "    STOP: this kernel ($(uname -r)) does not offer active monitor for this card."
  echo "    Do not continue. Note the kernel version and report back (plan B: other distro/kernel)."
  exit 1
fi

echo "==> Installing build dependencies"
sudo add-apt-repository -y universe >/dev/null
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
  git cmake build-essential pkg-config \
  libpcap-dev libev-dev libnl-3-dev libnl-genl-3-dev libnl-route-3-dev \
  python3-venv python3-pip iw tcpdump tshark network-manager

echo "==> Cloning airdrop-mt7921 into $WORK"
mkdir -p "$WORK"
cd "$WORK"
[ -d airdrop-mt7921/.git ] || git clone https://github.com/jedbillyb/airdrop-mt7921.git
git -C airdrop-mt7921 rev-parse HEAD > "$WORK/airdrop-mt7921.commit"
echo "    commit: $(cat "$WORK/airdrop-mt7921.commit")"

echo "==> Running airdrop-mt7921/install.sh (it will ask before adding a sudoers rule - answer y)"
cd airdrop-mt7921
./install.sh

echo
echo "Setup finished. Next: step 8 of docs/M0-GUIDE.pl.md (prepare the iPhone)."
