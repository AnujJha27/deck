#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace deck {

struct EnvironmentCapabilities {
  bool git = false;
  bool rg = false;
  bool vim = false;
  bool nvim = false;
  bool vscode = false;
  bool curl = false;
  bool pdftotext = false;
  bool pdftoppm = false;
  bool kitty_graphics = false;
  bool truecolor = false;
  bool is_wsl = false;
  bool python = false;
  bool sympy = false;
  std::string python_command;
  std::string url_opener;
  bool inside_tmux = false;
  bool finnhub_api_key = false;
  std::string finnhub_api_key_source;
  bool twelve_data_api_key = false;
  std::string twelve_data_api_key_source;
};

EnvironmentCapabilities detect_environment(const std::filesystem::path& root);
std::optional<std::string> resolve_env_var(const std::filesystem::path& root, const std::string& key);
std::vector<std::string> render_doctor_report(const EnvironmentCapabilities& caps);

}  // namespace deck
