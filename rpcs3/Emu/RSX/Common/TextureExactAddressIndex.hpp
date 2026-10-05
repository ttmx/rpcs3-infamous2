#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <unordered_map>
#include <vector>
namespace rsx::texture_exact_index
{
inline bool enabled() noexcept
{
 static const bool value = [] { const char* flag = std::getenv("RPCS3_VK_TEXTURE_EXACT_INDEX"); return flag && flag[0] == '1' && flag[1] == '\0'; }();
 return value;
}
inline bool shadow_enabled() noexcept
{
 static const bool value = [] { const char* flag = std::getenv("RPCS3_VK_TEXTURE_EXACT_INDEX_SHADOW"); return flag && flag[0] == '1' && flag[1] == '\0'; }();
 return value;
}
inline bool maintained() noexcept { return enabled() || shadow_enabled(); }
inline bool admit_shadow() noexcept
{
 static std::atomic<std::uint32_t> count{0};
 auto current = count.load(std::memory_order_relaxed);
 while (current < 128)
 {
  if (count.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) return true;
 }
 return false;
}
template <typename Section>
class ordered_address_index
{
public:
 struct entry { std::uint32_t ordinal; Section* section; };
private:
 std::unordered_map<std::uint32_t, std::vector<entry>> m_by_address;
 std::unordered_map<Section*, std::pair<std::uint32_t, std::uint32_t>> m_by_section;
 bool m_valid = true;
public:
 void publish(std::uint32_t address, std::uint32_t ordinal, Section* section)
 {
  if (!m_valid) return;
  // A reset may republish an unallocated section without a stock range-invalid callback.
  // Replace only index metadata; original overlap/protection callbacks are untouched.
  if (const auto old = m_by_section.find(section); old != m_by_section.end())
  {
   const auto previous = old->second;
   if (previous.second != ordinal) { m_valid = false; return; }
   revoke(previous.first, section);
   if (!m_valid) return;
  }
  auto& bucket = m_by_address[address];
  const auto position = std::lower_bound(bucket.begin(), bucket.end(), ordinal, [](const entry& item, std::uint32_t value) { return item.ordinal < value; });
  if ((position != bucket.end() && position->ordinal == ordinal) || std::any_of(bucket.begin(), bucket.end(), [section](const entry& item) { return item.section == section; }))
  {
   m_valid = false;
   return;
  }
  bucket.insert(position, {ordinal, section});
  m_by_section.emplace(section, std::pair{address, ordinal});
 }
 void revoke(std::uint32_t address, Section* section)
 {
  if (!m_valid) return;
  const auto registered = m_by_section.find(section);
  if (registered == m_by_section.end() || registered->second.first != address) { m_valid = false; return; }
  const auto bucket = m_by_address.find(address);
  if (bucket == m_by_address.end()) { m_valid = false; return; }
  const auto position = std::find_if(bucket->second.begin(), bucket->second.end(), [section](const entry& item) { return item.section == section; });
  if (position == bucket->second.end()) { m_valid = false; return; }
  bucket->second.erase(position);
  m_by_section.erase(registered);
  if (bucket->second.empty()) m_by_address.erase(bucket);
 }
 std::span<const entry> find(std::uint32_t address) const
 {
  const auto bucket = m_by_address.find(address);
  return bucket == m_by_address.end() ? std::span<const entry>{} : std::span<const entry>{bucket->second};
 }
 bool valid() const noexcept { return m_valid; }
 void clear() noexcept { m_by_address.clear(); m_by_section.clear(); m_valid = true; }
};
}
