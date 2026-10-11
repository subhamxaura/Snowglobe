#pragma once
// Shared secrets redaction (proxy storage + tracer argv). See ADR-0003.
// Thread ownership: stateless free functions; safe from any thread.
// Two independent rules:
//  1. Stored header VALUES for a fixed name set -> "REDACTED" (forwarded
//     request is untouched; that happens on a different code path).
//  2. Tokens inside free text (argv elements, URLs, paths): URL userinfo,
//     known key shapes, key=/token=/password= query params, and any token
//     equal to the value of a sensitive env var. UUIDs and git SHAs must
//     survive (unit-tested).
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace snowglobe::redact {

constexpr const char* kRedacted = "REDACTED";

// True if a header name's stored value must be redacted (case-insensitive).
bool isRedactedHeader(const std::string& name);

// Redact one header value for STORAGE (forwarding uses the original).
std::string redactHeaderValue(const std::string& name, const std::string& value);

// A sensitive environment variable: (name, value) pairs to match tokens
// against. Pass environ (char* const*) — names are filtered by
// (KEY|TOKEN|SECRET|PASSWORD|PASSWD|CREDENTIAL)$ case-insensitively and
// values shorter than 8 bytes are ignored (avoids redacting "x", "true",
// ...). Collected once per process start.
std::vector<std::pair<std::string, std::string>> sensitiveEnv(char* const* envp);

// Redact tokens inside free text using explicit patterns + env values.
// `env` is usually sensitiveEnv().
std::string redactText(const std::string& text,
                       const std::vector<std::pair<std::string, std::string>>& env);

// One shape match: the full consumed span plus the secret sub-span the
// audit reports (e.g. the token after "Bearer ", not the scheme word).
struct ShapeHit {
  size_t begin = 0; // consumed span [begin, end)
  size_t end = 0;
  size_t secretBegin = 0; // secret span [secretBegin, secretEnd)
  size_t secretEnd = 0;
  std::string replacement; // what redactText emits for [begin, end)
  const char* kind = "";   // bearer|basic|url-auth|sk|sk-ant|github|
                           // akia|slack|query-param
};

// Audit findings: shape-only (no env values — share-time env differs
// from record-time env, so values are unmatchable there by design).
struct SecretFinding {
  size_t begin = 0; // secret span, as in ShapeHit
  size_t end = 0;
  std::string kind;
};

// Scan text with the same shape table as redactText and return every
// match. What redactText would replace, auditSecrets reports.
std::vector<SecretFinding> auditSecrets(const std::string& text);

} // namespace snowglobe::redact
