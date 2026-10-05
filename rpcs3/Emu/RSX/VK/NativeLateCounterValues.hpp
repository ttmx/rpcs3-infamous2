#pragma once
#include <cstdint>

namespace native_late_counters
{
struct values
{
 std::int64_t setup_us = 0, texture_us = 0, vertex_us = 0, draw_us = 0;
 std::uint64_t draw_calls = 0;
 template<class Stats> static values read(const Stats& s)
 {
  return {s.setup_time, s.textures_upload_time, s.vertex_upload_time, s.draw_exec_time, s.draw_calls};
 }
 bool nonnegative() const noexcept
 {
  return setup_us >= 0 && texture_us >= 0 && vertex_us >= 0 && draw_us >= 0;
 }
 bool difference(const values& before, values& out) const noexcept
 {
  if (!nonnegative() || !before.nonnegative() || setup_us < before.setup_us || texture_us < before.texture_us || vertex_us < before.vertex_us || draw_us < before.draw_us || draw_calls < before.draw_calls)
   return false;
  out = {setup_us-before.setup_us, texture_us-before.texture_us, vertex_us-before.vertex_us, draw_us-before.draw_us, draw_calls-before.draw_calls};
  return true;
 }
};
}
