#include "streamvault/bounded_queue.hpp"
#include "streamvault/serialization.hpp"
#include "streamvault/session.hpp"
#include "streamvault/statistics.hpp"
#include "streamvault/telemetry.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

std::atomic_bool stop_requested{false};
extern "C" void request_stop(int) { stop_requested.store(true); }

struct Socket {
  int fd{-1};
  ~Socket() { if (fd >= 0) ::close(fd); }
};

struct Options {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{9000};
  std::filesystem::path output{"recordings/session.dat"};
  std::filesystem::path summary{"results/session.json"};
  std::size_t queue_capacity{4096};
  bool help{};
};

unsigned long long parse_integer(const std::string& text, const char* name) {
  std::size_t used = 0;
  try {
    const auto value = std::stoull(text, &used);
    if (used != text.size()) throw std::invalid_argument("trailing characters");
    return value;
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string("invalid value for ") + name + ": " + text);
  }
}

Options parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--help") { options.help = true; continue; }
    if (i + 1 >= argc) throw std::invalid_argument("missing value after " + argument);
    const std::string value = argv[++i];
    if (argument == "--bind") options.bind_address = value;
    else if (argument == "--port") {
      const auto parsed = parse_integer(value, "--port");
      if (parsed == 0 || parsed > 65535) throw std::invalid_argument("--port must be in [1, 65535]");
      options.port = static_cast<std::uint16_t>(parsed);
    } else if (argument == "--output") options.output = value;
    else if (argument == "--summary") options.summary = value;
    else if (argument == "--queue-capacity") {
      const auto parsed = parse_integer(value, "--queue-capacity");
      if (parsed == 0) throw std::invalid_argument("--queue-capacity must be positive");
      options.queue_capacity = static_cast<std::size_t>(parsed);
    } else throw std::invalid_argument("unknown argument: " + argument);
  }
  return options;
}

void print_help(const char* program) {
  std::cout << "Usage: " << program << " [options]\n"
            << "  --bind ADDRESS       local IPv4 address (default 127.0.0.1)\n"
            << "  --port PORT          UDP port (default 9000)\n"
            << "  --output PATH        binary session path\n"
            << "  --summary PATH       JSON summary path\n"
            << "  --queue-capacity N   bounded queue capacity (default 4096)\n"
            << "  --help               show this help\n";
}

void make_parent_directory(const std::filesystem::path& path) {
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
}

}  // namespace

int main(int argc, char** argv) {
  using namespace streamvault;
  try {
    const auto options = parse_options(argc, argv);
    if (options.help) { print_help(argv[0]); return 0; }
    make_parent_directory(options.output);
    make_parent_directory(options.summary);

    Socket socket{::socket(AF_INET, SOCK_DGRAM, 0)};
    if (socket.fd < 0) throw std::runtime_error("socket failed: " + std::string(std::strerror(errno)));
    int reuse = 1;
    if (::setsockopt(socket.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
      throw std::runtime_error("setsockopt failed: " + std::string(std::strerror(errno)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.port);
    if (::inet_pton(AF_INET, options.bind_address.c_str(), &address.sin_addr) != 1)
      throw std::invalid_argument("--bind must be a valid IPv4 address");
    if (::bind(socket.fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0)
      throw std::runtime_error("bind failed: " + std::string(std::strerror(errno)));

    stop_requested.store(false);
    auto writer = std::make_unique<SessionWriter>(options.output);
    BoundedQueue<Record> queue(options.queue_capacity);
    std::atomic_bool writer_failed{false};
    std::mutex writer_error_mutex;
    std::string writer_error;
    std::thread writer_thread([&] {
      try {
        Record record;
        while (queue.wait_pop(record)) writer->append(record);
        writer->close();
      } catch (const std::exception& error) {
        {
          std::lock_guard lock(writer_error_mutex);
          writer_error = error.what();
        }
        writer_failed.store(true);
        stop_requested.store(true);
      }
    });

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    RecorderStatistics statistics;
    statistics.queue_capacity = options.queue_capacity;
    statistics.sources[kImuSourceId] = {};
    statistics.sources[kGpsSourceId] = {};
    statistics.sources[kTemperatureSourceId] = {};
    SequenceTracker sequences;
    const auto started = std::chrono::steady_clock::now();
    std::cout << "recording UDP " << options.bind_address << ':' << options.port
              << " to " << options.output << " (Ctrl-C to stop)\n";

    std::string receive_error;
    try {
      std::array<std::uint8_t, 2048> buffer{};
      while (!stop_requested.load() && !writer_failed.load()) {
        pollfd descriptor{socket.fd, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, 200);
        if (ready < 0) {
          if (errno == EINTR) continue;
          throw std::runtime_error("poll failed: " + std::string(std::strerror(errno)));
        }
        if (ready == 0) continue;
        if (descriptor.revents & (POLLERR | POLLNVAL))
          throw std::runtime_error("UDP socket reported an error");
        if (!(descriptor.revents & POLLIN)) continue;
        const auto bytes_received =
            ::recvfrom(socket.fd, buffer.data(), buffer.size(), 0, nullptr, nullptr);
        if (bytes_received < 0) {
          if (errno == EINTR) continue;
          throw std::runtime_error("recvfrom failed: " + std::string(std::strerror(errno)));
        }
        const auto received_ns = unix_time_ns();
        TelemetryMessage message;
        std::string error;
        if (!deserialize_packet(
                std::span(buffer.data(), static_cast<std::size_t>(bytes_received)),
                message, error)) {
          ++statistics.malformed_packets;
          std::cerr << "discarding malformed packet: " << error << '\n';
          continue;
        }
        auto& source = statistics.sources[message.source_id];
        ++source.received;
        source.missing += sequences.observe(message.source_id, message.sequence);
        statistics.latency.add_ns(received_ns - message.generation_ns);
        if (!queue.try_push(Record{std::move(message), received_ns}))
          ++statistics.queue_drops;
      }
    } catch (const std::exception& error) {
      receive_error = error.what();
    }

    queue.close();
    writer_thread.join();
    statistics.queue_high_water_mark = queue.high_water_mark();
    statistics.recording_duration_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto json = statistics_json(statistics);
    std::ofstream summary(options.summary, std::ios::trunc);
    if (!summary) throw std::runtime_error("cannot open summary file: " + options.summary.string());
    summary << json;
    if (!summary) throw std::runtime_error("failed to write summary file");
    summary.close();
    if (writer_failed.load()) {
      std::lock_guard lock(writer_error_mutex);
      throw std::runtime_error("writer thread failed: " + writer_error);
    }
    if (!receive_error.empty()) throw std::runtime_error(receive_error);
    std::cout << json;
    std::cout << "session: " << options.output << "\nsummary: " << options.summary << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << argv[0] << ": " << error.what() << '\n';
    return 1;
  }
}
