#pragma once

#include "streamvault/telemetry.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace streamvault {

inline constexpr std::size_t kPacketHeaderSize = 32;
inline constexpr std::size_t kMaxPacketSize = kPacketHeaderSize + 6 * sizeof(double);

std::vector<std::uint8_t> serialize_packet(const TelemetryMessage& message);
bool deserialize_packet(std::span<const std::uint8_t> bytes,
                        TelemetryMessage& message, std::string& error);
bool validate_message(const TelemetryMessage& message, std::string& error);

}  // namespace streamvault
