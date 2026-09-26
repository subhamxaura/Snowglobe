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

}  // namespace snowglobe::doctor
