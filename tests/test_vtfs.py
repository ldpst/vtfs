#!/usr/bin/env python3
"""Behavior checks against two freshly mounted VTFS instances."""
import concurrent.futures
import ctypes
import errno
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT, OTHER = map(Path, sys.argv[1:3])
del sys.argv[1:3]


class VTFS(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="case-", dir=ROOT))

    def tearDown(self):
        shutil.rmtree(self.root)

    def fails(self, expected, action, *args):
        with self.assertRaises(OSError) as caught:
            action(*args)
        self.assertEqual(caught.exception.errno, expected)

    def test_directories_and_errors(self):
        path = self.root / "a" / "b"
        path.mkdir(parents=True)
        file = path / "file with spaces"
        file.touch()
        self.assertEqual(set(os.listdir(path)), {file.name})
        self.assertEqual(stat.S_IMODE(file.stat().st_mode), 0o777)
        self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o777)
        self.assertEqual(path.parent.stat().st_nlink, 3)
        self.fails(errno.EEXIST, path.mkdir)
        self.fails(errno.ENOTEMPTY, path.rmdir)
        self.fails(errno.ENOENT, (path / "missing").unlink)
        self.fails(errno.ENOTDIR, os.listdir, file)
        self.fails(errno.EISDIR, os.open, path, os.O_WRONLY)
        self.fails(errno.ENAMETOOLONG, (path / ("x" * 256)).touch)
        self.fails(errno.EPERM, os.link, path, self.root / "dir-link")
        file.unlink()
        path.rmdir()
        self.assertEqual(path.parent.stat().st_nlink, 2)

    def test_ascii_binary_and_partial_reads(self):
        path = self.root / "bytes"
        path.touch()
        self.assertEqual(path.read_bytes(), b"")
        contents = bytes(range(256)) * 1024
        path.write_bytes(contents)
        self.assertEqual(path.stat().st_size, len(contents))
        with path.open("rb", buffering=0) as file:
            self.assertEqual(file.read(7), contents[:7])
            self.assertEqual(file.read(13), contents[7:20])
            file.seek(-5, os.SEEK_END)
            self.assertEqual(file.read(30), contents[-5:])
            self.assertEqual(file.read(1), b"")
            self.assertEqual(file.read(0), b"")
            self.fails(errno.EINVAL, file.seek, -1)
        self.assertEqual(path.read_bytes(), contents)

    def test_offsets_holes_and_truncation(self):
        path = self.root / "offsets"
        with path.open("w+b", buffering=0) as file:
            file.write(b"abcdef")
            file.seek(2)
            file.write(b"XY")
            file.seek(0)
            self.assertEqual(file.read(), b"abXYef")
            file.seek(12)
            file.write(b"Z")
            file.seek(0)
            self.assertEqual(file.read(), b"abXYef" + b"\0" * 6 + b"Z")
            file.truncate(3)
            file.truncate(15)
            file.seek(0)
            self.assertEqual(file.read(), b"abX" + b"\0" * 12)
            # A zero-length write must not grow the file at a distant offset.
            file.seek(100)
            self.assertEqual(file.write(b""), 0)
            self.assertEqual(path.stat().st_size, 15)
        path.write_bytes(b"new")
        self.assertEqual(path.read_bytes(), b"new")
        os.truncate(path, 0)
        os.truncate(path, 32)
        self.assertEqual(path.read_bytes(), b"\0" * 32)

    def test_append_and_vectored_io(self):
        path = self.root / "append"
        path.write_bytes(b"first")
        fd = os.open(path, os.O_WRONLY | os.O_APPEND)
        try:
            os.lseek(fd, 0, os.SEEK_SET)
            self.assertEqual(os.writev(fd, [b"-", b"second"]), 7)
            os.fsync(fd)
        finally:
            os.close(fd)
        self.assertEqual(path.read_bytes(), b"first-second")
        fd = os.open(path, os.O_RDONLY)
        try:
            a, b = bytearray(3), bytearray(9)
            self.assertEqual(os.readv(fd, [a, b]), 12)
            self.assertEqual(bytes(a + b), b"first-second")
        finally:
            os.close(fd)

    def test_hard_links_and_open_unlinked_file(self):
        original, alias = self.root / "original", self.root / "alias"
        original.write_bytes(b"original data")
        (self.root / "sub").mkdir()
        third = self.root / "sub" / "third"
        os.link(original, alias)
        os.link(alias, third)
        self.assertEqual(original.stat().st_ino, third.stat().st_ino)
        self.assertEqual(alias.stat().st_nlink, 3)
        self.fails(errno.EEXIST, os.link, original, alias)
        alias.write_bytes(b"shared")
        self.assertEqual(third.read_bytes(), b"shared")
        with original.open("r+b", buffering=0) as file:
            original.unlink()
            alias.unlink()
            self.assertEqual(third.stat().st_nlink, 1)
            third.unlink()
            self.assertEqual(os.fstat(file.fileno()).st_nlink, 0)
            self.assertEqual(file.read(), b"shared")
            file.seek(0)
            file.write(b"alive")
            file.seek(0)
            self.assertEqual(file.read(), b"alived")
            # Reusing the old name must create an independent inode.
            original.write_bytes(b"replacement")
            self.assertNotEqual(original.stat().st_ino, os.fstat(file.fileno()).st_ino)

    def test_mount_isolation(self):
        original = self.root / "local"
        original.write_bytes(b"first mount")
        with tempfile.TemporaryDirectory(prefix="other-", dir=OTHER) as name:
            remote = Path(name) / "local"
            self.assertFalse(remote.exists())
            remote.write_bytes(b"second mount")
            self.fails(errno.EXDEV, os.link, original, Path(name) / "cross-link")
            self.assertEqual(original.read_bytes(), b"first mount")
            self.assertEqual(remote.read_bytes(), b"second mount")

    def test_large_directory(self):
        names = {f"file-{i:04d}" for i in range(1200)}
        for name in names:
            (self.root / name).touch()
        # Requires multiple getdents calls; verifies directory cursor handling.
        self.assertEqual(set(os.listdir(self.root)), names)

    def test_directory_cookies_and_small_buffers(self):
        names = {f"item{i}" for i in range(5)}
        for name in sorted(names):
            (self.root / name).touch()
        lib = ctypes.CDLL(None, use_errno=True)
        lib.getdents64.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t]
        lib.getdents64.restype = ctypes.c_ssize_t
        fd = os.open(self.root, os.O_RDONLY | os.O_DIRECTORY)

        def read_entries(size=32):
            buffer = ctypes.create_string_buffer(size)
            count = lib.getdents64(fd, buffer, size)
            if count < 0:
                error = ctypes.get_errno()
                raise OSError(error, os.strerror(error))
            entries, offset = [], 0
            while offset < count:
                ino, cookie, length, kind = struct.unpack_from("=QqHB", buffer.raw, offset)
                name = buffer.raw[offset + 19:offset + length].split(b"\0", 1)[0].decode()
                entries.append((name, ino, cookie, kind))
                offset += length
            return entries

        try:
            self.fails(errno.EINVAL, read_entries, 8)
            self.assertEqual(os.lseek(fd, 0, os.SEEK_CUR), 0)
            dot, = read_entries()
            parent, = read_entries()
            self.assertEqual((dot[0], dot[1], dot[3]), (".", self.root.stat().st_ino, 4))
            self.assertEqual((parent[0], parent[1], parent[3]), ("..", ROOT.stat().st_ino, 4))
            first, = read_entries()
            self.assertEqual(first[3], 8)  # DT_REG
            saved = os.lseek(fd, 0, os.SEEK_CUR)
            self.assertEqual(first[2], saved)
            (self.root / first[0]).unlink()
            (self.root / "later").touch()
            os.lseek(fd, saved, os.SEEK_SET)
            seen = []
            while batch := read_entries():
                seen.extend(entry[0] for entry in batch)
            expected = names - {first[0]} | {"later"}
            self.assertEqual(set(seen), expected)
            self.assertEqual(len(seen), len(expected))
            os.lseek(fd, 0, os.SEEK_SET)
            self.assertEqual(read_entries()[0][0], ".")
        finally:
            os.close(fd)

    def test_concurrent_namespace_changes(self):
        def change(worker):
            directory = self.root / f"worker-{worker}"
            directory.mkdir()
            for iteration in range(50):
                path = directory / "file"
                alias = self.root / f"link-{worker}"
                contents = f"{worker}:{iteration}".encode()
                path.write_bytes(contents)
                os.link(path, alias)
                path.unlink()
                self.assertEqual(alias.read_bytes(), contents)
                self.assertEqual(alias.stat().st_nlink, 1)
                os.listdir(self.root)
                alias.unlink()
            directory.rmdir()

        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            list(pool.map(change, range(6)))
        self.assertEqual(os.listdir(self.root), [])

    def test_concurrent_append(self):
        path = self.root / "concurrent"
        path.touch()

        def append(worker):
            record = f"{worker:02d}:".encode() + b"x" * 60 + b"\n"
            fd = os.open(path, os.O_WRONLY | os.O_APPEND)
            try:
                for _ in range(100):
                    self.assertEqual(os.write(fd, record), len(record))
            finally:
                os.close(fd)

        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            list(pool.map(append, range(6)))
        records = path.read_bytes().splitlines()
        self.assertEqual(len(records), 600)
        for worker in range(6):
            self.assertEqual(records.count(f"{worker:02d}:".encode() + b"x" * 60), 100)

    def test_bad_userspace_buffers(self):
        path = self.root / "fault"
        path.write_bytes(b"safe")
        lib = ctypes.CDLL(None, use_errno=True)
        lib.write.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t]
        lib.write.restype = ctypes.c_ssize_t
        lib.read.argtypes = lib.write.argtypes
        lib.read.restype = ctypes.c_ssize_t
        fd = os.open(path, os.O_RDWR)
        try:
            self.assertEqual(lib.write(fd, ctypes.c_void_p(1), 4), -1)
            self.assertEqual(ctypes.get_errno(), errno.EFAULT)
            self.assertEqual(lib.read(fd, ctypes.c_void_p(1), 4), -1)
            self.assertEqual(ctypes.get_errno(), errno.EFAULT)
            self.assertEqual(os.lseek(fd, 0, os.SEEK_CUR), 0)
            self.assertEqual(path.read_bytes(), b"safe")
        finally:
            os.close(fd)

    def test_file_size_limit(self):
        path = self.root / "limited"
        # Apply the limit in a child so the test runner remains unaffected.
        code = """
import errno, os, resource, signal, sys
signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
resource.setrlimit(resource.RLIMIT_FSIZE, (8, 8))
fd = os.open(sys.argv[1], os.O_WRONLY | os.O_CREAT, 0o777)
assert os.write(fd, b'0123456789') == 8
try:
    os.write(fd, b'x')
except OSError as error:
    assert error.errno == errno.EFBIG
else:
    raise AssertionError('write exceeded RLIMIT_FSIZE')
os.close(fd)
"""
        subprocess.run([sys.executable, "-c", code, str(path)], check=True)
        self.assertEqual(path.read_bytes(), b"01234567")

    def test_shell_commands(self):
        subprocess.run(["sh", "-ec", """
            cd "$1"
            mkdir directory
            touch empty
            echo hello > directory/file
            echo world >> directory/file
            ln directory/file alias
            cp alias copy
            cmp alias copy
            test "$(cat copy)" = "$(printf 'hello\nworld')"
            ls -lai directory
            rm directory/file
            test "$(cat alias)" = "$(cat copy)"
            rmdir directory
            rm empty alias copy
        """, "vtfs-test", str(self.root)], check=True, stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    unittest.main(verbosity=2)
