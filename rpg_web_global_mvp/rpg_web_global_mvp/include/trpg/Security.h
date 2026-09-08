#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace trpg {

struct JwtClaims {
  std::int64_t id{0};
  std::string username;
  bool isAdmin{false};
  std::int64_t issuedAt{0};
  std::int64_t expiresAt{0};
};

std::string hashPassword(const std::string& password, unsigned cost = 10);
bool verifyPassword(const std::string& password, const std::string& encodedHash);

std::string signJwt(const JwtClaims& claims, const std::string& secret);
std::optional<JwtClaims> verifyJwt(const std::string& token,
                                   const std::string& secret,
                                   std::int64_t now = 0);
std::int64_t unixNow();

}  // namespace trpg
