#include "streamvault/producer.hpp"

#include "streamvault/fault_injection.hpp"
#include "streamvault/serialization.hpp"

#include <arpa/inet.h>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace streamvault {
namespace {

std::atomic_bool stop_requested{false};

extern "C" void request_stop(int) { stop_requested.store(true); }

struct Socket {
  int fd{-1};
  ~Socket() { if (fd >= 0) ::close(fd); }
};

struct Options {
  std::string host{"127.0.0.1"};
  std::uint16_t port{9000};
  double hz{};
  double drop_rate{};
  int delay_ms{};
  int jitter_ms{};
  std::uint64_t seed{1};
  bool help{};
};

template <typename T, typename Parser>
T parse_number(const std::string& text, const char* name, Parser parser) {
  std::size_t used = 0;
  try {
    auto value = parser(text, &used);
    if (used != text.size()) throw std::invalid_argument("trailing characters");
    return static_cast<T>(value);
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string("invalid value for ") + name + ": " + text);
  }
}

Options parse_options(int argc, char** argv, double default_hz) {
  Options options;
  options.hz = default_hz;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--help") { options.help = true; continue; }
    if (i + 1 >= argc) throw std::invalid_argument("missing value after " + argument);
    const std::string value = argv[++i];
    if (argument == "--host") options.host = value;
    else if (argument == "--port") {
      const auto parsed = parse_number<unsigned long>(value, "--port", [](const auto& s, auto* n) { return std::stoul(s, n); });
      if (parsed == 0 || parsed > 65535) throw std::invalid_argument("--port must be in [1, 65535]");
      options.port = static_cast<std::uint16_t>(parsed);
    } else if (argument == "--hz") options.hz = parse_number<double>(value, "--hz", [](const auto& s, auto* n) { return std::stod(s, n); });
    else if (argument == "--drop-rate") options.drop_rate = parse_number<double>(value, "--drop-rate", [](const auto& s, auto* n) { return std::stod(s, n); });
    else if (argument == "--delay-ms") options.delay_ms = parse_number<int>(value, "--delay-ms", [](const auto& s, auto* n) { return std::stol(s, n); });
    else if (argument == "--jitter-ms") options.jitter_ms = parse_number<int>(value, "--jitter-ms", [](const auto& s, auto* n) { return std::stol(s, n); });
    else if (argument == "--seed") options.seed = parse_number<std::uint64_t>(value, "--seed", [](const auto& s, auto* n) { return std::stoull(s, n); });
    else throw std::invalid_argument("unknown argument: " + argument);
  }
  if (!(options.hz > 0.0) || options.hz > 100000.0) throw std::invalid_argument("--hz must be in (0, 100000]");
  if (options.drop_rate < 0.0 || options.drop_rate > 1.0) throw std::invalid_argument("--drop-rate must be in [0, 1]");
  if (options.delay_ms < 0 || options.jitter_ms < 0) throw std::invalid_argument("delay and jitter must be nonnegative");
  return options;
}

void print_help(const char* program, double default_hz) {
  std::cout << "Usage: " << program << " [options]\n"
            << "  --host ADDRESS       destination IPv4 address (default 127.0.0.1)\n"
            << "  --port PORT          destination UDP port (default 9000)\n"
            << "  --hz RATE            generation frequency (default " << default_hz << ")\n"
            << "  --drop-rate P        intentional drop probability in [0,1]\n"
            << "  --delay-ms N         fixed delay before transmission\n"
            << "  --jitter-ms N        uniform delay jitter in [-N,N]\n"
            << "  --seed N             deterministic random seed (default 1)\n"
            << "  --help               show this help\n";
}

std::vector<double> make_payload(TelemetryType type, std::mt19937_64& random) {
  switch (type) {
    case TelemetryType::Imu: {
      std::normal_distribution<double> accel(0.0, 0.2);
      std::normal_distribution<double> gyro(0.0, 0.02);
      return {accel(random), accel(random), 9.81 + accel(random),
              gyro(random), gyro(random), gyro(random)};
    }
    case TelemetryType::Gps: {
      std::uniform_real_distribution<double> position(-100.0, 100.0);
      return {position(random), position(random)};
    }
    case TelemetryType::Temperature: {
      std::normal_distribution<double> temperature(24.0, 0.5);
      return {temperature(random)};
    }
  }
  throw std::logic_error("unsupported producer type");
}

}  // namespace

int run_producer(int argc, char** argv, ProducerDefaults defaults) {
  try {
    const auto options = parse_options(argc, argv, defaults.frequency_hz);
    if (options.help) { print_help(argv[0], defaults.frequency_hz); return 0; }

    Socket socket{::socket(AF_INET, SOCK_DGRAM, 0)};
    if (socket.fd < 0) throw std::runtime_error("socket failed: " + std::string(std::strerror(errno)));
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(options.port);
    if (::inet_pton(AF_INET, options.host.c_str(), &destination.sin_addr) != 1)
      throw std::invalid_argument("--host must be a valid IPv4 address");

    stop_requested.store(false);
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    FaultInjector faults(options.drop_rate, options.delay_ms, options.jitter_ms, options.seed);
    std::mt19937_64 payload_random(options.seed ^ 0x9e3779b97f4a7c15ULL);
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / options.hz));
    auto next = std::chrono::steady_clock::now();
    std::uint64_t sequence = 0;
    std::uint64_t sent = 0;
    std::uint64_t dropped = 0;
    std::cout << type_name(defaults.type) << " producer sending to " << options.host
              << ':' << options.port << " at " << options.hz << " Hz\n";

    while (!stop_requested.load()) {
      TelemetryMessage message{defaults.source_id, defaults.type, sequence++,
                               unix_time_ns(), make_payload(defaults.type, payload_random)};
      if (faults.should_drop()) {
        ++dropped;
      } else {
        const auto delay = faults.transmission_delay();
        if (delay.count() > 0) std::this_thread::sleep_for(delay);
        const auto bytes = serialize_packet(message);
        const auto result = ::sendto(socket.fd, bytes.data(), bytes.size(), 0,
                                     reinterpret_cast<const sockaddr*>(&destination),
                                     sizeof(destination));
        if (result < 0) throw std::runtime_error("sendto failed: " + std::string(std::strerror(errno)));
        if (static_cast<std::size_t>(result) != bytes.size()) throw std::runtime_error("sendto wrote a partial datagram");
        ++sent;
      }
      next += period;
      std::this_thread::sleep_until(next);
    }
    std::cout << "stopped: generated=" << sequence << " sent=" << sent
              << " intentionally_dropped=" << dropped << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << argv[0] << ": " << error.what() << '\n';
    return 1;
  }
}

}  // namespace streamvault
