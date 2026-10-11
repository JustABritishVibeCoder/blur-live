#pragma once
#include <cstdint>

namespace bl {

// GPU engine utilisation from the Windows performance counters ("GPU Engine" set, the same data
// Task Manager shows). Works without administrator rights. Call sample() about once per second.
class GpuMonitor {
 public:
  ~GpuMonitor();
  bool init();
  void sample();
  bool valid() const { return m_valid; }
  float total3D() const { return m_total3D; }     // busiest-adapter 3D engine utilisation, %
  float process3D() const { return m_process3D; } // this process's share, %

 private:
  void* m_query = nullptr;
  void* m_counter = nullptr;
  bool m_valid = false;
  bool m_primed = false;
  float m_total3D = 0, m_process3D = 0;
  uint32_t m_pid = 0;
};

}  // namespace bl
