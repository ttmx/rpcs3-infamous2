#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
namespace native_texture_counters
{
inline bool enabled() noexcept
{
 static const bool value = [] { const char* flag = std::getenv("RPCS3_VK_TEXTURE_PHASE_COUNTERS"); return flag && flag[0] == '1' && flag[1] == '\0'; }();
 return value;
}
enum counter : std::size_t { load_calls, bind_blocks, fs_slots, vs_slots, clean_reuse, uploads, invalid_uploads, temporary_requests, layout_checks, uniform_binds, sampler_requests, sampler_reuse, oom_retries, interpreter_calls, regular_bind_calls, count };
enum domain : std::size_t { shader_read_uploads, framebuffer_uploads, blit_source_uploads, blit_destination_uploads, other_uploads, global_dirty_uploads, slot_dirty_uploads, compressed_uploads, shader_read_slot_dirty_without_global, cyclic_uploads, direct_image_uploads, invalid_domain_uploads, prospective_key_matches, stock_view_matches, stock_view_mismatches, domain_count };
struct values
{
 std::uint64_t load_us = 0, bind_us = 0;
 std::array<std::uint64_t, count> counters{};
 std::array<std::uint64_t, domain_count> domains{};
 bool invalid = false;
 void add_domain(domain index) noexcept
 {
  auto& item = domains[index];
  if (item == std::numeric_limits<std::uint64_t>::max()) invalid = true; else ++item;
 }
 void add(counter index) noexcept
 {
  auto& item = counters[index];
  if (item == std::numeric_limits<std::uint64_t>::max()) invalid = true;
  else ++item;
 }
 void elapsed(bool binding, std::int64_t us) noexcept
 {
  auto& item = binding ? bind_us : load_us;
  if (us < 0 || std::uint64_t(us) > std::numeric_limits<std::uint64_t>::max() - item) invalid = true;
  else item += std::uint64_t(us);
 }
 bool difference(const values& before, values& out) const noexcept
 {
  if (invalid || before.invalid || load_us < before.load_us || bind_us < before.bind_us) return false;
  out.load_us = load_us - before.load_us; out.bind_us = bind_us - before.bind_us;
  for (std::size_t i = 0; i < count; ++i)
  {
   if (counters[i] < before.counters[i]) return false;
   out.counters[i] = counters[i] - before.counters[i];
  }
  for (std::size_t i = 0; i < domain_count; ++i)
  {
   if (domains[i] < before.domains[i]) return false;
   out.domains[i] = domains[i] - before.domains[i];
  }
  return true;
 }
};
}
