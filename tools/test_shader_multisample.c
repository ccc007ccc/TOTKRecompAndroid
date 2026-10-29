// TLDS.MS uses Ra/Ra+1 for coordinates and Rb for the sample. Exercise the
// production emitter and result packing; generated shaders need SPIR-V validation.
#include <assert.h>
#include "../runtime/nvn/maxwell/translate.c"

int main(int argc, char **argv) {
    assert(argc == 2);
    unsigned count = 0;
    for (unsigned half = 0; half < 2; ++half)
    for (unsigned split = 0; split < 2; ++split)
    for (unsigned mask = 0; mask < 8; ++mask)
    for (unsigned edge = 0; edge < 3; ++edge) {
        unsigned d0 = edge ? 253 + edge : 8, d1 = split ? 12 : RZ;
        uint64_t word = 0xd200000000000000ull | (uint64_t)!half << 59 |
                        6ull << 53 | (uint64_t)mask << 50 | 8ull << 36 |
                        (uint64_t)d1 << 28 | 6ull << 20 | 7ull << 16 | 4ull << 8 | d0;
        unsigned char data[0xb0] = {0};
        // A dimension query may allocate a weak slot before TLDS establishes MS.
        uint64_t query = 0xdf48000000000000ull | 8ull << 36 | 15ull << 31 | 1ull << 22 | 7ull << 16 | 16;
        uint64_t code[] = {0, query, word, 0xe30000000007000full, 0, 0xe2400fffff87000full};
        uint32_t omap = 15;
        memcpy(data + 0x78, &omap, 4); memcpy(data + 0x80, code, sizeof code);
        ShaderKey key = {.glsl = 1, .vs_out_mask = ~0u};
        ShaderInfo *s = maxwell_translate(data, sizeof data, STAGE_FRAGMENT, &key);
        assert(s && s->msl && s->ntex == 1 && s->tex[0].type == TEXTYPE_2D_MS);
        assert(s->uses_rscale && s->tex[0].handle_offset == 0x20);
        assert(strstr(s->msl, "uniform sampler2DMS t0;") && strstr(s->msl, "textureSize(t0)"));
        assert(!strstr(s->msl, "textureSize(t0, "));
        assert(!strstr(s->msl, "textureQueryLevels(t0)") && !strstr(s->msl, "texture_levels"));
        assert(strstr(s->msl, "texelFetch(t0, ivec2(rsc(uvec2(r4, r5), rscale[1])), int(r6))"));
        assert(!strstr(s->msl, "r255 =") && !strstr(s->msl, "r256"));
        char path[1024]; snprintf(path, sizeof path, "%s/multisample-%u.frag", argv[1], count++);
        FILE *f = fopen(path, "w"); assert(f); fputs(s->msl, f); assert(!fclose(f));
        maxwell_free(s);
    }
    // A slot cannot simultaneously be a single-sample and multisample sampler.
    ShaderInfo info = {0}; Ctx c = {.info = &info, .glsl = true};
    tex_slot(&c, 8, TEXTYPE_2D_MS, false, false);
    tex_slot(&c, 8, TEXTYPE_2D, false, false);
    assert(c.fail);
    // GPU probes: use distinct physical coordinates, channels and sample values.
    // Scale only the coordinates (2x), never the sample index. Both result encodings
    // must recover the same four exactly representable values.
    for (unsigned half = 0; half < 2; ++half) {
        ShaderInfo probe_info = {0}; ShaderKey key = {.glsl = 1};
        Ctx probe = {.info = &probe_info, .key = &key, .glsl = true, .stage = STAGE_COMPUTE};
        uint64_t word = 0xd200000000000000ull | (uint64_t)!half << 59 | 6ull << 53 |
                        4ull << 50 | 8ull << 36 | 12ull << 28 | 6ull << 20 | 7ull << 16 | 4ull << 8 | 8;
        emit_texs(&probe, word, true); assert(!probe.fail);
        Str body = {0}; glsl_rewrite(&probe, &body, probe.body.buf);
        char path[1024]; snprintf(path, sizeof path, "%s/read-%u.comp", argv[1], half);
        FILE *f = fopen(path, "w"); assert(f);
        fputs("#version 450\nlayout(local_size_x=1,local_size_y=1,local_size_z=1) in;\n"
              "layout(set=0,binding=0) uniform sampler2DMS t0;\n"
              "layout(std430,set=0,binding=1) buffer Output {vec4 values[];};\n"
              "uint as_u(float x){return floatBitsToUint(x);}\n"
              "uvec2 rsc(uvec2 p,vec4 s){return uvec2(vec2(p)*s.xy);}\n"
              "void main(){vec4 rscale[2]=vec4[2](vec4(1),vec4(2,2,0,0));\n"
              "uint r4=gl_GlobalInvocationID.x,r5=gl_GlobalInvocationID.y,r6=gl_GlobalInvocationID.z;\n"
              "uint r8=0u,r9=0u,r12=0u,r13=0u;\n", f);
        fputs(body.buf, f);
        fprintf(f, "values[r4+2u*r5+4u*r6]=%s;}\n", half ?
                "vec4(unpackHalf2x16(r8),unpackHalf2x16(r12))" : "uintBitsToFloat(uvec4(r8,r9,r12,r13))");
        assert(!fclose(f)); free(body.buf); free(probe.body.buf);
    }
    printf("Generated %u TLDS.MS cases; coordinates, sample, resource type and register boundaries passed\n", count);
}
