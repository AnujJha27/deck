#pragma once

#include <filesystem>
#include <span>
#include <string>

namespace deck {

struct CliOptions {
  bool safe_mode = false;
  bool doctor = false;
  bool workspace_list = false;
  bool workspace_open = false;
  bool workspace_reset_layout = false;
  std::filesystem::path workspace_path = ".";
  std::string workspace_name;
};

CliOptions parse_cli(std::span<char*> argv);

}  // namespace deck
