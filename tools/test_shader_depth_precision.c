// Compile through the production shader path. The emitted modules can also be
// used in a GPU depth-EQUAL probe; no game data is needed for the regression.
#include <assert.h>
#include <unistd.h>
#include "../runtime/nvn/vulkan/shaders.c"

VkG g_vk;
void rt_fatal_trace(void) { abort(); }

static unsigned arithmetic_count(const uint32_t *s, size_t n, bool require_precise) {
    uint8_t *decorated = calloc(s[3], 1);
    assert(decorated);
    for (size_t i = 5; i < n; i += s[i] >> 16)
        if ((s[i] & 65535) == 71 && (s[i] >> 16) >= 3 && s[i + 2] == 42) {
            assert(s[i + 1] < s[3]);
            assert(!decorated[s[i + 1]]);  // no duplicate NoContraction decorations
            decorated[s[i + 1]] = 1;
        }
    unsigned count = 0;
    for (size_t i = 5; i < n; i += s[i] >> 16)
        if (float_arith(s + i, s[i] >> 16, s[i] & 65535)) {
            count++;
            if (require_precise) assert(decorated[s[i + 2]]);
        }
    free(decorated);
    return count;
}

#define PUSH "layout(push_constant) uniform Push { float z; float k; } pc;\n"
#define DEPTH "float depth(){float d=pc.z*uintBitsToFloat(0x3b808081u);d=fma(d,-pc.k,d);return d+pc.k;}\n"
static const char *names[] = {"depth.vert", "write.frag", "match.frag", "coord.frag", "explicit.frag"};
static const char *sources[] = {
    "#version 450\n" PUSH "invariant gl_Position;precise gl_Position;\n" DEPTH
    "void main(){vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(p*2-1,(depth()-0.5)*2,1);"
    "gl_Position.z=(gl_Position.z+gl_Position.w)*0.5;}\n",
    "#version 450\n" PUSH DEPTH "layout(location=0) out vec4 c;void main(){c=vec4(0);gl_FragDepth=depth();}\n",
    "#version 450\n" PUSH DEPTH "layout(location=0) out vec4 c;void main(){c=vec4(depth());}\n",
    "#version 450\n" PUSH "layout(location=0) out vec4 c;void main(){c=vec4(1);gl_FragDepth=gl_FragCoord.z;}\n",
    "#version 450\n" PUSH DEPTH "layout(location=0) out vec4 c;void main(){c=vec4(1);gl_FragDepth=depth();}\n",
};

int main(int argc, char **argv) {
    assert(argc >= 2 && chdir(argv[1]) == 0);
    unsetenv("LARECOMP_VK_OPT");
    setenv("LARECOMP_VK_NO_SPIRV_CACHE", "1", 1);
    for (int i = 0; i < 5; i++) {
        size_t n;
        int stage = i ? STAGE_FRAGMENT : STAGE_VERTEX;
        uint32_t *s = vk_glsl_to_spirv(sources[i], stage, names[i], &n);
        assert(s && vk_shader_cache_framed(s, n));
        assert(writes_fragment_depth(s, n) == (i == 1 || i >= 3));
        unsigned count = arithmetic_count(s, n, i != 2);
        assert(count || i == 3);
        if (i == 2) { // ordinary color arithmetic keeps its existing compilation policy
            uint32_t decorations = 0;
            for (size_t k = 5; k < n; k += s[k] >> 16)
                decorations += (s[k] & 65535) == 71 && (s[k] >> 16) >= 3 && s[k + 2] == 42;
            assert(!decorations);
            // The render probe's control outputs white with ordinary raster depth.
            free(s);
            s = vk_glsl_to_spirv("#version 450\nlayout(location=0) out vec4 c;void main(){c=vec4(1);}", stage, names[i], &n);
            assert(s);
        }
        size_t old_n = n;
        uint32_t *again = malloc(n * 4);
        assert(again);
        memcpy(again, s, n * 4);
        if (i != 2) {
            again = no_contraction(again, &old_n);
            assert(old_n == n && !memcmp(s, again, n * 4));
        }
        free(again);
        char path[80]; snprintf(path, sizeof path, "%s.spv", names[i]);
        FILE *f = fopen(path, "wb"); assert(f);
        assert(fwrite(s, 4, n, f) == n && fclose(f) == 0);
        free(s);
    }
    unsetenv("LARECOMP_VK_NO_SPIRV_CACHE");
    size_t n1, n2;
    uint32_t *a = vk_glsl_to_spirv(sources[1], STAGE_FRAGMENT, "cache-depth", &n1);
    uint32_t *b = vk_glsl_to_spirv(sources[1], STAGE_FRAGMENT, "cache-depth", &n2);
    assert(a && b && n1 == n2 && !memcmp(a, b, n1 * 4));
    assert(arithmetic_count(b, n2, true));
    assert(vk_shader_source_hash("", 0, false) == 0xcef29ce484222325ull);
    free(a); free(b);
    puts("PASS: vertex and explicit fragment depth preserve arithmetic; color, cache and idempotence checks passed");
    return 0;
}
