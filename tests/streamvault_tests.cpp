#include "streamvault/bounded_queue.hpp"
#include "streamvault/fault_injection.hpp"
#include "streamvault/replay.hpp"
#include "streamvault/serialization.hpp"
#include "streamvault/session.hpp"
#include "streamvault/statistics.hpp"
#include "streamvault/telemetry.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace streamvault;

TelemetryMessage imu_message(std::uint64_t sequence = 7) {
  return {kImuSourceId, TelemetryType::Imu, sequence, 123456789,
          {0.1, -0.2, 9.81, 0.01, 0.02, -0.03}};
}

std::filesystem::path temporary_path(const std::string& suffix) {
  static std::atomic<unsigned> counter{0};
  return std::filesystem::temp_directory_path() /
         ("streamvault_test_" + std::to_string(::getpid()) + "_" +
          std::to_string(counter++) + suffix);
}

TEST(Serialization, RoundTripsEveryTelemetryType) {
  const std::vector<TelemetryMessage> messages{
      imu_message(),
      {kGpsSourceId, TelemetryType::Gps, 8, 555, {12.5, -7.25}},
      {kTemperatureSourceId, TelemetryType::Temperature, 9, 777, {23.75}}};
  for (const auto& original : messages) {
    const auto bytes = serialize_packet(original);
    TelemetryMessage decoded;
    std::string error;
    ASSERT_TRUE(deserialize_packet(bytes, decoded, error)) << error;
    EXPECT_EQ(decoded, original);
  }
}

TEST(Serialization, RejectsMalformedPackets) {
  auto bytes = serialize_packet(imu_message());
  TelemetryMessage decoded;
  std::string error;
  EXPECT_FALSE(deserialize_packet(std::span(bytes).first(10), decoded, error));
  bytes[0] = 'X';
  EXPECT_FALSE(deserialize_packet(bytes, decoded, error));
  bytes = serialize_packet(imu_message());
  bytes[31] = 8;
  EXPECT_FALSE(deserialize_packet(bytes, decoded, error));
}

TEST(Serialization, RejectsSourceTypeMismatchAndNonFinitePayload) {
  std::string error;
  auto wrong_source = imu_message();
  wrong_source.source_id = kGpsSourceId;
  EXPECT_FALSE(validate_message(wrong_source, error));
  auto nonfinite = imu_message();
  nonfinite.payload[0] = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(validate_message(nonfinite, error));
}

TEST(BoundedQueue, RefusesOverflowAndDrainsAfterClose) {
  BoundedQueue<int> queue(2);
  EXPECT_TRUE(queue.try_push(1));
  EXPECT_TRUE(queue.try_push(2));
  EXPECT_FALSE(queue.try_push(3));
  EXPECT_EQ(queue.high_water_mark(), 2u);
  queue.close();
  int value = 0;
  EXPECT_TRUE(queue.wait_pop(value)); EXPECT_EQ(value, 1);
  EXPECT_TRUE(queue.wait_pop(value)); EXPECT_EQ(value, 2);
  EXPECT_FALSE(queue.wait_pop(value));
  EXPECT_FALSE(queue.try_push(4));
}

TEST(BoundedQueue, TransfersConcurrently) {
  BoundedQueue<int> queue(32);
  constexpr int count = 5000;
  std::vector<int> received;
  std::thread consumer([&] {
    int value;
    while (queue.wait_pop(value)) received.push_back(value);
  });
  for (int value = 0; value < count; ++value) {
    while (!queue.try_push(value)) std::this_thread::yield();
  }
  queue.close();
  consumer.join();
  ASSERT_EQ(received.size(), static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) EXPECT_EQ(received[i], i);
}

TEST(SequenceTracker, CountsOnlyForwardGaps) {
  SequenceTracker tracker;
  EXPECT_EQ(tracker.observe(1, 10), 0u);
  EXPECT_EQ(tracker.observe(1, 13), 2u);
  EXPECT_EQ(tracker.observe(1, 13), 0u);
  EXPECT_EQ(tracker.observe(1, 11), 0u);
  EXPECT_EQ(tracker.observe(1, 15), 1u);
  EXPECT_EQ(tracker.observe(2, 99), 0u);
}

TEST(Statistics, CalculatesNearestRankPercentiles) {
  LatencyStatistics statistics;
  for (int milliseconds = 1; milliseconds <= 100; ++milliseconds)
    statistics.add_ns(milliseconds * 1'000'000LL);
  const auto result = statistics.summarize();
  EXPECT_EQ(result.sample_count, 100u);
  EXPECT_DOUBLE_EQ(result.mean_ms, 50.5);
  EXPECT_DOUBLE_EQ(result.min_ms, 1.0);
  EXPECT_DOUBLE_EQ(result.max_ms, 100.0);
  EXPECT_DOUBLE_EQ(result.p50_ms, 50.0);
  EXPECT_DOUBLE_EQ(result.p99_ms, 99.0);
}

TEST(Statistics, HandlesNoLatencySamples) {
  const auto result = LatencyStatistics{}.summarize();
  EXPECT_EQ(result.sample_count, 0u);
  EXPECT_DOUBLE_EQ(result.mean_ms, 0.0);
}

TEST(Session, WritesAndReadsRecordsInOrder) {
  const auto path = temporary_path(".dat");
  const Record first{imu_message(1), 1000};
  const Record second{{kGpsSourceId, TelemetryType::Gps, 2, 900, {1.0, 2.0}}, 1100};
  {
    SessionWriter writer(path);
    writer.append(first);
    writer.append(second);
    writer.close();
  }
  SessionReader reader(path);
  EXPECT_EQ(reader.read_next(), first);
  EXPECT_EQ(reader.read_next(), second);
  EXPECT_EQ(reader.read_next(), std::nullopt);
  std::filesystem::remove(path);
}

TEST(Session, RejectsTruncatedRecord) {
  const auto path = temporary_path(".dat");
  {
    SessionWriter writer(path);
    writer.append(Record{imu_message(), 1000});
    writer.close();
  }
  const auto size = std::filesystem::file_size(path);
  std::filesystem::resize_file(path, size - 3);
  SessionReader reader(path);
  EXPECT_THROW(reader.read_next(), std::runtime_error);
  std::filesystem::remove(path);
}

TEST(Replay, ScalesOffsetsAndRejectsReordering) {
  EXPECT_EQ(scaled_replay_offset(100, 2100, 2.0), std::chrono::nanoseconds(1000));
  EXPECT_EQ(scaled_replay_offset(100, 2100, 0.5), std::chrono::nanoseconds(4000));
  EXPECT_THROW(scaled_replay_offset(100, 99, 1.0), std::invalid_argument);
  EXPECT_THROW(scaled_replay_offset(100, 200, 0.0), std::invalid_argument);
}

TEST(FaultInjection, SameSeedProducesSameDecisions) {
  FaultInjector first(0.3, 5, 3, 42);
  FaultInjector second(0.3, 5, 3, 42);
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(first.should_drop(), second.should_drop());
    EXPECT_EQ(first.transmission_delay(), second.transmission_delay());
  }
}

TEST(UdpIntegration, SendsAndDecodesLocalhostDatagram) {
  const int receiver = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(receiver, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  ASSERT_EQ(::bind(receiver, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  socklen_t length = sizeof(address);
  ASSERT_EQ(::getsockname(receiver, reinterpret_cast<sockaddr*>(&address), &length), 0);
  const int sender = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(sender, 0);
  const auto original = imu_message(123);
  const auto bytes = serialize_packet(original);
  ASSERT_EQ(::sendto(sender, bytes.data(), bytes.size(), 0,
                     reinterpret_cast<sockaddr*>(&address), sizeof(address)),
            static_cast<ssize_t>(bytes.size()));
  std::array<std::uint8_t, 256> buffer{};
  const auto received = ::recv(receiver, buffer.data(), buffer.size(), 0);
  ASSERT_EQ(received, static_cast<ssize_t>(bytes.size()));
  TelemetryMessage decoded;
  std::string error;
  EXPECT_TRUE(deserialize_packet(std::span(buffer).first(received), decoded, error)) << error;
  EXPECT_EQ(decoded, original);
  ::close(sender);
  ::close(receiver);
}

}  // namespace
