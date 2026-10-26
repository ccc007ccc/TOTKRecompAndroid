"""TotK input contracts; synthetic headers contain no game code or keys."""
import io
import struct
import tempfile
import unittest
from pathlib import Path

import totk
from pull_totk import extract
from totk_romfs import entries, extract_movies
from nso import Nso


class TotkInputs(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)
        for name, (_, build) in totk.MODULES.items():
            header = bytearray(0x100)
            header[:4] = b"NSO0"
            header[0x40:0x60] = bytes.fromhex(build.ljust(64, "0"))
            struct.pack_into("<I", header, 0x38, 0x1000)
            (self.root / name).write_bytes(header)
        npdm = bytearray(0x100)
        npdm[:4], npdm[0x80:0x84] = b"META", b"ACI0"
        struct.pack_into("<I", npdm, 0x70, 0x80)
        struct.pack_into("<Q", npdm, 0x90, totk.TITLE_ID)
        (self.root / "main.npdm").write_bytes(npdm)

    def test_four_modules(self):
        report = totk.inspect(self.root)
        self.assertEqual(report["version"], "1.4.3")
        self.assertEqual([m["name"] for m in report["modules"]], list(totk.MODULES))

    def test_reject_mixed_sdk(self):
        h = bytearray((self.root / "sdk").read_bytes())
        h[0x40] ^= 1
        (self.root / "sdk").write_bytes(h)
        with self.assertRaisesRegex(ValueError, "mixed build"):
            totk.inspect(self.root)

    def test_reject_botw_title(self):
        h = bytearray((self.root / "main.npdm").read_bytes())
        struct.pack_into("<Q", h, 0x90, 0x01007EF00011E000)
        (self.root / "main.npdm").write_bytes(h)
        with self.assertRaisesRegex(ValueError, "not Tears"):
            totk.inspect(self.root)

    def test_reject_overlap(self):
        h = bytearray((self.root / "main").read_bytes())
        struct.pack_into("<I", h, 0x3C, 0x4000000)
        (self.root / "main").write_bytes(h)
        with self.assertRaisesRegex(ValueError, "overlaps"):
            totk.inspect(self.root)

    def test_reject_missing_multimedia(self):
        (self.root / "subsdk0").unlink()
        with self.assertRaises(FileNotFoundError):
            totk.inspect(self.root)

    def package(self, name, data=b"payload"):
        names = name.encode() + b"\0"
        return io.BytesIO(struct.pack("<4sIII", b"PFS0", 1, len(names), 0) +
                          struct.pack("<QQII", 0, len(data), 0, 0) + names + data)

    def test_stream_extract(self):
        out = self.root / "ncas"
        extract(self.package("test.nca"), out)
        self.assertEqual((out / "test.nca").read_bytes(), b"payload")

    def test_stream_reject_traversal(self):
        with self.assertRaisesRegex(ValueError, "unsafe"):
            extract(self.package("../outside"), self.root / "ncas")

    def test_stream_truncation_not_published(self):
        data = self.package("test.nca").getvalue()[:-1]
        out = self.root / "ncas"
        with self.assertRaisesRegex(ValueError, "truncated"):
            extract(io.BytesIO(data), out)
        self.assertFalse((out / "test.nca").exists())

    def romfs(self):
        empty = 0xFFFFFFFF
        data = bytearray(519)
        struct.pack_into("<10Q", data, 0, 80, 80, 4, 96, 56, 84, 4, 160, 44, 512)
        struct.pack_into("<6I", data, 96, 0, empty, 24, empty, empty, 0)
        struct.pack_into("<6I", data, 120, 0, empty, empty, 0, empty, 5)
        data[144:149] = b"Movie"
        struct.pack_into("<IIQQII", data, 160, 24, empty, 0, 7, empty, 12)
        data[192:204] = b"opening.webm"
        data[512:] = b"payload"
        return data

    def test_romfs_movie_export(self):
        image = self.root / "romfs.bin"
        image.write_bytes(self.romfs())
        self.assertEqual(list(entries(image)), [("Movie/opening.webm", 512, 7)])
        extract_movies(image, self.root / "loose")
        self.assertEqual((self.root / "loose/Movie/opening.webm").read_bytes(), b"payload")

    def test_romfs_reject_cyclic_directory(self):
        data = self.romfs()
        struct.pack_into("<I", data, 128, 24)
        image = self.root / "romfs.bin"
        image.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "cyclic"):
            list(entries(image))

    def test_romfs_reject_truncated_payload(self):
        image = self.root / "romfs.bin"
        image.write_bytes(self.romfs()[:-1])
        with self.assertRaisesRegex(ValueError, "truncated"):
            list(entries(image))

    def uncompressed_nso(self):
        data = bytearray(0x180)
        data[:4] = b"NSO0"
        struct.pack_into("<III", data, 0x10, 0x100, 0, 0x80)
        struct.pack_into("<III", data, 0x20, 0x180, 0x80, 0)
        struct.pack_into("<III", data, 0x30, 0x180, 0x80, 0)
        struct.pack_into("<I", data, 0x104, 0x20)
        data[0x120:0x124] = b"MOD0"
        struct.pack_into("<i", data, 0x124, 0x30)  # dynamic table at image + 0x50
        return data

    def test_nso_uncompressed_without_compressed_size(self):
        image = self.root / "plain.nso"
        image.write_bytes(self.uncompressed_nso())
        self.assertEqual(Nso(image).image[0x20:0x24], b"MOD0")

    def test_nso_truncated_segment(self):
        image = self.root / "plain.nso"
        image.write_bytes(self.uncompressed_nso()[:-1])
        with self.assertRaisesRegex(ValueError, "truncated NSO segment"):
            Nso(image)


if __name__ == "__main__":
    unittest.main()
