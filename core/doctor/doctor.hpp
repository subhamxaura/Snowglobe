#pragma once
// Thread ownership: called from main thread; no internal threads.
#include <string>
#include <vector>

namespace snowglobe::doctor {

struct Capability {
  std::string name;
  bool available = false;
  std::string detail;
};

// Ordered capability table printed by `snowglobe doctor`.
std::vector<Capability> checkAll();
int printTable();

// --isolate readiness (Phase 2): one row per mechanism, probed for real
// (forked tester children, never guesses). required=false rows document
// graceful fallbacks (procfs, cgroup delegation, network). Exit 0 iff
// every required row is green, else 69. `run --isolate` prints this same
// table on setup failure.
struct IsolateRow {
  std::string name;
  bool required = true;
  bool ok = false;
  std::string detail;
};
std::vector<IsolateRow> checkIsolate();
std::string isolateReport();
int printIsolateTable();

} // namespace snowglobe::doctor
