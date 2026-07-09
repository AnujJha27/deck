#include "deck/process.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>

extern char** environ;

namespace deck {
namespace {

std::mutex spawn_cwd_mutex;

std::vector<char*> build_argv(const std::vector<std::string>& argv) {
  std::vector<char*> result;
  result.reserve(argv.size() + 1);
  for (const auto& arg : argv) {
    result.push_back(const_cast<char*>(arg.c_str()));
  }
  result.push_back(nullptr);
  return result;
}

bool set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::string read_available(int fd) {
  std::string output;
  std::array<char, 4096> buffer{};
  while (true) {
    ssize_t read_count = ::read(fd, buffer.data(), buffer.size());
    if (read_count > 0) {
      output.append(buffer.data(), static_cast<std::size_t>(read_count));
      continue;
    }
    break;
  }
  return output;
}

void close_pipe_pair(int pipe_pair[2]) {
  ::close(pipe_pair[0]);
  ::close(pipe_pair[1]);
}

struct SpawnContext {
  pid_t pid = -1;
  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
};

bool switch_cwd_for_spawn(const ProcessRequest& request, std::filesystem::path& original_cwd, std::string& error_text) {
#if defined(__GLIBC__)
  (void)request;
  (void)original_cwd;
  (void)error_text;
  return true;
#else
  std::unique_lock<std::mutex> cwd_lock(spawn_cwd_mutex);
  original_cwd = std::filesystem::current_path();
  if (!request.cwd.empty()) {
    std::error_code ec;
    std::filesystem::current_path(request.cwd, ec);
    if (ec) {
      error_text = ec.message();
      return false;
    }
  }
  cwd_lock.release();
  return true;
#endif
}

bool spawn_process(const ProcessRequest& request, SpawnContext& context, std::string& error_text) {
  if (pipe(context.stdout_pipe) != 0 || pipe(context.stderr_pipe) != 0) {
    error_text = std::strerror(errno);
    if (context.stdout_pipe[0] != -1) {
      close_pipe_pair(context.stdout_pipe);
    }
    if (context.stderr_pipe[0] != -1) {
      close_pipe_pair(context.stderr_pipe);
    }
    return false;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, context.stdout_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, context.stderr_pipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, context.stdout_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, context.stderr_pipe[0]);

#if defined(__GLIBC__)
  if (!request.cwd.empty()) {
    posix_spawn_file_actions_addchdir_np(&actions, request.cwd.c_str());
  }
#endif

  auto argv = build_argv(request.argv);

#if defined(__GLIBC__)
  int spawn_code = posix_spawnp(&context.pid, argv[0], &actions, nullptr, argv.data(), environ);
#else
  int spawn_code = 0;
  std::unique_lock<std::mutex> cwd_lock(spawn_cwd_mutex);
  auto original_cwd = std::filesystem::current_path();
  if (!request.cwd.empty()) {
    std::error_code ec;
    std::filesystem::current_path(request.cwd, ec);
    if (ec) {
      error_text = ec.message();
      posix_spawn_file_actions_destroy(&actions);
      close_pipe_pair(context.stdout_pipe);
      close_pipe_pair(context.stderr_pipe);
      return false;
    }
  }
  spawn_code = posix_spawnp(&context.pid, argv[0], &actions, nullptr, argv.data(), environ);
  if (!request.cwd.empty()) {
    std::error_code ec;
    std::filesystem::current_path(original_cwd, ec);
  }
#endif

  posix_spawn_file_actions_destroy(&actions);
  ::close(context.stdout_pipe[1]);
  ::close(context.stderr_pipe[1]);
  if (spawn_code != 0) {
    error_text = std::strerror(spawn_code);
    ::close(context.stdout_pipe[0]);
    ::close(context.stderr_pipe[0]);
    context.stdout_pipe[0] = -1;
    context.stderr_pipe[0] = -1;
    return false;
  }
  set_nonblocking(context.stdout_pipe[0]);
  set_nonblocking(context.stderr_pipe[0]);
  return true;
}

bool spawn_attached_process(const ProcessRequest& request, pid_t& pid, std::string& error_text) {
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);

#if defined(__GLIBC__)
  if (!request.cwd.empty()) {
    posix_spawn_file_actions_addchdir_np(&actions, request.cwd.c_str());
  }
#endif

  auto argv = build_argv(request.argv);

#if defined(__GLIBC__)
  const int spawn_code = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
#else
  int spawn_code = 0;
  std::unique_lock<std::mutex> cwd_lock(spawn_cwd_mutex);
  auto original_cwd = std::filesystem::current_path();
  if (!request.cwd.empty()) {
    std::error_code ec;
    std::filesystem::current_path(request.cwd, ec);
    if (ec) {
      error_text = ec.message();
      posix_spawn_file_actions_destroy(&actions);
      return false;
    }
  }
  spawn_code = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  if (!request.cwd.empty()) {
    std::error_code ec;
    std::filesystem::current_path(original_cwd, ec);
  }
#endif

  posix_spawn_file_actions_destroy(&actions);
  if (spawn_code != 0) {
    error_text = std::strerror(spawn_code);
    return false;
  }
  return true;
}

void append_and_notify(ProcessResult& result,
                       const ProcessCallbacks& callbacks,
                       int fd,
                       std::string& target,
                       bool stdout_stream) {
  auto chunk = read_available(fd);
  if (chunk.empty()) {
    return;
  }
  target.append(chunk);
  if (stdout_stream) {
    if (callbacks.on_stdout_chunk) {
      callbacks.on_stdout_chunk(chunk);
    }
  } else {
    if (callbacks.on_stderr_chunk) {
      callbacks.on_stderr_chunk(chunk);
    }
  }
}

}  // namespace

ProcessResult ProcessRunner::run(const ProcessRequest& request) const {
  return run_streaming(request, ProcessCallbacks{});
}

ProcessResult ProcessRunner::run_streaming(const ProcessRequest& request, const ProcessCallbacks& callbacks) const {
  ProcessResult result;
  if (request.argv.empty()) {
    result.stderr_text = "empty argv";
    return result;
  }
  if (request.use_pty) {
    result.stderr_text = "PTY execution is not implemented in this scaffold";
    return result;
  }

  SpawnContext context;
  if (!spawn_process(request, context, result.stderr_text)) {
    return result;
  }

  const auto deadline = request.timeout ? std::chrono::steady_clock::now() + *request.timeout
                                        : std::chrono::steady_clock::time_point::max();

  while (true) {
    append_and_notify(result, callbacks, context.stdout_pipe[0], result.stdout_text, true);
    append_and_notify(result, callbacks, context.stderr_pipe[0], result.stderr_text, false);

    int status = 0;
    pid_t waited = waitpid(context.pid, &status, WNOHANG);
    if (waited == context.pid) {
      if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
      }
      break;
    }

    if (callbacks.should_cancel && callbacks.should_cancel()) {
      result.cancelled = true;
      ::kill(context.pid, SIGKILL);
      waitpid(context.pid, nullptr, 0);
      result.exit_code = 130;
      break;
    }

    if (std::chrono::steady_clock::now() >= deadline) {
      result.timed_out = true;
      ::kill(context.pid, SIGKILL);
      waitpid(context.pid, nullptr, 0);
      result.exit_code = 124;
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }

  append_and_notify(result, callbacks, context.stdout_pipe[0], result.stdout_text, true);
  append_and_notify(result, callbacks, context.stderr_pipe[0], result.stderr_text, false);
  ::close(context.stdout_pipe[0]);
  ::close(context.stderr_pipe[0]);
  return result;
}

int ProcessRunner::run_attached(const ProcessRequest& request) const {
  if (request.argv.empty()) {
    return -1;
  }
  if (request.use_pty) {
    return -1;
  }

  pid_t pid = -1;
  std::string error_text;
  if (!spawn_attached_process(request, pid, error_text)) {
    return -1;
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return -1;
}

}  // namespace deck
