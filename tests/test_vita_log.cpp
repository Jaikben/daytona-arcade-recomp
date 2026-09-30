#include "../platform/vita/diagnostic_log.h"
#include <cassert>
#include <cstring>
#include <string>

namespace {
std::string disk;
int opens, closes, syncs, write_calls, flags_seen;
int open_result, write_result, sync_result, close_result;
void reset() {
    disk.clear(); opens = closes = syncs = write_calls = flags_seen = 0;
    open_result = 5; write_result = -999; sync_result = close_result = 0;
}
}
extern "C" {
SceUID sceIoOpen(const char *path, int flags, SceMode) {
    assert(std::strcmp(path, vita::DiagnosticLog::kPath) == 0);
    ++opens; flags_seen = flags;
    if (open_result >= 0 && (flags & SCE_O_TRUNC)) disk.clear();
    return open_result;
}
SceSSize sceIoWrite(SceUID fd, const void *data, SceSize size) {
    assert(fd == 5); ++write_calls;
    const int n = write_result == -999 ? int(size) : write_result;
    if (n > 0 && unsigned(n) <= size) disk.append(static_cast<const char *>(data), size_t(n));
    return n;
}
int sceIoSyncByFd(SceUID fd, int flag) { assert(fd == 5 && flag == 0); ++syncs; return sync_result; }
int sceIoClose(SceUID fd) { assert(fd == 5); ++closes; return close_result; }
}
int main() {
    reset(); vita::DiagnosticLog log;
    disk = "old";
    assert(log.begin() && disk.find("OPT03") != std::string::npos && disk.find("old") == std::string::npos);
    assert((flags_seen & SCE_O_TRUNC) && opens == 1 && closes == 1 && syncs == 1);
    assert(log.literal("stage: alive\n") && (flags_seen & SCE_O_APPEND));
    assert(disk.ends_with("stage: alive\n"));
    assert(log.log("number=%d %s\n", 37, "OK") && disk.ends_with("number=37 OK\n"));
    assert(log.bytes() == disk.size() && std::strcmp(log.state(), "OK") == 0);

    reset(); vita::DiagnosticLog partial; write_result = 3;
    assert(partial.append("abcdef", 6));
    assert(disk == "abcdef" && write_calls == 2 && closes == 1);

    reset(); vita::DiagnosticLog zero; write_result = 0;
    assert(!zero.literal("x") && zero.error() == -1 && closes == 1 && write_calls == 1);
    reset(); vita::DiagnosticLog negative; write_result = -22;
    assert(!negative.literal("x") && negative.error() == -22 && closes == 1);
    write_result = 0; assert(!negative.literal("x") && negative.error() == -22);

    reset(); vita::DiagnosticLog open_fail; open_result = -13;
    assert(!open_fail.begin() && open_fail.error() == -13 && closes == 0 && syncs == 0);
    assert(std::strcmp(open_fail.state(), "ERROR") == 0);

    reset(); vita::DiagnosticLog sync_fail; sync_result = -38;
    assert(sync_fail.literal("saved\n") && sync_fail.error() == 0 && sync_fail.sync_error() == -38);
    assert(disk == "saved\n" && closes == 1 && std::strcmp(sync_fail.state(), "SYNC WARNING") == 0);

    reset(); vita::DiagnosticLog close_fail; close_result = -5;
    assert(!close_fail.literal("x") && close_fail.error() == -5 && closes == 1);

    reset(); vita::DiagnosticLog truncated;
    const std::string huge(2048, 'X');
    assert(truncated.log("%s", huge.c_str()));
    assert(disk.size() == 1023 && disk.ends_with(" [truncated]\n"));

    reset(); vita::DiagnosticLog bounded;
    const std::string full(vita::DiagnosticLog::kLimit, 'Y');
    assert(bounded.append(full.data(), full.size()));
    assert(!bounded.literal("more") && bounded.limited() && opens == 1);
    assert(disk.size() == vita::DiagnosticLog::kLimit && std::strcmp(bounded.state(), "LIMIT") == 0);
    assert(bounded.begin() && !bounded.limited() && bounded.error() == 0);

    reset(); vita::DiagnosticLog invalid;
    assert(!invalid.append(nullptr, 2) && invalid.error() == -1 && opens == 0);
    reset(); vita::DiagnosticLog overreported; write_result = 2;
    assert(!overreported.literal("x") && overreported.error() == -1 && closes == 1);
    std::puts("Vita direct-log tests passed (header, append, partial/zero/error writes, open/sync/close failures, bounds)");
}
