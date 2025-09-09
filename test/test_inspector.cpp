#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void enable_ansi() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD m = 0;
    if (!GetConsoleMode(h, &m)) return;
    m |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(h, m);
}
#else
static void enable_ansi() {}
#endif
#include "shmx_common.h"
#include "shmx_inspector.h"
using namespace shmx;

static void enter_alt() {
    std::printf("\x1b[?1049h\x1b[?25l");
}
static void leave_alt() {
    std::printf("\x1b[?25h\x1b[?1049l");
}
static void clear_home() {
    std::printf("\x1b[H\x1b[2J");
}

static std::string bar_line(size_t w, char ch) {
    return std::string(w, ch);
}

static void draw_table(std::ostringstream& os, const std::vector<std::string>& headers, const std::vector<std::vector<std::string>>& rows, const std::vector<size_t>& widths) {
    const std::string sep = "+" + [&]() {
        std::string s;
        for (size_t i = 0; i < widths.size(); ++i) {
            s += bar_line(widths[i] + 2, '-');
            s += "+";
        }
        return s;
    }();
    os << sep << "\n";
    os << "|";
    for (size_t i = 0; i < headers.size(); ++i) {
        std::ostringstream cell;
        cell << " " << headers[i];
        std::string c = cell.str();
        if (c.size() < widths[i] + 1) c += std::string(widths[i] + 1 - c.size(), ' ');
        os << "\x1b[1m\x1b[36m" << c << "\x1b[0m" << "|";
    }
    os << "\n" << sep << "\n";
    for (const auto& r : rows) {
        os << "|";
        for (size_t i = 0; i < widths.size(); ++i) {
            std::string v = (i < r.size() ? r[i] : "");
            if (v.size() > widths[i]) v = v.substr(0, widths[i]);
            std::ostringstream cell;
            cell << " " << v;
            std::string c = cell.str();
            if (c.size() < widths[i] + 1) c += std::string(widths[i] + 1 - c.size(), ' ');
            os << c << "|";
        }
        os << "\n";
    }
    os << sep << "\n";
}

static std::string human_bytes(std::uint64_t v) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int i               = 0;
    double d            = (double) v;
    while (d >= 1024.0 && i < 4) {
        d /= 1024.0;
        ++i;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.2f %s (%llu B)", d, units[i], (unsigned long long) v);
    return std::string(buf);
}

int main(int argc, char** argv) {
    enable_ansi();
    std::string name = (argc >= 2) ? argv[1] : std::string("shmx_demo");
    Inspector ins;
    enter_alt();
    std::uint32_t last_gen = 0;
    std::vector<InspectDirEntry> dir;
    const size_t WBAR = 80;
    while (true) {
        if (!ins.header()) {
            if (!ins.open(name)) {
                clear_home();
                std::printf("\x1b[1m\x1b[33mshmx inspector\x1b[0m  name %s\n", name.c_str());
                std::printf("waiting for server...\n");
                std::fflush(stdout);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            last_gen = 0;
            dir.clear();
        }
        const auto* H  = ins.header();
        const auto gen = H->static_gen.load(std::memory_order_acquire);
        if (gen != last_gen) {
            dir      = ins.decode_static_dir();
            last_gen = gen;
        }
        auto L = ins.layout();

        std::uint64_t total_bytes   = (std::uint64_t) L.slots_offset + (std::uint64_t) L.slot_stride * (std::uint64_t) L.slots;
        std::uint64_t static_total  = L.static_cap;
        std::uint64_t readers_total = (std::uint64_t) L.reader_stride * (std::uint64_t) L.reader_slots;
        std::uint64_t control_total = (std::uint64_t) L.control_stride * (std::uint64_t) L.reader_slots;
        std::uint64_t frames_total  = (std::uint64_t) L.slot_stride * (std::uint64_t) L.slots;

        std::vector<char> bar(WBAR, ' ');
        auto map_pos = [&](std::uint64_t x) -> size_t {
            if (total_bytes == 0) return 0;
            long double r = (long double) x * (long double) WBAR / (long double) total_bytes;
            size_t p      = (size_t) r;
            if (p > WBAR) p = WBAR;
            return p;
        };
        const std::uint64_t hdr_aligned = align_up((std::uint32_t) sizeof(GlobalHeader), 64);
        auto paint_seg                  = [&](std::uint64_t start, std::uint64_t bytes, char ch) {
            if (bytes == 0) return;
            size_t a = map_pos(start);
            size_t b = map_pos(start + bytes);
            if (b <= a) b = std::min(a + 1, WBAR);
            for (size_t i = a; i < b && i < WBAR; ++i) bar[i] = ch;
        };
        paint_seg(0, hdr_aligned, 'H');
        paint_seg(L.static_offset, L.static_used, 'S');
        if (L.static_cap > L.static_used) paint_seg((std::uint64_t) L.static_offset + L.static_used, L.static_cap - L.static_used, 's');
        paint_seg(L.readers_offset, readers_total, 'R');
        if (L.control_per_reader) paint_seg(L.control_offset, control_total, 'C');
        paint_seg(L.slots_offset, frames_total, 'A');
        std::uint32_t latest_idx = 0;
        if (L.slots > 0) {
            const auto w = H->write_index.load(std::memory_order_acquire);
            if (w != 0u) latest_idx = (w - 1u) % L.slots;
        }
        for (std::uint32_t i = 0; i < L.slots; ++i) {
            std::uint64_t start = (std::uint64_t) L.slots_offset + (std::uint64_t) i * L.slot_stride;
            size_t a            = map_pos(start);
            size_t b            = map_pos(start + L.slot_stride);
            if (b <= a) b = std::min(a + 1, WBAR);
            InspectFrameView fv{};
            bool ok   = ins.slot_view(i, fv);
            char fill = '.';
            if (ok && fv.bytes > 0 && fv.checksum_ok)
                fill = '#';
            else if (ok && fv.bytes > 0 && !fv.checksum_ok)
                fill = '!';
            if (i == latest_idx) fill = 'L';
            for (size_t k = a; k < b && k < WBAR; ++k) bar[k] = fill;
        }

        std::ostringstream os;
        os << "\x1b[1m\x1b[35mshmx inspector\x1b[0m  name " << name << "\n";
        os << "session " << (unsigned long long) H->session_id << "  ver " << H->ver_major << "." << H->ver_minor << "  readers " << H->readers_connected.load() << "\n";
        os << "[" << std::string(bar.begin(), bar.end()) << "]\n";
        os << "legend: H header  S static-used  s static-free  R readers  C control  A slots-area  L latest  # ok  ! bad  . empty\n\n";

        {
            std::vector<std::string> headers{"field", "value"};
            std::vector<size_t> widths{18, 64};
            std::vector<std::vector<std::string>> rows;
            rows.push_back({"total shm", human_bytes(total_bytes)});
            {
                std::ostringstream v;
                v << "off " << L.static_offset << " used " << L.static_used << " cap " << L.static_cap << " -> total " << human_bytes(static_total);
                rows.push_back({"static", v.str()});
            }
            {
                std::ostringstream v;
                v << "off " << L.readers_offset << " stride " << L.reader_stride << " slots " << L.reader_slots << " -> total " << human_bytes(readers_total);
                rows.push_back({"readers", v.str()});
            }
            {
                std::ostringstream v;
                v << "off " << L.control_offset << " stride " << L.control_stride << " per " << L.control_per_reader << " slots " << L.reader_slots << " -> total " << human_bytes(control_total);
                rows.push_back({"control", v.str()});
            }
            {
                std::ostringstream v;
                v << "off " << L.slots_offset << " stride " << L.slot_stride << " slots " << L.slots << " cap " << L.frame_bytes_cap << " -> total " << human_bytes(frames_total);
                rows.push_back({"frames", v.str()});
            }
            draw_table(os, headers, rows, widths);
        }

        {
            auto readers = ins.snapshot_readers();
            std::vector<std::string> headers{"idx", "in_use", "id", "last", "hb"};
            std::vector<size_t> widths{5, 7, 18, 14, 14};
            std::vector<std::vector<std::string>> rows;
            size_t n = std::min<size_t>(readers.size(), 10);
            for (size_t i = 0; i < n; ++i) {
                rows.push_back({std::to_string(i), readers[i].in_use ? "1" : "0", std::to_string((unsigned long long) readers[i].reader_id), std::to_string((unsigned long long) readers[i].last_frame_seen), std::to_string((unsigned long long) readers[i].heartbeat)});
            }
            draw_table(os, headers, rows, widths);
        }

        {
            InspectFrameView fv{};
            if (ins.latest(fv)) {
                auto fid = fv.fh->frame_id.load(std::memory_order_acquire);
                std::vector<std::string> headers{"frame_id", "tlv", "bytes", "sim", "checksum"};
                std::vector<size_t> widths{18, 6, 12, 14, 10};
                std::vector<std::vector<std::string>> rows;
                rows.push_back({std::to_string((unsigned long long) fid), std::to_string(fv.fh->tlv_count), human_bytes(fv.bytes),
                    [&]() {
                        char buf[64];
                        std::snprintf(buf, sizeof(buf), "%.6f", fv.fh->sim_time);
                        return std::string(buf);
                    }(),
                    fv.checksum_ok ? "ok" : "bad"});
                draw_table(os, headers, rows, widths);

                std::vector<std::pair<std::uint32_t, InspectItem>> streams;
                ins.decode_frame(fv, streams);
                std::vector<std::string> h2{"stream_id", "name", "elems", "bytes"};
                std::vector<size_t> w2{10, 26, 8, 12};
                std::vector<std::vector<std::string>> r2;
                size_t m = std::min<size_t>(streams.size(), 10);
                for (size_t i = 0; i < m; ++i) {
                    auto sid       = streams[i].first;
                    auto it        = std::find_if(dir.begin(), dir.end(), [&](const InspectDirEntry& e) { return e.stream_id == sid; });
                    std::string nm = it != dir.end() ? it->name : "?";
                    r2.push_back({std::to_string(sid), nm, std::to_string(streams[i].second.elem_count), human_bytes(streams[i].second.bytes)});
                }
                draw_table(os, h2, r2, w2);
            } else {
                std::vector<std::string> headers{"frame", "value"};
                std::vector<size_t> widths{10, 30};
                std::vector<std::vector<std::string>> rows;
                rows.push_back({"latest", "none"});
                draw_table(os, headers, rows, widths);
            }
        }

        clear_home();
        std::string page = os.str();
        std::fwrite(page.data(), 1, page.size(), stdout);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    leave_alt();
    ins.close();
    return 0;
}
