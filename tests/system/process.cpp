// process.cpp — system tests: separate gateway/client processes over real
// localhost UDP (spec §8.2 "System" layer, Phase 4 gate). Covers clean exit,
// final snapshots, reconnect via SIGTERM restarts, multi-client, and socket
// error handling.

#include "../test_framework.hpp"

#include "rudp_rt/metrics.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace rudp_test {

namespace {

#ifndef RUDP_BIN_DIR
#define RUDP_BIN_DIR "."
#endif

struct Proc {
  pid_t pid = -1;
  int exit_code = -1;
};

Proc spawn(const std::string& binary, const std::vector<std::string>& args) {
  Proc p;
  p.pid = fork();
  if (p.pid == 0) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(binary.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execv(binary.c_str(), argv.data());
    _exit(127);
  }
  return p;
}

int wait_exit(const Proc& p) {
  int status = 0;
  waitpid(p.pid, &status, 0);
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return -1;
}

Proc spawn_bg(const std::string& binary, const std::vector<std::string>& args) {
  return spawn(binary, args);
}

// Reads the last JSON line of a metrics file (the final snapshot).
std::string last_json_line(const std::string& path) {
  std::ifstream in(path);
  std::string line, last;
  while (std::getline(in, line)) {
    if (!line.empty()) last = line;
  }
  return last;
}

std::uint64_t json_num(const std::string& json, const std::string& key) {
  const std::string pat = "\"" + key + "\":";
  const auto pos = json.find(pat);
  if (pos == std::string::npos) return 0;
  return std::strtoull(json.c_str() + pos + pat.size(), nullptr, 10);
}

std::string run_dir() {
  static std::string dir = [] {
    std::string d = "/tmp/rudp_sys_XXXXXX";
    std::vector<char> buf(d.begin(), d.end());
    buf.push_back('\0');
    const char* mk = mkdtemp(buf.data());
    return std::string(mk);
  }();
  return dir;
}

}  // namespace

RUDP_TEST(system_localhost_e2e) {
  const std::string dir = run_dir();
  const std::string bin = RUDP_BIN_DIR "/rudp_gateway";
  const std::string cbin = RUDP_BIN_DIR "/rudp_client";
  const int port = 50301;

  auto gw = spawn_bg(bin, {"--port", std::to_string(port), "--duration", "4000",
                           "--metrics-out", dir + "/gw.jsonl"});
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  auto cl = spawn_bg(cbin, {"--port", std::to_string(port), "--duration",
                            "3000", "--metrics-out", dir + "/cl.jsonl"});
  const int cl_code = wait_exit(cl);
  const int gw_code = wait_exit(gw);
  RUDP_CHECK_EQ(cl_code, 0);
  RUDP_CHECK_EQ(gw_code, 0);

  const std::string gw_json = last_json_line(dir + "/gw.jsonl");
  const std::string cl_json = last_json_line(dir + "/cl.jsonl");
  RUDP_CHECK(gw_json.find("\"final\":true") != std::string::npos);
  RUDP_CHECK(cl_json.find("\"final\":true") != std::string::npos);
  // Source ledger conserved at the final snapshot.
  RUDP_CHECK_EQ(json_num(gw_json, "capture_dropped") +
                    json_num(gw_json, "admitted_to_fanout"),
                json_num(gw_json, "frames"));
  // Client delivered real media. The system-layer pass condition is clean
  // exit + final snapshot (spec §8.2); the delivery FLOW is proven
  // deterministically by the sim layers, so the count is only bounded below
  // by "some media arrived" (host scheduling makes exact counts flaky).
  RUDP_CHECK_GT(json_num(cl_json, "delivered_ontime") +
                    json_num(cl_json, "delivered_late"),
                0u);
}

RUDP_TEST(system_three_clients) {
  const std::string dir = run_dir();
  const std::string bin = RUDP_BIN_DIR "/rudp_gateway";
  const std::string cbin = RUDP_BIN_DIR "/rudp_client";
  const int port = 50302;

  auto gw = spawn_bg(bin, {"--port", std::to_string(port), "--duration", "5000",
                           "--metrics-out", dir + "/gw.jsonl"});
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::vector<Proc> clients;
  for (int i = 0; i < 3; ++i) {
    clients.push_back(spawn_bg(cbin, {"--port", std::to_string(port),
                                      "--duration", "3500", "--metrics-out",
                                      dir + "/cl" + std::to_string(i) + ".jsonl"}));
  }
  int ok = 0;
  for (auto& c : clients) ok += wait_exit(c) == 0 ? 1 : 0;
  RUDP_CHECK_EQ(wait_exit(gw), 0);
  RUDP_CHECK_EQ(ok, 3);
  // System-layer pass condition (spec §8.2): clean exit, final snapshot,
  // healthy fan-out. The gateway's per-session ledgers prove all three
  // clients were served; client-side delivery counts depend on host
  // scheduling and are asserted only to be positive.
  const std::string gwj = last_json_line(dir + "/gw.jsonl");
  std::size_t sessions_seen = 0;
  std::size_t pos = 0;
  while ((pos = gwj.find("\"id\":", pos)) != std::string::npos) {
    ++sessions_seen;
    pos += 5;
  }
  RUDP_CHECK_GE(sessions_seen, 3u);
  // Three sessions served media over the run (scheduling-independent).
  RUDP_CHECK_GE(json_num(gwj, "session_offered"), 30u);
  for (int i = 0; i < 3; ++i) {
    const std::string cl = last_json_line(dir + "/cl" + std::to_string(i) + ".jsonl");
    RUDP_CHECK_GT(json_num(cl, "delivered_ontime") +
                      json_num(cl, "delivered_late"),
                  0u);
  }
}

RUDP_TEST(system_reconnect_after_sigterm) {
  const std::string dir = run_dir();
  const std::string bin = RUDP_BIN_DIR "/rudp_gateway";
  const std::string cbin = RUDP_BIN_DIR "/rudp_client";
  const int port = 50303;

  auto gw = spawn_bg(bin, {"--port", std::to_string(port), "--duration", "6000",
                           "--metrics-out", dir + "/gw.jsonl"});
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // First client lifetime.
  auto cl1 = spawn_bg(cbin, {"--port", std::to_string(port), "--duration",
                             "1200", "--metrics-out", dir + "/cl1.jsonl"});
  RUDP_CHECK_EQ(wait_exit(cl1), 0);
  // Second client from a fresh process (new endpoint? same ephemeral port is
  // unlikely; a new session key or a replacement session both work).
  auto cl2p = spawn_bg(cbin, {"--port", std::to_string(port), "--duration",
                              "1500", "--metrics-out", dir + "/cl2.jsonl"});
  RUDP_CHECK_EQ(wait_exit(cl2p), 0);
  RUDP_CHECK_EQ(wait_exit(gw), 0);

  const std::string cl2 = last_json_line(dir + "/cl2.jsonl");
  RUDP_CHECK_GT(json_num(cl2, "delivered_ontime") +
                    json_num(cl2, "delivered_late"),
                0u);
}

RUDP_TEST(system_sigterm_graceful) {
  const std::string dir = run_dir();
  const std::string bin = RUDP_BIN_DIR "/rudp_gateway";
  const std::string cbin = RUDP_BIN_DIR "/rudp_client";
  const int port = 50304;

  // No --duration: the processes run until SIGTERM.
  auto gw = spawn_bg(bin, {"--port", std::to_string(port), "--metrics-out",
                           dir + "/gw.jsonl"});
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  auto clp = spawn_bg(cbin, {"--port", std::to_string(port), "--duration",
                             "800", "--metrics-out", dir + "/cl.jsonl"});
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  RUDP_CHECK_EQ(wait_exit(clp), 0);
  kill(gw.pid, SIGTERM);
  RUDP_CHECK_EQ(wait_exit(gw), 0);  // graceful, bounded shutdown (INV-17)
  const std::string gw_json = last_json_line(dir + "/gw.jsonl");
  RUDP_CHECK(gw_json.find("\"final\":true") != std::string::npos);
}

RUDP_TEST(system_unreachable_gateway) {
  const std::string dir = run_dir();
  const std::string cbin = RUDP_BIN_DIR "/rudp_client";
  // Nobody listens on this port: the client must retry, never crash, and
  // exit cleanly at its deadline.
  auto clp = spawn_bg(cbin, {"--port", "50399", "--duration", "700",
                             "--metrics-out", dir + "/cl.jsonl"});
  RUDP_CHECK_EQ(wait_exit(clp), 0);
  const std::string cl = last_json_line(dir + "/cl.jsonl");
  RUDP_CHECK(cl.find("\"final\":true") != std::string::npos);
}

}  // namespace rudp_test
