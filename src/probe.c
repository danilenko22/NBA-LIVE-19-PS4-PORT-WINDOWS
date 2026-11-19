/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <cpuid.h>
#include "runtime.h"
#include "build_version.h"
/* Referenced by --version so LTO retains this searchable, NUL-terminated identity. */
static const char game_build_identity[] __attribute__((used)) = BB_GAME_VERSION_MARKER BB_BUILD_VERSION;
#include "gpu/bbgpu.h"
#ifndef _WIN32
/* Releases before the detector lack this export; still report a mixed installation. */
extern const char *bbgpu_version(void) __attribute__((weak));
#endif
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#include <unistd.h> /* _exit */
/* Host services (signals or vectored exceptions, stacks, memory, restart): src/platform. */

#ifdef _WIN32
static void *log_flusher(void *unused) {
    (void)unused;
    for (;;) {
        usleep(500000);
        fflush(stdout);
    }
    return NULL;
}
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
int vulkan_smoke(void);

static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }

/* The game's code was compiled for the PS4's CPU (AMD Jaguar) and runs as it is: AVX, BMI1
 * (andn/bextr/blsr/tzcnt), MOVBE, LZCNT and POPCNT, thousands of each in eboot.bin. A CPU without
 * them stops at the first one with SIGILL (exit code 132, issue #26), and lzcnt/tzcnt even run as
 * bsr/bsf there, with other results. Said before the game starts; BB_SKIP_CPU_CHECK=1 skips it.
 * Exits with code 24 so the launchers can name the cause (GitHub #21). */
static void check_cpu(void) {
    const char *skip = getenv("BB_SKIP_CPU_CHECK");
    if (skip && !strcmp(skip, "1")) return;
    unsigned a, b, c, d, leaf1_c = 0, leaf7_b = 0, ext1_c = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) leaf1_c = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) leaf7_b = b;
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d)) ext1_c = c;
    int avx = (leaf1_c & bit_AVX) && (leaf1_c & bit_OSXSAVE);
    if (avx) {
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        avx = (lo & 6) == 6; /* the OS saves SSE and AVX state */
    }
    const struct { int present; const char *name; } features[] = {
        {avx, "AVX"}, {(leaf7_b & bit_BMI) != 0, "BMI1"}, {(leaf1_c & bit_MOVBE) != 0, "MOVBE"},
        {(ext1_c & bit_LZCNT) != 0, "LZCNT"}, {(leaf1_c & bit_POPCNT) != 0, "POPCNT"},
        {(leaf1_c & bit_SSE4_2) != 0, "SSE4.2"},
    };
    char missing[64] = "";
    for (unsigned i = 0; i < sizeof(features) / sizeof(features[0]); ++i) {
        if (features[i].present) continue;
        if (missing[0]) strcat(missing, ", ");
        strcat(missing, features[i].name);
    }
    if (!missing[0]) return;
    fprintf(stderr,
            "ERROR: this CPU lacks %s. The game's code was compiled for the PS4's CPU and uses "
            "these instructions directly: it needs an Intel Haswell (4th generation Core, 2013) or "
            "newer, or an AMD Ryzen. BB_SKIP_CPU_CHECK=1 starts anyway.\n", missing);
    exit(24); /* a code of its own: the launchers explain it (GitHub #21) */
}
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
/* Host memory the guest sees (image, trampolines): below 1 TiB (40-bit guest pointers). */
static void *allocate(size_t size) {
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) return low;
    void *p = platform_map_anonymous(size, PROT_READ | PROT_WRITE);
    if (!p) fail("host memory allocation failed");
    return p;
}
/* Stubs the image jumps to with rel32: low memory, mapped after the image (runtime_low_map). */
static void *map_near_image(size_t size) { return runtime_low_map(size, PROT_READ | PROT_WRITE); }
/* ELF segment flags (1 execute, 2 write, 4 read) to a host protection. */
static void protect(void *p, size_t size, unsigned flags) {
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (platform_protect(p, size, mode)) fail("changing page protection failed");
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", runtime_import_name(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)image, argument);
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)image;
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,m==0 ? "libc.prx" : "system module",caller-modules[m].base);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
/* Fault handling (platform_install_fault_handlers): GPU page tracking first, then a speculative
 * read's recovery point, then the fatal report with the guest write log. */
/* ufc3: a speculative read (a draw-preparation worker walking guest tables under the pipeline
 * cache's shared lock) must not be resolved by a GPU readback: the readback waits for the GPU
 * thread, which waits for that lock. Its recovery point takes over and the GPU thread redoes it. */
static int gpu_fault(void *context, void *address) {
    if (runtime_fault_recover) return 0;
    return gpu_enabled && bbgpu_handle_fault(context, address);
}
static int gpu_step(void *context) { return gpu_enabled && bbgpu_handle_step(context); }
static bb_fault_jmp_buf *recover_point(void) { bb_fault_jmp_buf *r = runtime_fault_recover; runtime_fault_recover = NULL; return r; }
/* UFC-57: Career crashes with a null write at +0x1a3d204, in the UI scene's snapshot builder
 * (+0x1a3d100, this in r13). Each update it copies every node of its list at this+0x20 into a
 * 0x1e8-byte block from the allocator at this+0x18 (+0x1a3d442) and stores the block in
 * node+0x20 without checking for NULL; the next build writes through it. The allocator ran dry.
 * Built snapshots wait for the renderer in the vector at this+0xc0 (count +0xc8), finished ones
 * in this+0xf0 (count +0xf8); +0x108 counts builds. This names the allocator and shows whether
 * snapshots piled up (renderer not consuming) or the heap is simply full. */
static void ui_snapshot_report(void *context) {
    uint64_t r[17];
    platform_fault_registers(context, r);
    const uintptr_t base = (uintptr_t)image;
    if (r[16] != base + 0x1a3d204) return;
    const uintptr_t scene = r[13];
    uint64_t f[0x110 / 8];
    if (!platform_safe_read(scene, f, sizeof(f))) { fprintf(stderr, "  UFC-57: UI scene %#" PRIxPTR " unreadable\n", scene); return; }
    fprintf(stderr, "  UFC-57: UI scene %#" PRIxPTR ": snapshots waiting for the renderer %" PRIu64 ", finished not yet freed %" PRIu64
            ", stack %" PRIu64 ", builds %" PRIu64 "\n", scene, f[0xc8 / 8], f[0xf8 / 8], f[0xb0 / 8], f[0x108 / 8]);
    for (int slot = 0; slot < 2; ++slot) {
        const uintptr_t allocator = f[slot ? 0x18 / 8 : 0];
        uint64_t vtable = 0, entry = 0;
        if (allocator) platform_safe_read(allocator, &vtable, 8);
        if (vtable) platform_safe_read(vtable + (slot ? 0x58 : 0x50), &entry, 8);
        fprintf(stderr, "  UFC-57: allocator at scene+%#x: %#" PRIxPTR ", vtable +%#" PRIx64 ", alloc +%#" PRIx64 "\n",
                slot ? 0x18 : 0, allocator, vtable ? vtable - base : 0, entry ? entry - base : 0);
    }
    /* Oldest and newest waiting snapshot (their build number is at +0x48). */
    const uint64_t waiting = f[0xc8 / 8];
    if (waiting && waiting < (1u << 24)) {
        uint64_t first = 0, last = 0, n_first = 0, n_last = 0;
        platform_safe_read(f[0xc0 / 8], &first, 8);
        platform_safe_read(f[0xc0 / 8] + (waiting - 1) * 8, &last, 8);
        if (first) platform_safe_read(first + 0x48, &n_first, 8);
        if (last) platform_safe_read(last + 0x48, &n_last, 8);
        fprintf(stderr, "  UFC-57: waiting snapshots span builds %" PRIu64 "..%" PRIu64 "\n", n_first, n_last);
    }
    /* Nodes of the list at scene+0x20, and how many got no copy. */
    uint64_t nodes = 0, empty = 0, node = f[0x28 / 8];
    while (node && node != scene + 0x20 && nodes < 100000) {
        uint64_t n[5];
        if (!platform_safe_read(node, n, sizeof(n))) break;
        ++nodes; empty += !n[4];
        node = n[1];
    }
    fprintf(stderr, "  UFC-57: %" PRIu64 " nodes, %" PRIu64 " without a copy\n", nodes, empty);
}
/* NBA: BB_TRACE_IMPORTS=1 keeps the last 256 runtime import calls (dumped on a fault); =2 also prints each. */
static int trace_imports;
static __thread uint32_t trace_ring[8192];
static __thread long trace_next;
static uint32_t *trace_all_rings[256];
static long *trace_all_next[256];
#ifdef _WIN32
#include <windows.h>
static HANDLE trace_all_handles[256];
static unsigned long trace_all_tids[256];
#endif
static volatile long trace_threads;
static __thread int trace_registered;
static ABI void trace_import(uint32_t id) {
    if (!trace_registered) {
        trace_registered = 1;
        long k = __atomic_fetch_add(&trace_threads, 1, __ATOMIC_RELAXED);
        if (k < 256) {
            trace_all_rings[k] = trace_ring; trace_all_next[k] = &trace_next;
#ifdef _WIN32
            trace_all_tids[k] = GetCurrentThreadId();
            trace_all_handles[k] = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, GetCurrentThreadId());
#endif
        }
    }
    long n = trace_next++;
    trace_ring[n & 8191] = id;
    /* BB_IMPORT_TRAP=<import name>: when the game calls it (the first 3 times), print the game's
     * return addresses on this thread's stack, keeping only those right after a call
     * instruction (real returns, not stale data), innermost first. Needs BB_TRACE_IMPORTS. */
    {
        static const char *trap; static int trap_read; static volatile long trap_hits;
        if (!trap_read) { trap = getenv("BB_IMPORT_TRAP"); trap_read = 1; }
        const char *s = trap ? runtime_symbol(names[id]) : NULL; if (trap && !s) s = names[id];
        if (trap && s && !strcmp(s, trap) && __atomic_add_fetch(&trap_hits, 1, __ATOMIC_RELAXED) <= 3) {
            const uintptr_t lo = 0x800000000ull, hi = 0x800000000ull + 0x2f00000ull;
            const uintptr_t top = platform_thread_stack_top();
            const uintptr_t *sp = (const uintptr_t *)__builtin_frame_address(0);
            char line[2048]; int o = 0, found = 0;
            for (int i = 0; i < 40000 && (uintptr_t)(sp + i + 1) <= top && found < 40 && o < (int)sizeof line - 24; ++i) {
                const uintptr_t r = sp[i];
                if (r < lo + 8 || r >= hi) continue;
                const uint8_t *b = (const uint8_t *)r;
                const int call = b[-5] == 0xe8 || (b[-2] == 0xff && (b[-1] & 0x38) == 0x10) ||
                                 (b[-3] == 0xff && (b[-2] & 0x38) == 0x10) || (b[-6] == 0xff && (b[-5] & 0x38) == 0x10) ||
                                 (b[-7] == 0xff && (b[-6] & 0x38) == 0x10);
                if (!call) continue;
                o += snprintf(line + o, sizeof line - (size_t)o, " +%#lx", (unsigned long)(r - lo)); ++found;
            }
            fprintf(stderr, "IMPORT TRAP %s: game returns:%s\n", trap, found ? line : " (none)");
        }
    }
    if (trace_imports > 1) {
        static volatile long thread_ids; static __thread long my_id;
        if (!my_id) my_id = __atomic_add_fetch(&thread_ids, 1, __ATOMIC_RELAXED);
        const char *s = runtime_symbol(names[id]); if (!s) s = names[id];
        static const char *noisy[] = {"Mutex", "strlen", "memset", "memcpy", "memmove", "memcmp", "Usleep", "TimeCounter",
                                      "Cond", "Sema", "EventFlag", "Equeue", "HRTimer", "GetStatus", "Rwlock", "Yield", "lock", "guard", "atexit", "strn", "strc", "malloc", "free", "Gettime", 0};
        for (const char **n = noisy; *n; ++n) if (strstr(s, *n)) return;
        fprintf(stderr, "IMP t%ld %s\n", my_id, s);
    }
}
/* BB_TRACE_DUMP_AT=<seconds>: then prints the last calls of every guest thread (where each one waits). */
static void *trace_watch(void *arg) {
    platform_sleep_ns((uint64_t)(uintptr_t)arg * 1000000000ull);
    long count = trace_threads < 256 ? trace_threads : 256;
    fprintf(stderr, "THREADS at %lus: %ld guest threads called the runtime\n", (unsigned long)(uintptr_t)arg, count);
    for (long k = 0; k < count; ++k) {
        long end = *trace_all_next[k], start = end > 6 ? end - 6 : 0;
#ifdef _WIN32
        fprintf(stderr, "  thread %ld tid %lu (%ld calls):", k + 1, trace_all_tids[k], end);
#else
        fprintf(stderr, "  thread %ld (%ld calls):", k + 1, end);
#endif
        for (long i = start; i < end; ++i) {
            uint32_t id = trace_all_rings[k][i & 8191]; const char *s = runtime_symbol(names[id]);
            fprintf(stderr, " %s", s ? s : names[id]);
        }
        fprintf(stderr, "\n");
    }
#ifdef _WIN32
    /* Where each thread is right now: 8 samples of rip (guest offsets when in the game image). */
    for (long k = 0; k < count; ++k) {
        if (!trace_all_handles[k]) continue;
        fprintf(stderr, "  thread %ld rip:", k + 1);
        for (int sample = 0; sample < 4; ++sample) {
            CONTEXT c; memset(&c, 0, sizeof(c)); c.ContextFlags = CONTEXT_CONTROL;
            if (SuspendThread(trace_all_handles[k]) == (DWORD)-1) break;
            BOOL ok = GetThreadContext(trace_all_handles[k], &c);
            ResumeThread(trace_all_handles[k]);
            if (!ok) break;
            uintptr_t rip = (uintptr_t)c.Rip;
            if (rip - (uintptr_t)image < 0x5000000) fprintf(stderr, " g%#llx", (unsigned long long)(rip - (uintptr_t)image));
            else fprintf(stderr, " h%#llx", (unsigned long long)rip);
            platform_sleep_ns(10000000ull);
        }
        fprintf(stderr, "\n");
    }
#endif
    fflush(stderr);
    return NULL;
}
static void trace_dump(void) {
    if (!trace_imports) return;
    long end = trace_next, start = end > 8192 ? end - 8192 : 0;
    fprintf(stderr, "  Last %ld runtime imports (oldest first):\n", end - start);
    for (long i = start; i < end; ++i) {
        uint32_t id = trace_ring[i & 8191]; const char *s = runtime_symbol(names[id]);
        fprintf(stderr, "    %s\n", s ? s : names[id]);
    }
}
/* NBA: BB_GAME_LOG=<guest offset hex> replaces the game's variadic log function (obj, level, fmt, ...)
 * with a printer (NBA LIVE 19: 1cfaf50). Only the three register arguments after fmt are shown. */
static void game_log_str(char *out, size_t cap, uint64_t p) {
    char s[160]; size_t n = 0;
    if (p < 0x10000 || !platform_safe_read((uintptr_t)p, s, 1)) { snprintf(out, cap, "%#llx", (unsigned long long)p); return; }
    while (n + 1 < sizeof(s) && platform_safe_read((uintptr_t)p + n, s + n, 1) && s[n]) ++n;
    s[n] = 0; snprintf(out, cap, "%s", s);
}
static ABI int game_log(void *obj, int level, uint64_t fmt, uint64_t a, uint64_t b, uint64_t c) {
    (void)obj; char f[512], line[1024], tmp[200]; uint64_t args[3] = {a, b, c}; int k = 0; size_t o = 0;
    game_log_str(f, sizeof(f), fmt);
    for (const char *p = f; *p && o + 210 < sizeof(line); ++p) {
        if (*p != '%') { line[o++] = *p; continue; }
        const char *q = p + 1; while (*q && strchr("-+ #0123456789.lhzjt", *q)) ++q;
        if (*q == '%') { line[o++] = '%'; p = q; continue; }
        uint64_t v = k < 3 ? args[k++] : 0;
        if (*q == 's') game_log_str(tmp, sizeof(tmp), v);
        else if (*q == 'd' || *q == 'i') snprintf(tmp, sizeof(tmp), "%lld", strchr(p, 'l') && strchr(p, 'l') < q ? (long long)v : (long long)(int32_t)v);
        else if (*q == 'u') snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
        else if (*q == 'c') snprintf(tmp, sizeof(tmp), "%c", (int)v);
        else snprintf(tmp, sizeof(tmp), "%#llx", (unsigned long long)v);
        size_t m = strlen(tmp); memcpy(line + o, tmp, m); o += m; p = *q ? q : q - 1;
    }
    line[o] = 0;
    fprintf(stderr, "GAME[%d] %s%s", level, line, o && line[o - 1] == '\n' ? "" : "\n");
    return 0;
}
#ifdef _WIN32
/* BB_OSDK_LOG=<prefixes,...>: the game's OSDK/Blaze log lines are formatted only for a logger the
 * shipped game never enables. Each call site whose format string (lea rdx) starts with one of the
 * prefixes gets its indirect log call replaced by int3 + nops; the handler prints the format and
 * the next three integer arguments, then resumes after the call. */
#define OSDK_SITES 4096
static struct { uintptr_t at; const char *fmt; uint8_t len; } osdk_sites[OSDK_SITES];
static int osdk_count;
/* BB_NOTIFY_TRACE=1: NBA LIVE 19's UI adapters notify the script through guest +0x6e4680
 * (this, class, event, ...). Each `call` to it becomes int3 + nops; the handler logs
 * "NOTIFY class event" and performs the call. */
#define NOTIFY_SITES 4096
static uintptr_t notify_sites[NOTIFY_SITES], notify_target;
static int notify_count;
static void notify_patch(unsigned char *image, uint64_t size) {
    const uint64_t end = size < 0x2d00000ull ? size : 0x2d00000ull, target = 0x6e4680;
    for (uint64_t i = 0x10000; i + 5 <= end && notify_count < NOTIFY_SITES; ++i) {
        if (image[i] != 0xe8) continue;
        const int32_t rel = (int32_t)(image[i + 1] | image[i + 2] << 8 | image[i + 3] << 16 | (uint32_t)image[i + 4] << 24);
        /* +0x9f1213 calls LiveUpdateError(+0x55a260) with the live update's result code. */
        if (i == 0x9f1213 && (int64_t)i + 5 + rel == 0x55a260) {
            image[i] = 0xcc; memset(image + i + 1, 0x90, 4);
            notify_sites[notify_count++] = (uintptr_t)(image + i);
            continue;
        }
        if ((int64_t)i + 5 + rel != (int64_t)target) continue;
        image[i] = 0xcc; memset(image + i + 1, 0x90, 4);
        notify_sites[notify_count++] = (uintptr_t)(image + i);
    }
    notify_target = (uintptr_t)image + target;
    printf("NBA: %d script notifications traced (BB_NOTIFY_TRACE)\n", notify_count);
}
static int notify_hit(CONTEXT *c, uintptr_t at) {
    int lo = 0, hi = notify_count - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (notify_sites[mid] == at && at == notify_target - 0x6e4680 + 0x9f1213) {
            fprintf(stderr, "NOTIFY live update result %d\n", (int)c->Rsi);
            c->Rsp -= 8; *(uint64_t *)(uintptr_t)c->Rsp = at + 5;
            c->Rip = notify_target - 0x6e4680 + 0x55a260;
            return 1;
        }
        if (notify_sites[mid] == at) {
            char a[64] = {0}, e[64] = {0};
            platform_safe_read((uintptr_t)c->Rsi, a, sizeof a - 1);
            platform_safe_read((uintptr_t)c->Rdx, e, sizeof e - 1);
            fprintf(stderr, "NOTIFY %s %s\n", a, e);
            c->Rsp -= 8; *(uint64_t *)(uintptr_t)c->Rsp = at + 5;
            c->Rip = notify_target;
            return 1;
        }
        if (notify_sites[mid] < at) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}
static LONG CALLBACK osdk_break(EXCEPTION_POINTERS *info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = info->ContextRecord;
    const uintptr_t at = (uintptr_t)info->ExceptionRecord->ExceptionAddress;
    if (notify_count && notify_hit(c, at)) return EXCEPTION_CONTINUE_EXECUTION;
    int lo = 0, hi = osdk_count - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (osdk_sites[mid].at == at) {
            char a[3][96];
            const DWORD64 v[3] = {c->Rcx, c->R8, c->R9};
            for (int k = 0; k < 3; ++k) {
                char s[64] = {0};
                if (v[k] > 0x10000 && platform_safe_read((uintptr_t)v[k], s, sizeof s - 1) && s[0] >= 0x20 && s[0] < 0x7f) {
                    for (int j = 0; s[j]; ++j) if (s[j] < 0x20 || s[j] >= 0x7f) { s[j] = 0; break; }
                    snprintf(a[k], sizeof a[k], "%#llx \"%s\"", (unsigned long long)v[k], s);
                } else snprintf(a[k], sizeof a[k], "%#llx", (unsigned long long)v[k]);
            }
            fprintf(stderr, "OSDK[%u] %s | %s | %s | %s\n", (unsigned)c->Rsi, osdk_sites[mid].fmt, a[0], a[1], a[2]);
            c->Rip = at + osdk_sites[mid].len;
            c->Rax = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (osdk_sites[mid].at < at) lo = mid + 1; else hi = mid - 1;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void osdk_log_patch(unsigned char *image, uint64_t size, const char *prefixes) {
    const uint64_t end = size < 0x2d00000ull ? size : 0x2d00000ull;
    for (uint64_t i = 0x10000; i + 7 < end && osdk_count < OSDK_SITES; ++i) {
        if (image[i] != 0x48 || image[i + 1] != 0x8d || image[i + 2] != 0x15) continue;
        const int32_t disp = (int32_t)(image[i + 3] | image[i + 4] << 8 | image[i + 5] << 16 | (uint32_t)image[i + 6] << 24);
        const int64_t s = (int64_t)i + 7 + disp;
        if (s < 0x2d00000 || (uint64_t)s >= size) continue;
        const char *fmt = (const char *)image + s;
        int match = 0;
        for (const char *p = prefixes; *p && !match;) {
            const char *q = strchr(p, ',');
            const size_t n = q ? (size_t)(q - p) : strlen(p);
            if (n && !strncmp(fmt, p, n)) match = 1;
            p = q ? q + 1 : p + n;
        }
        if (!match) continue;
        /* The first indirect call within the next 40 bytes: ff /2 with a register or [reg]/[reg+d8] operand. */
        for (uint64_t j = i + 7; j < i + 47 && j + 3 < end; ++j) {
            if (image[j] == 0xcc) break; /* the log call this format shares with another site */
            const uint64_t k = image[j] == 0x41 ? j + 1 : j;
            if (image[k] != 0xff || (image[k + 1] & 0x38) != 0x10) continue;
            const unsigned mod = image[k + 1] >> 6, rm = image[k + 1] & 7;
            if (rm == 4 || (mod == 0 && rm == 5) || mod == 2) continue;
            const uint8_t len = (uint8_t)(k + 2 + (mod == 1) - j);
            image[j] = 0xcc;
            memset(image + j + 1, 0x90, len - 1u);
            osdk_sites[osdk_count].at = (uintptr_t)(image + j);
            osdk_sites[osdk_count].fmt = fmt;
            osdk_sites[osdk_count].len = len;
            ++osdk_count;
            break;
        }
    }
    AddVectoredExceptionHandler(1, osdk_break);
    printf("NBA: %d OSDK log calls traced (BB_OSDK_LOG=%s)\n", osdk_count, prefixes);
}
#endif
/* BB_SCRIPT_TRACE=<substrings,...> (or *): each UI-script native call whose class or method
 * contains one of them, as "SCRIPT Class.Method" (repeats of the same call are counted). */
#ifdef _WIN32
/* Events the game posts to the UI script go through the dispatcher at guest *(+0x3a47840),
 * virtual slot 0xd8 (this, adaptor, event, argument, controller): logged as "EVENT ...". */
typedef ABI void (*PostEvent)(uintptr_t, const char *, const char *, const char *, const char *, uintptr_t);
static PostEvent post_event_original;
static ABI void post_event_hook(uintptr_t self, const char *a, const char *e, const char *s, const char *c, uintptr_t r9) {
    char x[96], y[96], z[96], w[96];
    game_log_str(x, sizeof x, (uint64_t)(uintptr_t)a); game_log_str(y, sizeof y, (uint64_t)(uintptr_t)e);
    game_log_str(z, sizeof z, (uint64_t)(uintptr_t)s); game_log_str(w, sizeof w, (uint64_t)(uintptr_t)c);
    fprintf(stderr, "EVENT %s %s | %s | %s\n", x, y, z, w);
    post_event_original(self, a, e, s, c, r9);
}
static void post_event_install(void) {
    static int done;
    if (done) return;
    const uintptr_t holder = *(const uintptr_t *)(0x800000000ull + 0x3a47840);
    if (!holder) return;
    uintptr_t *slot = (uintptr_t *)(*(const uintptr_t *)holder + 0xd8);
    done = 1;
    DWORD old;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) { fprintf(stderr, "EVENT hook: protect failed\n"); return; }
    post_event_original = (PostEvent)*slot;
    *slot = (uintptr_t)post_event_hook;
    VirtualProtect(slot, 8, old, &old);
    fprintf(stderr, "EVENT hook installed (original %#llx)\n", (unsigned long long)((uintptr_t)post_event_original - 0x800000000ull));
}
#endif
static ABI void script_native_hook(const char *cls, const char *method) {
    static const char *filter; static int read;
    if (!read) { filter = getenv("BB_SCRIPT_TRACE"); read = 1; }
#ifdef _WIN32
    if (getenv("BB_SCRIPT_EVENTS")) post_event_install();
#endif
    char c[96], m[96];
    game_log_str(c, sizeof c, (uint64_t)(uintptr_t)cls);
    game_log_str(m, sizeof m, (uint64_t)(uintptr_t)method);
    if (filter && strcmp(filter, "*")) {
        int hit = 0; const char *p = filter;
        while (*p && !hit) {
            const char *e = strchr(p, ','); size_t n = e ? (size_t)(e - p) : strlen(p);
            char want[64]; if (n >= sizeof want) n = sizeof want - 1; memcpy(want, p, n); want[n] = 0;
            hit = n && (strstr(c, want) || strstr(m, want));
            p = e ? e + 1 : p + n;
        }
        if (!hit) return;
    }
    if (!strcmp(c, "LoginAdaptor") && (!strcmp(m, "StartLogout") || !strcmp(m, "IsLoginInProgress"))) {
        /* The LoginManager the LoginAdaptor natives call (guest +0x3a47080 -> +0x70): its state,
         * read through the same virtual slots (0x100 IsLoginInProgress, 0xd8 IsLoggedIntoNetwork,
         * 0xe0/0xf0 IsLoggedIntoEA/IsPassivelyLoggedIntoEA for user 0). */
        const uintptr_t base = 0x800000000ull;
        uintptr_t holder = *(const uintptr_t *)(base + 0x3a47080);
        uintptr_t mgr = holder ? *(const uintptr_t *)(holder + 0x70) : 0;
        if (mgr) {
            const uintptr_t *vt = *(const uintptr_t *const *)mgr;
            /* Only the call the native makes as is (no arguments): others need state set up first. */
            typedef ABI uint8_t (*Get0)(uintptr_t);
            typedef ABI int32_t (*GetI)(uintptr_t);
            typedef ABI int32_t (*ScriptArg)(int32_t);
            /* +0x6e4540(i): the native's integer argument i, as StartLogout itself reads it. */
            fprintf(stderr, "SCRIPT %s.%s: login in progress %d, controller %d, arg0 %d\n", c, m, ((Get0)vt[0x100 / 8])(mgr),
                    ((GetI)vt[0x110 / 8])(mgr), !strcmp(m, "StartLogout") ? ((ScriptArg)(base + 0x6e4540))(0) : -99);
        }
        if (!strcmp(m, "StartLogout")) {
            /* The whole guest call chain (real return addresses only), to find the native event that
             * ran the script handler: the outermost frames below the script interpreter. */
            const uintptr_t lo = base, hi = base + 0x2f00000ull, top = platform_thread_stack_top();
            const uintptr_t *sp = (const uintptr_t *)__builtin_frame_address(0);
            char line[4096]; int o = 0, found = 0;
            for (int i = 0; i < 200000 && (uintptr_t)(sp + i + 1) <= top && o < (int)sizeof line - 24; ++i) {
                const uintptr_t r = sp[i];
                if (r < lo + 8 || r >= hi) continue;
                const uint8_t *b = (const uint8_t *)r;
                if (!(b[-5] == 0xe8 || (b[-2] == 0xff && (b[-1] & 0x38) == 0x10) || (b[-3] == 0xff && (b[-2] & 0x38) == 0x10) ||
                      (b[-6] == 0xff && (b[-5] & 0x38) == 0x10) || (b[-7] == 0xff && (b[-6] & 0x38) == 0x10))) continue;
                o += snprintf(line + o, sizeof line - (size_t)o, " +%#lx", (unsigned long)(r - lo)); ++found;
            }
            fprintf(stderr, "SCRIPT StartLogout chain (%d):%s\n", found, line);
        }
    }
    static __thread char last[200]; static __thread unsigned repeats;
    char now[200]; snprintf(now, sizeof now, "%s.%s", c, m);
    if (!strcmp(now, last)) { ++repeats; return; }
    if (repeats) fprintf(stderr, "SCRIPT   (x%u more)\n", repeats);
    repeats = 0; snprintf(last, sizeof last, "%s", now);
    fprintf(stderr, "SCRIPT %s\n", now);
}
static unsigned char *trace_thunks;
static uintptr_t trace_wrap(uint32_t id, uintptr_t target) {
    unsigned char *t = trace_thunks + (size_t)id * 192, *p = t;
    static const unsigned char save[] = {0x50,0x51,0x52,0x56,0x57,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,
                                         0x48,0x81,0xEC,0x80,0,0,0};
    static const unsigned char rest[] = {0x48,0x81,0xC4,0x80,0,0,0,
                                         0x41,0x5B,0x41,0x5A,0x41,0x59,0x41,0x58,0x5F,0x5E,0x5A,0x59,0x58};
    memcpy(p, save, sizeof(save)); p += sizeof(save);
    for (int k = 0; k < 8; ++k) { *p++ = 0xF3; *p++ = 0x0F; *p++ = 0x7F; *p++ = (unsigned char)(0x44 | (k << 3)); *p++ = 0x24; *p++ = (unsigned char)(16 * k); }
    *p++ = 0xBF; memcpy(p, &id, 4); p += 4;
    uintptr_t fn = (uintptr_t)trace_import; *p++ = 0x48; *p++ = 0xB8; memcpy(p, &fn, 8); p += 8;
    *p++ = 0xFF; *p++ = 0xD0;
    for (int k = 0; k < 8; ++k) { *p++ = 0xF3; *p++ = 0x0F; *p++ = 0x6F; *p++ = (unsigned char)(0x44 | (k << 3)); *p++ = 0x24; *p++ = (unsigned char)(16 * k); }
    memcpy(p, rest, sizeof(rest)); p += sizeof(rest);
    *p++ = 0xFF; *p++ = 0x25; memset(p, 0, 4); p += 4; memcpy(p, &target, 8);
    return (uintptr_t)t;
}
static void fault_extra(void *context) {
    trace_dump();
    fprintf(stderr, "  Version: game %s\n", BB_BUILD_VERSION);
    if (gpu_enabled) bbgpu_dump_guest_writes(context);
    ui_snapshot_report(context);
}
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
/* BBPATCH2 (patches.py): the patches' image base, then byte writes at image offsets, applied
 * after relocation. A write may replace a whole base-relative pointer slot (60/90 FPS++ swap
 * function pointers): the patch holds the address at the patches' base, rebased here. */
static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"BBPATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0;
    unsigned char data[4096];
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !mapped(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data)/8]; size_t nslots=0;
        for (uint64_t r=0;r<nr;++r) {
            if (!(relocs[r].target<offset+length && offset<relocs[r].target+8)) continue;
            uint64_t target=relocs[r].target, value;
            if (relocs[r].kind || target<offset || target+8>offset+length) fail("patch overlaps a relocation");
            memcpy(&value,data+(target-offset),8);
            if (value<base || !mapped(segments,ns,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(image+offset,data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,image+slots[s],8);
            value=(uint64_t)(uintptr_t)image+(value-base);
            memcpy(image+slots[s],&value,8);
        }
        rebased+=nslots;
        bytes+=length;
    }
    if (fgetc(f)!=EOF) fail("trailing data in patch file");
    fclose(f);
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",count,bytes,rebased);
}
/* Restarts the game (the settings menu: a new render resolution is a patch applied at start):
 * run.sh again on Linux, the launcher's command line on Windows (platform_restart). Descriptors
 * are closed first so the old GPU device and its memory are released before the new process
 * opens its own. */
volatile int runtime_restarting;
void runtime_restart(void) {
    fflush(NULL);
    /* The GPU threads still run until the new process starts: their Vulkan calls fail once the
     * device is closed, and an assertion there must not end the process (exit 23) first. */
    runtime_restarting = 1;
    __sync_synchronize();
    platform_restart();
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        puts(game_build_identity + sizeof(BB_GAME_VERSION_MARKER) - 1);
        return 0;
    }
#ifdef __linux__
    /* ufc3: BB_ALLOW_PTRACE=1 lets a debugger attach later (Yama ptrace_scope 1 allows only
     * descendants otherwise): stacks of a hang that does not happen under gdb. */
    if (getenv("BB_ALLOW_PTRACE")) prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    /* NBA LIVE 19: unbuffered, MinGW's printf writes a redirected log one character per
     * WriteFile under the stream lock; the loading thread (a line per missing file) spent
     * about half its time there. A log file is buffered and flushed twice a second instead. */
    if (!isatty(fileno(stdout)) && !getenv("BB_LOG_UNBUFFERED")) {
        static char log_buffer[1 << 16];
        setvbuf(stdout, log_buffer, _IOFBF, sizeof(log_buffer));
        pthread_t flusher;
        if (pthread_create(&flusher, NULL, log_flusher, NULL) == 0) pthread_detach(flusher);
    }
#endif
    /* Host preparation before anything else allocates: glibc's heap below 1 TiB (Linux); the
     * PS4 user range reserved and the guest TCB's TEB TLS slot claimed before any driver loads
     * (Windows). The guest packs pointers into 40 bits. */
    platform_process_init();
    runtime_memory_reserve();
    runtime_thread_host_init();
#ifdef _WIN32
    const char *platform = "Windows";
#else
    const char *platform = "Linux";
#endif
#ifdef _WIN32
    const char *gpu_version = bbgpu_version();
#else
    const char *gpu_version = bbgpu_version ? bbgpu_version() : "unknown";
#endif
    bb_print_versions(stdout, BB_BUILD_VERSION, gpu_version, platform);
    fflush(stdout);
    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
    check_cpu();
    int cpu_only = 0, strict_imports = 0, list_imports = 0;
    unsigned timeout_seconds = 10;
    int64_t tls_loads = -1; /* --tls-loads: the preparation step's count of guest TCB loads (link.json) */
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL, *patch_file=NULL, *import_names=NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        /* SacredFire: --list-imports resolves every import, prints the outcome and exits without running. */
        else if (!strcmp(argv[i], "--list-imports")) { list_imports = 1; cpu_only = 1; }
        else if (!strcmp(argv[i], "--import-names") && i+1<argc) import_names=argv[++i];
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches") && i+1<argc) patch_file=argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else if (!strcmp(argv[i], "--tls-loads") && i+1<argc) tls_loads=strtoll(argv[++i],NULL,10);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--list-imports] [--import-names file] [--content-profile file] [--app0 dir] [--user dir] [--patches file] [--timeout seconds] [--tls-loads count] | --vulkan-only\n", argv[0]);
        return 1;
    }
    if (import_names) {
        int loaded = runtime_load_import_names(import_names);
        if (loaded < 0) fail("cannot read the import names file");
        printf("Import names: %d symbols from %s\n", loaded, import_names);
    }
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"BBCONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        runtime_content_configure(values);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
    }
    bbgpu_register_kernel();
    page_size = platform_page_size();
    platform_install_fault_handlers(gpu_fault, recover_point, fault_extra, NULL, 0);
    platform_install_step_handler(gpu_step);
    platform_watchdog_start(timeout_seconds, NULL); /* 0 disables the watchdog */
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "BBPROBE1", 8) && memcmp(magic, "BBPROBE2", 8) && memcmp(magic,"BBPROBE3",8) && memcmp(magic,"BBPROBE4",8) && memcmp(magic,"BBPROBE5",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "BBPROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int multi=!memcmp(magic,"BBPROBE5",8);
    int linked=!memcmp(magic,"BBPROBE3",8) || !memcmp(magic,"BBPROBE4",8) || multi;
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        /* BBPROBE5: procparam, eboot TLS, module table, bindings (link_modules.py). */
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"BBPROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>7)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    image = allocate(round_page(size));
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    /* Fault reports and thread dumps name guest offsets from here on. */
    platform_install_fault_handlers(gpu_fault, recover_point, fault_extra, image, round_page(size));
    platform_watchdog_start(0, image);
    runtime_ui_backpressure_init(image, size); /* UFC-57 */
    runtime_appearance_init(image, size); /* UFC-58 */
    if (!cpu_only) {
        char title[128]="PS4 game", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        char icon[4200];
        snprintf(icon,sizeof(icon),"%s/sce_sys/icon0.png",app0);
        BbGpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,1920,1080,icon};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        if (bbgpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,bbgpu_symbol_count());
    }
    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        /* SysV: mov edi, index; movabs rax, handler; jmp rax. No fake return values. */
        uint32_t index = (uint32_t)i;
        uintptr_t handler = (uintptr_t)unresolved;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe; /* mov rsi,rdi: preserve arg0 */
        t[3] = 0xbf; memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &handler, 8);
        t[18] = 0xff; t[19] = 0xe0;
    }
    const char *trace_env = getenv("BB_TRACE_IMPORTS");
    trace_imports = trace_env ? atoi(trace_env) : 0;
    if (trace_imports) trace_thunks = allocate(round_page((import_count + 1) * 192));
    if (trace_imports && getenv("BB_TRACE_DUMP_AT")) {
        pthread_t watcher;
        pthread_create(&watcher, NULL, trace_watch, (void *)(uintptr_t)atoi(getenv("BB_TRACE_DUMP_AT")));
        pthread_detach(watcher);
    }
    /* SacredFire (--list-imports): how each import was bound: 1 runtime, 2 native module, 3 trap. */
    unsigned char *outcome = calloc(import_count ? import_count : 1, 1);
    if (!outcome) fail("allocation failed");
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            uintptr_t resolved = runtime_resolve(names[relocs[i].value], relocs[i].kind == 2);
            if (resolved && trace_imports && relocs[i].kind == 1) {
                /* BB_TRACE_ONLY=a,b,c: wrap only imports whose name contains one of these (less timing change). */
                const char *only = getenv("BB_TRACE_ONLY"), *sym = runtime_symbol(names[relocs[i].value]);
                int take = !only;
                if (only && sym) {
                    char list[512]; snprintf(list, sizeof(list), "%s", only);
                    for (char *tok = strtok(list, ","); tok && !take; tok = strtok(NULL, ",")) take = strstr(sym, tok) != NULL;
                }
                if (take) resolved = trace_wrap((uint32_t)relocs[i].value, resolved);
            }
            if (resolved) { value = resolved + relocs[i].addend; outcome[relocs[i].value] = 1; }
            else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address=bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value]!=relocs[i].kind || !mapped(segments,ns,address,relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)image+address+relocs[i].addend;
                outcome[relocs[i].value] = 2;
            } else outcome[relocs[i].value] = 3;
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    if (list_imports) {
        static const char *words[] = {"unreferenced", "runtime", "native", "unresolved"};
        unsigned counts[4] = {0};
        for (uint64_t i = 0; i < import_count; ++i) {
            const char *symbol = runtime_symbol(names[i]);
            printf("%-12s %-40s %s\n", words[outcome[i]], names[i], symbol ? symbol : "?");
            ++counts[outcome[i]];
        }
        printf("Imports: %u runtime, %u native, %u unresolved, %u unreferenced\n", counts[1], counts[2], counts[3], counts[0]);
        return 0;
    }
    if (patch_file) apply_patches(patch_file, segments, ns, relocs, nr);
    if (getenv("BB_GAME_LOG")) {
        uint64_t off = strtoull(getenv("BB_GAME_LOG"), NULL, 16);
        uintptr_t host = (uintptr_t)game_log;
        unsigned char *at = image + off;
        at[0] = 0x48; at[1] = 0xB8; memcpy(at + 2, &host, 8); at[10] = 0xFF; at[11] = 0xE0;
        printf("NBA: game log function at guest offset %#llx redirected (BB_GAME_LOG)\n", (unsigned long long)off);
    }
    {
        /* NBA LIVE 19 live updates: the online server has no live update content (it answers
         * GetLiveUpdatePaths with an error or an empty list), so the update ends with a nonzero
         * result; +0x9f1205 then posts LiveUpdateError to the UI script, which logs out of EA
         * ~10 s after login ("connection lost", NBA-DCFEA). `test esi,esi; je` becomes `jmp`:
         * the update completes as "nothing new" (LiveUpdateComplete only). */
        static const unsigned char want[] = {0x8b, 0x73, 0x50, 0x85, 0xf6, 0x74, 0x13};
        if (0x9f1205 + sizeof want <= size && !memcmp(image + 0x9f1205, want, sizeof want)) {
            image[0x9f1205 + 5] = 0xeb;
            printf("NBA: live update errors ignored (no live update content online)\n");
        }
    }
    {
        /* NBA LIVE 19 online play: ConnApiAdapter turns on DirtySock's encrypted ProtoTunnel
         * (ConnApiControl('tunl', 1, ...)) while setting up the mesh; the community's dedicated
         * game server speaks plain CommUDP on the ConnApi game port. `mov edx,1` -> `mov edx,0`
         * at both sites, as the shadPS4 patch "Online: sin ProtoTunnel" does. */
        static const uint64_t sites[] = {0x1d4118b, 0x1d3e58d};
        static const unsigned char want[] = {0xbe, 0x6c, 0x6e, 0x75, 0x74, 0xba, 0x01, 0x00, 0x00, 0x00};
        int patched = 0;
        for (int i = 0; i < 2; ++i) {
            const uint64_t at = sites[i] - 5;
            if (at + sizeof want <= size && !memcmp(image + at, want, sizeof want)) { image[sites[i] + 1] = 0; ++patched; }
        }
        if (patched) printf("NBA: ProtoTunnel off at %d of 2 sites (plain CommUDP to the game server)\n", patched);
    }
    {
        /* NBA LIVE 19 online: the rest of the shadPS4 patches the community plays with
         * (NBA19_online.xml; guest offset = shadPS4 address - 0x400000). Each applies only when
         * the original bytes match. */
        static const struct { uint64_t at; unsigned char want[6], put[6]; uint8_t n; const char *what; } fixes[] = {
            /* Online::Manager::GetMachineId (+0xa0b290) returns a cached value unless the online
             * session is in state 1 or 2; left at -1, the online PauseManager (2-machine table)
             * writes out of bounds and the match crashes at kickoff. Always read ConnApi. */
            {0xa0b2ca, {0x74, 0x07}, {0x90, 0x90}, 2, "machine id always from ConnApi"},
            /* Live Run lobby: numPlayersRequired is a fixed 4; the countdown starts with 2 humans
             * and the CPU fills the rest. */
            {0x44b9f9, {0xb8, 0x04, 0x00, 0x00, 0x00}, {0xb8, 0x02, 0x00, 0x00, 0x00}, 5, "Live Run starts with 2 players"},
            /* Online::PrivateManagerData::Update disconnects a player idle ~90 s
             * (DISCONNECT_REASON_IDLE_USER) and Live Run ends the match for everyone: never. */
            {0xa0b02d, {0x0f, 0x84}, {0x90, 0xe9}, 2, "no disconnect for idle players"},
        };
        for (size_t i = 0; i < sizeof fixes / sizeof *fixes; ++i)
            if (fixes[i].at + fixes[i].n <= size && !memcmp(image + fixes[i].at, fixes[i].want, fixes[i].n)) {
                memcpy(image + fixes[i].at, fixes[i].put, fixes[i].n);
                printf("NBA: %s\n", fixes[i].what);
            }
    }
    if (getenv("BB_SCRIPT_TRACE") && 0x39f2d88 + 8 <= size) {
        /* NBA LIVE 19: the UI script calls its native functions through guest +0x6e3a80, which
         * first calls the hook pointer at +0x39f2d88 (empty in the shipped game) with the class
         * and method names. Pointing it here logs each call by name. */
        uintptr_t host = (uintptr_t)script_native_hook;
        memcpy(image + 0x39f2d88, &host, 8);
        printf("NBA: script native calls traced (BB_SCRIPT_TRACE=%s)\n", getenv("BB_SCRIPT_TRACE"));
    }
#ifdef _WIN32
    if (getenv("BB_NOTIFY_TRACE")) {
        notify_patch(image, size);
        if (!getenv("BB_OSDK_LOG")) AddVectoredExceptionHandler(1, osdk_break);
    }
    if (getenv("BB_OSDK_LOG")) osdk_log_patch(image, size, getenv("BB_OSDK_LOG"));
#endif
    if (!cpu_only) bbgpu_patch_image(image, size);
#ifdef _WIN32
    /* Guest code reads its TCB with `mov rax, gs:[0]` (link_modules.py rewrote fs:[0]); on Windows GS
     * is the TEB, so the displacement becomes the TEB TLS slot holding the guest TCB. Every site is
     * decoded first, and the count must agree with the preparation step's (--tls-loads). */
    {
        uint64_t *addresses = calloc(ns ? ns : 1, sizeof(uint64_t)), *sizes = calloc(ns ? ns : 1, sizeof(uint64_t)), *flags = calloc(ns ? ns : 1, sizeof(uint64_t));
        if (!addresses || !sizes || !flags) fail("allocation failed");
        for (uint64_t i = 0; i < ns; ++i) { addresses[i] = segments[i].address; sizes[i] = segments[i].size; flags[i] = segments[i].flags; }
        printf("Guest TCB loads redirected to the TEB TLS slot: %" PRIu64 "%s\n", platform_patch_guest_tls_loads(image, addresses, sizes, flags, ns, tls_loads),
               tls_loads < 0 ? " (count not cross-checked: --tls-loads not given)" : " (matches the preparation step)");
        free(addresses); free(sizes); free(flags);
    }
#else
    (void)tls_loads;
#endif
    /* ufc3 online (UFC-25): approximate reciprocals that give the same bits on every CPU. */
    if (runtime_fp_deterministic()) {
        uint64_t *addresses = calloc(ns, sizeof(uint64_t)), *sizes = calloc(ns, sizeof(uint64_t)), *flags = calloc(ns, sizeof(uint64_t));
        uint64_t *pointers = calloc(nr ? nr : 1, sizeof(uint64_t)), pointer_count = 0;
        if (!addresses || !sizes || !flags || !pointers) fail("allocation failed");
        for (uint64_t i = 0; i < ns; ++i) { addresses[i] = segments[i].address; sizes[i] = segments[i].size; flags[i] = segments[i].flags; }
        for (uint64_t i = 0; i < nr; ++i) if (!relocs[i].kind) pointers[pointer_count++] = relocs[i].value;
        RuntimeFpStats fp;
        const uint64_t start = platform_monotonic_ns();
        runtime_fp_patch_image(image, size, addresses, sizes, flags, ns, pointers, pointer_count, map_near_image, &fp);
        printf("Deterministic FP (BB_DETERMINISTIC_FP): %" PRIu64 " of %" PRIu64 " rcp/rsqrt instructions patched in %" PRIu64 " functions "
               "(%" PRIu64 " kept as the CPU computes them), %" PRIu64 " KiB of stubs, %.0f ms; guest threads use MXCSR 0x9fc0\n",
               fp.patched, fp.found, fp.functions, fp.unpatched, fp.stub_bytes / 1024, (double)(platform_monotonic_ns() - start) / 1e6);
        free(addresses); free(sizes); free(flags); free(pointers);
    }
    protect(traps, round_page((import_count + 1) * 32), 5);
    if (trace_thunks) protect(trace_thunks, round_page((import_count + 1) * 192), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image+procparam);
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<module_count;++m) {
            /* NBA LIVE 19: the last linked module (EAWebkit.prx) starts when the game loads it
             * (sceKernelLoadStartModule, runtime_kernel.c), from a game thread, as on the console. */
            if (getenv("BB_NBA_WEBKIT_LINKED") && m==module_count-1 && module_count>2) {
                runtime_deferred_module_init=(uintptr_t)(image+modules[m].init);
                printf("Module %" PRIu64 " start deferred to sceKernelLoadStartModule\n",m);
                continue;
            }
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            typedef int (ABI *ModuleInit)(uint64_t,void *,void *);
            int result=((ModuleInit)(image+modules[m].init))(0,NULL,NULL);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
    struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks (platform_enter_guest_main). */
    runtime_fp_thread_setup(); /* also when no linked module attached the main thread */
    platform_enter_guest_main(image+entry,&params,(void *)guest_exit);
    fail("entry unexpectedly returned");
}
