#pragma once

#include "streamvault/telemetry.hpp"

#include <cstdint>

namespace streamvault {

struct ProducerDefaults {
  TelemetryType type;
  std::uint32_t source_id;
  double frequency_hz;
};

int run_producer(int argc, char** argv, ProducerDefaults defaults);

}  // namespace streamvault
