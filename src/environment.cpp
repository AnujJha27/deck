#include "deck/environment.h"
#include "deck/process.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
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

struct ResolvedEnvVar {
  std::string value;
  std::string source;
};

std::vector<std::filesystem::path> dotenv_search_roots(const std::filesystem::path& root) {
  std::vector<std::filesystem::path> roots;
  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  if (!ec) {
    roots.push_back(cwd);
    if (cwd.filename() == "build" && cwd.has_parent_path()) {
      roots.push_back(cwd.parent_path());
    }
  }
  if (!root.empty()) {
    roots.push_back(root);
  }
  return roots;
}

std::vector<std::filesystem::path> dotenv_candidate_files(const std::filesystem::path& root) {
  std::vector<std::filesystem::path> files;
  std::set<std::filesystem::path> seen;
  for (const auto& search_root : dotenv_search_roots(root)) {
    auto cursor = search_root;
    while (!cursor.empty()) {
      const auto env_file = cursor / ".env";
      if (seen.insert(env_file).second) {
        files.push_back(env_file);
      }
      if (!cursor.has_parent_path() || cursor.parent_path() == cursor) {
        break;
      }
      cursor = cursor.parent_path();
    }
  }
  return files;
}

std::optional<ResolvedEnvVar> resolve_env_var_details(const std::filesystem::path& root, const std::string& key) {
  if (const char* value = std::getenv(key.c_str()); value != nullptr && *value != '\0') {
    return ResolvedEnvVar{std::string(value), "process environment"};
  }

  for (const auto& env_file : dotenv_candidate_files(root)) {
    if (auto resolved = read_dotenv_value(env_file.parent_path(), key)) {
      return ResolvedEnvVar{*resolved, env_file.string()};
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> resolve_env_var(const std::filesystem::path& root, const std::string& key) {
  if (auto resolved = resolve_env_var_details(root, key)) {
    return resolved->value;
  }
  return std::nullopt;
}

EnvironmentCapabilities detect_environment(const std::filesystem::path& root) {
  EnvironmentCapabilities caps;
  caps.git = executable_on_path("git");
  caps.rg = executable_on_path("rg");
  caps.vim = executable_on_path("vim");
  caps.nvim = executable_on_path("nvim");
  caps.vscode = executable_on_path("code");
  caps.curl = executable_on_path("curl");
  caps.pdftotext = executable_on_path("pdftotext");
  caps.pdftoppm = executable_on_path("pdftoppm");
  caps.kitty_graphics = std::getenv("KITTY_WINDOW_ID") != nullptr;
  caps.truecolor = std::getenv("COLORTERM") != nullptr &&
                   std::string(std::getenv("COLORTERM")).find("truecolor") != std::string::npos;
  caps.inside_tmux = std::getenv("TMUX") != nullptr;
  caps.is_wsl = file_contains("/proc/version", "Microsoft") || file_contains("/proc/sys/kernel/osrelease", "WSL");
  if (executable_on_path("wslview")) caps.url_opener = "wslview";
  else if (executable_on_path("xdg-open")) caps.url_opener = "xdg-open";
  else if (executable_on_path("open")) caps.url_opener = "open";
  else if (caps.is_wsl && executable_on_path("rundll32.exe")) caps.url_opener = "rundll32.exe";
  for (const auto& candidate : {root / ".venv/bin/python", root / "venv/bin/python"}) {
    if (std::filesystem::exists(candidate)) {
      caps.python = true;
      caps.python_command = candidate.string();
      break;
    }
  }
  if (!caps.python && executable_on_path("python3")) {
    caps.python = true;
    caps.python_command = "python3";
  }
  if (caps.python) {
    ProcessRequest request;
    request.argv = {caps.python_command, "-E", "-s", "-P", "-c", "import sympy"};
    request.cwd = root;
    request.timeout = std::chrono::milliseconds(8000);
    caps.sympy = ProcessRunner{}.run(request).exit_code == 0;
  }
  if (auto finnhub = resolve_env_var_details(root, "FINNHUB_API_KEY")) {
    caps.finnhub_api_key = true;
    caps.finnhub_api_key_source = finnhub->source;
  }
  if (auto twelve_data = resolve_env_var_details(root, "TWELVE_DATA_API_KEY")) {
    caps.twelve_data_api_key = true;
    caps.twelve_data_api_key_source = twelve_data->source;
  }
  return caps;
}

std::vector<std::string> render_doctor_report(const EnvironmentCapabilities& caps) {
  return {
      std::string("git: ") + (caps.git ? "ok" : "missing"),
      std::string("rg: ") + (caps.rg ? "ok" : "missing"),
      std::string("vim: ") + (caps.vim ? "ok" : "missing"),
      std::string("nvim: ") + (caps.nvim ? "ok" : "missing"),
      std::string("vscode (code): ") + (caps.vscode ? "ok" : "missing"),
      std::string("curl: ") + (caps.curl ? "ok" : "missing"),
      std::string("python: ") + (caps.python ? "ok (" + caps.python_command + ")" : "missing"),
      std::string("sympy: ") + (caps.sympy ? "ok" : "missing (optional; math tab is disabled)"),
      std::string("url_opener: ") + (caps.url_opener.empty() ? "missing" : caps.url_opener),
      std::string("finnhub_api_key: ") + (caps.finnhub_api_key ? "present" : "missing") +
          (caps.finnhub_api_key_source.empty() ? "" : " (" + caps.finnhub_api_key_source + ")"),
      std::string("twelve_data_api_key: ") + (caps.twelve_data_api_key ? "present" : "missing") +
          (caps.twelve_data_api_key_source.empty() ? "" : " (" + caps.twelve_data_api_key_source + ")"),
      std::string("pdftotext: ") + (caps.pdftotext ? "ok" : "missing"),
      std::string("pdftoppm: ") + (caps.pdftoppm ? "ok" : "missing"),
      std::string("kitty_graphics: ") + (caps.kitty_graphics ? "enabled" : "unavailable"),
      std::string("truecolor: ") + (caps.truecolor ? "enabled" : "unavailable"),
      std::string("wsl: ") + (caps.is_wsl ? "yes" : "no"),
      std::string("tmux: ") + (caps.inside_tmux ? "inside" : "no"),
  };
}

std::vector<std::string> url_open_argv(const EnvironmentCapabilities& caps,
                                       const std::string& url) {
  if (caps.url_opener.empty() || url.empty()) return {};
  if (caps.url_opener == "rundll32.exe") {
    return {caps.url_opener, "url.dll,FileProtocolHandler", url};
  }
  return {caps.url_opener, url};
}

}  // namespace deck
