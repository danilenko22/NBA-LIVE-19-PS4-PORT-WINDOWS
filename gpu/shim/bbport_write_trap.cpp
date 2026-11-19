// SPDX-License-Identifier: GPL-2.0-or-later
// ufc3 BB_WRITE_TRAP=<hex address>,<hex size> (diagnostics, Windows): who writes a range of guest
// memory. Its pages are made read-only once mapped and again every 300 ms; each write fault prints
// the writer (RIP, return addresses on the stack) once per distinct RIP, and leaves the page
// writable until the next re-arm. Off unless the variable is set.
#ifdef _WIN32
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace BbWriteTrap {
namespace {
constexpr std::uint64_t Page = 4096;
struct Range {
    std::uint64_t start = 0, end = 0;
};
const Range& Watched() {
    static const Range r = [] {
        Range out;
        const char* e = std::getenv("BB_WRITE_TRAP");
        if (!e) {
            return out;
        }
        char* end = nullptr;
        const std::uint64_t a = std::strtoull(e, &end, 16);
        const std::uint64_t s = (end && *end == ',') ? std::strtoull(end + 1, nullptr, 16) : Page;
        out.start = a & ~(Page - 1);
        out.end = (a + s + Page - 1) & ~(Page - 1);
        return out;
    }();
    return r;
}
std::atomic<bool> armed_once{false};
std::mutex lock;
std::vector<std::uint64_t> seen_rips;
std::atomic<std::uint64_t> faults{0};

void Protect(std::uint64_t start, std::uint64_t end, DWORD prot) {
    for (std::uint64_t p = start; p < end; p += Page) {
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(p), Page, prot, &old);
    }
}
} // namespace

void OnGuestMap(std::uint64_t address, std::uint64_t size) {
    const Range& r = Watched();
    if (!r.end || !(address < r.end && r.start < address + size) || armed_once.exchange(true)) {
        return;
    }
    std::printf("Write trap: armed %#llx..%#llx\n", (unsigned long long)r.start,
                (unsigned long long)r.end);
    std::fflush(stdout);
    std::thread([] {
        const Range& w = Watched();
        for (;;) {
            Protect(w.start, w.end, PAGE_READONLY);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }).detach();
}

bool OnFault(void* context, std::uint64_t address) {
    const Range& r = Watched();
    if (!r.end || address < r.start || address >= r.end || !armed_once.load()) {
        return false;
    }
    auto* info = static_cast<EXCEPTION_POINTERS*>(context);
    if (info->ExceptionRecord->ExceptionInformation[0] != 1) { // not a write
        return false;
    }
    const std::uint64_t page = address & ~(Page - 1);
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(page), Page, PAGE_READWRITE, &old);
    const CONTEXT* c = info->ContextRecord;
    const std::uint64_t n = faults.fetch_add(1, std::memory_order_relaxed);
    {
        std::scoped_lock l{lock};
        for (const std::uint64_t rip : seen_rips) {
            if (rip == c->Rip) {
                return true;
            }
        }
        if (seen_rips.size() >= 300) {
            return true;
        }
        seen_rips.push_back(c->Rip);
    }
    char line[1024];
    int len = std::snprintf(line, sizeof(line),
                            "Write trap: %#llx (+%#llx) by rip %#llx tid %lu fault %llu rdi %#llx "
                            "rsi %#llx rdx %#llx rcx %#llx; stack:",
                            (unsigned long long)address, (unsigned long long)(address - r.start),
                            (unsigned long long)c->Rip, GetCurrentThreadId(),
                            (unsigned long long)n, (unsigned long long)c->Rdi,
                            (unsigned long long)c->Rsi, (unsigned long long)c->Rdx,
                            (unsigned long long)c->Rcx);
    const auto* sp = reinterpret_cast<const std::uint64_t*>(c->Rsp);
    for (int i = 0, shown = 0; i < 64 && shown < 10 && len < int(sizeof(line)) - 24; ++i) {
        std::uint64_t v = 0;
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &v, sizeof(v), &got) || got != 8) {
            break;
        }
        // Code addresses (the host image and the guest's loaded modules).
        MEMORY_BASIC_INFORMATION mbi{};
        if (v > 0x10000 && VirtualQuery(reinterpret_cast<void*>(v), &mbi, sizeof(mbi)) &&
            (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY))) {
            len += std::snprintf(line + len, sizeof(line) - len, " %#llx", (unsigned long long)v);
            ++shown;
        }
    }
    std::printf("%s\n", line);
    std::fflush(stdout);
    return true;
}
} // namespace BbWriteTrap
#else
#include <cstdint>
namespace BbWriteTrap {
void OnGuestMap(std::uint64_t, std::uint64_t) {}
bool OnFault(void*, std::uint64_t) { return false; }
} // namespace BbWriteTrap
#endif
