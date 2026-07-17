#include "deck/environment.h"
#include "deck/app.h"
#include "deck/cli.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sstream>
#include <utility>
#include <vector>

namespace deck::test {
using TestFn = std::function<void()>;
std::vector<std::pair<std::string, TestFn>>& registry();
}  // namespace deck::test

#define DECK_TEST(name)                                                         \
  void name();                                                                  \
  namespace {                                                                   \
  const bool name##_registered = [] {                                           \
    deck::test::registry().push_back({#name, name});                            \
    return true;                                                                \
  }();                                                                          \
  }                                                                             \
  void name()

#define DECK_ASSERT(condition)                                                  \
  do {                                                                          \
    if (!(condition)) {                                                         \
      throw std::runtime_error("assertion failed: " #condition);                \
    }                                                                           \
  } while (false)

DECK_TEST(resolve_env_var_reads_workspace_dotenv) {
  const auto original_cwd = std::filesystem::current_path();
  const auto root = std::filesystem::temp_directory_path() / "deck_env_test";
  std::filesystem::create_directories(root);
  {
    std::ofstream out(root / ".env", std::ios::trunc);
    out << "# comment\n";
    out << "FINNHUB_API_KEY=test_key\n";
  }

  std::filesystem::current_path(root);
  auto value = deck::resolve_env_var(root, "FINNHUB_API_KEY");
  std::filesystem::current_path(original_cwd);
  DECK_ASSERT(value.has_value());
  DECK_ASSERT(*value == "test_key");

  std::filesystem::remove(root / ".env");
  std::filesystem::remove(root);
}

DECK_TEST(resolve_env_var_prefers_launch_directory_dotenv) {
  const auto original_cwd = std::filesystem::current_path();
  const auto launch_root = std::filesystem::temp_directory_path() / "deck_launch_env_test";
  const auto workspace_root = std::filesystem::temp_directory_path() / "deck_workspace_env_test";
  std::filesystem::create_directories(launch_root);
  std::filesystem::create_directories(workspace_root);
  {
    std::ofstream out(launch_root / ".env", std::ios::trunc);
    out << "FINNHUB_API_KEY=launch_key\n";
  }
  {
    std::ofstream out(workspace_root / ".env", std::ios::trunc);
    out << "FINNHUB_API_KEY=workspace_key\n";
  }

  std::filesystem::current_path(launch_root);
  auto value = deck::resolve_env_var(workspace_root, "FINNHUB_API_KEY");
  std::filesystem::current_path(original_cwd);

  DECK_ASSERT(value.has_value());
  DECK_ASSERT(*value == "launch_key");

  std::filesystem::remove(launch_root / ".env");
  std::filesystem::remove(workspace_root / ".env");
  std::filesystem::remove(launch_root);
  std::filesystem::remove(workspace_root);
}

DECK_TEST(detect_environment_reports_finnhub_key_source) {
  const auto original_cwd = std::filesystem::current_path();
  const auto root = std::filesystem::temp_directory_path() / "deck_env_source_test";
  std::filesystem::create_directories(root);
  {
    std::ofstream out(root / ".env", std::ios::trunc);
    out << "FINNHUB_API_KEY=source_key\n";
  }

  std::filesystem::current_path(root);
  auto caps = deck::detect_environment(root);
  std::filesystem::current_path(original_cwd);

  DECK_ASSERT(caps.finnhub_api_key);
  DECK_ASSERT(caps.finnhub_api_key_source.find(".env") != std::string::npos);

  std::filesystem::remove(root / ".env");
  std::filesystem::remove(root);
}

DECK_TEST(safe_mode_renders_read_only_summary) {
  const auto original_cwd = std::filesystem::current_path();
  const auto root = std::filesystem::temp_directory_path() / "deck_safe_mode_test";
  std::filesystem::create_directories(root);
  std::filesystem::current_path(root);

  deck::CliOptions options;
  options.safe_mode = true;
  std::ostringstream captured;
  auto* const original_buffer = std::cout.rdbuf(captured.rdbuf());
  const auto exit_code = deck::run_app(options);
  std::cout.rdbuf(original_buffer);
  std::filesystem::current_path(original_cwd);
  std::filesystem::remove(root);

  DECK_ASSERT(exit_code == 0);
  DECK_ASSERT(captured.str().find("deck safe mode (read-only summary)") != std::string::npos);
  DECK_ASSERT(captured.str().find("interactive workspace") != std::string::npos);
}

DECK_TEST(wsl_url_opener_uses_fixed_rundll32_arguments) {
  deck::EnvironmentCapabilities caps;
  caps.url_opener = "rundll32.exe";
  const auto argv = deck::url_open_argv(caps, "https://example.com/a?x=1&y=2");
  DECK_ASSERT(argv.size() == 3);
  DECK_ASSERT(argv[0] == "rundll32.exe");
  DECK_ASSERT(argv[1] == "url.dll,FileProtocolHandler");
  DECK_ASSERT(argv[2] == "https://example.com/a?x=1&y=2");
}
