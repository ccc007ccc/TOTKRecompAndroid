#!/usr/bin/env python3
"""Exercise shared-list filtering and fidelity through the actual upstream parser."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest

import import_shader_warm_list as importer
import build_shader_warm_manifest as manifest
from shader_list import ShaderList, canonical, pack_key

VS, FS, CS, FOREIGN = (f'{i:064x}' for i in range(1, 5))


def variant(key):
    return key + '-0123456789ab_b'


def pipeline(vs=VS, fs=FS):
    data = bytearray(544)
    for offset, key in ((0, vs), (80, fs)):
        name = variant(key).encode()
        data[offset:offset + len(name)] = name
    data[160:162] = bytes((0, 1))
    return data.hex()


class SharedListTests(unittest.TestCase):
    def test_portable_binding_types_and_merged_outputs(self):
        data = bytearray.fromhex(pipeline())
        import struct
        struct.pack_into('<I', data, 336 + 4 * 2, 0x27 | (3 << 27))  # NVN RGBA8_UINT, stream 3
        struct.pack_into('<I', data, 336 + 4 * 3, 0x28 | (17 << 27))  # unbound stream -> float default
        struct.pack_into('<I', data, 216, 114)  # Metal RGBA16_SINT
        data[162], data[528] = 1, 5            # merged attachment 0 takes logical output 5
        attr, output = manifest.render_types(data.hex())
        self.assertEqual((attr[2], attr[3], output[5], output[0]), (2, 0, 1, 0))
        struct.pack_into('<I', data, 220, 73)   # same logical output bound as uint is contradictory
        data[529] = 5
        with self.assertRaises(ValueError):
            manifest.render_types(data.hex())

    def test_portable_dedup_promotes_title(self):
        shared = ShaderList()
        shared.add_warm(('R', pipeline()), 'm')
        shared.add_warm(('T', pipeline(), '3:0;'), 't')
        blob, summary = manifest.build(shared)
        self.assertEqual((summary['requests'], summary['title_requests']), (1, 1))
        self.assertEqual(blob[:8], manifest.MAGIC)
        row = manifest.RECORD.unpack(blob[16:])
        self.assertEqual((row[0], row[1], row[3].hex(), row[4].hex()), (manifest.PAIR, 1, VS, FS))

    def test_native_key_validation_and_aliases(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'keys.tsv'
            path.write_text(f'{VS}\t0\t0\n{VS}\t0\t1\n{FS}\t1\t2\n')
            self.assertEqual(importer.raw_keys(path)[VS]['records'], [0, 1])
            for text in ('', f'{VS}\t3\t0', f'{VS}\t0\t-1',
                         f'{VS}\t0\t0\n{FS}\t1\t0', f'{VS}\t0\t0\n{VS}\t1\t1'):
                path.write_text(text)
                with self.assertRaises(ValueError):
                    importer.raw_keys(path)

    def test_upstream_roundtrip_title_and_types(self):
        deps = canonical(dict(attr={'2': 'uint'}, fout={'0': 'int'},
                              tex={'0': '*_depth'}, img={'1': {'type': '2d', 'fmt': 'r32i'}}))
        row = ('R', pipeline())
        unknown = ('R', pipeline(fs=FOREIGN))
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'shared.tsv'
            path.write_text(f'V\t{VS}\t{deps}\nR\t{pack_key(row[1])}\tm\n'
                            f'R\t{row[1]}\tt\nR\t{unknown[1]}\tm\n'
                            f'S\t{variant(VS)}\t0\tt\nC\t{variant(CS)}\tm\n'
                            f'S\t{variant(FS)}\t0\tm\nR\t00\tm\n')
            shared = ShaderList.load(path)
            self.assertEqual(shared.warm[row], 'tm')
            shaders = {VS: dict(stage=0, records=[0]), FS: dict(stage=1, records=[1]),
                       CS: dict(stage=5, records=[2])}
            output = Path(temp) / 'out'
            summary = importer.export(shared, shaders, output, dict(commit='test'))
            selected = ShaderList.load(output / 'shader_list.tsv')
            self.assertEqual(selected.variants, {(VS, deps)})
            self.assertEqual(selected.warm[row], 'tm')
            self.assertEqual(summary['rejected_warm_rows'], dict(shader_not_in_selected_game=1,
                             stage_mismatch=1, unsupported_pipeline_abi_or_custom_shader=1))
            self.assertEqual(summary['title_warm_rows'], 2)
            self.assertEqual(summary['title_raw_keys'], 2)
            title = (output / 'pso_title.tsv').read_text()
            self.assertLess(title.index('R\t'), title.index('S\t'))
            jobs = [json.loads(line) for line in (output / 'seed-jobs.jsonl').read_text().splitlines()]
            listed = next(job for job in jobs if job['reason'] == 'listed')
            self.assertEqual(listed['answers']['attr'], {'2': 'uint'})
            self.assertEqual(listed['answers']['fout'], {'0': 'int'})
            self.assertEqual(listed['answers']['tex'], {'0': '2d_depth'})
            self.assertEqual(listed['answers']['img'], {'1': {'type': '2d', 'fmt': 'r32i'}})
            self.assertFalse(summary['spirv_prepared'])
            self.assertFalse(summary['vulkan_pipeline_prepared'])

    def test_borders_tiles_and_paired_stage_are_preserved(self):
        shared = ShaderList()
        border = ('B', pipeline(), '1:7:0x0p+0,0x0p+0,0x0p+0,0x1p+0;', '-')
        tile = ('T', pipeline(), '3:0;')
        shared.add_warm(border, 't')
        shared.add_warm(tile, 'm')
        shaders = {VS: dict(stage=0), FS: dict(stage=1)}
        selected, rejected = importer.selected_list(shared, shaders)
        self.assertEqual(selected.warm, shared.warm)
        self.assertFalse(rejected)
        shaders[FS]['stage'] = 5
        selected, rejected = importer.selected_list(shared, shaders)
        self.assertFalse(selected.warm)
        self.assertEqual(rejected, {'stage_mismatch': 2})

    def test_pinned_real_list(self):
        shared, provenance = importer.verified_list()
        self.assertEqual(provenance['commit'], 'f024ef04c707c4b4bb5b6328bfc55ae89642af40')
        self.assertEqual(len(shared.variants), 13802)
        self.assertEqual(len(shared.warm), 70971)
        self.assertEqual(sum('t' in flags for flags in shared.warm.values()), 1177)

    def test_portable_sha256_against_hashlib(self):
        library = os.environ.get('TOTK_TEST_DIGEST_LIB')
        if not library:
            self.skipTest('run tools/test_shader_warm_list.sh for portable C digest checks')
        digest = ctypes.CDLL(library).CC_SHA256
        digest.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
        digest.restype = ctypes.c_void_p
        for size in (0, 1, 3, 55, 56, 63, 64, 65, 127, 128, 1024, 4097):
            data = bytes((i * 37 + 11) % 256 for i in range(size))
            result = ctypes.create_string_buffer(32)
            digest(data, len(data), result)
            self.assertEqual(result.raw, hashlib.sha256(data).digest())


if __name__ == '__main__':
    unittest.main()
