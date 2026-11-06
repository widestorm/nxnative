// Observation gate OBS_PROBE: a read-only diagnostic for finding a game's frame clock. A profile's patches.py puts the gate on an instruction whose base register points at a float
// (the gate number's upper bits: register in bits 0-4, offset / 4 in bits 5-16, probe slot 0-7 in bits 17-19). With DM_OBS_PROBE=1 the host logs each slot every 2 s: hits per second
// and the float's last, min and max over the window. Off (the default) the gate only passes through. Compare a run at the game's own rate with one held at a lower rate
// (DM_PRESENT_INTERVAL_MIN=2): a per-frame step that stays the same at both is a fixed step, one that doubles is measured time
#include "hle.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>

enum { PROBE_SLOTS = 8 };
static struct { _Atomic uint64_t hits; float last, mn, mx; uint64_t t0, h0; } g_probe[PROBE_SLOTS];
static int g_probe_on = -1;

static void obs_probe(uint64_t *r, int param) {
    if (g_probe_on < 0) { const char *e = getenv("DM_OBS_PROBE"); g_probe_on = e && atoi(e) > 0; if (g_probe_on) LOG("[probe] DM_OBS_PROBE=1: probe gates log every 2 s"); }
    if (!g_probe_on) return;
    int reg = param & 31, off = ((param >> 5) & 0xfff) * 4, slot = (param >> 17) & 7;
    if (reg > 28 || !r[reg]) return;
    float v; memcpy(&v, (const void *)(uintptr_t)(r[reg] + (uint64_t)off), sizeof v);
    __typeof__(g_probe[0]) *p = &g_probe[slot];
    uint64_t h = atomic_fetch_add_explicit(&p->hits, 1, memory_order_relaxed) + 1, t = now_ns();
    if (!p->t0) { p->t0 = t; p->h0 = h; p->mn = p->mx = v; }
    if (v < p->mn) p->mn = v;
    if (v > p->mx) p->mx = v;
    p->last = v;
    if (t - p->t0 >= 2000000000ull) {
        double s = (t - p->t0) / 1e9;
        LOG("[probe] slot %d (x%d+%#x): %.1f hits/s, value last %.6g min %.6g max %.6g", slot, reg, off, (h - p->h0) / s, p->last, p->mn, p->mx);
        p->t0 = t; p->h0 = h; p->mn = p->mx = v;
    }
}
__attribute__((constructor)) static void obs_probe_attach(void) { gate_obs_set(OBS_PROBE, obs_probe); }
