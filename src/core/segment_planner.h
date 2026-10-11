#pragma once
#include <span>
#include <vector>

#include "core/weighting.h"

namespace bl {

// One captured frame held in the GPU history ring.
struct HistoryFrame {
  int slot = 0;                      // texture-array slice
  double time = 0.0;                 // capture timestamp, seconds
  bool discontinuityBefore = false;  // no valid motion between this frame and the previous one
};

// A piece of the shutter window between two consecutive frames (prev -> cur).
// u is the interpolation parameter inside the pair (0 = prev, 1 = cur);
// tau is the normalised position inside the whole shutter window (0 = oldest, 1 = now).
struct PlannedSegment {
  int prevSlot = 0;
  int curSlot = 0;
  float u0 = 0, u1 = 1;
  float tau0 = 0, tau1 = 1;
  int samples = 0;
};

struct FlowPlan {
  std::vector<PlannedSegment> segments;
  float coverage = 0;  // fraction of the shutter window that history covers (0..1)
  int totalSamples = 0;
};

// history: newest first. shutterSeconds: length of the causal window [newest - S, newest].
// sampleBudget: number of temporal samples for a fully-covered window (density is kept constant).
FlowPlan planFlowSegments(std::span<const HistoryFrame> history, double shutterSeconds, int sampleBudget,
                          int maxSegments);

struct BlendPlan {
  std::vector<int> slots;      // newest first
  std::vector<float> weights;  // same order, sum to 1
};

// Classic frame blending over the last `frames` frames (stops at discontinuities).
BlendPlan planBlend(std::span<const HistoryFrame> history, int frames, const WeightingParams& weighting);

}  // namespace bl
