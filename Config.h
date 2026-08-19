#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace trpg {

struct Config {
  std::string databaseUrl;
  std::string jwtSecret;
  std::string adminUsername;
  std::string adminPassword;
  std::string documentRoot{"./public"};
  std::uint16_t port{10000};
  std::size_t httpThreads{2};
  std::size_t databaseConnections{4};
  bool production{false};

  static Config fromEnvironment();
};

}  // namespace trpg
