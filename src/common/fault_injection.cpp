#include "streamvault/fault_injection.hpp"

#include <algorithm>
#include <stdexcept>

namespace streamvault {

FaultInjector::FaultInjector(double drop_rate, int delay_ms, int jitter_ms,
                             std::uint64_t seed)
    : drop_rate_(drop_rate), delay_ms_(delay_ms), jitter_ms_(jitter_ms), random_(seed) {
  if (drop_rate < 0.0 || drop_rate > 1.0) throw std::invalid_argument("drop rate must be in [0, 1]");
  if (delay_ms < 0 || jitter_ms < 0) throw std::invalid_argument("delay and jitter must be nonnegative");
}

bool FaultInjector::should_drop() { return probability_(random_) < drop_rate_; }

std::chrono::milliseconds FaultInjector::transmission_delay() {
  int jitter = 0;
  if (jitter_ms_ > 0) {
    std::uniform_int_distribution<int> distribution(-jitter_ms_, jitter_ms_);
    jitter = distribution(random_);
  }
  return std::chrono::milliseconds(std::max(0, delay_ms_ + jitter));
}

}  // namespace streamvault
