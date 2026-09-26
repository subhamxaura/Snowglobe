#pragma once
// Thread ownership: stateless; safe from any thread.
// Minimal SHA-256 (public-domain style, no external dep per AGENTS.md §1.2).
#include <cstdint>
#include <string>
#include <vector>

namespace snowglobe::util {

std::string sha256Hex(const std::string& data);
std::string sha256Hex(const std::vector<uint8_t>& data);

}  // namespace snowglobe::util
