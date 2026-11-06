// DM_WATCH=<main offset of a pointer>:<offset>:<length> (hex): a diagnostic write watch on guest data reached through a global pointer of main, to find the code that writes a field
// (Ocarina of Time: the time object the frame rate rows write is set back every frame). A host thread write-protects the page of [ptr]+offset every 1 ms; the fault hook records the
// writing instruction and the address, unprotects the page and lets the write run. Writes to the rest of the page are caught too and logged with their offset from the object. Every 2 s
// the thread logs the distinct writers (pc as a module offset, the field offset, hits). The host's own writes unprotect first (dm_guest_write_prepare). Off unless set
#include "hle.h"
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>

enum { WMAX = 48 };
static uint64_t g_wptr, g_woff, g_wlen;
static _Atomic uintptr_t g_wpage, g_wobj;   // the page being watched, the object
static _Atomic int g_warmed; static _Atomic uint64_t g_wfaults;   // faults so far: the thread waits 5 ms after each before protecting again
static struct { _Atomic uint64_t pc; _Atomic int64_t off; _Atomic uint64_t hits; } g_w[WMAX];
static _Atomic uint64_t g_wdropped;
static size_t pagesz(void) { static size_t p; if (!p) p = (size_t)getpagesize(); return p; }

static int watch_fault(void *addr, int is_write, void *uc_) {
    uintptr_t page = atomic_load(&g_wpage), a = (uintptr_t)addr;
    if (!page || a < page || a >= page + pagesz() || !is_write) return 0;
    ucontext_t *uc = uc_; uint64_t pc = uc->uc_mcontext->__ss.__pc; int64_t off = (int64_t)(a - atomic_load(&g_wobj));
    int i;
    for (i = 0; i < WMAX; i++) {
        uint64_t p = atomic_load(&g_w[i].pc);
        if (p == pc && atomic_load(&g_w[i].off) == off) { atomic_fetch_add(&g_w[i].hits, 1); break; }
        if (!p) { uint64_t z = 0; if (atomic_compare_exchange_strong(&g_w[i].pc, &z, pc)) { atomic_store(&g_w[i].off, off); atomic_store(&g_w[i].hits, 1); break; } i--; continue; }
    }
    if (i == WMAX) atomic_fetch_add(&g_wdropped, 1);
    mprotect((void *)page, pagesz(), PROT_READ | PROT_WRITE); atomic_store(&g_warmed, 0); atomic_fetch_add(&g_wfaults, 1);
    return 1;
}
static void watch_prep(void *addr, size_t len) {   // a host write into the watched page: unprotect first (a system call would get EFAULT, a store would be logged as the game's)
    uintptr_t page = atomic_load(&g_wpage), a = (uintptr_t)addr;
    if (page && a < page + pagesz() && a + len > page && atomic_exchange(&g_warmed, 0)) mprotect((void *)page, pagesz(), PROT_READ | PROT_WRITE);
}
static void *watch_main(void *arg) {
    (void)arg; uint8_t *base = NULL; uint64_t tlog = 0, faults = 0;
    for (;;) {
        if (!base) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) base = g_mod[i].base; if (!base) { usleep(100000); continue; } }
        uint64_t obj = 0; memcpy(&obj, base + g_wptr, sizeof obj);
        if (!obj) { usleep(100000); continue; }
        uintptr_t target = (uintptr_t)(obj + g_woff), page = target & ~(uintptr_t)(pagesz() - 1);
        if (!atomic_load(&g_wpage)) { atomic_store(&g_wobj, (uintptr_t)obj); atomic_store(&g_wpage, page);
            LOG("[watch] DM_WATCH: [main+%#llx] = %#llx, watching +%#llx..+%#llx (page %#llx)", (unsigned long long)g_wptr, (unsigned long long)obj, (unsigned long long)g_woff, (unsigned long long)(g_woff + g_wlen), (unsigned long long)page); }
        // After a fault the page stays writable for 5 ms: a page that also holds locks (an exclusive load / store loop) would otherwise fault again before its store completes, and the
        // game hung at boot with the page protected again every 200 us
        uint64_t f = atomic_load(&g_wfaults);
        if (f != faults) { faults = f; usleep(5000); continue; }
        if (!atomic_exchange(&g_warmed, 1)) mprotect((void *)page, pagesz(), PROT_READ);
        usleep(1000);
        uint64_t t = now_ns();
        if (t - tlog >= 2000000000ull) {
            tlog = t;
            for (int i = 0; i < WMAX; i++) {
                uint64_t pc = atomic_load(&g_w[i].pc); if (!pc) break;
                int64_t off = atomic_load(&g_w[i].off); module_t *m = module_of((void *)pc);
                int in = off >= (int64_t)g_woff && off < (int64_t)(g_woff + g_wlen);
                LOG("[watch] %s+%#llx writes obj%s%#llx%s: %llu", m ? m->name : "?", (unsigned long long)(m ? pc - (uint64_t)m->base : pc), off < 0 ? "-" : "+",
                    (unsigned long long)(off < 0 ? -off : off), in ? " (watched)" : "", (unsigned long long)atomic_load(&g_w[i].hits));
            }
            if (atomic_load(&g_wdropped)) LOG("[watch] %llu writes from writers past the table", (unsigned long long)atomic_load(&g_wdropped));
        }
    }
    return NULL;
}
__attribute__((constructor)) static void watch_attach(void) {
    const char *e = getenv("DM_WATCH"); if (!e) return;
    char *p;
    g_wptr = strtoull(e, &p, 16); if (*p != ':') return;
    g_woff = strtoull(p + 1, &p, 16); if (*p != ':') return;
    g_wlen = strtoull(p + 1, &p, 16); if (!g_wptr || !g_wlen) return;
    dm_fault_hook_register(watch_fault); dm_guest_write_prep_register(watch_prep);
    pthread_t t; pthread_create(&t, NULL, watch_main, NULL); pthread_detach(t);
}
