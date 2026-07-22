#include "deck/app_support.h"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace deck {

std::string format_timestamp(std::chrono::system_clock::time_point time_point) {
  const std::time_t raw = std::chrono::system_clock::to_time_t(time_point);
  std::tm local_tm{};
#if defined(_WIN32)
  localtime_s(&local_tm, &raw);
#else
  localtime_r(&raw, &local_tm);
#endif
  std::ostringstream out;
  out << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S");
  return out.str();
}

std::string join_argv(const std::vector<std::string>& argv) {
  std::ostringstream out;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) out << ' ';
    out << argv[i];
  }
  return out.str();
}

std::string clip_text(std::string text, std::size_t limit) {
  return text.size() <= limit ? text : text.substr(0, limit) + "...";
}

void append_tail(std::string& target, const std::string& chunk, std::size_t limit) {
  target.append(chunk);
  if (target.size() > limit) target.erase(0, target.size() - limit);
}

std::vector<std::string> split_command_line(const std::string& command) {
  std::vector<std::string> argv;
  std::string current;
  bool in_single = false;
  bool in_double = false;
  bool escaping = false;
  for (char ch : command) {
    if (escaping) {
      current.push_back(ch);
      escaping = false;
    } else if (ch == '\\') {
      escaping = true;
    } else if (ch == '\'' && !in_double) {
      in_single = !in_single;
    } else if (ch == '"' && !in_single) {
      in_double = !in_double;
    } else if (std::isspace(static_cast<unsigned char>(ch)) && !in_single && !in_double) {
      if (!current.empty()) {
        argv.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) argv.push_back(current);
  return argv;
}

TaskRecord make_task_record(const std::string& name, const std::vector<std::string>& argv, bool use_pty) {
  TaskRecord record;
  record.name = name;
  record.argv = argv;
  record.use_pty = use_pty;
  record.command = join_argv(argv);
  record.state = TaskState::Starting;
  record.started_at = format_timestamp(std::chrono::system_clock::now());
  return record;
}

TaskRecord execute_task_probe(const std::string& name,
                              const ProcessRequest& request,
                              const ProcessRunner& runner) {
  auto record = make_task_record(name, request.argv);
  const auto result = runner.run(request);
  record.finished_at = format_timestamp(std::chrono::system_clock::now());
  record.exit_code = result.exit_code;
  record.stdout_excerpt = clip_text(result.stdout_text);
  record.stderr_excerpt = clip_text(result.stderr_text);
  record.timed_out = result.timed_out;
  record.cancelled = result.cancelled;
  record.state = result.cancelled ? TaskState::Cancelled
               : result.timed_out || result.exit_code != 0 ? TaskState::Failed
                                                           : TaskState::Exited;
  return record;
}

}  // namespace deck
