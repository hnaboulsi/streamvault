#pragma once

#include "streamvault/telemetry.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace streamvault {

class SequenceTracker {
 public:
  std::uint64_t observe(std::uint32_t source_id, std::uint64_t sequence);

 private:
  std::map<std::uint32_t, std::uint64_t> highest_;
};

struct LatencySummary {
  std::size_t sample_count{};
  double mean_ms{};
  double min_ms{};
  double max_ms{};
  double p50_ms{};
  double p99_ms{};
};

class LatencyStatistics {
 public:
  void add_ns(std::int64_t latency_ns);
  LatencySummary summarize() const;

 private:
  std::vector<std::int64_t> samples_;
};

struct SourceStatistics {
  std::uint64_t received{};
  std::uint64_t missing{};
};

struct RecorderStatistics {
  std::map<std::uint32_t, SourceStatistics> sources;
  LatencyStatistics latency;
  std::uint64_t queue_drops{};
  std::uint64_t malformed_packets{};
  std::size_t queue_capacity{};
  std::size_t queue_high_water_mark{};
  double recording_duration_seconds{};
};

std::string statistics_json(const RecorderStatistics& statistics);

}  // namespace streamvault
