#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace deck {

struct ProcessRequest {
  std::vector<std::string> argv;
  std::filesystem::path cwd;
  std::map<std::string, std::string> env;
  bool use_pty = false;
  std::optional<std::chrono::milliseconds> timeout;
};

struct ProcessResult {
  int exit_code = -1;
  std::string stdout_text;
  std::string stderr_text;
  bool timed_out = false;
  bool cancelled = false;
};

struct ProcessCallbacks {
  std::function<void(const std::string&)> on_stdout_chunk;
  std::function<void(const std::string&)> on_stderr_chunk;
  std::function<bool()> should_cancel;
};

class ProcessRunner {
 public:
  ProcessResult run(const ProcessRequest& request) const;
  ProcessResult run_streaming(const ProcessRequest& request, const ProcessCallbacks& callbacks) const;
  int run_attached(const ProcessRequest& request) const;
};

}  // namespace deck
