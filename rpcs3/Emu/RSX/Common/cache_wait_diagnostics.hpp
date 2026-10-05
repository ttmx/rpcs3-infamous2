#pragma once

#include "Utilities/Thread.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace rsx
{
 namespace cache_wait_trace
 {
  inline const char* const path = std::getenv("RPCS3_CACHE_LOCK_TRACE");
  inline const bool enabled = path && *path;
  inline std::atomic<unsigned> active_gpu_waits{0};

  inline u64 now()
  {
   return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  // Diagnostic only. Bound output, measure intervals before logging, retain
  // all original lock operations and GPU completion waits.
  inline void record(const char* label, u64 begin, u64 end, u64 tid,
   unsigned active_begin, unsigned active_end, u64 address, u64 length)
  {
   struct output_state
   {
    std::mutex guard;
    std::FILE* file = std::fopen(path, "w");
    u64 rows = 0;
    output_state()
    {
     if (file) std::fputs("label,cpu_begin_ns,cpu_end_ns,thread_id,active_gpu_begin,active_gpu_end,address,length\n", file);
    }
    ~output_state() { if (file) std::fclose(file); }
   };
   static output_state output;
   std::lock_guard lock(output.guard);
   if (!output.file || output.rows >= 100000) return;
   std::fprintf(output.file, "%s,%llu,%llu,%llu,%u,%u,%llu,%llu\n",
    label, static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end),
    static_cast<unsigned long long>(tid), active_begin, active_end,
    static_cast<unsigned long long>(address), static_cast<unsigned long long>(length));
   if (!(++output.rows % 128)) std::fflush(output.file);
  }

  class probe
  {
   const char* m_label;
   u64 m_begin = 0, m_tid = 0, m_address = 0, m_length = 0;
   unsigned m_active_begin = 0;
   bool m_gpu;
  public:
   explicit probe(const char* label, bool gpu = false, u64 address = 0, u64 length = 0)
    : m_label(label), m_address(address), m_length(length), m_gpu(gpu)
   {
    if (!enabled) return;
    m_begin = now();
    m_tid = thread_ctrl::get_tid();
    m_active_begin = active_gpu_waits.load(std::memory_order_relaxed);
    if (m_gpu) active_gpu_waits.fetch_add(1, std::memory_order_relaxed);
   }
   probe(const probe&) = delete;
   probe& operator=(const probe&) = delete;
   ~probe() { finish(); }
   void finish()
   {
    if (!m_begin) return;
    const auto end = now();
    const auto active_end = active_gpu_waits.load(std::memory_order_relaxed);
    if (m_gpu) active_gpu_waits.fetch_sub(1, std::memory_order_relaxed);
    // Keep every readback wait; omit sub-10us lock acquisitions. The resulting
    // lock totals are a lower bound; exact GPU/lock overlap uses interval joins.
    if (m_gpu || end - m_begin >= 10000)
     record(m_label, m_begin, end, m_tid, m_active_begin, active_end, m_address, m_length);
    m_begin = 0;
   }
  };
 }
}
