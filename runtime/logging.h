// PrototypeRecomp Phase 2B runtime — categorized logging.
#pragma once

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace pr {

enum class LogCategory {
    kError, kWarn, kImport, kThread, kMemory, kFilesystem, kGpu,
    kInput, kException, kTrace, kObject, kSync, kLoader,
};

// Category enable flags (TRACE off by default; toggle with PR_LOG_TRACE=1).
extern bool g_log_enabled[14];
void LogInit();

void LogLine(LogCategory cat, const char* fmt, ...);
// Deduplicate identical (category, message) warnings: each unique message
// logs once per process (repeats counted). Prevents multi-GB spin logs.
void LogLineOnce(LogCategory cat, const char* fmt, ...);

#define PRLOG(cat, ...) ::pr::LogLine(::pr::LogCategory::k##cat, __VA_ARGS__)
#define PRLOGI(...) ::pr::LogLine(::pr::LogCategory::kImport, __VA_ARGS__)
#define PRLOGE(...) ::pr::LogLine(::pr::LogCategory::kError, __VA_ARGS__)
#define PRLOGW(...) ::pr::LogLine(::pr::LogCategory::kWarn, __VA_ARGS__)
#define PRLOGONCE(cat, ...) ::pr::LogLineOnce(::pr::LogCategory::k##cat, __VA_ARGS__)

// Fatal: log then abort (used for KeBugCheck etc).
[[noreturn]] void LogFatal(const char* fmt, ...);

}  // namespace pr
