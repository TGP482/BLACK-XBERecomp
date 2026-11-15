// The machine at a run's start: the SDK's system_info.cpp reads the Windows registry and WMI, this asks sysctl the
// same questions so a log from a mac run says what it ran on.
#include "app/app.h"

#include <sys/sysctl.h>
#include <sys/utsname.h>

#include <cstdint>
#include <ctime>

#include "core/log.h"

namespace app {

void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    LOGR("%ls, %s", kGame.title, date);
    LOGR("Path: %ls", ExeDir().c_str());

    utsname u{};
    if (uname(&u) == 0) LOGR("OS: %s %s (%s)", u.sysname, u.release, u.machine);

    char cpu[128] = {};
    size_t n = sizeof(cpu);
    if (sysctlbyname("machdep.cpu.brand_string", cpu, &n, nullptr, 0) != 0) cpu[0] = '\0';
    int threads = 0;
    n = sizeof(threads);
    sysctlbyname("hw.ncpu", &threads, &n, nullptr, 0);
    LOGR("CPU: %s (%d threads)", cpu, threads);

    uint64_t memory = 0;
    n = sizeof(memory);
    sysctlbyname("hw.memsize", &memory, &n, nullptr, 0);
    LOGR("RAM: %.1f GB", double(memory) / double(1ull << 30));
}

}  // namespace app
