// Formal translation configuration and identity, shared by preparation and runtime.
#include "shader_config.h"
#include "shader_prepare.h"
#include "../maxwell/shader.h"
#include "shader_cache.h"
#include "../../rt.h"
#include "../../settings.h"
#include <stdlib.h>
#include <stdio.h>
#ifdef __ANDROID__
// The structured control flow (maxwell_structured) unless LARECOMP_NO_STRUCTURE - and not on Qualcomm's own driver,
// whose shader compiler crashed on it inside vkCreateGraphicsPipelines (libllvm-qgl, 2026-10-16: the overworld) -
// LARECOMP_STRUCTURE=1 there anyway
extern bool vk_driver_is_qualcomm(void);
static bool structure_on(void) {
    static int on = -1;
    if (on < 0) {
        extern bool vk_driver_is_qualcomm(void);
        on = !getenv("LARECOMP_NO_STRUCTURE") && (!vk_driver_is_qualcomm() || getenv("LARECOMP_STRUCTURE"));
        if (!on && vk_driver_is_qualcomm()) LOG("[vk] Qualcomm's driver: shaders keep their branch state machine (its compiler)\n");
    }
    return on;
}
#endif

// Qualcomm's driver with the structured control flow (LARECOMP_STRUCTURE=1): its compiler crashed on a shader whose
// structured form has a GLSL switch (a jump table) - those keep their state machine there. LARECOMP_QCOM_SWITCH=1: not.
static bool qcom_no_switch(void) {
    static int on = -1;
    if (on < 0) {
        extern bool vk_driver_is_qualcomm(void);
        on = vk_driver_is_qualcomm() && !getenv("LARECOMP_QCOM_SWITCH");
    }
    return on;
}

uint8_t vk_shader_gather_bias_bits(void) {
    static int bits = -1;
    if (bits < 0) {
        // Eden a95efaf / #4565: Maxwell's gather footprint rounds at 8-bit
        // subtexel precision, independently of the host's advertised precision.
        int automatic = (g_vk.props.vendorID == 0x1002 || g_vk.props.vendorID == 0x8086 ||
                         g_vk.props.vendorID == 0x5143) ? 8 : 0;
#ifdef __ANDROID__
        extern bool vk_driver_is_turnip(void);
        extern bool vk_driver_is_qualcomm(void);
        if (vk_driver_is_turnip() || vk_driver_is_qualcomm()) automatic = 8;
#endif
        const char *e = getenv("LARECOMP_GATHER_BIAS_BITS");
        bits = e ? atoi(e) : automatic;
        if (bits < 0 || bits > 23) bits = 0;
        LOG("[vk] gather subtexel correction: %d bits%s (device precision %u)\n", bits,
            e ? " (override)" : "", g_vk.props.limits.subTexelPrecisionBits);
    }
    return (uint8_t)bits;
}

bool vk_shader_vertex_snap_on(void) { // shader keys are constructed on the render thread
    static int on = -1;
    if(on<0) {
        const char *value=getenv("LARECOMP_VS_SNAP");
        on=value&&atoi(value)!=0&&!getenv("LARECOMP_NO_VS_SNAP");
        if(on)LOG("[vk] experimental vertex sampling quantization enabled\n");
    }
    return on!=0;
}

void vk_shader_prepare_configure(void) {
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
#ifdef __ANDROID__
    maxwell_no_int64 = !g_vk.shader_int64 || g_settings.shader_int32 || getenv("LARECOMP_VK_INT32") != NULL;
    maxwell_fp32_double = g_settings.shader_fp32;
    maxwell_gmem_map_all = (vk_shader_memory_modes() & 1) != 0;
    maxwell_gmem_miss_log = (vk_shader_memory_modes() & 2) && !getenv("LARECOMP_NO_GMISS");
    maxwell_gmem_miss_stages = getenv("LARECOMP_GMISS_STAGES") ?
        (unsigned)strtoul(getenv("LARECOMP_GMISS_STAGES"), NULL, 0) : ~0u;
    maxwell_brx_window = !getenv("LARECOMP_BRX_ALL_LABELS");
    maxwell_static_stacks = !getenv("LARECOMP_DYN_STACKS");
    maxwell_structured = structure_on();
    maxwell_no_switch = qcom_no_switch();
    maxwell_no_local_struct = qcom_no_switch() && !getenv("LARECOMP_QCOM_LOCAL");
    maxwell_explicit_fallthrough = vk_driver_is_qualcomm();
    maxwell_native_half = !getenv("LARECOMP_BITWISE_HALF");
    maxwell_fast_minmax = !getenv("LARECOMP_SAFE_MATH");
#endif
}
unsigned vk_shader_prepare_gather(void) { return vk_shader_gather_bias_bits(); }
bool vk_shader_prepare_vertex_snap(void) { return vk_shader_vertex_snap_on(); }
uint64_t vk_shader_prepare_profile(void) {
    static const char *names[] = {"LARECOMP_VK_OPT", "LARECOMP_VK_INT32", "LARECOMP_NO_STRUCTURE", "LARECOMP_STRUCTURE",
        "LARECOMP_QCOM_SWITCH", "LARECOMP_QCOM_LOCAL", "LARECOMP_BRX_ALL_LABELS", "LARECOMP_DYN_STACKS",
        "LARECOMP_GMISS_STAGES", "LARECOMP_BITWISE_HALF", "LARECOMP_SAFE_MATH", "LARECOMP_GATHER_BIAS_BITS",
        "LARECOMP_VS_SNAP", "LARECOMP_NO_VS_SNAP", "LARECOMP_NO_BANK2", "LARECOMP_NO_DISCARD", "LARECOMP_SAVE_MSL"};
    char text[192];
    unsigned int32_setting = 0, fp32_setting = 0;
    unsigned api=VK_API_VERSION_1_3;
#ifdef __ANDROID__
    int32_setting = g_settings.shader_int32;
    fp32_setting = g_settings.shader_fp32;
    api=g_vk.api;
#endif
    snprintf(text, sizeof text, "%llu:%llu:%u:%u:%u:%u:%u:%u:%u", (unsigned long long)HEAP_BASE,
             (unsigned long long)HEAP_MAX, g_vk.props.vendorID, g_vk.props.deviceID,
             g_vk.props.driverVersion, g_vk.shader_int64, int32_setting, fp32_setting, api);
    uint64_t h = vk_shader_source_hash(text, 0, false);
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
        const char *v = getenv(names[i]);
        h = (h ^ vk_shader_source_hash(v ? v : "<unset>", (int)i, false)) * 0x100000001b3ull;
    }
    // Receipt validity includes the current heap memory access path.
    unsigned memory=0;
#ifdef __ANDROID__
    memory=vk_shader_memory_modes();if(getenv("LARECOMP_NO_GMISS"))memory&=~2u;
#endif
    return h ^ vk_shader_gather_bias_bits() ^ ((uint64_t)memory<<24) ^ ((uint64_t)g_vk.props.apiVersion << 32);
}


bool vk_shader_prepare_compute_linear(void) {
#ifdef __ANDROID__
    extern bool vk_driver_is_qualcomm(void);
    return vk_driver_is_qualcomm();
#else
    return false;
#endif
}
