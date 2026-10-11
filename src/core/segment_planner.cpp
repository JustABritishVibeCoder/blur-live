#include "core/segment_planner.h"

#include <algorithm>
#include <cmath>

namespace bl {

FlowPlan planFlowSegments(std::span<const HistoryFrame> history, double shutterSeconds, int sampleBudget,
                          int maxSegments) {
  FlowPlan plan;
  if (history.size() < 2 || !(shutterSeconds > 0) || sampleBudget <= 0 || maxSegments <= 0) return plan;

  const double tNow = history[0].time;
  const double tStart = tNow - shutterSeconds;
  const double eps = shutterSeconds * 1e-4;  // ignore floating-point slivers at the window start

  for (size_t i = 0; i + 1 < history.size() && int(plan.segments.size()) < maxSegments; ++i) {
    const HistoryFrame& cur = history[i];
    const HistoryFrame& prev = history[i + 1];
    if (cur.discontinuityBefore) break;
    const double dt = cur.time - prev.time;
    if (!(dt > 0)) break;
    if (cur.time <= tStart + eps) break;

    const double segStart = std::max(prev.time, tStart);
    PlannedSegment s;
    s.prevSlot = prev.slot;
    s.curSlot = cur.slot;
    s.u0 = float((segStart - prev.time) / dt);
    s.u1 = 1.0f;
    s.tau0 = float((segStart - tStart) / shutterSeconds);
    s.tau1 = float((cur.time - tStart) / shutterSeconds);
    const double span = double(s.tau1) - double(s.tau0);
    s.samples = std::max(2, int(std::ceil(double(sampleBudget) * span)));
    plan.coverage += float(span);
    plan.totalSamples += s.samples;
    plan.segments.push_back(s);

    if (prev.time <= tStart + eps) break;  // window fully covered
  }
  plan.coverage = std::clamp(plan.coverage, 0.0f, 1.0f);
  return plan;
}

BlendPlan planBlend(std::span<const HistoryFrame> history, int frames, const WeightingParams& weighting) {
  BlendPlan plan;
  if (history.empty() || frames <= 0) return plan;
  // Collect frames newest-first until a discontinuity separates them.
  for (size_t i = 0; i < history.size() && int(plan.slots.size()) < frames; ++i) {
    plan.slots.push_back(history[i].slot);
    if (history[i].discontinuityBefore) break;
  }
  const int n = int(plan.slots.size());
  // Weights are defined oldest-first (Blur's convention); reverse them to match newest-first slots.
  auto w = computeWeights(n, weighting);
  plan.weights.assign(w.rbegin(), w.rend());
  return plan;
}

}  // namespace bl
