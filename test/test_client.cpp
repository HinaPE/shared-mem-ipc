#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include "shmx_common.h"
#include "shmx_client.h"

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

struct LastSeen {
    std::uint64_t frame_id{0};
    std::chrono::steady_clock::time_point time{};
};

static void send_bye_best_effort(Client& cli) {
    for (int i = 0; i < 3; ++i) {
        if (cli.control_send(CTRL_BYE, nullptr, 0u)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int main(int argc, char** argv) {
    std::string name = (argc >= 2) ? argv[1] : std::string("shmx_demo");

#if defined(_WIN32)
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, sigint_handler);
    std::signal(SIGTERM, sigint_handler);
#endif

    Client cli;
    bool connected = false;
    std::uint64_t last_session = 0;
    LastSeen seen{};
    auto t0 = std::chrono::steady_clock::now();
    std::uint64_t recv_in_sec = 0;
    std::uint64_t last_print = 0;
    auto last_hb = std::chrono::steady_clock::now();

    auto try_open = [&](const char* reason) {
        if (connected) return;
        if (cli.open(name)) {
            auto* H = cli.header();
            if (H) {
                connected = true;
                last_session = H->session_id;
                seen = {}; seen.time = std::chrono::steady_clock::now();
                std::printf("[client] connected name %s session %llu reason %s\n",
                            name.c_str(),
                            static_cast<unsigned long long>(last_session),
                            reason);
                HelloMsg hello{VER_MAJOR, VER_MINOR};
                if (cli.control_send(CTRL_HELLO, &hello, sizeof(hello))) {
                    std::printf("[client] sent HELLO\n");
                }
                StaticState st{};
                if (cli.refresh_static(st)) {
                    std::printf("[client] static %zu entries\n", st.dir.size());
                    for (const auto& d : st.dir) {
                        std::printf("         stream %u name %s elem_type %u comps %u bytes_per_elem %u\n",
                                    d.id, d.name.c_str(), d.elem_type, d.components, d.bytes_per_elem);
                    }
                }
            }
        } else {
            std::printf("[client] server not found name %s reason %s\n", name.c_str(), reason);
        }
    };

    try_open("startup probe");

    while (g_run.load()) {
        if (!connected) {
            try_open("periodic probe");
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        FrameView fv{};
        bool ok = cli.latest(fv);
        if (!ok) {
            auto now = std::chrono::steady_clock::now();
            if (now - seen.time > std::chrono::seconds(2)) {
                std::printf("[client] no frames for a while, reconnecting\n");
                send_bye_best_effort(cli);
                cli.close();
                connected = false;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;
        }

        const std::uint64_t fid = fv.fh->frame_id.load(std::memory_order_acquire);
        const double sim = fv.fh->sim_time;

        if (fid != seen.frame_id) {
            seen.frame_id = fid;
            seen.time = std::chrono::steady_clock::now();
            ++recv_in_sec;

            if (fv.session_mismatch) {
                std::printf("[client] session changed old %llu new %llu\n",
                            static_cast<unsigned long long>(last_session),
                            static_cast<unsigned long long>(cli.header()->session_id));
                last_session = cli.header()->session_id;
            }

            DecodedFrame df{};
            cli.decode(fv, df);
            std::uint64_t tick_seq = 0;
            double tick_sim = 0.0;
            for (const auto& p : df.streams) {
                if (p.first == 42u && p.second.bytes == sizeof(std::uint64_t)) {
                    std::memcpy(&tick_seq, p.second.ptr, sizeof(std::uint64_t));
                } else if (p.first == 43u && p.second.bytes == sizeof(double)) {
                    std::memcpy(&tick_sim, p.second.ptr, sizeof(double));
                }
            }

            std::printf("[client] frame id %llu sim %.3f seq %llu tlv %u bytes %u\n",
                        static_cast<unsigned long long>(fid),
                        sim,
                        static_cast<unsigned long long>(tick_seq),
                        fv.fh->tlv_count,
                        fv.fh->payload_bytes);
        }

        auto now = std::chrono::steady_clock::now();
        auto sec = std::chrono::duration_cast<std::chrono::seconds>(now - t0).count();
        if (static_cast<std::uint64_t>(sec) != last_print) {
            last_print = static_cast<std::uint64_t>(sec);
            std::printf("[client] sec %llu recv %llu last_frame %llu\n",
                        static_cast<unsigned long long>(last_print),
                        static_cast<unsigned long long>(recv_in_sec),
                        static_cast<unsigned long long>(seen.frame_id));
            recv_in_sec = 0;
        }

        if (now - last_hb > std::chrono::seconds(1)) {
            last_hb = now;
            std::uint64_t stamp = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            cli.control_send(CTRL_HEARTBEAT, &stamp, sizeof(stamp));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    std::printf("[client] exiting\n");
    if (connected) send_bye_best_effort(cli);
    cli.close();
    return 0;
}
