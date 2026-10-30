#include "shader_artifact.h"
#include "shader_prepare.h"
#include "shader_spirv.h"
#include "shader_artifact_storage.hpp"
#include "shader_keys.h"
#include "../../diagnostics_build.h"
extern "C" {
#include "../../../third_party/d2r-digest/include/CommonCrypto/CommonDigest.h"
}
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef SHADER_BUILD_ID
#define SHADER_BUILD_ID "development"
#endif
#ifndef SHADER_TRANSLATION_ID
#define SHADER_TRANSLATION_ID SHADER_BUILD_ID
#endif
namespace {
using Bytes = std::vector<uint8_t>;
using Digest = std::array<uint8_t, 32>;
std::atomic<uint64_t> hits{}, misses{};
constexpr size_t max_file = 4096;
Digest digest(const void *p, size_t bytes) {
    Digest d{}; CC_SHA256(p, static_cast<CC_LONG>(bytes), d.data()); return d;
}
void word(Bytes& out, uint64_t v, unsigned n = 4) {
    for (unsigned i = 0; i < n; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
struct Reader {
    const Bytes& bytes; size_t cursor;
    uint64_t get(unsigned n = 4) {
        if (cursor > bytes.size() || n > bytes.size() - cursor) throw std::runtime_error("short shader metadata");
        uint64_t v = 0; for (unsigned i = 0; i < n; ++i) v |= uint64_t(bytes[cursor++]) << (8 * i); return v;
    }
};
Bytes state(const ShaderKey& k) {
    Bytes out(std::begin(k.attr_kind), std::end(k.attr_kind));
    out.insert(out.end(), std::begin(k.rt_kind), std::end(k.rt_kind));
    out.push_back(k.depth_minus_one); out.push_back(k.flip_y); out.push_back(k.flip_x); out.push_back(k.no_ds_write);
    word(out, k.vs_out_mask);
    out.push_back(k.glsl); out.push_back(k.gmem_linear_lookup); out.push_back(k.gather_bias_bits); out.push_back(k.vertex_snap);
    out.insert(out.end(), std::begin(k.image_kind), std::end(k.image_kind));
    return out;
}
bool valid_info(const ShaderInfo& s) {
    if ((s.stage != 0 && s.stage != 1 && s.stage != 5) || s.ntex < 0 || s.ntex > 64 ||
        (s.cbuf_mask >> 18) || (s.cbuf_dynamic & ~s.cbuf_mask) ||
        s.num_insts > 65536 || s.local_mem > (16u << 20)) return false;
    for (auto size : s.cbuf_size) if (size > (16u << 20)) return false;
    if (s.stage != STAGE_FRAGMENT && (s.writes_depth || s.uses_tint)) return false;
    if (s.stage != STAGE_VERTEX && s.writes_point_size) return false;
    if (s.uses_gmem_map && !s.uses_gmem) return false;
    if (s.uses_gmem_store && !s.uses_gmem) return false;
    for (int i = 0; i < s.ntex; ++i) {
        const auto& t = s.tex[i];
        if (t.type > TEXTYPE_2D_MS || t.image_kind > 2 ||
            (!t.bindless && t.handle_offset >= (16u<<20)) ||
            (t.bindless && (t.bindless_cbuf >= 18 || t.bindless_offset >= (16u<<20)))) return false;
    }
    return true;
}
uint32_t flags(const ShaderInfo& s, bool vid) {
    return s.writes_depth | (s.uses_discard << 1) | (s.writes_point_size << 2) |
        (s.uses_gmem << 3) | (s.uses_gmem_map << 4) | (s.uses_gmem_store << 5) |
        (s.uses_tint << 6) | (s.uses_rscale << 7) | (vid << 8);
}
Bytes encode(const ShaderInfo& s) {
    Bytes out;
    word(out, s.stage); word(out, vk_shader_prepare_key(s.msl, s.stage), 8);
    word(out, flags(s, s.stage == STAGE_VERTEX && strstr(s.msl, "gl_VertexIndex")));
    word(out, s.cbuf_mask); word(out, s.cbuf_dynamic);
    for (auto size : s.cbuf_size) word(out, size);
    word(out, s.attr_mask); word(out, s.out_mask); word(out, s.local_mem); word(out, s.num_insts);
    word(out, s.image_format_mask); word(out, s.ntex);
    for (int i = 0; i < s.ntex; ++i) {
        const auto& t = s.tex[i];
        word(out, t.handle_offset); word(out, t.type);
        word(out, t.shadow | (t.is_array << 1) | (t.is_image << 2) | (t.bindless << 3) |
             (t.image_raw << 4) | (t.image_write << 5));
        word(out, t.bindless_cbuf); word(out, t.bindless_offset); word(out, t.image_kind);
    }
    return out;
}
bool decode(const Bytes& bytes, VkShaderArtifact& out) {
    Reader r{bytes, 40};
    auto info = static_cast<ShaderInfo *>(calloc(1, sizeof(ShaderInfo)));
    if (!info) return false;
    out.info = info;
    info->stage = r.get(); out.source_key = r.get(8);
    auto f = r.get(); if (f & ~511u) return false;
    info->writes_depth = f & 1; info->uses_discard = f & 2; info->writes_point_size = f & 4;
    info->uses_gmem = f & 8; info->uses_gmem_map = f & 16; info->uses_gmem_store = f & 32;
    info->uses_tint = f & 64; info->uses_rscale = f & 128; out.vertex_index = f & 256;
    if (out.vertex_index && info->stage != STAGE_VERTEX) return false;
    info->cbuf_mask = r.get(); info->cbuf_dynamic = r.get();
    for (auto& size : info->cbuf_size) size = r.get();
    info->attr_mask = r.get(); info->out_mask = r.get(); info->local_mem = r.get(); info->num_insts = r.get();
    auto image_mask = r.get(); if (image_mask > 255) return false; info->image_format_mask = image_mask;
    auto ntex = r.get(); if (ntex > 64) return false; info->ntex = ntex;
    for (unsigned i = 0; i < ntex; ++i) {
        auto& t = info->tex[i];
        t.handle_offset = r.get();
        auto type = r.get(); if (type > TEXTYPE_2D_MS) return false; t.type = type;
        auto tf = r.get(); if (tf & ~63u) return false;
        t.shadow = tf & 1; t.is_array = tf & 2; t.is_image = tf & 4; t.bindless = tf & 8;
        t.image_raw = tf & 16; t.image_write = tf & 32;
        t.bindless_cbuf = r.get(); t.bindless_offset = r.get();
        auto kind = r.get(); if (kind > 2) return false; t.image_kind = kind;
    }
    return r.cursor == bytes.size() - 32 && out.source_key && valid_info(*info);
}
bool request(const uint8_t raw[32], const ShaderKey *key, uint64_t profile,int stage, Digest& id, std::string& path) {
    const char *enabled = getenv("LARECOMP_SHADER_ARTIFACTS");
    if (!raw || !key || !key->glsl || (enabled && !atoi(enabled))) return false;
    auto canonical=*key;vk_shader_key_canonical(&canonical,stage);
    auto input = state(canonical);input.push_back(uint8_t(stage));
    input.insert(input.end(), raw, raw + 32); word(input, profile, 8);
    std::string build = std::string(SHADER_TRANSLATION_ID) + ":" + vk_shader_cache_namespace();
    auto build_hash = digest(build.data(), build.size());
    input.insert(input.end(), build_hash.begin(), build_hash.end()); id = digest(input.data(), input.size());
    static const char hex[] = "0123456789abcdef";
    auto hex_string = [&](const Digest& d) { std::string s; for (auto b : d) { s += hex[b >> 4]; s += hex[b & 15]; } return s; };
    path = "cache/shader-artifacts/" + hex_string(build_hash) + "/" + hex_string(id) + ".bin";
    return true;
}

}
extern "C" bool vk_shader_artifact_input_key(const void *control, size_t control_bytes, const void *prefix,
                                             size_t prefix_bytes, const void *data, size_t data_bytes, uint8_t key[32]) {
    if (!control || !data || !key || control_bytes < 0x718 || (prefix_bytes && !prefix) ||
        prefix_bytes > (16u << 20) || data_bytes > (16u << 20) - prefix_bytes) return false;
    auto c = static_cast<const uint8_t *>(control);
    auto u32 = [&](size_t at) { return uint32_t(c[at]) | uint32_t(c[at+1])<<8 | uint32_t(c[at+2])<<16 | uint32_t(c[at+3])<<24; };
    auto length = u32(0x20);
    if (u32(0) != 0x98761234 || (length != 0x880 && length != 0x7d1) || length > control_bytes ||
        u32(0x704) != prefix_bytes + data_bytes) return false;
    CC_SHA256_CTX hash; CC_SHA256_Init(&hash);
    CC_SHA256_Update(&hash, control, length); CC_SHA256_Update(&hash, prefix, prefix_bytes);
    CC_SHA256_Update(&hash, data, data_bytes); CC_SHA256_Final(key, &hash); return true;
}
extern "C" uint64_t vk_shader_artifact_profile(uint64_t renderer_profile) {
    Bytes b; word(b, renderer_profile, 8); word(b, maxwell_gmem_base, 8); word(b, maxwell_gmem_size, 8);
    for (bool flag : {maxwell_gmem_map_all, maxwell_no_int64, maxwell_fp32_double, maxwell_gmem_miss_log,
                     maxwell_brx_window, maxwell_static_stacks, maxwell_structured, maxwell_no_switch,
                     maxwell_no_local_struct, maxwell_explicit_fallthrough, maxwell_native_half, maxwell_fast_minmax}) b.push_back(flag);
    word(b, maxwell_gmem_miss_stages); auto d = digest(b.data(), b.size());
    uint64_t value = 0; for (unsigned i = 0; i < 8; ++i) value |= uint64_t(d[i]) << (i * 8); return value;
}
extern "C" bool vk_shader_artifact_source_required(void) {
    if (maxwell_dbg_code || maxwell_dbg_vs) return true;
    for (auto name : {"LARECOMP_SAVE_MSL", "LARECOMP_SHADER_SUBST", "LARECOMP_DUMP_FS",
                      "LARECOMP_DUMP_VERTEX_INPUT", "LARECOMP_DUMP_VERTEX_OUTPUT", "LARECOMP_DIAGNOSTIC_VS"}) if (diag_getenv(name)) return true;
    return false;
}
extern "C" bool vk_shader_artifact_store(const uint8_t raw[32], const ShaderKey *key, uint64_t profile, const ShaderInfo *info) {
    try {
        Digest id; std::string path;
        if (!info || !info->msl || !valid_info(*info) || vk_shader_artifact_source_required() || !request(raw, key, profile,info->stage, id, path)) return false;
        Bytes b{'T','K','S','A','R','T','0','1'}; b.insert(b.end(), id.begin(), id.end());
        auto body = encode(*info); b.insert(b.end(), body.begin(), body.end());
        auto checksum = digest(b.data(), b.size()); b.insert(b.end(), checksum.begin(), checksum.end());
        if (b.size() > max_file) return false;
        return shader_artifact_storage::enqueue(std::move(path),std::move(b));
    } catch (...) { return false; }
}
extern "C" bool vk_shader_artifact_load(const uint8_t raw[32], const ShaderKey *key, uint64_t profile, int stage, VkShaderArtifact *out) {
    if (!out) return false;
    *out = {};
    try {
        Digest id; std::string path;
        if (vk_shader_artifact_source_required() || !request(raw, key, profile,stage, id, path)) { ++misses; return false; }
        shader_artifact_storage::wait(path);
        Bytes b(max_file + 1);
        FILE *f = fopen(path.c_str(), "rb");
        if (!f) { ++misses; return false; }
        size_t n = fread(b.data(), 1, b.size(), f);
        bool read_ok = !ferror(f); fclose(f);
        if (!read_ok || n > max_file || n < 192) { ++misses; return false; }
        b.resize(n); auto checksum = digest(b.data(), n - 32);
        if (memcmp(b.data(), "TKSART01", 8) || memcmp(b.data()+8, id.data(), 32) ||
            memcmp(b.data()+n-32, checksum.data(), 32) || !decode(b, *out) || out->info->stage != stage)
            throw std::runtime_error("invalid shader artifact");
        out->spirv = vk_shader_cache_load_key(out->source_key, false, &out->words);
        if (!out->spirv || !vk_shader_spirv_stage(out->spirv, out->words, stage)) throw std::runtime_error("missing artifact SPIR-V");
        ++hits; return true;
    } catch (...) { vk_shader_artifact_free(out); ++misses; return false; }
}
extern "C" void vk_shader_artifact_free(VkShaderArtifact *artifact) {
    if (!artifact) return;
    maxwell_free(artifact->info); free(artifact->spirv); *artifact = {};
}
extern "C" VkShaderArtifactStats vk_shader_artifact_stats(void) { auto storage=shader_artifact_storage::stats();return {hits.load(),misses.load(),storage.stored,storage.failed}; }
extern "C" void vk_shader_artifact_flush(void) {
    try{shader_artifact_storage::flush();}catch(...){}
}
