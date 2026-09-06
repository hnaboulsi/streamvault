#pragma once

#include <chrono>
#include <cstdint>
#include <random>

namespace streamvault {

class FaultInjector {
 public:
  FaultInjector(double drop_rate, int delay_ms, int jitter_ms, std::uint64_t seed);
  bool should_drop();
  std::chrono::milliseconds transmission_delay();

 private:
  double drop_rate_;
  int delay_ms_;
  int jitter_ms_;
  std::mt19937_64 random_;
  std::uniform_real_distribution<double> probability_{0.0, 1.0};
};

}  // namespace streamvault
