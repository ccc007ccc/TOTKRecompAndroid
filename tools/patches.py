"""Per-instruction replacements applied by gen.py (guest address -> function(gen, addr) returning C code).

Each entry replaces the one instruction at that address of `main` (canonical address, 0x7000000000 + offset) with the C
the function returns; gen.py rebases the addresses in it for --dyn-base. Check the instruction word first (an assert),
so a wrong build fails here instead of patching the wrong code. Example, from LAS Recomp's widescreen: a camera's
`ldr sN, [xM, #imm]` of the 16:9 aspect literal, replaced by a call into the runtime:

    def _aspect_load(gen, a):
        w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
        assert (w & 0xffc00000) == 0xbd400000, hex(w)  # ldr s, [x, #imm]
        rt = w & 31
        return (f'{{ extern float rt_aspect_site(int); float f = rt_aspect_site(0); '
                f'c->v[{rt}] = 0; memcpy(&c->v[{rt}], &f, 4); }}  /* {a:#x}: ldr s{rt}, =16/9 */')

    PATCHES[0x7000xxxxxx] = _aspect_load

BOTW v1.0.0 (the code addresses are that build's; docs/BOTW-PORT.md):
- 0x70010c1aa0: act::ClusteredRenderer's teardown (f_70010c1a04) calls the virtual destroy of its display-list heap
  at this+0xc60 without a null check. Its setup (f_70010c084c) sets the "created" flag even when that 7 MB heap
  could not be made - here OpenWorldStage runs ~13 MB short and it never is - so a reload (dying, loading a save)
  crashed. A null heap now skips the destroy (the field and the flag are cleared as before).
"""
import struct
from lifter import emit_asm_batch

PATCHES = {}      # v1.0.0 (main build 2094f152...)
PATCHES_160 = {}  # v1.6.0 (main build 8e9978d5...): the same fixes at that build's addresses
PATCHES_182 = {}  # v1.8.2 (main build de0b6ac4...)
PATCHES_143 = {}  # TotK v1.4.3; never use BOTW's VFR layout for this build
PRE_PATCHES_143 = {}
BUILD_1_0_0 = '2094f152836683a3632c6153fd84ab80'
BUILD_1_6_0 = '8e9978d50bdd20b4c8395a106c27ffde'
BUILD_1_8_2 = 'de0b6ac4efc1dda7f80f6d7598ec7a7d'
BUILD_TOTK_1_4_3 = '277178b7dba1b6d4949a84778d4abc58'


def _clustered_dl_heap_null(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xf9400008, hex(w)  # ldr x8, [x0]
    return ('if (c->x[0] == 0) { c->x[21] = c->x[20] + 0xc60ull; goto L_70010c1ad8; }  /* (tools/patches.py) */ '
            '{ uint64_t ea = c->x[0] + 0x0ull; uint64_t v = LD64(ea); c->x[8] = (uint64_t)(v); }')


PATCHES[0x70010c1aa0] = _clustered_dl_heap_null


# - 0x7000c4f4d4: sead's frame loop (f_7000c4f430) picks the present interval from the last frame's duration
#   (this+0x240, ticks): floor(vsyncs) + 1, or + 2 past a 0.9 fraction, never under 2. A frame over ~1.9 vsyncs takes it
#   to 20 fps (interval 3), where our GPU's clocks settle low and it stays. The menu's fixed frame rate (Video: Frame
#   rate 30 / 60) caps the duration read here (rt_frame_ticks), so the game itself keeps 2 - its state matches what is
#   presented; a frame that doesn't fit plays slightly slower. Auto: the game's own choice.
def _frame_ticks(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xf9412269, hex(w)  # ldr x9, [x19, #0x240]
    return ('{ extern int64_t rt_frame_ticks(int64_t); uint64_t ea = c->x[19] + 0x240ull; '
            'c->x[9] = (uint64_t)rt_frame_ticks((int64_t)LD64(ea)); }  /* (tools/patches.py) */')


PATCHES[0x7000c4f4d4] = _frame_ticks


# v1.6.0 (found by the same code: the instructions around each 1.0.0 site; its structs moved by a few bytes)
# - 0x7001861440: act::ClusteredRenderer's teardown - here without the "created" check of 1.0.0: it reads the
#   display-list heap at this+0xbf0 and calls its virtual destroy unconditionally; a null heap now goes on to the
#   flags' update (w8 = this+0xc9c, as the destroy's two paths load it) and the field's clearing at 0x7001861470.
def _clustered_dl_heap_null_160(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xf9400008, hex(w)  # ldr x8, [x0]
    return ('if (c->x[0] == 0) { uint64_t ea = c->x[20] + 0xc9cull; c->x[8] = (uint64_t)(uint32_t)LD32(ea); '
            'goto L_7001861470; }  /* (tools/patches.py) */ '
            '{ uint64_t ea = c->x[0] + 0x0ull; uint64_t v = LD64(ea); c->x[8] = (uint64_t)(v); }')


PATCHES_160[0x7001861440] = _clustered_dl_heap_null_160


# - 0x700161495c: sead's frame loop, the last frame's duration at this+0x248 (1.0.0: +0x240, into x9)
def _frame_ticks_160(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xf9412668, hex(w)  # ldr x8, [x19, #0x248]
    return ('{ extern int64_t rt_frame_ticks(int64_t); uint64_t ea = c->x[19] + 0x248ull; '
            'c->x[8] = (uint64_t)rt_frame_ticks((int64_t)LD64(ea)); }  /* (tools/patches.py) */')


PATCHES_160[0x700161495c] = _frame_ticks_160


# v1.8.2, frame rate (runtime/hle/framepace.c; SavageWindRecomp's patches, MIT): the runtime paces the frames at the
# present and sets the game's clock. The frame loop's vsync wait (sead framework, 0x7000d811fc; only the first frames
# after start-up come here) is skipped (the game's own runs with LARECOMP_FRAMEPACE=0). ksys::VFR::updateInterval
# (x19 = VFR) has just set the interval ratio the game's time step comes from: the runtime puts the frame's real length
# there, before the time multipliers and the deltas are worked out from it.
def _frame_wait_182(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xa9bd7bfd, hex(w)  # stp x29, x30, [sp, #-0x30]!
    return ('{ extern int rt_frame_wait(void); if (rt_frame_wait()) return; '
            'uint64_t ea = c->sp - 0x30; ST64(ea, c->x[29]); ST64(ea + 8, c->x[30]); c->sp = ea; }'
            f'  /* {a:#x}: stp x29, x30, [sp, #-0x30]! (frame wait; tools/patches.py) */')


def _vfr_ratio_182(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xb9407a68, hex(w)  # ldr w8, [x19, #0x78]
    return ('{ extern void rt_vfr_update(uint64_t); rt_vfr_update(c->x[19]); c->x[8] = LD32(c->x[19] + 0x78); }'
            f'  /* {a:#x}: ldr w8, [x19, #0x78] (VFR: interval ratio; tools/patches.py) */')


# sead::PerspectiveProjection::doUpdateMatrix (x0 = the projection; the vtable at 0x7001ba4438, entry 11): before it
# reads near, far, tan and aspect, the runtime widens a 16:9 aspect to the output's shape (runtime/hle/widescreen.c
# rt_persp_update: the main camera, whose aspect the game sets back to 16:9 before each rebuild).
def _persp_matrix_182(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xbd409800, hex(w)  # ldr s0, [x0, #0x98]
    return ('{ extern void rt_persp_update(uint64_t); rt_persp_update(c->x[0]); c->v[0] = LD32(c->x[0] + 0x98); }'
            f'  /* {a:#x}: ldr s0, [x0, #0x98] (projection matrix: widescreen; tools/patches.py) */')


# sead::PerspectiveProjection::getAspect (vtable entry 7): the widened aspect too (runtime/hle/widescreen.c
# rt_persp_aspect), so the code asking for it agrees with the matrix.
def _persp_get_aspect_182(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xbd40b000, hex(w)  # ldr s0, [x0, #0xb0]
    return ('{ extern uint32_t rt_persp_aspect(uint64_t); c->v[0] = rt_persp_aspect(c->x[0]); }'
            f'  /* {a:#x}: ldr s0, [x0, #0xb0] (projection aspect: widescreen; tools/patches.py) */')


PATCHES_182[0x7000d811fc] = _frame_wait_182
PATCHES_182[0x70014092f4] = _vfr_ratio_182
PATCHES_182[0x7000da13f4] = _persp_matrix_182
PATCHES_182[0x7000da13d8] = _persp_get_aspect_182


def patches_for(build_id):
    """The patches for a module, by its build id (sdk and anything unknown: none)."""
    return {BUILD_1_0_0: PATCHES, BUILD_1_6_0: PATCHES_160, BUILD_1_8_2: PATCHES_182,
            BUILD_TOTK_1_4_3: PATCHES_143}.get(build_id[:32], {})


def prepatches_for(build_id):
    """Instrumentation immediately before the original instruction; BLs stay normal generated calls."""
    return PRE_PATCHES_143 if build_id[:32] == BUILD_TOTK_1_4_3 else {}


# Locations and controller replacements adapted from MacNativeGames (MIT),
# games/totk/runtime/game_patches_v143.c @ ff63a162b35d8b486b941191785d8e957c1f5df2.
# See runtime/hle/LICENSE-MacNativeGames.txt. Checked against our own 1.4.3 input.
# UltraTears PR #27 exposes FPS controls, but its runtime says these game patches are not ported yet.
# The original instructions remain available with LARECOMP_FRAMEPACE=0.
def _totk_controller(original, replacement):
    def patch(gen, a):
        w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
        assert w == original, f'TotK controller {a:#x}: {w:#x} != {original:#x}'
        old, new = gen.lifter.lift(w, a), gen.lifter.lift(replacement, a)
        assert old.kind == new.kind == 'op'
        return ('{ extern int rt_frame_wait(void); if (rt_frame_wait()) { ' + new.code +
                ' } else { ' + old.code + ' } }')
    return patch


for _off, _old, _new in (
    (0x2db204, 0x6b08015f, 0x7100055f),
    (0x2db208, 0x1a89b10a, 0x1a9fa52a),
    (0x2db22c, 0xb940468a, 0xd503201f),
    (0x2db360, 0x6b0801bf, 0x710005bf),
    (0x2db36c, 0x1a88c1ac, 0x1a9fc5ac),
    (0x2db340, 0xb9400288, 0x52800028),
    (0x2db34c, 0x2a0a03f5, 0x1a9fa555),
):
    PATCHES_143[0x7000000000 + _off] = _totk_controller(_old, _new)


def _totk_before(site, guards):
    def patch(gen, a):
        for off, original in guards:
            word = struct.unpack_from('<I', gen.nso.image, off)[0]
            assert word == original, f'TotK timing {off:#x}: {word:#x} != {original:#x}'
        return f'{{ extern void rt_totk_frame_hook(Cpu *, int); rt_totk_frame_hook(c, {site}); }}'
    return patch


# Replace the producer's nominal interval BEFORE its 0.5/1.0 selector. Patching FrameRateMgr's callee would lose it.
PRE_PATCHES_143[0x70003e5658] = _totk_before(0, [(0x3e5658, 0x1e210808)])
# Controller hold/repeat counter: the existing fmul by 0.5 converts the measured 60 Hz interval to a 30 Hz ratio.
PRE_PATCHES_143[0x70002daa78] = _totk_before(1, [
    (0x2daa54, 0xbd425260), (0x2daa58, 0x52a84e08), (0x2daa6c, 0x5e21d800),
    (0x2daa70, 0x1e201820), (0x2daa74, 0x1e2c1001), (0x2daa78, 0x1e210800)])
# Independent cloth/hair clock. The third caller already passes measured seconds: leave it untouched.
PRE_PATCHES_143[0x7000246eac] = _totk_before(2, [(0x246ea8, 0xbd0367ea), (0x246eac, 0x1e200940)])
PRE_PATCHES_143[0x7000345904] = _totk_before(3, [(0x3458f8, 0xbd0117e0), (0x345904, 0x1e210801)])
PRE_PATCHES_143[0x700024944c] = _totk_before(4, [(0x24944c, 0x94015e7d)])
PRE_PATCHES_143[0x700028bc98] = _totk_before(4, [(0x28bc98, 0x9400546a)])
# Keep the slow-motion bounce threshold independent of FPS, including 120 FPS.
PRE_PATCHES_143[0x7000562a68] = _totk_before(5, [
    (0x562a64, 0xbd402920), (0x562a68, 0x52a85909), (0x562a6c, 0x1e270121)])

# mamcjr/TOTKRecompAndroid @ da06d5a7f3fb1378eeefbcb13dc2276096a1aebc (MIT).
# Window-only replacement leaves the game's remembered 2/3-vsync choice intact. Override w21 at the
# common merge point, before interval/FPS stores and the NVN call; keep the original MOV and all flags.
PRE_PATCHES_143[0x70002db238] = _totk_before(6, [
    (0x2db238, 0x52800788), (0x2db240, 0xb9004295), (0x2db244, 0x1ad50d09),
    (0x2db280, 0xb9008a75), (0x2db2d0, 0x3902a115)])

# Some cutscene animation counts the interval instead of measured seconds: present at 30 and tell the
# controller 2 together. Preserve the original entry instruction and generator function/call discovery.
for _site, (_off, _word) in enumerate(((0x45c700, 0xd10103ff), (0x2b23c7c, 0xd100c3ff),
                                     (0x42c3bc, 0xd10283ff), (0xa5a188, 0xf9401008)), 7):
    PRE_PATCHES_143[0x7000000000 + _off] = _totk_before(_site, [(_off, _word)])


def _totk_fade_step(rd):
    def patch(gen, a):
        word = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
        assert word == (0x1e380000 | rd), f'TotK fade {a:#x}: {word:#x}'
        ins = gen.lifter.lift(word, a)
        assert ins.kind in ('op', 'asm')
        # Retain the normal lifter's conversion (including saturation, NaNs and native FP behavior).
        original = ins.code if ins.kind == 'op' else emit_asm_batch([ins.asm])
        site = 11 if rd == 8 else 12
        return '{ ' + original + f' extern void rt_totk_frame_hook(Cpu *, int); rt_totk_frame_hook(c, {site}); }}'
    return patch


for _off, _rd in ((0xa7ac4, 8), (0x95dd24, 8), (0x6155e4, 10), (0x615660, 10)):
    PATCHES_143[0x7000000000 + _off] = _totk_fade_step(_rd)
