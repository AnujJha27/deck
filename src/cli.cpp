#include "deck/cli.h"

namespace deck {

CliOptions parse_cli(std::span<char*> argv) {
  CliOptions options;
  for (std::size_t i = 1; i < argv.size(); ++i) {
    std::string arg = argv[i];
    if (arg == "--safe") {
      options.safe_mode = true;
    } else if (arg == "doctor") {
      options.doctor = true;
    } else if (arg == "workspace" && i + 1 < argv.size()) {
      std::string sub = argv[++i];
      if (sub == "list") {
        options.workspace_list = true;
      } else if (sub == "open" && i + 1 < argv.size()) {
        options.workspace_open = true;
        options.workspace_path = argv[++i];
      } else if (sub == "reset-layout" && i + 1 < argv.size()) {
        options.workspace_reset_layout = true;
        options.workspace_path = argv[++i];
      }
    }
  }
  return options;
}

}  // namespace deck
