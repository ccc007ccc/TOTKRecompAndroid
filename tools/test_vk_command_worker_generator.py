#!/usr/bin/env python3
"""Fail closed for newly introduced recording pointers, including array elements and headers."""
from pathlib import Path
import tempfile
import unittest

from gen_vk_command_worker import generate


class GeneratorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sources = self.root / 'runtime/nvn/vulkan'
        self.sources.mkdir(parents=True)
        self.header = (Path(__file__).resolve().parents[1] /
                       'android/deps/out/include/vulkan/vulkan_core.h').read_text()
        self.out = self.root / 'out'

    def test_headers_and_deterministic_generation(self):
        (self.sources / 'capture.h').write_text('vkCmdSetStencilReference(cb, faces, value);')
        generate(self.root, self.header, self.out)
        files = sorted(self.out.iterdir())
        before = [(p.read_bytes(), p.stat().st_mtime_ns) for p in files]
        self.assertIn('#define vkCmdSetStencilReference(...)', files[0].read_text())
        generate(self.root, self.header, self.out)
        self.assertEqual(before, [(p.read_bytes(), p.stat().st_mtime_ns) for p in files])

    def test_unknown_pointer(self):
        (self.sources / 'new.c').write_text('vkCmdUpdateBuffer(cb, buffer, off, n, data);')
        with self.assertRaisesRegex(ValueError, 'No snapshot rule for vkCmdUpdateBuffer'):
            generate(self.root, self.header, self.out)
        self.assertFalse(self.out.exists())

    def test_unknown_array_element(self):
        (self.sources / 'new.c').write_text('vkCmdFuture(cb, count, regions);')
        declaration = ('typedef void (VKAPI_PTR *PFN_vkCmdFuture)(VkCommandBuffer commandBuffer, '
                       'uint32_t regionCount, const VkBufferCopy2* pRegions);')
        with self.assertRaisesRegex(ValueError, 'No snapshot rule for vkCmdFuture'):
            generate(self.root, self.header + declaration, self.out)
        self.assertFalse(self.out.exists())


if __name__ == '__main__':
    unittest.main()
