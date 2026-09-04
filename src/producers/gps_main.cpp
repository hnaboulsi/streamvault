#include "streamvault/producer.hpp"

int main(int argc, char** argv) {
  return streamvault::run_producer(
      argc, argv, {streamvault::TelemetryType::Gps, streamvault::kGpsSourceId, 10.0});
}
