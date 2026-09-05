#include "streamvault/session.hpp"

#include "streamvault/serialization.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace streamvault {
namespace {

constexpr std::array<char, 4> kSessionMagic{'S', 'V', 'S', '1'};
constexpr std::uint16_t kSessionVersion = 1;
constexpr std::uint32_t kFixedRecordBytes = 36;
constexpr std::uint32_t kMaximumRecordBytes = kFixedRecordBytes + 6 * sizeof(double);

void put_u16(std::ostream& out, std::uint16_t value) {
  const std::array<char, 2> bytes{static_cast<char>(value >> 8), static_cast<char>(value)};
  out.write(bytes.data(), bytes.size());
}

void put_u32(std::ostream& out, std::uint32_t value) {
  std::array<char, 4> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<char>(value >> (24 - 8 * i));
  out.write(bytes.data(), bytes.size());
}

void put_u64(std::ostream& out, std::uint64_t value) {
  std::array<char, 8> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<char>(value >> (56 - 8 * i));
  out.write(bytes.data(), bytes.size());
}

std::uint16_t get_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8) |
                                    bytes[offset + 1]);
}

std::uint32_t get_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) value = (value << 8) | bytes[offset + i];
  return value;
}

std::uint64_t get_u64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) value = (value << 8) | bytes[offset + i];
  return value;
}

std::uint32_t read_u32(std::istream& in) {
  std::array<unsigned char, 4> bytes{};
  in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  if (!in) throw std::runtime_error("truncated session record length");
  std::uint32_t value = 0;
  for (auto byte : bytes) value = (value << 8) | byte;
  return value;
}

}  // namespace

SessionWriter::SessionWriter(const std::filesystem::path& path)
    : stream_(path, std::ios::binary | std::ios::trunc) {
  if (!stream_) throw std::runtime_error("cannot open session file: " + path.string());
  stream_.write(kSessionMagic.data(), kSessionMagic.size());
  put_u16(stream_, kSessionVersion);
  put_u16(stream_, 0);
  if (!stream_) throw std::runtime_error("failed to write session header");
}

SessionWriter::~SessionWriter() {
  if (!closed_) {
    stream_.flush();
    stream_.close();
  }
}

void SessionWriter::append(const Record& record) {
  std::string error;
  if (!validate_message(record.message, error))
    throw std::invalid_argument("invalid session record: " + error);
  const auto payload_bytes = static_cast<std::uint32_t>(record.message.payload.size() * 8);
  put_u32(stream_, kFixedRecordBytes + payload_bytes);
  put_u32(stream_, record.message.source_id);
  put_u16(stream_, static_cast<std::uint16_t>(record.message.type));
  put_u16(stream_, 0);
  put_u64(stream_, record.message.sequence);
  put_u64(stream_, static_cast<std::uint64_t>(record.message.generation_ns));
  put_u64(stream_, static_cast<std::uint64_t>(record.receive_ns));
  put_u32(stream_, payload_bytes);
  for (double value : record.message.payload)
    put_u64(stream_, std::bit_cast<std::uint64_t>(value));
  if (!stream_) throw std::runtime_error("failed to write session record");
}

void SessionWriter::close() {
  if (closed_) return;
  stream_.flush();
  if (!stream_) throw std::runtime_error("failed to flush session file");
  stream_.close();
  if (stream_.fail()) throw std::runtime_error("failed to close session file");
  closed_ = true;
}

SessionReader::SessionReader(const std::filesystem::path& path)
    : stream_(path, std::ios::binary) {
  if (!stream_) throw std::runtime_error("cannot open session file: " + path.string());
  std::array<char, 8> header{};
  stream_.read(header.data(), header.size());
  if (!stream_) throw std::runtime_error("truncated session header");
  if (!std::equal(kSessionMagic.begin(), kSessionMagic.end(), header.begin()))
    throw std::runtime_error("bad session magic");
  const auto version = static_cast<std::uint16_t>(
      (static_cast<unsigned char>(header[4]) << 8) |
      static_cast<unsigned char>(header[5]));
  if (version != kSessionVersion) throw std::runtime_error("unsupported session version");
}

std::optional<Record> SessionReader::read_next() {
  if (stream_.peek() == std::char_traits<char>::eof()) {
    if (stream_.eof()) return std::nullopt;
    throw std::runtime_error("failed while reading session file");
  }
  const auto record_bytes = read_u32(stream_);
  if (record_bytes < kFixedRecordBytes || record_bytes > kMaximumRecordBytes)
    throw std::runtime_error("invalid session record length");
  std::vector<std::uint8_t> bytes(record_bytes);
  stream_.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  if (!stream_) throw std::runtime_error("truncated session record");

  Record record;
  record.message.source_id = get_u32(bytes, 0);
  record.message.type = static_cast<TelemetryType>(get_u16(bytes, 4));
  record.message.sequence = get_u64(bytes, 8);
  record.message.generation_ns = static_cast<std::int64_t>(get_u64(bytes, 16));
  record.receive_ns = static_cast<std::int64_t>(get_u64(bytes, 24));
  const auto payload_bytes = get_u32(bytes, 32);
  if (payload_bytes % 8 != 0 || record_bytes != kFixedRecordBytes + payload_bytes)
    throw std::runtime_error("invalid session payload length");
  for (std::size_t offset = kFixedRecordBytes; offset < bytes.size(); offset += 8)
    record.message.payload.push_back(std::bit_cast<double>(get_u64(bytes, offset)));
  std::string error;
  if (!validate_message(record.message, error))
    throw std::runtime_error("invalid session record: " + error);
  return record;
}

}  // namespace streamvault
