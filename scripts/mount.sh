#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
target=${1:-/mnt/vtfs}
if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo: sudo sh scripts/mount.sh [mountpoint]" >&2
    exit 1
fi
mkdir -p "$target"
if mountpoint -q "$target"; then
    echo "Already mounted: $target" >&2
    exit 1
fi
if [ -n "$(ls -A "$target")" ]; then
    echo "Mountpoint must be empty: $target" >&2
    exit 1
fi
loaded=0
if [ ! -d /sys/module/vtfs ]; then
    insmod ./vtfs.ko
    loaded=1
fi
if ! mount -t vtfs none "$target"; then
    if [ "$loaded" = 1 ]; then rmmod vtfs; fi
    exit 1
fi
echo "VTFS mounted at $target"
