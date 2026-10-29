#!/usr/bin/env python3
"""Compare original upstream scans of local SHARCB/BNSH files with native raw keys.

Reports identifiers and validation failures only; never exports game code. Invalid
heuristic hits are distinguished from valid shaders missing from our catalog.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct

from import_shader_warm_list import raw_keys
from prepare_shader_seed import bnsh_shaders, sharcb_shaders


def invalid_reason(stage, control, code):
    if len(control) < 0x718 or struct.unpack_from('<I', control)[0] != 0x98761234:
        return 'invalid_control_magic_or_length'
    constants, length, entry = struct.unpack_from('<III', control, 0x700)
    expected_stage = {'vs': 0, 'ps': 1, 'cs': 5}[stage]
    if struct.unpack_from('<I', control, 0x714)[0] != expected_stage:
        return 'control_stage_mismatch'
    if length != len(code) or length % 8 or constants % 8 or not entry < constants <= length:
        return 'invalid_code_bounds'
    if entry != (0x100 if stage == 'cs' else 0x30):
        return 'invalid_code_entry'
    if len(code) < 4 or struct.unpack_from('<I', code)[0] != (0x12345679 if stage == 'cs' else 0x12345678):
        return 'invalid_code_magic'
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('raw_keys', type=Path)
    parser.add_argument('archive_directory', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    keys = raw_keys(args.raw_keys)
    results, invalid, missing = [], Counter(), set()
    for path in sorted(args.archive_directory.rglob('*')):
        if not path.is_file() or path.suffix not in ('.bnsh', '.sharcb'):
            continue
        data = path.read_bytes()
        scan = bnsh_shaders if data[:4] == b'BNSH' else sharcb_shaders if data[:4] == b'BAHS' else None
        if scan is None:
            raise ValueError(f'{path}: declared archive has an invalid signature')
        matched, failures, unresolved = 0, [], []
        for key, stage, control, code in scan(data):
            reason = invalid_reason(stage, control, code)
            if reason:
                invalid[reason] += 1
                failures.append(dict(key=key, stage=stage, reason=reason, code_length=len(code)))
            elif key not in keys:
                missing.add(key)
                unresolved.append(key)
            else:
                expected = {'vs': 0, 'ps': 1, 'cs': 5}[stage]
                if keys[key]['stage'] != expected:
                    raise ValueError(f'{path}: same raw key has different stages: {key}')
                matched += 1
        results.append(dict(file=str(path), matched_valid_rows=matched, invalid_candidates=failures, valid_unmatched=unresolved))
    if not results:
        raise ValueError('no local BNSH/SHARCB archives found')
    summary = dict(archives=len(results), matched_valid_rows=sum(r['matched_valid_rows'] for r in results),
                   invalid_candidates=dict(invalid), valid_unmatched_keys=len(missing),
                   scope='only the supplied local BNSH/SHARCB archives')
    args.output.write_text(json.dumps(dict(summary=summary, archives=results), indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return int(bool(missing))


if __name__ == '__main__':
    raise SystemExit(main())
