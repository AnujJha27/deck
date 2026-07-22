#pragma once

#include "deck/process.h"
#include "deck/workspace.h"

#include <chrono>
#include <string>
#include <vector>

namespace deck {

std::string format_timestamp(std::chrono::system_clock::time_point time_point);
std::string join_argv(const std::vector<std::string>& argv);
std::string clip_text(std::string text, std::size_t limit = 240);
void append_tail(std::string& target, const std::string& chunk, std::size_t limit = 4096);
std::vector<std::string> split_command_line(const std::string& command);
TaskRecord make_task_record(const std::string& name, const std::vector<std::string>& argv, bool use_pty = false);
TaskRecord execute_task_probe(const std::string& name,
                              const ProcessRequest& request,
                              const ProcessRunner& runner);

}  // namespace deck
