#pragma once

#include "Utilities/File.h"
#include <chrono>
#include <cstdlib>
#include <fstream>

namespace rsx::FIFO
{
 inline const bool semaphore_trace_enabled = []
 {
  const char* value = std::getenv("RPCS3_FIFO_IDLE_TRACE");
  return value && value[0] == '1' && value[1] == '\0';
 }();

 // Diagnostic elapsed spans include any backend work serviced inside the wait.
 class semaphore_trace_sink
 {
  std::ofstream m_output;
  u64 m_rows = 0, m_dropped = 0;
  static constexpr u64 maximum_rows = 262144;
 public:
  static u64 now()
  {
   return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  semaphore_trace_sink()
  {
   const char* requested = std::getenv("RPCS3_FIFO_IDLE_TRACE_PATH");
   const std::string path = requested && *requested ? requested : fs::get_cache_dir() + "fifo-idle.csv";
   m_output.open(path + ".semaphore.csv", std::ios::out | std::ios::trunc);
   if (m_output)
    m_output << "label,cpu_begin_ns,cpu_end_ns,address,argument,get_begin,put_begin,get_end,put_end,exit_reason,dropped_total\n";
  }
  bool available() const { return !!m_output; }
  void write(u64 begin, u64 end, u32 address, u32 argument, u32 get_begin, u32 put_begin, u32 get_end, u32 put_end, const char* exit_reason)
  {
   if (!m_output)
    return;
   if (m_rows >= maximum_rows)
   {
    ++m_dropped;
    if (!(m_dropped & (m_dropped - 1)))
    {
     m_output << "trace_overflow," << begin << ',' << end << ',' << address << ',' << argument << ','
      << get_begin << ',' << put_begin << ',' << get_end << ',' << put_end << ",overflow," << m_dropped << '\n';
     m_output.flush();
    }
    return;
   }
   m_output << "semaphore_wait," << begin << ',' << end << ',' << address << ',' << argument << ','
    << get_begin << ',' << put_begin << ',' << get_end << ',' << put_end << ',' << exit_reason << ',' << m_dropped << '\n';
   if (!(++m_rows % 16))
    m_output.flush();
  }
 };

 inline semaphore_trace_sink& get_semaphore_trace_sink()
 {
  thread_local semaphore_trace_sink sink;
  return sink;
 }

 template <typename Fifo, typename Ctrl>
 class semaphore_wait_span
 {
  semaphore_trace_sink* m_sink = nullptr;
  const Fifo& m_fifo;
  const Ctrl& m_ctrl;
  u64 m_begin = 0;
  u32 m_address = 0, m_argument = 0, m_get = 0, m_put = 0;
  const char* m_exit = "satisfied";
 public:
  semaphore_wait_span(bool enabled, u32 address, u32 argument, const Fifo& fifo, const Ctrl& ctrl)
   : m_fifo(fifo), m_ctrl(ctrl)
  {
   if (!enabled)
    return;
   m_sink = &get_semaphore_trace_sink();
   if (!m_sink->available())
   {
    m_sink = nullptr;
    return;
   }
   m_address = address;
   m_argument = argument;
   m_get = fifo.get_pos();
   m_put = ctrl.put.load();
   m_begin = semaphore_trace_sink::now();
  }
  ~semaphore_wait_span()
  {
   if (!m_sink)
    return;
   const u64 end = semaphore_trace_sink::now();
   m_sink->write(m_begin, end, m_address, m_argument, m_get, m_put,
    m_fifo.get_pos(), m_ctrl.put.load(), m_exit);
  }
  void stopped() { m_exit = "stopped"; }
  void timed_out() { m_exit = "timed_out"; }
 };
}
