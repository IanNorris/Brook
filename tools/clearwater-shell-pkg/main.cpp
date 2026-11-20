#include "chrome_shell_model.h"
#include "chrome_shell_wire.h"
#include "session_services.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
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

static pid_t StartSessionServices()
{
    const auto parent = getpid();
    const pid_t pid = fork();
    if (pid < 0) { std::perror("clearwater-shell fork"); return pid; }
    if (pid > 0) {
        std::fprintf(stderr, "CLEARWATER_TRAY_STARTED pid=%ld\n", static_cast<long>(pid));
        return pid;
    }
    if (setgid(100) || setuid(1000)) {
        std::perror("clearwater session service credentials");
        _exit(1);
    }
    const auto publish = [](const ChromeTrayState& state) {
        return Exchange(ShellServiceOp::TrayPublish, nullptr, const_cast<ChromeTrayState*>(&state), sizeof(state)) == 0;
    };
    const auto input = [](TrayInput& event) {
        if (!Exchange(ShellServiceOp::TrayNextInput, nullptr, &event, sizeof(event))) return 1;
        return errno == EAGAIN || errno == EINTR ? 0 : -1;
    };
    _exit(RunSessionServices(publish, input, parent));
}

// Interrupt a blocked Receive. The timer also covers exits between waitpid
// and Receive, and lets a failed worker restart even on an idle desktop.
static void WakeForSessionServices(int) {}

static void InstallSessionServiceWakeups()
{
    struct sigaction act{};
    act.sa_handler = WakeForSessionServices;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &act, nullptr)) std::perror("clearwater-shell SIGCHLD");
    act.sa_flags = 0;
    if (sigaction(SIGALRM, &act, nullptr)) {
        std::perror("clearwater-shell SIGALRM");
        return;
    }
    const itimerval timer{{1, 0}, {1, 0}};
    if (setitimer(ITIMER_REAL, &timer, nullptr)) std::perror("clearwater-shell setitimer");
}

struct SessionServiceChild {
    using Clock = std::chrono::steady_clock;
    pid_t pid = -1;
    Clock::time_point started{}, retry{};
    unsigned delay = 1;

    void BackOff(Clock::time_point now)
    {
        retry = now + std::chrono::seconds(delay);
        if (delay < 8) delay *= 2;
    }

    void Poll()
    {
        const auto now = Clock::now();
        if (pid > 0) {
            const pid_t exited = waitpid(pid, nullptr, WNOHANG);
            if (exited != pid && !(exited < 0 && errno == ECHILD)) return;
            pid = -1;
            if (now - started >= std::chrono::seconds(10)) delay = 1;
            BackOff(now);
        }
        if (now < retry) return;
        pid = StartSessionServices();
        if (pid > 0) started = now;
        else BackOff(now);
    }
};

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
    InstallSessionServiceWakeups();
    SessionServiceChild tray;
    bool ready = false;
    for (;;) {
        tray.Poll();
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
