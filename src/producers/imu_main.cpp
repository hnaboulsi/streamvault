#include "streamvault/producer.hpp"

int main(int argc, char** argv) {
  return streamvault::run_producer(
      argc, argv, {streamvault::TelemetryType::Imu, streamvault::kImuSourceId, 100.0});
}
