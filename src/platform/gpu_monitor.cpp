#include "platform/gpu_monitor.h"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace bl {

GpuMonitor::~GpuMonitor() {
  if (m_query) PdhCloseQuery(static_cast<PDH_HQUERY>(m_query));
}

bool GpuMonitor::init() {
  m_pid = GetCurrentProcessId();
  PDH_HQUERY q = nullptr;
  if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return false;
  PDH_HCOUNTER c = nullptr;
  if (PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &c) != ERROR_SUCCESS) {
    PdhCloseQuery(q);
    return false;
  }
  m_query = q;
  m_counter = c;
  PdhCollectQueryData(q);  // first sample primes the rate counters
  m_valid = true;
  return true;
}

void GpuMonitor::sample() {
  if (!m_valid) return;
  auto q = static_cast<PDH_HQUERY>(m_query);
  auto c = static_cast<PDH_HCOUNTER>(m_counter);
  if (PdhCollectQueryData(q) != ERROR_SUCCESS) return;
  DWORD bytes = 0, count = 0;
  PDH_STATUS st = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
  if (st != PDH_MORE_DATA) return;
  std::vector<uint8_t> buf(bytes);
  auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
  if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return;

  // Instance names look like "pid_1234_luid_0x0000_0x0000ABCD_phys_0_eng_0_engtype_3D".
  std::map<std::wstring, double> perEngine;  // luid+eng -> summed utilisation across processes
  double mine = 0;
  const std::wstring myPid = L"pid_" + std::to_wstring(m_pid) + L"_";
  for (DWORD i = 0; i < count; ++i) {
    std::wstring name = items[i].szName;
    if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
    bool is3D = name.find(L"engtype_3D") != std::wstring::npos || name.find(L"engtype_Compute") != std::wstring::npos;
    if (!is3D) continue;
    double v = items[i].FmtValue.doubleValue;
    auto luid = name.find(L"luid_");
    if (luid != std::wstring::npos) perEngine[name.substr(luid)] += v;
    if (name.rfind(myPid, 0) == 0) mine += v;
  }
  double busiest = 0;
  for (auto& [k, v] : perEngine) busiest = std::max(busiest, v);
  m_total3D = float(std::min(100.0, busiest));
  m_process3D = float(std::min(100.0, mine));
  m_primed = true;
}

}  // namespace bl
