#!/usr/bin/env python3
"""Query metadata produced by the native shader catalog audit (no game code is exported)."""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--asset', help='Substring of the model file path')
    parser.add_argument('--material', help='Substring of the material name')
    parser.add_argument('--named', help='Substring of a SHARCB archive/program name')
    parser.add_argument('--limit', type=int, default=3, help='Maximum matching materials or named groups')
    parser.add_argument('--program-limit', type=int, default=8, help='Maximum displayed programs per match')
    args = parser.parse_args()
    if args.limit < 1 or args.program_limit < 1:
        parser.error('limits must be positive')
    graph = json.loads((args.directory / 'catalog.json').read_text())
    names = ('vertex', 'fragment', 'geometry', 'tess_control', 'tess_eval', 'compute')

    def stages(ids):
        return {name: {'record': ident, 'input_hash': graph['records'][ident]['input_hash']}
                for name, ident in zip(names, ids) if ident >= 0}

    found = []
    if args.named:
        for group in graph['named_programs']:
            if args.named not in group['archive'] + '/' + group['name']:
                continue
            found.append({'source': group['source'], 'archive': group['archive'], 'program': group['name'],
                          'variation_count': len(group['variations']),
                          'variations': [stages(ids) for ids in group['variations'][:args.program_limit]]})
            if len(found) >= args.limit:
                break
    else:
        with (args.directory / 'materials.jsonl').open() as stream:
            for line in stream:
                row = json.loads(line)
                if args.asset and args.asset not in row['source']:
                    continue
                if args.material and args.material not in row['material']:
                    continue
                programs = []
                model = graph['models'][row['catalog_model']] if row['catalog_model'] >= 0 else None
                for ident in row['candidate_programs'][:args.program_limit]:
                    program = model['programs'][ident]
                    dynamic = {o['name']: o['choices'][(program['key'][o['word']] & o['mask']) >> o['shift']]
                               for o in model['options'] if o['dynamic']}
                    programs.append({'program': ident, 'stages': stages(program['stages']), 'dynamic_options': dynamic})
                found.append({'source': row['source'], 'model': row['model'], 'material': row['material'],
                              'shader_file': model['source'] if model else None, 'shader_model': row['shader_model'],
                              'candidate_count': len(row['candidate_programs']), 'shown_programs': programs,
                              'shapes': row['shapes'], 'unresolved': row['unresolved']})
                if len(found) >= args.limit:
                    break
    print(json.dumps({'runtime_state_complete': False, 'matches': found}, ensure_ascii=False, indent=2))
    return 0 if found else 1


if __name__ == '__main__':
    raise SystemExit(main())
