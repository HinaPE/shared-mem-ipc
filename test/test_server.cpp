#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include "shmx_common.h"
#include "shmx_server.h"

using namespace shmx;

static std::atomic<bool> g_run{true};
#if defined(_WIN32)
BOOL WINAPI console_handler(DWORD) { g_run = false; return TRUE; }
#else
void sigint_handler(int) { g_run = false; }
#endif

namespace {
constexpr std::uint32_t CTRL_HELLO = 0x48454C4F;
constexpr std::uint32_t CTRL_HEARTBEAT = 0x48425254;
constexpr std::uint32_t CTRL_BYE = 0x4259455F;
struct HelloMsg { std::uint32_t ver_major; std::uint32_t ver_minor; };
}

struct TickPayload { std::uint64_t seq; double sim; };

int main(int argc, char** argv) {
    std::string name = (argc >= 2) ? argv[1] : std::string("shmx_demo");

#if defined(_WIN32)
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, sigint_handler);
    std::signal(SIGTERM, sigint_handler);
#endif

    Server::Config cfg{};
    cfg.name = name;
    cfg.slots = 4u;
    cfg.reader_slots = 16u;
    cfg.static_bytes_cap = 4096u;
    cfg.frame_bytes_cap = 65536u;
    cfg.control_per_reader = 4096u;

    std::vector<StaticStream> streams;
    streams.push_back({42u, DT_U64, 1u, LAYOUT_SOA_SCALAR, static_cast<std::uint32_t>(sizeof(std::uint64_t)), "tick_seq", {}});
    streams.push_back({43u, DT_F64, 1u, LAYOUT_SOA_SCALAR, static_cast<std::uint32_t>(sizeof(double)), "tick_sim", {}});

    Server srv;
    if (!srv.create(cfg, streams)) {
        std::fprintf(stderr, "[server] create failed name %s\n", name.c_str());
        return 1;
    }
    std::printf("[server] up name %s session %llu\n", name.c_str(), static_cast<unsigned long long>(srv.header()->session_id));

    auto t0 = std::chrono::steady_clock::now();
    std::uint64_t seq = 0;
    std::uint64_t last_print = 0;
    std::uint64_t frames_in_sec = 0;

    std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> last_seen;
    std::unordered_set<std::uint64_t> connected_now;

    const auto timeout = std::chrono::seconds(3);

    while (g_run.load()) {
        auto fm = srv.begin_frame();
        const double sim = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        TickPayload payload{seq + 1u, sim};
        bool ok = Server::append_stream(fm, 42u, &payload.seq, 1u, static_cast<std::uint32_t>(sizeof(payload.seq)));
        if (ok) ok = Server::append_stream(fm, 43u, &payload.sim, 1u, static_cast<std::uint32_t>(sizeof(payload.sim)));
        if (ok) { srv.publish_frame(fm, sim); ++seq; ++frames_in_sec; }

        std::vector<Server::ControlMsg> msgs;
        if (srv.poll_control(msgs, 256)) {
            for (const auto& m : msgs) {
                const auto now = std::chrono::steady_clock::now();
                if (m.type == CTRL_HELLO && m.bytes == sizeof(HelloMsg)) {
                    HelloMsg hello{};
                    std::memcpy(&hello, m.data, sizeof(hello));
                    last_seen[m.reader_id] = now;
                    if (connected_now.insert(m.reader_id).second) {
                        std::printf("[server] reader %llu hello %u.%u\n",
                                    static_cast<unsigned long long>(m.reader_id),
                                    hello.ver_major, hello.ver_minor);
                    }
                } else if (m.type == CTRL_HEARTBEAT && m.bytes == sizeof(std::uint64_t)) {
                    last_seen[m.reader_id] = now;
                } else if (m.type == CTRL_BYE) {
                    if (connected_now.erase(m.reader_id) > 0) {
                        last_seen.erase(m.reader_id);
                        std::printf("[server] reader %llu bye\n", static_cast<unsigned long long>(m.reader_id));
                    }
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        for (auto it = connected_now.begin(); it != connected_now.end(); ) {
            auto id = *it;
            auto jt = last_seen.find(id);
            bool expired = false;
            if (jt == last_seen.end()) {
                expired = true;
            } else {
                if (now - jt->second > timeout) expired = true;
            }
            if (expired) {
                std::printf("[server] reader %llu lost\n", static_cast<unsigned long long>(id));
                last_seen.erase(id);
                it = connected_now.erase(it);
            } else {
                ++it;
            }
        }

        auto sec = std::chrono::duration_cast<std::chrono::seconds>(now - t0).count();
        if (static_cast<std::uint64_t>(sec) != last_print) {
            last_print = static_cast<std::uint64_t>(sec);
            auto readers = srv.snapshot_readers();
            std::size_t in_use = 0;
            std::size_t registered = 0;
            for (const auto& r : readers) {
                if (r.in_use) { ++in_use; if (r.reader_id != 0) ++registered; }
            }
            std::printf("[server] sec %llu pub %llu total %llu in_use %zu registered %zu hdr_count %u active %zu\n",
                        static_cast<unsigned long long>(last_print),
                        static_cast<unsigned long long>(frames_in_sec),
                        static_cast<unsigned long long>(seq),
                        in_use, registered, srv.readers_connected(), connected_now.size());
            frames_in_sec = 0;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    std::printf("[server] shutdown\n");
    srv.destroy();
    return 0;
}
