#pragma once

#include "streamvault/telemetry.hpp"

#include <filesystem>
#include <fstream>
#include <optional>

namespace streamvault {

class SessionWriter {
 public:
  explicit SessionWriter(const std::filesystem::path& path);
  ~SessionWriter();
  SessionWriter(const SessionWriter&) = delete;
  SessionWriter& operator=(const SessionWriter&) = delete;

  void append(const Record& record);
  void close();

 private:
  std::ofstream stream_;
  bool closed_{};
};

class SessionReader {
 public:
  explicit SessionReader(const std::filesystem::path& path);
  std::optional<Record> read_next();

 private:
  std::ifstream stream_;
};

}  // namespace streamvault
