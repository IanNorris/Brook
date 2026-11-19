#include "chrome_shell_model.h"
#include "chrome_shell_wire.h"

#include <cerrno>
#include <cstdio>
#include <unistd.h>

using namespace brook;

static ShellSnapshot snapshot;
static ChromeShellStorage storage;
static ShellWireList submission;
static ChromeMaterialCache material;

static long Exchange(ShellServiceOp op, ShellExchange* exchange, void* data, size_t bytes)
{
    return syscall(ShellServiceSyscall, uint64_t(op), exchange, data, bytes, 0, 0);
}

int main()
{
    for (unsigned retries = 0; ; ++retries) {
        if (!Exchange(ShellServiceOp::Register, nullptr, nullptr, ShellServiceVersion)) break;
        if (errno != EBUSY || retries == 50) {
            std::perror("clearwater-shell register");
            return 1;
        }
        usleep(100000);
    }
    bool ready = false;
    for (;;) {
        ShellExchange offer{};
        if (Exchange(ShellServiceOp::Receive, &offer, &snapshot, sizeof(snapshot))) {
            if (errno == EINTR) continue;
            std::perror("clearwater-shell receive");
            return 1;
        }
        const auto scale = snapshot.config.metrics.percent;
        const auto& fonts = scale == 200 ? g_chromeFonts200 : scale == 150 ? g_chromeFonts150 : g_chromeFonts100;
        auto list = ChromeListOn(storage);
        BuildShellModel(snapshot.model, snapshot.config.Style(fonts, material), list);
        if (!PackShellList(list, submission)) {
            std::fputs("clearwater-shell display list exceeded its budget\n", stderr);
            return 1;
        }
        if (Exchange(ShellServiceOp::Present, &offer, &submission, sizeof(submission))) {
            if (errno == ESTALE) continue;
            std::perror("clearwater-shell present");
            return 1;
        }
        if (!ready && snapshot.model.kind == ChromeShellKind::Taskbar) {
            ready = true;
            std::fprintf(stderr, "CLEARWATER_SHELL_READY pid=%ld\n", static_cast<long>(getpid()));
        }
    }
}
