"""Exercise the production full texture hash, including protected memory boundaries (run in dev)."""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
text = (root/'runtime/nvn/vulkan/textures.c').read_text()
hashes = text[text.index('#define XXH_INLINE_ALL'):text.index('// (1024 bytes spread')]
source = r'''
#define _GNU_SOURCE
#include "runtime/diagnostics_build.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <inttypes.h>
''' + hashes + r'''
int main(void) {
    // Published XXH3 vectors, independent of this runtime's implementation.
    int legacy=0;
#if TOTK_DIAGNOSTICS
    const char *e=getenv("LARECOMP_TEX_LEGACY_HASH"); legacy=e && atoi(e)!=0;
#endif
    if (!legacy) {
        assert(hash_full((const uint8_t *)"",0)==UINT64_C(0x2d06800538d394c2));
        assert(hash_full((const uint8_t *)"abc",3)==UINT64_C(0x78af5f94892f3950));
    } else {
        assert(hash_full((const uint8_t *)"abc",3)!=UINT64_C(0x78af5f94892f3950));
    }
    const size_t page=(size_t)sysconf(_SC_PAGESIZE);
    const size_t extra[]={511,512,513,1023,1024,1025,4095,4096,4097,1u<<20,8u<<20,17u<<20};
    unsigned checks=0;
    for(unsigned test=0;test<258+sizeof extra/sizeof extra[0];++test) {
        size_t n=test<258 ? test : extra[test-258];
        size_t bytes=((n+64+page-1)/page)*page;
        uint8_t *map=mmap(NULL,bytes+2*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        assert(map!=MAP_FAILED && !mprotect(map+page,bytes,PROT_READ|PROT_WRITE));
        for(unsigned edge=0;edge<2;++edge) {
            uint8_t *p=edge ? map+page+bytes-n : map+page;
            for(size_t i=0;i<n;++i) p[i]=(uint8_t)(i*131+(i>>8)*17);
            uint64_t h=hash_full(p,n);
            uint8_t *copy=malloc(n+64); assert(copy);
            for(unsigned align=0;align<32;++align) {
                memcpy(copy+align,p,n); assert(hash_full(copy+align,n)==h); ++checks;
            }
            // Every byte for short inputs; spread mutations and the final byte for larger inputs.
            size_t step=n<=1025 ? 1 : n/32;
            for(size_t i=0;i<n;i+=step) {
                p[i]^=0x80; assert(hash_full(p,n)!=h); p[i]^=0x80; ++checks;
            }
            if(n) {p[n-1]^=1; assert(hash_full(p,n)!=h); p[n-1]^=1; ++checks;}
            free(copy);
        }
        assert(!munmap(map,bytes+2*page));
    }
    printf("PASS: %u content, alignment and guard-page checks; legacy=%d\n",checks,legacy);
}
'''
path = out/'texture-hash.c'
path.write_text(source)
for diagnostics in (0, 1):
    binary = out/f'texture-hash-{diagnostics}'
    subprocess.run([os.environ.get('CC','clang'), '-O1', '-g', '-fsanitize=address,undefined',
                    f'-DTOTK_DIAGNOSTICS={diagnostics}', '-I'+str(root),
                    '-I'+str(root/'android/deps/out/include'), str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=120)
    subprocess.run([str(binary)], check=True, timeout=120,
                   env={**os.environ, 'LARECOMP_TEX_LEGACY_HASH':'1'})
