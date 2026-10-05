#pragma once

#include "VKGSRenderTypes.hpp"
#include "../Common/readback_chain_diagnostics.hpp"
#include <array>
#include <cstring>
#include <fstream>
#include <memory>
#include <new>
#include <string_view>

namespace vk
{
// Observational: GPU timestamp spans include dependencies and diagnostic pipeline
// drains, not shader-active cycles. The only owner is the original RSX thread.
class bounded_draw_attribution
{
public:
 static constexpr u32 capacity = 32;
 static constexpr u32 invalid = ~0u;
 struct tag
 {
  u64 pipeline = 0, vertex_source_hash = 0, fragment_source_hash = 0;
  u64 vertex_module = 0, fragment_module = 0;
  std::array<u32, 4> color_address{}, color_pitch{}, color_width{}, color_height{}, color_vk_format{}, color_samples{};
  std::array<u64, 4> color_image{};
  u32 depth_address = 0, depth_pitch = 0;
  u64 depth_image = 0;
  u32 guest_width = 0, guest_height = 0;
  u32 width = 0, height = 0, primitive = 0, vertices = 0, passes = 0;
  u32 indexed = 0, subdraw = 0, color_format = 0, depth_format = 0, aa = 0, flags = 0;
 };
 static u64 host_source_hash(std::string_view text)
 {
  u64 h = 14695981039346656037ull;
  for (const unsigned char c : text) { h ^= c; h *= 1099511628211ull; }
  return h;
 }
private:
 struct transfer
 {
  u64 ns = 0, command = 0, generation = 0, sequence = 0, frame = 0;
  u64 cookie = 0, image = 0;
  u32 address = 0, length = 0, width = 0, height = 0, pitch = 0;
  u64 reset_generation = 0;
 };
 struct row
 {
  tag data{};
  u64 command = 0, generation = 0, reset_generation = 0, sequence = 0, frame = 0;
  u64 begin_ns = 0, end_ns = 0, begin_tick = 0, end_tick = 0, ordinal = 0;
  bool ended = false, collected = false, available = false;
 };
 VkDevice m_device = VK_NULL_HANDLE;
 VkQueryPool m_pool = VK_NULL_HANDLE;
 double m_period = 0;
 u32 m_bits = 0, m_count = 0, m_open = invalid;
 u64 m_skipped = 0, m_capacity_skipped = 0, m_wrong_command = 0, m_seen = 0, m_skip_prefix = 0, m_pending_ordinal = 0;
 bool m_primed = false;
 u64 m_prime_frame = 0;
 bool m_reset_recorded = false, m_selected = false, m_terminal = false, m_written = false;
 transfer m_a{}, m_b{};
 std::array<row, capacity> m_rows{};
 std::ofstream m_output;
 static constexpr u32 source_capacity = 4 * 1024 * 1024;
 struct source_entry { u64 hash = 0; u32 offset = 0, length = 0, domain = 0; };
 std::array<source_entry, capacity * 2> m_sources{};
 std::unique_ptr<char[]> m_source_bytes;
 u32 m_source_count = 0, m_source_used = 0, m_source_dropped = 0;
 std::string m_source_path;

 void save_source(u32 domain, u64 hash, std::string_view text)
 {
  for (u32 i = 0; i < m_source_count; ++i)
  {
   const auto& s = m_sources[i];
   if (s.domain == domain && s.hash == hash && s.length == text.size() &&
    std::memcmp(m_source_bytes.get() + s.offset, text.data(), s.length) == 0) return;
  }
  if (!m_source_bytes || text.empty() || text.size() > source_capacity - m_source_used || m_source_count == m_sources.size())
   { ++m_source_dropped; return; }
  const u32 length = static_cast<u32>(text.size());
  m_sources[m_source_count++] = {hash, m_source_used, length, domain};
  std::memcpy(m_source_bytes.get() + m_source_used, text.data(), length);
  m_source_used += length;
 }

public:
 explicit bounded_draw_attribution(VkDevice device, double period, u32 bits)
  : m_device(device), m_period(period), m_bits(bits)
 {
  const auto* flag = std::getenv("RPCS3_VK_B_DRAW_ATTRIBUTION");
  const auto* path = std::getenv("RPCS3_VK_B_DRAW_ATTRIBUTION_PATH");
  if (!flag || std::strcmp(flag, "1") || !path || !*path || !bits || bits > 64 || period <= 0) return;
  if (const auto* skip = std::getenv("RPCS3_VK_B_DRAW_SKIP"))
  {
   char* end = nullptr; const auto n = std::strtoull(skip, &end, 10);
   if (!end || *end || n > 4096) return;
   m_skip_prefix = n;
  }
  m_output.open(path, std::ios::out | std::ios::trunc);
  if (!m_output) return;
  VkQueryPoolCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  info.queryCount = capacity * 2;
  if (vkCreateQueryPool(device, &info, nullptr, &m_pool) != VK_SUCCESS) { m_pool = VK_NULL_HANDLE; return; }
  m_source_bytes.reset(new (std::nothrow) char[source_capacity]);
  if (!m_source_bytes) { vkDestroyQueryPool(device, m_pool, nullptr); m_pool = VK_NULL_HANDLE; return; }
  std::memset(m_source_bytes.get(), 0, source_capacity); // Prefault outside measurement.
  m_source_path = path;
 }
 ~bounded_draw_attribution() { if (m_pool) vkDestroyQueryPool(m_device, m_pool, nullptr); }
 bool ready() const { return m_pool != VK_NULL_HANDLE; }
 void initialize(const command_buffer_chunk& cmd)
 {
  if (!m_pool || m_reset_recorded) return;
  // This is called only at the existing primary diagnostic_command_begin,
  // outside render passes. Queries are used once and never reset/reused.
  vkCmdResetQueryPool(cmd, m_pool, 0, capacity * 2);
  m_reset_recorded = true;
 }
 void readback(const command_buffer_chunk& cmd, u64 sequence, u64 frame,
  u32 address, u32 length, u32 width, u32 height, u32 pitch, u64 cookie, u64 image, bool armed)
 {
  if (!m_pool || !m_reset_recorded || !armed || m_terminal || !cookie || !image ||
   length != 3686400 || width != 1280 || height != 720 || pitch != 5120) return;
  if (address == 0x37400b80 && !m_a.cookie)
  {
   if (!m_primed) { m_primed = true; m_prime_frame = frame; return; }
   if (frame <= m_prime_frame) return; // Require an original completed-flip boundary after arming.
   m_a = {rsx::readback_chain_trace::now(), cmd.diagnostic_command_key(),
    cmd.diagnostic_recording_generation(), sequence, frame, cookie, image,
    address, length, width, height, pitch, cmd.reset_id};
   m_selected = true;
  }
  else if (address == 0x37784b80 && m_a.cookie)
  {
   m_b = {rsx::readback_chain_trace::now(), cmd.diagnostic_command_key(),
    cmd.diagnostic_recording_generation(), sequence, frame, cookie, image,
    address, length, width, height, pitch, cmd.reset_id};
   m_selected = false;
   m_terminal = true;
  }
 }
 bool selected(const command_buffer_chunk& cmd, u64 frame)
 {
  if (!m_pool || !m_selected || m_terminal || m_open != invalid || frame != m_a.frame + 1 ||
   (cmd.diagnostic_command_key() == m_a.command && cmd.diagnostic_recording_generation() == m_a.generation)) return false;
  m_pending_ordinal = m_seen++;
  if (m_pending_ordinal < m_skip_prefix) return false;
  if (m_count == capacity) { ++m_capacity_skipped; return false; }
  return true;
 }
 u32 begin(const command_buffer_chunk& cmd, u64 sequence, u64 frame, const tag& data,
  std::string_view vertex_source, std::string_view fragment_source)
 {
  if (!m_pool || !m_selected || m_terminal || m_open != invalid || m_count == capacity || frame != m_a.frame + 1 ||
   (cmd.diagnostic_command_key() == m_a.command && cmd.diagnostic_recording_generation() == m_a.generation))
   { ++m_skipped; return invalid; }
  const u64 ordinal = m_pending_ordinal;
  const u32 index = m_count++;
  auto& r = m_rows[index];
  save_source(0, data.vertex_source_hash, vertex_source);
  save_source(1, data.fragment_source_hash, fragment_source);
  r.data = data; r.command = cmd.diagnostic_command_key();
  r.generation = cmd.diagnostic_recording_generation(); r.reset_generation = cmd.reset_id;
  r.sequence = sequence; r.frame = frame; r.ordinal = ordinal; r.begin_ns = rsx::readback_chain_trace::now();
  m_open = index;
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool, index * 2);
  return index;
 }
 void note_unsupported() { if (m_selected && !m_terminal) ++m_skipped; }
 void end(const command_buffer_chunk& cmd, u64 sequence, u32 token)
 {
  if (token == invalid || token >= m_count || m_open != token) return;
  auto& r = m_rows[token]; m_open = invalid;
  if (r.command != cmd.diagnostic_command_key() || r.generation != cmd.diagnostic_recording_generation() ||
   r.reset_generation != cmd.reset_id || r.sequence != sequence) { ++m_wrong_command; return; }
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool, token * 2 + 1);
  r.end_ns = rsx::readback_chain_trace::now(); r.ended = true;
 }
 void collect_after_original_completion(u64 sequence)
 {
  if (!m_pool) return;
  for (u32 i = 0; i < m_count; ++i)
  {
   auto& r = m_rows[i];
   if (r.sequence != sequence || !r.ended || r.collected) continue;
   r.collected = true;
   u64 result[4]{};
   const auto status = vkGetQueryPoolResults(m_device, m_pool, i * 2, 2,
    sizeof(result), result, 2 * sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
   if (status == VK_SUCCESS && result[1] && result[3])
   {
    const u64 mask = m_bits == 64 ? ~0ull : ((1ull << m_bits) - 1);
    r.begin_tick = result[0] & mask; r.end_tick = result[2] & mask; r.available = true;
   }
  }
 }
 void publish_owned_output(const char* finalization_kind)
 {
  if (!m_output || m_written) return;
  m_written = true;
  m_output.precision(17);
  m_output << "kind,frame,submission,command,generation,cookie,address,length,image,width,height,pitch,begin_ns,end_ns,begin_tick,end_tick,available,ended,pipeline,vertex_source_hash,fragment_source_hash,vertex_module,fragment_module,primitive,vertices,passes,indexed,subdraw,color_format,depth_format,aa,flags,depth_address,depth_pitch,depth_image,guest_width,guest_height";
  for (u32 i = 0; i < 4; ++i) m_output << ",color" << i << "_address,color" << i << "_pitch,color" << i << "_image,color" << i << "_width,color" << i << "_height,color" << i << "_vk_format,color" << i << "_samples";
  m_output << ",ordinal,reset_generation\n";
  const auto packet = [&](const char* kind, const transfer& t)
  {
   m_output << kind << ',' << t.frame << ',' << t.sequence << ',' << t.command << ',' << t.generation
    << ',' << t.cookie << ',' << t.address << ',' << t.length << ',' << t.image << ',' << t.width << ',' << t.height
    << ',' << t.pitch << ',' << t.ns << ',' << t.ns;
   for (u32 i = 14; i < 66; ++i) m_output << ",0";
   m_output << ',' << t.reset_generation << '\n';
  };
  packet("A_readback", m_a); packet("B_readback", m_b);
  for (u32 i = 0; i < m_count; ++i)
  {
   const auto& r = m_rows[i]; const auto& t = r.data;
   m_output << "draw," << r.frame << ',' << r.sequence << ',' << r.command << ',' << r.generation
    << ',' << m_a.cookie << ",0,0,0," << t.width << ',' << t.height << ",0," << r.begin_ns << ',' << r.end_ns
    << ',' << r.begin_tick << ',' << r.end_tick << ',' << r.available << ',' << r.ended << ',' << t.pipeline
    << ',' << t.vertex_source_hash << ',' << t.fragment_source_hash << ',' << t.vertex_module << ',' << t.fragment_module
    << ',' << t.primitive << ',' << t.vertices << ',' << t.passes << ',' << t.indexed << ',' << t.subdraw
    << ',' << t.color_format << ',' << t.depth_format << ',' << t.aa << ',' << t.flags
    << ',' << t.depth_address << ',' << t.depth_pitch << ',' << t.depth_image << ',' << t.guest_width << ',' << t.guest_height;
   for (u32 j = 0; j < 4; ++j) m_output << ',' << t.color_address[j] << ',' << t.color_pitch[j] << ',' << t.color_image[j] << ',' << t.color_width[j] << ',' << t.color_height[j] << ',' << t.color_vk_format[j] << ',' << t.color_samples[j];
   m_output << ',' << r.ordinal << ',' << r.reset_generation << '\n';
  }
  m_output << "#closed,count=" << m_count << ",capacity=" << capacity << ",capacity_skipped=" << m_capacity_skipped
   << ",seen=" << m_seen << ",selection_frame=" << (m_a.frame + 1) << ",skip_prefix=" << m_skip_prefix << ",unsupported=" << m_skipped << ",wrong_command=" << m_wrong_command << ",open=" << (m_open != invalid)
   << ",A_cookie=" << m_a.cookie << ",B_cookie=" << m_b.cookie << ",period_ns=" << m_period << ",valid_bits=" << m_bits << ",sources=" << m_source_count << ",source_bytes=" << m_source_used
   << ",source_dropped=" << m_source_dropped << ",finalization_kind=" << finalization_kind << ",finalized_ns=" << rsx::readback_chain_trace::now() << '\n';
  m_output.flush();
  std::ofstream sources(m_source_path + ".sources.bin", std::ios::binary | std::ios::trunc);
  std::ofstream index(m_source_path + ".sources.csv", std::ios::out | std::ios::trunc);
  index << "domain,hash,offset,length\n";
  for (u32 i = 0; i < m_source_count; ++i)
  {
   const auto& s = m_sources[i]; index << s.domain << ',' << s.hash << ',' << s.offset << ',' << s.length << '\n';
  }
  if (sources) sources.write(m_source_bytes.get(), m_source_used);
  sources.flush(); index.flush();
 }
 void finish_after_completed_window()
 {
  if (!m_output || m_written) return;
  const u64 armed = rsx::readback_chain_trace::armed_ns.load(std::memory_order_acquire);
  if (!armed || rsx::readback_chain_trace::now() <= armed + 5000000000ull || m_open != invalid) return;
  // Stop selection after the existing armed window. Publication observes only
  // already-proven original parent completion; it never waits or polls a fence.
  m_selected = false;
  m_terminal = true;
  for (u32 i = 0; i < m_count; ++i)
   if (m_rows[i].ended && !m_rows[i].collected) return;
  // Unended, unavailable, or missing packets remain explicit failure rows.
  publish_owned_output("completed_window");
 }
 void finish_after_original_device_idle()
 {
  publish_owned_output("original_device_idle");
 }
};
}
