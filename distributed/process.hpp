#pragma once
// Minimal OS-process spawn/kill/wait helper for the distributed proof.
// Windows first-class; POSIX supported.

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <signal.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace slofabric_proof {

struct Process {
#if defined(_WIN32)
  HANDLE h = nullptr;
  DWORD pid = 0;
#else
  pid_t pid = -1;
#endif
  bool valid() const {
#if defined(_WIN32)
    return h != nullptr;
#else
    return pid > 0;
#endif
  }
};

inline Process spawn(const std::string& program, const std::vector<std::string>& args) {
  Process p;
#if defined(_WIN32)
  std::string cmd = "\"" + program + "\"";
  for (auto& a : args) { cmd += " \"" + a + "\""; }
  std::vector<char> cmdline(cmd.begin(), cmd.end());
  cmdline.push_back('\0');
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  BOOL ok = ::CreateProcessA(program.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  if (ok) { p.h = pi.hProcess; p.pid = pi.dwProcessId; }
#else
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(program.c_str()));
  for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = ::fork();
  if (pid == 0) { ::execv(program.c_str(), argv.data()); _exit(127); }
  p.pid = pid;
#endif
  return p;
}

inline void kill(const Process& p) {
  if (!p.valid()) return;
#if defined(_WIN32)
  if (p.h) { ::TerminateProcess((HANDLE)p.h, 1); }
#else
  ::kill(p.pid, SIGKILL);
#endif
}

// Wait up to timeout_ms for the process to exit; capture exit code.
inline bool wait_exit(const Process& p, int timeout_ms, bool& exited, int& code) {
  exited = false; code = 0;
  if (!p.valid()) { exited = true; return true; }
#if defined(_WIN32)
  DWORD r = ::WaitForSingleObject((HANDLE)p.h, static_cast<DWORD>(timeout_ms));
  if (r == WAIT_OBJECT_0) {
    DWORD ec = 0; ::GetExitCodeProcess((HANDLE)p.h, &ec);
    exited = true; code = static_cast<int>(ec);
    return true;
  }
  return false;
#else
  auto start = std::chrono::steady_clock::now();
  while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < timeout_ms) {
    int st = 0; pid_t r = ::waitpid(p.pid, &st, WNOHANG);
    if (r == p.pid) { exited = true; code = WIFEXITED(st) ? WEXITSTATUS(st) : -1; return true; }
    if (r < 0) { exited = true; code = -1; return true; }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
#endif
}

inline bool wait(const Process& p, int timeout_ms) {
  bool exited = false; int code = 0;
  return wait_exit(p, timeout_ms, exited, code);
}

}  // namespace slofabric_proof
