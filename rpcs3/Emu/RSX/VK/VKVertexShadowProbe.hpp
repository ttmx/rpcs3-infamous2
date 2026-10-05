#pragma once

// Offline telemetry candidate. It observes stock completed CPU upload bytes;
// it never reads VM, changes a draw binding, or omits an allocation/copy.
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace vk::vertex_shadow_probe
{
inline bool enabled()
{
    static const bool value = [] {
        const char* flag = std::getenv("RPCS3_VK_VERTEX_SHADOW_PROBE");
        const char* path = std::getenv("RPCS3_VK_VERTEX_SHADOW_PROBE_PATH");
        return flag && std::strcmp(flag, "1") == 0 && path && *path;
    }();
    return value;
}
inline std::uint64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct counters
{
    std::uint64_t requests{}, unique{}, key_hits{}, equal{}, changed{};
    std::uint64_t requested_bytes{}, equal_bytes{}, changed_bytes{}, shadow_bytes{};
    std::uint64_t budget_drop{}, samples{}, sample_bytes{};
};
struct entry
{
    std::vector<std::byte> bytes;
    std::uint64_t original_offset{};
};
class probe
{
    static constexpr unsigned frame_cap = 16;
    static constexpr std::size_t shadow_cap = 8 * 1024 * 1024;
    static constexpr std::size_t sample_cap = 8 * 1024 * 1024;
    static constexpr std::size_t entry_cap = 2048;
    static constexpr std::size_t largest_sample = 65536;
    std::uint64_t gate_calls{}, observed_calls{}, guard_skips{}, frame{}, gpu_uid{}, upload_uid{};
    unsigned complete_frames{};
    bool armed{}, have_frame{}, ignore_first_frame{}, stopped{};
    std::size_t shadow_size{}, sample_size{};
    FILE* output{};
    FILE* samples{};
    std::array<counters, 8> counts{};
    std::array<unsigned, 8> equal_samples{}, changed_samples{};
    std::unordered_map<std::string, entry> entries;

    static unsigned size_class(std::size_t size)
    {
        const std::array<std::size_t, 7> bounds{256, 512, 1024, 2048, 4096, 16384, 65536};
        unsigned result = 0;
        while (result < bounds.size() && size > bounds[result]) ++result;
        return result;
    }
    void status(const char* kind, const char* reason = "")
    {
        if (output) {
            std::fprintf(output, "%s,%llu,%llu,0,0,0,0,0,0,0,0,0,0,0,0,0,0,%s\n", kind,
                static_cast<unsigned long long>(now_ns()), static_cast<unsigned long long>(frame), reason);
            std::fflush(output);
        }
    }
    void flush_frame()
    {
        if (!output) return;
        for (unsigned bin = 0; bin < counts.size(); ++bin) {
            const auto& c = counts[bin];
            if (!c.requests && !c.budget_drop) continue;
            std::fprintf(output, "frame,%llu,%llu,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,\n",
                static_cast<unsigned long long>(now_ns()), static_cast<unsigned long long>(frame), bin,
                static_cast<unsigned long long>(c.requests), static_cast<unsigned long long>(c.unique),
                static_cast<unsigned long long>(c.key_hits), static_cast<unsigned long long>(c.equal),
                static_cast<unsigned long long>(c.changed), static_cast<unsigned long long>(c.requested_bytes),
                static_cast<unsigned long long>(c.equal_bytes), static_cast<unsigned long long>(c.changed_bytes),
                static_cast<unsigned long long>(c.shadow_bytes), static_cast<unsigned long long>(c.budget_drop),
                static_cast<unsigned long long>(c.samples), static_cast<unsigned long long>(c.sample_bytes),
                static_cast<unsigned long long>(shadow_size));
        }
        std::fflush(output);
        if (samples) std::fflush(samples);
        entries.clear(); shadow_size = 0; counts = {};
    }
    void sample(bool equal, unsigned bin, std::uint64_t offset,
        const std::string& key, const entry& old, const std::byte* current, std::size_t length)
    {
        auto& quota = equal ? equal_samples[bin] : changed_samples[bin];
        const std::size_t record_size = 8 * sizeof(std::uint64_t) + key.size() + 2 * length;
        if (!samples || quota >= 2 || length > largest_sample || key.size() > 4096 ||
            sample_size + record_size > sample_cap) return;
        // Native-host u64 records. Parser verifies magic/endian marker before decoding.
        const std::array<std::uint64_t, 8> header{equal ? 1ull : 2ull, frame, gpu_uid, upload_uid,
            old.original_offset, offset, key.size(), length};
        if (std::fwrite(header.data(), sizeof(header), 1, samples) != 1 ||
            std::fwrite(key.data(), 1, key.size(), samples) != key.size() ||
            std::fwrite(old.bytes.data(), 1, length, samples) != length ||
            std::fwrite(current, 1, length, samples) != length) {
            stopped = true; status("stop", "sample_io_error"); return;
        }
        ++quota; sample_size += record_size; ++counts[bin].samples; counts[bin].sample_bytes += record_size;
    }
public:
    // Single main-RSX-thread ownership. Gate call must precede any key construction.
    bool ready()
    {
        if (!enabled() || stopped) return false;
        if (armed) return true;
        static const std::string marker = [] {
            const char* path = std::getenv("RPCS3_VK_VERTEX_SHADOW_PROBE_ARM_FILE");
            return path ? std::string(path) : std::string{};
        }();
        if (!marker.empty()) {
            if ((gate_calls++ & 255) != 0) return false;
            std::error_code error;
            if (!std::filesystem::exists(marker, error) || error) return false;
        }
        const char* prefix = std::getenv("RPCS3_VK_VERTEX_SHADOW_PROBE_PATH");
        output = std::fopen((std::string(prefix) + ".csv").c_str(), "w");
        samples = std::fopen((std::string(prefix) + ".bin").c_str(), "wb");
        if (!output || !samples) {
            stopped = true;
            if (output) { std::fclose(output); output = nullptr; }
            if (samples) { std::fclose(samples); samples = nullptr; }
            return false;
        }
        std::fputs("kind,mono_ns,frame,size_class,requests,unique,key_hits,equal,changed,requested_bytes,equal_bytes,changed_bytes,shadow_capture_bytes,budget_drops,samples,sample_bytes,frame_shadow_bytes,reason\n", output);
        const std::array<std::uint64_t, 2> magic{0x31564b5053484457ull, 0x0102030405060708ull};
        if (std::fwrite(magic.data(), sizeof(magic), 1, samples) != 1) { stopped = true; status("stop", "sample_header_io_error"); return false; }
        armed = true; ignore_first_frame = true; status("armed"); return true;
    }
    // Call immediately after the existing synchronous stock copy and before unmap.
    void observe(std::uint64_t current_frame, std::uint64_t current_gpu_uid,
        std::uint64_t current_upload_uid, std::uint32_t memory_flags,
        bool strict, bool multithreaded, std::uint64_t offset,
        const std::string& exact_tuple, const void* mapped_persistent, std::size_t length)
    {
        if (!armed || stopped) return;
        if (++observed_calls > 100000) { stopped = true; status("stop", "request_cap_partial_frame"); return; }
        if (!have_frame) { frame = current_frame; have_frame = true; }
        if (current_frame != frame) {
            if (!ignore_first_frame) { flush_frame(); ++complete_frames; }
            else { ignore_first_frame = false; entries.clear(); counts = {}; shadow_size = 0; }
            frame = current_frame;
            if (complete_frames >= frame_cap) { stopped = true; status("stop", "frame_cap"); return; }
        }
        if (ignore_first_frame) return;
        if (!strict || multithreaded || (memory_flags & 14) != 14 || !mapped_persistent || !length) {
            if (++guard_skips == 1) status("skip", "strict_MT_mapping_or_memory_flags"); return;
        }
        if (gpu_uid != current_gpu_uid || upload_uid != current_upload_uid) {
            if (!entries.empty()) status("invalidate", "heap_uid_changed");
            entries.clear(); shadow_size = 0; gpu_uid = current_gpu_uid; upload_uid = current_upload_uid;
        }
        const auto* current = static_cast<const std::byte*>(mapped_persistent);
        const unsigned bin = size_class(length); auto& c = counts[bin];
        ++c.requests; c.requested_bytes += length;
        auto found = entries.find(exact_tuple);
        if (found == entries.end()) {
            if (exact_tuple.size() > 4096 || entries.size() >= entry_cap || length > shadow_cap - shadow_size) { ++c.budget_drop; return; }
            entry value; value.bytes.resize(length); std::memcpy(value.bytes.data(), current, length); value.original_offset = offset;
            entries.emplace(exact_tuple, std::move(value)); shadow_size += length; ++c.unique; c.shadow_bytes += length; return;
        }
        auto& old = found->second;
        // The serialized exact tuple contains all source spans/lengths; a size mismatch is a guard rejection.
        if (old.bytes.size() != length) { ++c.budget_drop; status("skip", "tuple_size_mismatch"); return; }
        ++c.key_hits;
        const bool equal = std::memcmp(old.bytes.data(), current, length) == 0;
        if (equal) { ++c.equal; c.equal_bytes += length; }
        else { ++c.changed; c.changed_bytes += length; }
        sample(equal, bin, offset, exact_tuple, old, current, length);
        if (!equal && !stopped) {
            std::memcpy(old.bytes.data(), current, length); old.original_offset = offset; c.shadow_bytes += length;
        }
    }
    ~probe()
    {
        if (output) { if (!stopped) status("stop", "process_exit_partial_frame"); std::fclose(output); }
        if (samples) std::fclose(samples);
    }
};
inline probe& instance() { static probe value; return value; }
}
