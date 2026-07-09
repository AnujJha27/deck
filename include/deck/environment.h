#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace deck {

struct EnvironmentCapabilities {
  bool git = false;
  bool rg = false;
  bool curl = false;
  bool pdftotext = false;
  bool pdftoppm = false;
  bool kitty_graphics = false;
  bool truecolor = false;
  bool is_wsl = false;
  bool inside_tmux = false;
  bool finnhub_api_key = false;
};

EnvironmentCapabilities detect_environment(const std::filesystem::path& root);
std::optional<std::string> resolve_env_var(const std::filesystem::path& root, const std::string& key);
std::vector<std::string> render_doctor_report(const EnvironmentCapabilities& caps);

}  // namespace deck
