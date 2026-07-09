#include "deck/environment.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace deck {
namespace {

bool executable_on_path(const std::string& binary) {
  const char* path = std::getenv("PATH");
  if (path == nullptr) {
    return false;
  }
  std::string_view view(path);
  std::size_t cursor = 0;
  while (cursor <= view.size()) {
    auto next = view.find(':', cursor);
    auto part = std::string(view.substr(cursor, next == std::string::npos ? std::string::npos : next - cursor));
    if (!part.empty() && std::filesystem::exists(std::filesystem::path(part) / binary)) {
      return true;
    }
    if (next == std::string::npos) {
      break;
    }
    cursor = next + 1;
  }
  return false;
}

bool file_contains(const std::filesystem::path& path, const std::string& needle) {
  std::ifstream input(path);
  if (!input) {
    return false;
  }
  std::string line;
  while (std::getline(input, line)) {
    if (line.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string trim(std::string value) {
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return {};
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

std::optional<std::string> read_dotenv_value(const std::filesystem::path& root, const std::string& key) {
  std::ifstream input(root / ".env");
  if (!input) {
    return std::nullopt;
  }

  std::string line;
  while (std::getline(input, line)) {
    auto cleaned = trim(line);
    if (cleaned.empty() || cleaned[0] == '#') {
      continue;
    }
    if (cleaned.rfind("export ", 0) == 0) {
      cleaned = trim(cleaned.substr(7));
    }
    const auto split = cleaned.find('=');
    if (split == std::string::npos) {
      continue;
    }
    auto candidate_key = trim(cleaned.substr(0, split));
    if (candidate_key != key) {
      continue;
    }
    auto value = trim(cleaned.substr(split + 1));
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
      value = value.substr(1, value.size() - 2);
    }
    if (value.empty()) {
      return std::nullopt;
    }
    return value;
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> resolve_env_var(const std::filesystem::path& root, const std::string& key) {
  if (const char* value = std::getenv(key.c_str()); value != nullptr && *value != '\0') {
    return std::string(value);
  }

  std::vector<std::filesystem::path> search_roots;
  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  if (!ec) {
    search_roots.push_back(cwd);
    if (cwd.filename() == "build" && cwd.has_parent_path()) {
      search_roots.push_back(cwd.parent_path());
    }
  }
  if (!root.empty()) {
    search_roots.push_back(root);
  }

  for (const auto& candidate_root : search_roots) {
    if (auto resolved = read_dotenv_value(candidate_root, key)) {
      return resolved;
    }
  }
  return std::nullopt;
}

EnvironmentCapabilities detect_environment(const std::filesystem::path& root) {
  EnvironmentCapabilities caps;
  caps.git = executable_on_path("git");
  caps.rg = executable_on_path("rg");
  caps.curl = executable_on_path("curl");
  caps.pdftotext = executable_on_path("pdftotext");
  caps.pdftoppm = executable_on_path("pdftoppm");
  caps.kitty_graphics = std::getenv("KITTY_WINDOW_ID") != nullptr;
  caps.truecolor = std::getenv("COLORTERM") != nullptr &&
                   std::string(std::getenv("COLORTERM")).find("truecolor") != std::string::npos;
  caps.inside_tmux = std::getenv("TMUX") != nullptr;
  caps.is_wsl = file_contains("/proc/version", "Microsoft") || file_contains("/proc/sys/kernel/osrelease", "WSL");
  caps.finnhub_api_key = resolve_env_var(root, "FINNHUB_API_KEY").has_value();
  return caps;
}

std::vector<std::string> render_doctor_report(const EnvironmentCapabilities& caps) {
  return {
      std::string("git: ") + (caps.git ? "ok" : "missing"),
      std::string("rg: ") + (caps.rg ? "ok" : "missing"),
      std::string("curl: ") + (caps.curl ? "ok" : "missing"),
      std::string("finnhub_api_key: ") + (caps.finnhub_api_key ? "present" : "missing"),
      std::string("pdftotext: ") + (caps.pdftotext ? "ok" : "missing"),
      std::string("pdftoppm: ") + (caps.pdftoppm ? "ok" : "missing"),
      std::string("kitty_graphics: ") + (caps.kitty_graphics ? "enabled" : "unavailable"),
      std::string("truecolor: ") + (caps.truecolor ? "enabled" : "unavailable"),
      std::string("wsl: ") + (caps.is_wsl ? "yes" : "no"),
      std::string("tmux: ") + (caps.inside_tmux ? "inside" : "no"),
  };
}

}  // namespace deck
