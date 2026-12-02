#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo: sudo sh scripts/test.sh" >&2
    exit 1
fi
if [ -d /sys/module/vtfs ]; then
    echo "VTFS is already loaded. Unmount and unload it before this isolated test." >&2
    exit 1
fi

work=$(mktemp -d /tmp/vtfs-test.XXXXXX)
loaded=0
taint_before=$(cat /proc/sys/kernel/tainted)
cleanup() {
    # Never unload a module that still has mounts after a failed cleanup.
    mountpoint -q "$work/second" && umount "$work/second"
    mountpoint -q "$work/first" && umount "$work/first"
    if [ "$loaded" = 1 ]; then rmmod vtfs; fi
    rmdir "$work/first" "$work/second" "$work"
}
trap cleanup EXIT
mkdir "$work/first" "$work/second"
chmod 755 "$work"
insmod ./vtfs.ko
loaded=1
mount -t vtfs none "$work/first"
mount -t vtfs none "$work/second"

if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
    runuser -u "$SUDO_USER" -- python3 tests/test_vtfs.py "$work/first" "$work/second"
else
    python3 tests/test_vtfs.py "$work/first" "$work/second"
fi

# Reclaim unused dentries: lookup must restore names from our own RAM entries.
mkdir -p "$work/first/cache/nested"
printf 'after dcache reclaim\n' > "$work/first/cache/nested/file"
ln "$work/first/cache/nested/file" "$work/first/cache/link"
inode_before=$(stat -c %i "$work/first/cache/link")
printf '2\n' > /proc/sys/vm/drop_caches
test "$(cat "$work/first/cache/nested/file")" = 'after dcache reclaim'
test "$(cat "$work/first/cache/link")" = 'after dcache reclaim'
test "$(stat -c %i "$work/first/cache/link")" = "$inode_before"
test "$(stat -c %h "$work/first/cache/link")" = 2
rm "$work/first/cache/nested/file" "$work/first/cache/link"
rmdir "$work/first/cache/nested" "$work/first/cache"
echo "PASS: namespace and hard links survive dentry-cache reclaim"

# Leave a linked, nonempty tree for our namespace teardown to release.
mkdir -p "$work/first/left/a/b"
printf 'unmount cleanup\n' > "$work/first/left/a/b/file"
ln "$work/first/left/a/b/file" "$work/first/left/link"
umount "$work/first"
mount -t vtfs none "$work/first"
test -z "$(ls -A "$work/first")"
test "$(stat -c %i "$work/first")" = 1000
echo "PASS: remount is empty; root inode is 1000"
taint_after=$(cat /proc/sys/kernel/tainted)
# Out-of-tree (4096) and unsigned (8192) are expected for this local module.
test "$(( (taint_after ^ taint_before) & ~12288 ))" -eq 0
echo "PASS: no new unexpected kernel taint flags"
