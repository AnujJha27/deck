#include "deck/app_support.h"
#include "deck/process.h"

#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
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

DECK_TEST(process_runner_streams_non_pty_output) {
  deck::ProcessRunner runner;
  deck::ProcessRequest request;
  request.argv = {"sh", "-lc", "printf 'hello'; printf 'err' >&2"};

  auto result = runner.run(request);

  DECK_ASSERT(result.exit_code == 0);
  DECK_ASSERT(result.stdout_text == "hello");
  DECK_ASSERT(result.stderr_text == "err");
}

DECK_TEST(process_runner_supports_pty_output) {
  deck::ProcessRunner runner;
  deck::ProcessRequest request;
  request.argv = {"sh", "-lc", "printf 'pty-ok'"};
  request.use_pty = true;

  auto result = runner.run(request);

  DECK_ASSERT(result.exit_code == 0);
  DECK_ASSERT(result.stdout_text.find("pty-ok") != std::string::npos);
  DECK_ASSERT(result.stderr_text.empty());
}

DECK_TEST(process_runner_writes_stdin_for_pipe_processes) {
  deck::ProcessRunner runner;
  deck::ProcessRequest request;
  request.argv = {"sh", "-lc", "cat"};
  request.stdin_text = std::string("patch-body");

  auto result = runner.run(request);

  DECK_ASSERT(result.exit_code == 0);
  DECK_ASSERT(result.stdout_text == "patch-body");
}

DECK_TEST(process_runner_cancels_pty_tasks) {
  deck::ProcessRunner runner;
  deck::ProcessRequest request;
  request.argv = {"sh", "-lc", "sleep 5"};
  request.use_pty = true;

  auto result = runner.run_streaming(
      request,
      deck::ProcessCallbacks{
          .should_cancel =
              [started = std::chrono::steady_clock::now()] {
                return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(150);
              },
      });

  DECK_ASSERT(result.cancelled);
  DECK_ASSERT(result.exit_code == 130);
}

DECK_TEST(app_support_splits_quoted_commands_and_bounds_output) {
  const auto argv = deck::split_command_line("git commit -m \"calm checkpoint\"");
  DECK_ASSERT(argv.size() == 4);
  DECK_ASSERT(argv[3] == "calm checkpoint");
  std::string output = "old";
  deck::append_tail(output, "-new", 5);
  DECK_ASSERT(output == "d-new");
}
