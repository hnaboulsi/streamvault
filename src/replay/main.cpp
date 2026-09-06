#include "streamvault/session.hpp"
#include "streamvault/telemetry.hpp"
#include "streamvault/replay.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

struct Options {
  std::string path;
  double speed{1.0};
  bool maximum_speed{};
  bool help{};
};

Options parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--help") { options.help = true; continue; }
    if (argument == "--speed") {
      if (++i >= argc) throw std::invalid_argument("missing value after --speed");
      const std::string value = argv[i];
      if (value == "max") options.maximum_speed = true;
      else {
        std::size_t used = 0;
        try { options.speed = std::stod(value, &used); }
        catch (const std::exception&) { throw std::invalid_argument("invalid --speed value: " + value); }
        if (used != value.size() || !(options.speed > 0.0))
          throw std::invalid_argument("--speed must be positive or 'max'");
      }
    } else if (!argument.empty() && argument[0] == '-') {
      throw std::invalid_argument("unknown argument: " + argument);
    } else if (options.path.empty()) options.path = argument;
    else throw std::invalid_argument("only one session path may be supplied");
  }
  if (!options.help && options.path.empty()) throw std::invalid_argument("missing session path");
  return options;
}

void print_help(const char* program) {
  std::cout << "Usage: " << program << " SESSION [--speed RATE|max]\n"
            << "Replay records using their original relative receive timing.\n";
}

void print_record(const streamvault::Record& record) {
  std::cout << streamvault::type_name(record.message.type)
            << " source=" << record.message.source_id
            << " sequence=" << record.message.sequence
            << " generation_ns=" << record.message.generation_ns
            << " receive_ns=" << record.receive_ns << " values=[";
  for (std::size_t i = 0; i < record.message.payload.size(); ++i) {
    if (i) std::cout << ", ";
    std::cout << std::fixed << std::setprecision(4) << record.message.payload[i];
  }
  std::cout << "]\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse_options(argc, argv);
    if (options.help) { print_help(argv[0]); return 0; }
    streamvault::SessionReader reader(options.path);
    const auto wall_start = std::chrono::steady_clock::now();
    std::int64_t first_receive_ns = 0;
    std::int64_t previous_receive_ns = 0;
    std::uint64_t records = 0;
    while (auto record = reader.read_next()) {
      if (records == 0) {
        first_receive_ns = record->receive_ns;
      } else if (record->receive_ns < previous_receive_ns) {
        throw std::runtime_error("session records are not in receive-time order");
      }
      if (!options.maximum_speed) {
        const auto scaled = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            streamvault::scaled_replay_offset(first_receive_ns, record->receive_ns,
                                              options.speed));
        std::this_thread::sleep_until(wall_start + scaled);
      }
      print_record(*record);
      previous_receive_ns = record->receive_ns;
      ++records;
    }
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - wall_start).count();
    std::cout << "replay complete: records=" << records << " elapsed_seconds="
              << std::fixed << std::setprecision(3) << elapsed << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << argv[0] << ": " << error.what() << '\n';
    return 1;
  }
}
