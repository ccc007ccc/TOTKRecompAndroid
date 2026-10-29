#!/usr/bin/env python3
"""Import the pinned shared list using its original parser and seed planner.

Produces reviewable metadata for a Vulkan warm-up adapter, not SPIR-V or a ready
marker. The selected game's raw keys come from audit_shader_raw_keys.cpp. Metal
pipeline states and dependency types are retained exactly; no Metal-only matching
rule is used to collapse Vulkan interface types.
"""
import argparse
from collections import Counter
import gzip
import hashlib
import json
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / 'third_party/macnativegames'
sys.path.insert(0, str(VENDOR))
from shader_list import KEY, VARIANT, ShaderList, canonical  # noqa: E402
from prepare_shader_seed import plan  # noqa: E402


def verified_list():
    provenance = json.loads((VENDOR / 'UPSTREAM.json').read_text())
    for name, entry in provenance['files'].items():
        data = (VENDOR / name).read_bytes()
        if name.endswith('.gz'):
            data = gzip.decompress(data)
        if hashlib.sha256(data).hexdigest() != entry['sha256_uncompressed']:
            raise ValueError(f'pinned upstream file changed: {name}')
    data = gzip.decompress((VENDOR / 'shader_list.tsv.gz').read_bytes())
    # Use the unchanged upstream parser, including its validation, normalization,
    # duplicate merging and recorded warm order.
    with tempfile.TemporaryDirectory(prefix='totk-shader-list-') as temporary:
        path = Path(temporary) / 'shader_list.tsv'
        path.write_bytes(data)
        return ShaderList.load(path), provenance


def raw_keys(path):
    shaders, records = {}, set()
    for number, line in enumerate(Path(path).read_text().splitlines(), 1):
        if not line or line.startswith('#'):
            continue
        fields = line.split('\t')
        if len(fields) != 3 or not KEY.fullmatch(fields[0]):
            raise ValueError(f'{path}:{number}: invalid native raw-key row')
        key, stage, record = fields[0], int(fields[1]), int(fields[2])
        if stage not in (0, 1, 5) or record < 0 or record in records:
            raise ValueError(f'{path}:{number}: invalid/duplicate stage or catalog record')
        records.add(record)
        if key in shaders and shaders[key]['stage'] != stage:
            raise ValueError(f'{path}:{number}: one shader key has different stages')
        shaders.setdefault(key, dict(stage=stage, records=[]))['records'].append(record)
    if not shaders:
        raise ValueError('selected game has no shader keys')
    return shaders


def warm_shaders(row):
    """The exact raw keys/stages a warm row needs, or None for another ABI.

    R/B/T use the 544-byte upstream pso_skey, whose leading names are 80 bytes
    each and whose stage bytes follow. Built-in/custom names are not game keys.
    """
    if row[0] in ('S', 'C'):
        return [(row[1][:64], int(row[2]) if row[0] == 'S' else 5)]
    if row[0] not in ('R', 'B', 'T'):
        return None
    data = bytes.fromhex(row[1])
    if len(data) != 544:
        return None
    result = []
    for at, stage in ((0, data[160]), (80, data[161])):
        try:
            name = data[at:at + 80].split(b'\0', 1)[0].decode('ascii')
        except UnicodeError:
            return None
        if not VARIANT.fullmatch(name):
            return None
        result.append((name[:64], stage))
    return result


def selected_list(shared, shaders):
    selected, rejected = ShaderList(), Counter()
    selected.variants = {(key, deps) for key, deps in shared.variants if key in shaders}
    selected.locators = {row for row in shared.locators if row[0] in shaders}
    for row, flags in shared.warm.items():
        needed = warm_shaders(row)
        if needed is None:
            rejected['unsupported_pipeline_abi_or_custom_shader'] += 1
        elif any(key not in shaders for key, _ in needed):
            rejected['shader_not_in_selected_game'] += 1
        elif any(shaders[key]['stage'] != stage for key, stage in needed):
            rejected['stage_mismatch'] += 1
        else:
            selected.add_warm(row, flags)
    return selected, dict(rejected)


def export(shared, shaders, output, provenance):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    selected, rejected = selected_list(shared, shaders)
    selected.save(output / 'shader_list.tsv')
    (output / 'pso_title.tsv').write_text(selected.export('t'))
    (output / 'pso_manifest.tsv').write_text(selected.export('m'))
    # Call the original default + observed-state seed planner. Texture wildcard
    # handling is inherited; attr/fout/img answers remain present in the output.
    jobs = plan(shaders, set(), selected.variants, all_default=True)
    with (output / 'seed-jobs.jsonl').open('w') as stream:
        for key, answers, variant, reason in jobs:
            stream.write(json.dumps(dict(key=key, stage=shaders[key]['stage'],
                                        answers=answers, variant=variant, reason=reason),
                                    sort_keys=True, separators=(',', ':')) + '\n')
    # Pairing, render state, border colours and tile reads cannot be treated as
    # compiled Vulkan pipelines. Preserve them in the exported upstream format.
    title_keys = {key for row, flags in selected.warm.items() if 't' in flags
                  for key, _ in warm_shaders(row)}
    referenced = {key for row in shared.warm for key, _ in warm_shaders(row) or ()}
    listed_keys = {key for key, _ in shared.variants} | referenced
    missing = sorted(listed_keys - shaders.keys())
    (output / 'unmatched-keys.txt').write_text(''.join(key + '\n' for key in missing))
    summary = dict(schema=1, upstream_commit=provenance['commit'],
                   catalog_records=sum(len(v['records']) for v in shaders.values()),
                   selected_game_raw_keys=len(shaders),
                   keys_with_multiple_catalog_records=sum(len(v['records']) > 1 for v in shaders.values()),
                   shared_variants=len(shared.variants), matched_variants=len(selected.variants),
                   shared_warm_rows=len(shared.warm), matched_warm_rows=len(selected.warm),
                   matched_warm_by_kind=dict(Counter(row[0] for row in selected.warm)),
                   title_warm_rows=sum('t' in f for f in selected.warm.values()),
                   title_raw_keys=len(title_keys), unmatched_shared_keys=len(missing),
                   rejected_warm_rows=rejected, upstream_seed_requests=len(jobs),
                   seed_requests_by_reason=dict(Counter(job[3] for job in jobs)),
                   spirv_prepared=False, vulkan_pipeline_prepared=False)
    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('raw_keys', help='native audit TSV from the selected RomFS')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    shared, provenance = verified_list()
    print(json.dumps(export(shared, raw_keys(args.raw_keys), args.output, provenance), indent=2))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError) as error:
        sys.exit(str(error))
