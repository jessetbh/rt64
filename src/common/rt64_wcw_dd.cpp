//
// RT64
//

#include "rt64_wcw_dd.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#   include <winsock2.h>
#   include <ws2tcpip.h>
#   include <windows.h>
#   include <psapi.h>
#   include <dxgi1_4.h>
#else
#   include <arpa/inet.h>
#   include <netinet/in.h>
#   include <sys/socket.h>
#   include <unistd.h>
#endif

#include "common/rt64_plume.h"

namespace RT64::WcwDd {
    namespace {
        double nowMs() {
            return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
        }

        struct Sink {
            std::mutex mutex;
            WcwLogic::DdFrameWindow window;
            WcwLogic::DdSession session;
            std::string game;
            std::string aggregationKey;
            bool echo = false;
            uint16_t port = 8125;

            std::mutex socketMutex;
#       if defined(_WIN32)
            SOCKET socket = INVALID_SOCKET;
            std::vector<IDXGIAdapter3 *> adapters;
#       else
            int socket = -1;
#       endif

            std::once_flag startOnce;
            std::thread thread;
            std::mutex stopMutex;
            std::condition_variable stopCondition;
            bool stop = false;
            bool deviceLostSent = false;
        };

        // Never destroyed: the renderer threads may still note presents while the process exits. atexit stops the
        // sender thread and sends the last window.
        Sink &sink() {
            static Sink *s = new Sink();
            return *s;
        }

        std::string exeName() {
#       if defined(_WIN32)
            char path[MAX_PATH] = {};
            const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
            return std::string(path, n);
#       else
            char path[4096] = {};
            const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
            return (n > 0) ? std::string(path, size_t(n)) : std::string("unknown");
#       endif
        }

        unsigned long processId() {
#       if defined(_WIN32)
            return GetCurrentProcessId();
#       else
            return (unsigned long)getpid();
#       endif
        }

        void openSocket(Sink &s) {
#       if defined(_WIN32)
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
                s.socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            }
#       else
            s.socket = ::socket(AF_INET, SOCK_DGRAM, 0);
#       endif
        }

        // Not connect()ed: on Windows a connected UDP socket fails later sends after an ICMP port unreachable (no
        // Agent listening), as ddmetrics.ps1 notes.
        void sendLines(Sink &s, const std::vector<std::string> &lines) {
            const std::vector<std::string> packets = WcwLogic::ddPackets(lines);
            std::scoped_lock lock(s.socketMutex);
            sockaddr_in to = {};
            to.sin_family = AF_INET;
            to.sin_port = htons(s.port);
            to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            for (const std::string &p : packets) {
                if (s.echo) {
                    fprintf(stderr, "[wcw dd] %s\n", p.c_str());
                }

#           if defined(_WIN32)
                if (s.socket != INVALID_SOCKET) {
                    sendto(s.socket, p.data(), int(p.size()), 0, reinterpret_cast<const sockaddr *>(&to), sizeof(to));
                }
#           else
                if (s.socket >= 0) {
                    sendto(s.socket, p.data(), p.size(), 0, reinterpret_cast<const sockaddr *>(&to), sizeof(to));
                }
#           endif
            }
        }

        void openAdapters(Sink &s) {
#       if defined(_WIN32)
            IDXGIFactory1 *factory = nullptr;
            if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) {
                return;
            }

            IDXGIAdapter1 *adapter = nullptr;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
                DXGI_ADAPTER_DESC1 desc = {};
                IDXGIAdapter3 *adapter3 = nullptr;
                if (SUCCEEDED(adapter->GetDesc1(&desc)) && ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) &&
                    SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void **>(&adapter3))))
                {
                    s.adapters.push_back(adapter3);
                }

                adapter->Release();
            }

            factory->Release();
#       endif
        }

        WcwLogic::DdProcessSample sampleProcess(Sink &s) {
            WcwLogic::DdProcessSample p;
#       if defined(_WIN32)
            PROCESS_MEMORY_COUNTERS_EX mem = {};
            mem.cb = sizeof(mem);
            DWORD handles = 0;
            if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&mem), sizeof(mem)) &&
                GetProcessHandleCount(GetCurrentProcess(), &handles))
            {
                p.has = true;
                p.privateMb = double(mem.PrivateUsage) / (1024.0 * 1024.0);
                p.workingSetMb = double(mem.WorkingSetSize) / (1024.0 * 1024.0);
                p.handles = uint32_t(handles);
            }

            // This process's local (dedicated) memory on every adapter (WcwLogic::ddPickVram).
            std::vector<WcwLogic::DdVramAdapter> vram;
            for (IDXGIAdapter3 *adapter : s.adapters) {
                DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
                if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                    vram.push_back({ info.CurrentUsage, info.Budget });
                }
            }

            if (!vram.empty()) {
                const WcwLogic::DdVramAdapter v = WcwLogic::ddPickVram(vram);
                p.hasVram = true;
                p.vramMb = double(v.usage) / (1024.0 * 1024.0);
                p.vramBudgetMb = double(v.budget) / (1024.0 * 1024.0);
            }
#       else
            (void)s;
#       endif
            return p;
        }

        std::string eventLine(Sink &s, const std::string &title, const std::string &text, const char *alertType) {
            // The caller holds s.mutex.
            return WcwLogic::ddEventLine("aki-game " + s.game + ": " + title, text, s.session.tags(), (alertType != nullptr) ? alertType : "info", s.aggregationKey);
        }

        // withProcess: false for the last window at exit, when the renderer is already torn down.
        void flushWindow(Sink &s, bool withProcess) {
            WcwLogic::DdFrameStats stats;
            std::vector<std::string> tags;
            {
                std::scoped_lock lock(s.mutex);
                stats = s.window.take(nowMs(), s.session.context().fpsTarget);
                tags = s.session.tags();
            }

            const WcwLogic::DdProcessSample process = withProcess ? sampleProcess(s) : WcwLogic::DdProcessSample();
            std::vector<std::string> lines;
            for (const auto &m : WcwLogic::ddWindowMetrics(stats, process)) {
                lines.push_back(WcwLogic::ddGaugeLine(m.first, m.second, tags));
            }

            sendLines(s, lines);
        }

        void threadLoop() {
            Sink &s = sink();
            openAdapters(s);
            double windowStart = nowMs();
            std::unique_lock<std::mutex> stopLock(s.stopMutex);
            while (!s.stop) {
                // Wakes every second for the device-lost check; the window goes out every DdWindowSeconds, also when
                // nothing presents (a hang reads as 0 presents/s instead of a gap).
                s.stopCondition.wait_for(stopLock, std::chrono::seconds(1));
                if (s.stop) {
                    break;
                }

                stopLock.unlock();
                if (plume::wcwDeviceLostFlag().load() && !s.deviceLostSent) {
                    s.deviceLostSent = true;
                    event("device lost", "The GPU device was lost (hang, removal or reset); the game is quitting.", "error");
                }

                if (WcwLogic::ddWindowDue(nowMs(), windowStart)) {
                    windowStart = nowMs();
                    flushWindow(s, true);
                }

                stopLock.lock();
            }
        }

        void stopAtExit() {
            Sink &s = sink();
            {
                std::scoped_lock lock(s.stopMutex);
                s.stop = true;
            }

            s.stopCondition.notify_all();
            if (s.thread.joinable()) {
                s.thread.join();
            }

            flushWindow(s, false);
            event("session end", "The game exited.", "info");
        }

        void start() {
            Sink &s = sink();
            std::call_once(s.startOnce, [&s]() {
                const char *game = getenv("WCW_DD_GAME");
                s.game = ((game != nullptr) && (game[0] != '\0')) ? std::string(game) : WcwLogic::ddGameFromExe(exeName());

                s.echo = WcwLogic::envSwitchOn(getenv("WCW_DD_ECHO"));
                const char *port = getenv("WCW_DD_PORT");
                const int p = (port != nullptr) ? atoi(port) : 0;
                s.port = ((p > 0) && (p < 65536)) ? uint16_t(p) : uint16_t(8125);
                s.aggregationKey = WcwLogic::ddTag("k", "aki-game-" + s.game + "-" + std::to_string(processId())).substr(2);
                {
                    std::scoped_lock lock(s.mutex);
                    s.session.setIdentity(s.game, WcwLogic::ddParseTags(getenv("WCW_DD_TAGS")));
                    s.window.start(nowMs());
                }

                openSocket(s);
                fprintf(stderr, "[wcw dd] WCW_DD_METRICS: sending aki.game.* to 127.0.0.1:%u every %.0f s (game %s)\n", unsigned(s.port),
                    WcwLogic::DdWindowSeconds, s.game.c_str());
                s.thread = std::thread(threadLoop);
                atexit(stopAtExit);
            });
        }
    };

    bool enabled() {
        static const bool on = WcwLogic::envSwitchOn(getenv("WCW_DD_METRICS"));
        return on;
    }

    void notePresent(Clock::time_point t, bool generated, bool repeated) {
        if (!enabled()) {
            return;
        }

        start();
        Sink &s = sink();
        std::scoped_lock lock(s.mutex);
        s.window.addPresent(std::chrono::duration<double, std::milli>(t.time_since_epoch()).count(), generated, repeated);
    }

    void noteGpuRow(const WcwPerf::GpuRow &row) {
        if (!enabled()) {
            return;
        }

        start();
        Sink &s = sink();
        std::scoped_lock lock(s.mutex);
        if (row.present) {
            s.window.addGpuPresent(row.gpuMs);
        }
        else {
            s.window.addGpuRender(row.presentId, row.gpuMs, row.passMs[size_t(WcwPerf::GpuPass::RtBuild)], row.passMs[size_t(WcwPerf::GpuPass::RtTrace)]);
        }
    }

    void noteContext(const WcwLogic::DdContext &context) {
        if (!enabled()) {
            return;
        }

        start();
        Sink &s = sink();
        std::string line;
        {
            std::scoped_lock lock(s.mutex);
            const WcwLogic::DdSession::Decision d = s.session.noteContext(context, nowMs());
            if (d.event == WcwLogic::DdSession::Event::SessionStart) {
                line = eventLine(s, "session start", "WCW_DD_METRICS play session, pid " + std::to_string(processId()) + ".", "info");
            }
            else if (d.event == WcwLogic::DdSession::Event::SettingsChanged) {
                line = eventLine(s, "settings changed", d.changes, "info");
            }
        }

        if (!line.empty()) {
            sendLines(s, { line });
        }
    }

    void noteVenue(const std::string &venue) {
        if (!enabled()) {
            return;
        }

        start();
        Sink &s = sink();
        std::string line;
        {
            std::scoped_lock lock(s.mutex);
            const WcwLogic::DdSession::Decision d = s.session.noteVenue(venue);
            if (d.event == WcwLogic::DdSession::Event::VenueChanged) {
                line = eventLine(s, "venue " + (venue.empty() ? std::string("(none)") : venue), d.changes, "info");
            }
        }

        if (!line.empty()) {
            sendLines(s, { line });
        }
    }

    void event(const std::string &title, const std::string &text, const char *alertType) {
        if (!enabled()) {
            return;
        }

        start();
        Sink &s = sink();
        std::string line;
        {
            std::scoped_lock lock(s.mutex);
            line = eventLine(s, title, text, alertType);
        }

        sendLines(s, { line });
    }
};
