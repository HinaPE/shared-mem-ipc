#ifndef SHMX_INSPECTOR_H
#define SHMX_INSPECTOR_H
#include "shmx_common.h"
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace shmx {

    struct InspectLayout {
        std::uint32_t static_offset;
        std::uint32_t static_used;
        std::uint32_t static_cap;
        std::uint32_t readers_offset;
        std::uint32_t reader_stride;
        std::uint32_t reader_slots;
        std::uint32_t control_offset;
        std::uint32_t control_stride;
        std::uint32_t control_per_reader;
        std::uint32_t slots_offset;
        std::uint32_t slot_stride;
        std::uint32_t slots;
        std::uint32_t frame_bytes_cap;
    };

    struct InspectReader {
        std::uint64_t reader_id;
        std::uint64_t heartbeat;
        std::uint64_t last_frame_seen;
        bool in_use;
    };

    struct InspectDirEntry {
        std::uint32_t stream_id;
        std::uint32_t element_type;
        std::uint32_t components;
        std::uint32_t layout;
        std::uint32_t bytes_per_elem;
        std::string name;
        std::vector<std::uint8_t> extra;
    };

    struct InspectFrameView {
        const FrameHeader* fh;
        const std::uint8_t* payload;
        std::uint32_t bytes;
        bool checksum_ok;
    };

    struct InspectSlotView {
        std::uint32_t index;
        InspectFrameView view;
    };

    struct InspectControlMsg {
        std::uint64_t reader_id;
        std::uint32_t type;
        std::vector<std::uint8_t> data;
    };

    struct InspectItem {
        const void* ptr;
        std::uint32_t bytes;
        std::uint32_t elem_count;
    };

    class Inspector {
    public:
        Inspector() = default;
        ~Inspector() {
            close();
        }
        Inspector(const Inspector&)            = delete;
        Inspector& operator=(const Inspector&) = delete;

        bool open(const std::string& name) {
            close();
            const std::size_t hdr_bytes = align_up(static_cast<std::uint32_t>(sizeof(GlobalHeader)), 64);
            if (!map_.open(name, hdr_bytes)) return false;
            GH_ = reinterpret_cast<GlobalHeader*>(map_.data());
            if (!validate_header_min()) {
                close();
                return false;
            }
            const auto total_bytes = compute_total_bytes(*GH_);
            if (total_bytes == 0) {
                close();
                return false;
            }
            if (!map_.remap(total_bytes)) {
                close();
                return false;
            }
            GH_ = reinterpret_cast<GlobalHeader*>(map_.data());
            return validate_header_min();
        }

        void close() {
            map_.close();
            GH_ = nullptr;
        }

        const GlobalHeader* header() const {
            return reinterpret_cast<const GlobalHeader*>(map_.data());
        }

        InspectLayout layout() const {
            const auto* H = header();
            InspectLayout L{};
            if (!H) return L;
            L.static_offset      = H->static_offset;
            L.static_used        = H->static_bytes_used;
            L.static_cap         = H->static_bytes_cap;
            L.readers_offset     = H->readers_offset;
            L.reader_stride      = H->reader_slot_stride;
            L.reader_slots       = H->reader_slots;
            L.control_offset     = H->control_offset;
            L.control_stride     = H->control_stride;
            L.control_per_reader = H->control_per_reader;
            L.slots_offset       = H->slots_offset;
            L.slot_stride        = H->slot_stride;
            L.slots              = H->slots;
            L.frame_bytes_cap    = H->frame_bytes_cap;
            return L;
        }

        std::vector<InspectReader> snapshot_readers() const {
            std::vector<InspectReader> v;
            const auto* H = header();
            if (!H) return v;
            v.reserve(H->reader_slots);
            for (std::uint32_t i = 0; i < H->reader_slots; ++i) {
                auto* RS = reinterpret_cast<const ReaderSlot*>(map_.data() + H->readers_offset + i * H->reader_slot_stride);
                InspectReader r{};
                r.reader_id       = RS->reader_id.load(std::memory_order_acquire);
                r.heartbeat       = RS->heartbeat.load(std::memory_order_acquire);
                r.last_frame_seen = RS->last_frame_seen.load(std::memory_order_acquire);
                r.in_use          = RS->in_use.load(std::memory_order_acquire) != 0u;
                v.push_back(r);
            }
            return v;
        }

        std::vector<InspectDirEntry> decode_static_dir() const {
            std::vector<InspectDirEntry> out;
            const auto* H = header();
            if (!H) return out;
            const std::uint8_t* cur = map_.data() + H->static_offset;
            const std::uint8_t* end = cur + H->static_bytes_used;
            while (cur + sizeof(TLV) <= end) {
                TLV tlv{};
                std::memcpy(&tlv, cur, sizeof(TLV));
                const auto tlv_end = cur + sizeof(TLV) + tlv.length;
                if (tlv_end > end) break;
                if (tlv.type == TLV_STATIC_DIR) {
                    if (tlv.length < sizeof(StaticStreamDesc)) break;
                    StaticStreamDesc ss{};
                    std::memcpy(&ss, cur + sizeof(TLV), sizeof(StaticStreamDesc));
                    const auto* pName = reinterpret_cast<const char*>(cur + sizeof(TLV) + sizeof(StaticStreamDesc));
                    if (sizeof(StaticStreamDesc) + ss.name_len + ss.extra_len > tlv.length) break;
                    InspectDirEntry de{};
                    de.stream_id      = ss.stream_id;
                    de.element_type   = ss.element_type;
                    de.components     = ss.components;
                    de.layout         = ss.layout;
                    de.bytes_per_elem = ss.bytes_per_elem;
                    de.name.assign(pName, pName + ss.name_len);
                    if (ss.extra_len) {
                        const auto* pExtra = reinterpret_cast<const std::uint8_t*>(pName + ss.name_len);
                        de.extra.assign(pExtra, pExtra + ss.extra_len);
                    }
                    out.push_back(std::move(de));
                }
                cur += align_up(static_cast<std::uint32_t>(sizeof(TLV)) + tlv.length, 16);
            }
            return out;
        }

        bool latest(InspectFrameView& out) const {
            const auto* H = header();
            if (!H) return false;
            if (H->slots == 0) return false;
            const auto w = H->write_index.load(std::memory_order_acquire);
            if (w == 0u) return false;
            const auto slot = (w - 1u) % H->slots;
            return slot_view(static_cast<std::uint32_t>(slot), out);
        }

        bool slot_view(std::uint32_t slot, InspectFrameView& out) const {
            const auto* H = header();
            if (!H) return false;
            if (slot >= H->slots) return false;
            const auto* base_slot = map_.data() + H->slots_offset + slot * H->slot_stride;
            const auto* FH        = reinterpret_cast<const FrameHeader*>(base_slot);
            const auto* payload   = base_slot + align_up(static_cast<std::uint32_t>(sizeof(FrameHeader)), 64);
            const auto bytes      = FH->payload_bytes;
            if (bytes == 0 || bytes > H->frame_bytes_cap) return false;
            if (FH->session_id_copy != H->session_id) return false;
            const auto calc = checksum32(payload, bytes);
            out             = InspectFrameView{FH, payload, bytes, calc == FH->checksum};
            return true;
        }

        std::vector<InspectSlotView> list_slots() const {
            std::vector<InspectSlotView> v;
            const auto* H = header();
            if (!H) return v;
            v.reserve(H->slots);
            for (std::uint32_t i = 0; i < H->slots; ++i) {
                InspectFrameView fv{};
                if (slot_view(i, fv)) v.push_back(InspectSlotView{i, fv});
            }
            return v;
        }

        static bool decode_frame(const InspectFrameView& fv, std::vector<std::pair<std::uint32_t, InspectItem>>& streams) {
            streams.clear();
            const auto* cur = fv.payload;
            const auto* end = fv.payload + fv.bytes;
            while (cur + sizeof(TLV) <= end) {
                TLV tlv{};
                std::memcpy(&tlv, cur, sizeof(TLV));
                const auto tlv_end = cur + sizeof(TLV) + tlv.length;
                if (tlv_end > end) break;
                if (tlv.type == TLV_FRAME_STREAM) {
                    if (tlv.length < sizeof(FrameStreamTLV)) break;
                    FrameStreamTLV fs{};
                    std::memcpy(&fs, cur + sizeof(TLV), sizeof(FrameStreamTLV));
                    const auto* body = cur + sizeof(TLV) + sizeof(FrameStreamTLV);
                    const auto have  = static_cast<std::size_t>(end - body);
                    if (have < fs.bytes_payload) break;
                    streams.emplace_back(fs.stream_id, InspectItem{body, fs.bytes_payload, fs.elem_count});
                }
                cur += align_up(static_cast<std::uint32_t>(sizeof(TLV)) + tlv.length, 16);
            }
            return true;
        }

        std::vector<InspectControlMsg> peek_control(std::uint32_t reader_index, std::uint32_t max_msgs) const {
            std::vector<InspectControlMsg> out;
            const auto* H = header();
            if (!H) return out;
            if (H->control_per_reader == 0u) return out;
            if (reader_index >= H->reader_slots) return out;
            auto* CH       = map_.data() + H->control_offset + reader_index * H->control_stride;
            const auto cap = H->control_per_reader;
            auto* r64      = reinterpret_cast<const std::atomic<std::uint64_t>*>(CH);
            auto* w64      = r64 + 1;
            const auto rv  = r64->load(std::memory_order_acquire);
            const auto wv  = w64->load(std::memory_order_acquire);
            auto rd        = rv;
            while (rd != wv && out.size() < max_msgs) {
                auto off = 16u + static_cast<std::uint32_t>(rd % (cap - 16u));
                if (off + sizeof(TLV) > cap) {
                    rd += static_cast<std::uint64_t>(cap - off);
                    off = 16u;
                }
                TLV tlv{};
                std::memcpy(&tlv, CH + off, sizeof(TLV));
                const auto body = align_up(static_cast<std::uint32_t>(sizeof(TLV)) + tlv.length, 16);
                if (off + body > cap) {
                    if (tlv.type == 0u) {
                        rd += static_cast<std::uint64_t>(cap - off);
                        continue;
                    }
                    break;
                }
                if (tlv.type != 0u) {
                    InspectControlMsg m{};
                    m.reader_id = reader_id_of(reader_index);
                    m.type      = tlv.type;
                    m.data.resize(tlv.length);
                    std::memcpy(m.data.data(), CH + off + sizeof(TLV), tlv.length);
                    out.push_back(std::move(m));
                }
                rd += body;
            }
            return out;
        }

    private:
        static std::size_t compute_total_bytes(const GlobalHeader& H) {
            if (H.slots == 0u) return 0;
            const std::uint64_t total64 = static_cast<std::uint64_t>(H.slots_offset) + static_cast<std::uint64_t>(H.slots) * static_cast<std::uint64_t>(H.slot_stride);
            if (total64 > std::numeric_limits<std::uint32_t>::max()) return 0;
            return static_cast<std::size_t>(total64);
        }
        bool validate_header_min() const {
            if (!GH_) return false;
            if (GH_->magic != MAGIC || GH_->ver_major != VER_MAJOR || GH_->ver_minor != VER_MINOR || GH_->endianness != ENDIAN_TAG) return false;
            return true;
        }
        std::uint64_t reader_id_of(std::uint32_t idx) const {
            const auto* H = header();
            auto* RS      = reinterpret_cast<const ReaderSlot*>(map_.data() + H->readers_offset + idx * H->reader_slot_stride);
            return RS->reader_id.load(std::memory_order_acquire);
        }

        Map map_;
        GlobalHeader* GH_ = nullptr;
    };

} // namespace shmx
#endif
