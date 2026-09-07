#include "streamvault/statistics.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace streamvault {

std::uint64_t SequenceTracker::observe(std::uint32_t source_id,
                                       std::uint64_t sequence) {
  const auto found = highest_.find(source_id);
  if (found == highest_.end()) {
    highest_[source_id] = sequence;
    return 0;
  }
  if (sequence <= found->second) return 0;
  const auto difference = sequence - found->second;
  const auto gap = difference > 1 ? difference - 1 : 0;
  found->second = sequence;
  return gap;
}

void LatencyStatistics::add_ns(std::int64_t latency_ns) {
  samples_.push_back(latency_ns);
}

LatencySummary LatencyStatistics::summarize() const {
  LatencySummary result;
  result.sample_count = samples_.size();
  if (samples_.empty()) return result;
  auto sorted = samples_;
  std::sort(sorted.begin(), sorted.end());
  const long double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0L);
  const auto percentile = [&sorted](double fraction) {
    const auto rank = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(sorted.size())));
    return sorted[std::max<std::size_t>(1, rank) - 1];
  };
  constexpr double ns_to_ms = 1.0 / 1'000'000.0;
  result.mean_ms = static_cast<double>(sum / sorted.size()) * ns_to_ms;
  result.min_ms = sorted.front() * ns_to_ms;
  result.max_ms = sorted.back() * ns_to_ms;
  result.p50_ms = percentile(0.50) * ns_to_ms;
  result.p99_ms = percentile(0.99) * ns_to_ms;
  return result;
}

std::string statistics_json(const RecorderStatistics& statistics) {
  const auto latency = statistics.latency.summarize();
  std::ostringstream out;
  out << std::fixed << std::setprecision(6);
  out << "{\n"
      << "  \"recording_duration_seconds\": " << statistics.recording_duration_seconds << ",\n"
      << "  \"queue\": {\"capacity\": " << statistics.queue_capacity
      << ", \"high_water_mark\": " << statistics.queue_high_water_mark
      << ", \"drops\": " << statistics.queue_drops << "},\n"
      << "  \"malformed_packets\": " << statistics.malformed_packets << ",\n"
      << "  \"latency_ms\": {\"samples\": " << latency.sample_count
      << ", \"mean\": " << latency.mean_ms << ", \"min\": " << latency.min_ms
      << ", \"max\": " << latency.max_ms << ", \"p50\": " << latency.p50_ms
      << ", \"p99\": " << latency.p99_ms << "},\n"
      << "  \"sources\": {\n";
  bool first = true;
  for (const auto& [id, source] : statistics.sources) {
    if (!first) out << ",\n";
    first = false;
    out << "    \"" << id << "\": {\"received\": " << source.received
        << ", \"missing\": " << source.missing << "}";
  }
  out << "\n  }\n}\n";
  return out.str();
}

}  // namespace streamvault
