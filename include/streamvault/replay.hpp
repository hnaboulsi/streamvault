#pragma once

#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace streamvault {

inline std::chrono::nanoseconds scaled_replay_offset(std::int64_t first_receive_ns,
                                                      std::int64_t receive_ns,
                                                      double speed) {
  if (!(speed > 0.0)) throw std::invalid_argument("replay speed must be positive");
  if (receive_ns < first_receive_ns)
    throw std::invalid_argument("receive timestamps are out of order");
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::duration<double, std::nano>(receive_ns - first_receive_ns) / speed);
}

}  // namespace streamvault
