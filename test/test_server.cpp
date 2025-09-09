#include "shmx_common.h"
#include "shmx_server.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace shmx;

static std::atomic<bool> g_run{true};
#if defined(_WIN32)
BOOL WINAPI console_handler(DWORD) {
    g_run = false;
    return TRUE;
}
#else
void sigint_handler(int) {
    g_run = false;
}
#endif

namespace {
    constexpr std::uint32_t CTRL_HELLO     = 0x48454C4Fu;
    constexpr std::uint32_t CTRL_HEARTBEAT = 0x48425254u;
    constexpr std::uint32_t CTRL_BYE       = 0x4259455Fu;
    struct HelloMsg {
        std::uint32_t ver_major;
        std::uint32_t ver_minor;
    };
} // namespace

static std::uint64_t now_ticks() {
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

int main(int argc, char** argv) {
    std::string name = (argc >= 2) ? argv[1] : std::string("shmx_demo");
#if defined(_WIN32)
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, sigint_handler);
    std::signal(SIGTERM, sigint_handler);
#endif

    Server::Config cfg{.name = name, .slots = 4u, .reader_slots = 16u, .static_bytes_cap = 4096u, .frame_bytes_cap = 65536u, .control_per_reader = 4096u};

    std::vector<StaticStream> streams;
    streams.push_back(StaticStream{.stream_id = 42u, .element_type = DT_U64, .components = 1u, .layout = LAYOUT_SOA_SCALAR, .bytes_per_elem = static_cast<std::uint32_t>(sizeof(std::uint64_t)), .name_utf8 = "tick_seq", .extra = {}});
    streams.push_back(StaticStream{.stream_id = 43u, .element_type = DT_F64, .components = 1u, .layout = LAYOUT_SOA_SCALAR, .bytes_per_elem = static_cast<std::uint32_t>(sizeof(double)), .name_utf8 = "tick_sim", .extra = {}});

    Server srv;
    if (!srv.create(cfg, streams)) throw std::runtime_error("server create failed");

    std::printf("[server] up name %s session %llu\n", name.c_str(), static_cast<unsigned long long>(srv.header()->session_id));

    auto t0           = std::chrono::steady_clock::now();
    std::uint64_t seq = 0, last_print = 0, frames_in_sec = 0;
    std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> last_seen;
    std::unordered_set<std::uint64_t> connected_now;
    const auto timeout = std::chrono::seconds(3);

    while (g_run.load()) {
        auto fm          = srv.begin_frame();
        const double sim = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        bool ok          = Server::append_stream(fm, 42u, &seq, 1u, static_cast<std::uint32_t>(sizeof(seq)));
        if (ok) ok = Server::append_stream(fm, 43u, &sim, 1u, static_cast<std::uint32_t>(sizeof(sim)));
        if (ok) {
            (void) srv.publish_frame(fm, sim);
            ++seq;
            ++frames_in_sec;
        }

        std::vector<Server::ControlMsg> msgs;
        if (srv.poll_control(msgs, 256u)) {
            for (const auto& [reader_id, type, data] : msgs) {
                const auto now = std::chrono::steady_clock::now();
                if (type == CTRL_HELLO && data.size() == sizeof(HelloMsg)) {
                    HelloMsg hello{};
                    std::memcpy(&hello, data.data(), sizeof(hello));
                    last_seen[reader_id] = now;
                    if (connected_now.insert(reader_id).second) {
                        std::printf("[server] reader %llu hello %u.%u\n", static_cast<unsigned long long>(reader_id), hello.ver_major, hello.ver_minor);
                    }
                } else if (type == CTRL_HEARTBEAT && data.size() == sizeof(std::uint64_t)) {
                    last_seen[reader_id] = now;
                } else if (type == CTRL_BYE) {
                    if (connected_now.erase(reader_id) > 0) {
                        last_seen.erase(reader_id);
                        std::printf("[server] reader %llu bye\n", static_cast<unsigned long long>(reader_id));
                    }
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        for (auto it = connected_now.begin(); it != connected_now.end();) {
            auto id      = *it;
            auto jt      = last_seen.find(id);
            bool expired = (jt == last_seen.end()) || (now - jt->second > timeout);
            if (expired) {
                std::printf("[server] reader %llu lost\n", static_cast<unsigned long long>(id));
                last_seen.erase(id);
                it = connected_now.erase(it);
            } else {
                ++it;
            }
        }

        (void) srv.reap_stale_readers(now_ticks(), static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count()));

        auto sec = std::chrono::duration_cast<std::chrono::seconds>(now - t0).count();
        if (static_cast<std::uint64_t>(sec) != last_print) {
            last_print         = static_cast<std::uint64_t>(sec);
            auto readers       = srv.snapshot_readers();
            std::size_t in_use = 0, registered = 0;
            for (const auto& r : readers) {
                if (r.in_use) {
                    ++in_use;
                    if (r.reader_id != 0) ++registered;
                }
            }
            std::printf("[server] sec %llu pub %llu total %llu in_use %zu registered %zu hdr_count %u active %zu\n", static_cast<unsigned long long>(last_print), static_cast<unsigned long long>(frames_in_sec), static_cast<unsigned long long>(seq), in_use, registered, srv.readers_connected(), connected_now.size());
            frames_in_sec = 0;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    std::printf("[server] shutdown\n");
    srv.destroy();
    return 0;
}
