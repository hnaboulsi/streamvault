#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace streamvault {

enum class TelemetryType : std::uint16_t { Imu = 1, Gps = 2, Temperature = 3 };

struct TelemetryMessage {
  std::uint32_t source_id{};
  TelemetryType type{};
  std::uint64_t sequence{};
  std::int64_t generation_ns{};
  std::vector<double> payload;

  bool operator==(const TelemetryMessage&) const = default;
};

struct Record {
  TelemetryMessage message;
  std::int64_t receive_ns{};

  bool operator==(const Record&) const = default;
};

inline constexpr std::uint32_t kImuSourceId = 1;
inline constexpr std::uint32_t kGpsSourceId = 2;
inline constexpr std::uint32_t kTemperatureSourceId = 3;

constexpr std::size_t payload_value_count(TelemetryType type) {
  switch (type) {
    case TelemetryType::Imu: return 6;
    case TelemetryType::Gps: return 2;
    case TelemetryType::Temperature: return 1;
  }
  return 0;
}

constexpr std::uint32_t expected_source_id(TelemetryType type) {
  switch (type) {
    case TelemetryType::Imu: return kImuSourceId;
    case TelemetryType::Gps: return kGpsSourceId;
    case TelemetryType::Temperature: return kTemperatureSourceId;
  }
  return 0;
}

constexpr std::string_view type_name(TelemetryType type) {
  switch (type) {
    case TelemetryType::Imu: return "imu";
    case TelemetryType::Gps: return "gps";
    case TelemetryType::Temperature: return "temperature";
  }
  return "unknown";
}

inline std::int64_t unix_time_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace streamvault
