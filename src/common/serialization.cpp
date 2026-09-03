#include "streamvault/serialization.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace streamvault {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'S', 'V', 'L', 'T'};
constexpr std::uint16_t kVersion = 1;

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value >> 8));
  out.push_back(static_cast<std::uint8_t>(value));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8)
    out.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint16_t get_u16(std::span<const std::uint8_t> in, std::size_t offset) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[offset]) << 8) |
                                    in[offset + 1]);
}

std::uint32_t get_u32(std::span<const std::uint8_t> in, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) value = (value << 8) | in[offset + i];
  return value;
}

std::uint64_t get_u64(std::span<const std::uint8_t> in, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) value = (value << 8) | in[offset + i];
  return value;
}

bool known_type(TelemetryType type) { return payload_value_count(type) != 0; }

}  // namespace

bool validate_message(const TelemetryMessage& message, std::string& error) {
  if (!known_type(message.type)) {
    error = "unknown telemetry type";
    return false;
  }
  if (message.source_id != expected_source_id(message.type)) {
    error = "source ID does not match telemetry type";
    return false;
  }
  if (message.payload.size() != payload_value_count(message.type)) {
    error = "payload length does not match telemetry type";
    return false;
  }
  for (double value : message.payload) {
    if (!std::isfinite(value)) {
      error = "payload contains a non-finite value";
      return false;
    }
  }
  return true;
}

std::vector<std::uint8_t> serialize_packet(const TelemetryMessage& message) {
  std::string error;
  if (!validate_message(message, error)) throw std::invalid_argument(error);
  std::vector<std::uint8_t> bytes;
  bytes.reserve(kPacketHeaderSize + message.payload.size() * sizeof(double));
  bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
  put_u16(bytes, kVersion);
  put_u16(bytes, static_cast<std::uint16_t>(message.type));
  put_u32(bytes, message.source_id);
  put_u64(bytes, message.sequence);
  put_u64(bytes, static_cast<std::uint64_t>(message.generation_ns));
  put_u32(bytes, static_cast<std::uint32_t>(message.payload.size() * sizeof(double)));
  for (double value : message.payload) put_u64(bytes, std::bit_cast<std::uint64_t>(value));
  return bytes;
}

bool deserialize_packet(std::span<const std::uint8_t> bytes,
                        TelemetryMessage& message, std::string& error) {
  if (bytes.size() < kPacketHeaderSize) {
    error = "packet is shorter than the header";
    return false;
  }
  if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
    error = "bad packet magic";
    return false;
  }
  if (get_u16(bytes, 4) != kVersion) {
    error = "unsupported packet version";
    return false;
  }
  const auto payload_bytes = get_u32(bytes, 28);
  if (payload_bytes > 6 * sizeof(double) || payload_bytes % sizeof(double) != 0 ||
      bytes.size() != kPacketHeaderSize + payload_bytes) {
    error = "invalid packet payload length";
    return false;
  }

  TelemetryMessage decoded;
  decoded.type = static_cast<TelemetryType>(get_u16(bytes, 6));
  decoded.source_id = get_u32(bytes, 8);
  decoded.sequence = get_u64(bytes, 12);
  decoded.generation_ns = static_cast<std::int64_t>(get_u64(bytes, 20));
  for (std::size_t offset = kPacketHeaderSize; offset < bytes.size(); offset += 8) {
    decoded.payload.push_back(std::bit_cast<double>(get_u64(bytes, offset)));
  }
  if (!validate_message(decoded, error)) return false;
  message = std::move(decoded);
  return true;
}

}  // namespace streamvault
