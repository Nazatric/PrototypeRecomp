// PrototypeRecomp Phase 2B runtime — logging implementation.
#include "logging.h"

#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
#include <ucontext.h>
#include <cstdio>
#include <dlfcn.h>

// Crash diagnostics: capture SIGSEGV/SIGBUS/SIGABRT with a host backtrace.
// Uses an alternate signal stack so host-stack exhaustion can still report.
static char g_altstack[64 * 1024];

extern "C" void* pr_guest_base_for_crash_c();

static void CrashHandler(int sig, siginfo_t* info, void* ctx_) {
    ucontext_t* uc = (ucontext_t*)ctx_;
    uintptr_t rip = uc ? uc->uc_mcontext.gregs[REG_RIP] : 0;
    uintptr_t rsp = uc ? uc->uc_mcontext.gregs[REG_RSP] : 0;
    // For a jump/call to NULL the caller's return address sits at [RSP].
    uintptr_t ret_at_rsp = rsp ? *(uintptr_t*)rsp : 0;
    void* bt[64];
    int n = backtrace(bt, 64);
    fprintf(stdout,
            "[CRASH   ] signal %d at host addr %p RIP=%p RSP=%p ret@rsp=%p\n",
            sig, info ? info->si_addr : nullptr, (void*)rip, (void*)rsp,
            (void*)ret_at_rsp);
    if (uc) {
        fprintf(stdout,
                "[CRASH   ] RAX=%llx RBX=%llx RCX=%llx RDX=%llx RSI=%llx "
                "RDI=%llx R8=%llx R9=%llx R10=%llx R11=%llx R12=%llx\n",
                (unsigned long long)uc->uc_mcontext.gregs[REG_RAX],
                (unsigned long long)uc->uc_mcontext.gregs[REG_RBX],
                (unsigned long long)uc->uc_mcontext.gregs[REG_RCX],
                (unsigned long long)uc->uc_mcontext.gregs[REG_RDX],
                (unsigned long long)uc->uc_mcontext.gregs[REG_RSI],
                (unsigned long long)uc->uc_mcontext.gregs[REG_RDI],
                (unsigned long long)uc->uc_mcontext.gregs[REG_R8],
                (unsigned long long)uc->uc_mcontext.gregs[REG_R9],
                (unsigned long long)uc->uc_mcontext.gregs[REG_R10],
                (unsigned long long)uc->uc_mcontext.gregs[REG_R11],
                (unsigned long long)uc->uc_mcontext.gregs[REG_R12]);
        fflush(stdout);
    }
    {
        Dl_info dli{};
        if (dladdr((void*)rip, &dli)) {
            if (dli.dli_sname) {
                fprintf(stdout, "[CRASH   ] faulting symbol: %s +%ld\n",
                        dli.dli_sname, (long)((char*)rip - (char*)dli.dli_saddr));
            }
            if (dli.dli_fbase) {
                fprintf(stdout, "[CRASH   ] module offset: +%lX\n",
                        (unsigned long)((char*)rip - (char*)dli.dli_fbase));
            }
        }
        if (ret_at_rsp && dladdr((void*)ret_at_rsp, &dli) && dli.dli_sname) {
            fprintf(stdout, "[CRASH   ] caller symbol: %s +%ld\n",
                    dli.dli_sname,
                    (long)((char*)ret_at_rsp - (char*)dli.dli_saddr));
        }
        fflush(stdout);
    }
    {
        uintptr_t gbase = (uintptr_t)pr_guest_base_for_crash_c();
        if (gbase && info && (uintptr_t)info->si_addr >= gbase &&
            (uintptr_t)info->si_addr < gbase + 0x100000000ull) {
            fprintf(stdout, "[CRASH   ] guest fault address: %08X\n",
                    (uint32_t)((uintptr_t)info->si_addr - gbase));
            fflush(stdout);
        }
    }
    fflush(stdout);
    backtrace_symbols_fd(bt, n, STDOUT_FILENO);
    fflush(stdout);
    _exit(128 + sig);
}

namespace pr {

bool g_log_enabled[14] = {
    true,  // Error
    true,  // Warn
    true,  // Import
    true,  // Thread
    true,  // Memory
    true,  // Filesystem
    true,  // GPU
    true,  // Input
    true,  // Exception
    false, // Trace (verbose; enable with PR_LOG_TRACE=1)
    true,  // Object
    true,  // Sync
    true,  // Loader
};

static std::mutex g_log_mutex;

static const char* kCategoryTags[] = {
    "ERROR ", "WARN  ", "IMPORT", "THREAD", "MEMORY", "FILESY", "GPU   ",
    "INPUT ", "EXCEPT", "TRACE ", "OBJECT", "SYNC  ", "LOADER",
};

static bool g_crash_handler_installed = false;

void LogInit() {
    if (!g_crash_handler_installed) {
        g_crash_handler_installed = true;
        static stack_t altstack{};
        altstack.ss_sp = g_altstack;
        altstack.ss_size = sizeof(g_altstack);
        altstack.ss_flags = 0;
        sigaltstack(&altstack, nullptr);
        struct sigaction sa {};
        sa.sa_sigaction = CrashHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGBUS, &sa, nullptr);
        sigaction(SIGABRT, &sa, nullptr);
        sigaction(SIGFPE, &sa, nullptr);
        sigaction(SIGILL, &sa, nullptr);
    }
    if (getenv("PR_LOG_TRACE")) g_log_enabled[(size_t)LogCategory::kTrace] = true;
    if (getenv("PR_QUIET")) {
        for (auto& e : g_log_enabled) e = false;
        g_log_enabled[(size_t)LogCategory::kError] = true;
    }
}

static double NowSec() {
    static auto t0 = timespec{};
    static bool init = false;
    if (!init) { clock_gettime(CLOCK_MONOTONIC, &t0); init = true; }
    auto t = timespec{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec - t0.tv_sec) + double(t.tv_nsec - t0.tv_nsec) * 1e-9;
}

void LogLine(LogCategory cat, const char* fmt, ...) {
    if (!g_log_enabled[(size_t)cat]) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_log_mutex);
    fprintf(stdout, "[%9.3f|%s] %s\n", NowSec(), kCategoryTags[(size_t)cat], buf);
    fflush(stdout);
}

void LogLineOnce(LogCategory cat, const char* fmt, ...) {
    if (!g_log_enabled[(size_t)cat]) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    static std::mutex once_mutex;
    static std::set<std::string> seen;
    static std::unordered_map<std::string, uint64_t> counts;
    std::lock_guard<std::mutex> lk(once_mutex);
    uint64_t n = ++counts[buf];
    if (seen.insert(buf).second) {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        fprintf(stdout, "[%9.3f|%s] %s\n", NowSec(),
                kCategoryTags[(size_t)cat], buf);
        fflush(stdout);
    } else if (n == 10000 || n == 1000000) {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        fprintf(stdout, "[%9.3f|%s] (x%llu suppressed) %s\n", NowSec(),
                kCategoryTags[(size_t)cat], (unsigned long long)n, buf);
        fflush(stdout);
    }
}

void LogFatal(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        fprintf(stdout, "[%9.3f|FATAL ] %s\n", NowSec(), buf);
        fflush(stdout);
    }
    abort();
}

}  // namespace pr
