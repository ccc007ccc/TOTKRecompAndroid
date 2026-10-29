// (From SavageWindRecomp, MIT: github.com/Revivedx/SavageWindRecomp.)
// Guest setjmp / longjmp (musl's, in sdk). libjpeg and libpng in nn::image leave their errors with longjmp: the
// JPEG encoder of the autosave's screenshot, for one. The guest longjmp returns into the middle of the function
// that called setjmp, and recompiled code can't resume there (dispatch only knows function entries). So the
// generator (tools/gen.py setjmp_call) wraps every call to the guest setjmp in a host setjmp, in the C function of
// the caller: rt_setjmp_slot gives it a host jmp_buf for the guest one (per thread, by guest address). The guest
// setjmp still runs and fills the guest jmp_buf. The guest longjmp is HLE (runtime/hle/hle_symbols.txt): it
// restores the registers musl saved there and jumps to the host jmp_buf, back into that C function with x0 set.
//   musl aarch64 jmp_buf: x19-x28 at 0x00, x29 0x50, x30 0x58, sp 0x68, d8-d15 0x70
#include "../diagnostics_build.h"
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "../hle.h"

#define SLOTS 32
typedef struct {
    uint64_t gbuf;  // guest jmp_buf (0: free)
    uint64_t gsp;   // guest sp at the setjmp: the sp musl saves in it
    jmp_buf hb;
} JmpSlot;
static __thread JmpSlot slots[SLOTS];
static __thread unsigned slot_next;

jmp_buf *rt_setjmp_slot(Cpu *c, uint64_t gbuf) {
    JmpSlot *s = NULL;
    for (int i = 0; i < SLOTS && !s; i++)
        if (slots[i].gbuf == gbuf) s = &slots[i];
    // a new buffer: a free slot, else one set up by a deeper frame (its sp below ours: that function has returned)
    for (int i = 0; i < SLOTS && !s; i++)
        if (!slots[i].gbuf || slots[i].gsp < c->sp) s = &slots[i];
    if (!s) s = &slots[slot_next++ % SLOTS];
    s->gbuf = gbuf;
    s->gsp = c->sp;
    return &s->hb;
}

static uint64_t g_longjmps;  // (stats)

void hle_longjmp(Cpu *c) {
    uint64_t gbuf = c->x[0];
    uint32_t val = (uint32_t)c->x[1];
    JmpSlot *s = NULL;
    for (int i = 0; i < SLOTS && !s; i++)
        if (slots[i].gbuf == gbuf) s = &slots[i];
    if (!s) FATAL("[setjmp] longjmp to %llx: no setjmp of it on this thread (lr=%llx)\n", gbuf, c->x[30]);
    if (LD64(gbuf + 0x68) != s->gsp)
        FATAL("[setjmp] longjmp to %llx: its sp %llx isn't the setjmp's %llx\n", gbuf, LD64(gbuf + 0x68), s->gsp);
    if (g_longjmps++ < 8 || DIAG_ENV("LARECOMP_LOG_LONGJMP"))
        LOG("[setjmp] longjmp to %llx val %u (lr=%llx, back to %llx)\n", gbuf, val, c->x[30], LD64(gbuf + 0x58));
    for (int r = 19; r <= 30; r++) c->x[r] = LD64(gbuf + 8 * (r - 19));
    c->sp = s->gsp;
    for (int r = 8; r <= 15; r++) c->v[r] = LD64(gbuf + 0x70 + 8 * (r - 8));  // (ldp d: upper half zeroed)
    c->x[0] = val ? val : 1;
    longjmp(s->hb, 1);
}
