#include "trpg/Config.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>

namespace trpg {
namespace {

std::string readEnv(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string{value};
}

std::string requireEnv(const char* name) {
  auto value = readEnv(name);
  if (value.empty()) {
    throw std::runtime_error(std::string{name} + " is required");
  }
  return value;
}

std::size_t parseSize(const char* name, std::size_t fallback,
                      std::size_t minimum, std::size_t maximum) {
  const auto raw = readEnv(name);
  if (raw.empty()) {
    return fallback;
  }
  try {
    std::size_t consumed = 0;
    const auto value = std::stoull(raw, &consumed, 10);
    if (consumed != raw.size() || value < minimum || value > maximum) {
      throw std::out_of_range{"outside accepted range"};
    }
    return static_cast<std::size_t>(value);
  } catch (const std::exception&) {
    throw std::runtime_error(std::string{name} + " has an invalid value");
  }
}

}  // namespace

Config Config::fromEnvironment() {
  Config config;
  config.databaseUrl = requireEnv("DATABASE_URL");
  config.jwtSecret = requireEnv("JWT_SECRET");
  config.adminUsername = requireEnv("ADMIN_USERNAME");
  config.adminPassword = requireEnv("ADMIN_PASSWORD");
  config.production = readEnv("NODE_ENV") == "production" ||
                      readEnv("APP_ENV") == "production";

  if (config.jwtSecret.size() < 32) {
    throw std::runtime_error("JWT_SECRET must contain at least 32 characters");
  }
  if (config.adminUsername.size() < 2 || config.adminUsername.size() > 20) {
    throw std::runtime_error("ADMIN_USERNAME must contain 2-20 characters");
  }
  if (config.adminPassword.size() < 6) {
    throw std::runtime_error("ADMIN_PASSWORD must contain at least 6 characters");
  }
  if (config.adminPassword.size() > 72 ||
      config.adminPassword.find('\0') != std::string::npos) {
    throw std::runtime_error("ADMIN_PASSWORD must be valid bcrypt input (maximum 72 bytes)");
  }

  const auto port = parseSize("PORT", 10000, 1,
                              std::numeric_limits<std::uint16_t>::max());
  config.port = static_cast<std::uint16_t>(port);
  config.httpThreads = parseSize("HTTP_THREADS", 2, 1, 64);
  config.databaseConnections = parseSize("PGPOOL_MAX", 4, 1, 64);

  const auto root = readEnv("DOCUMENT_ROOT");
  if (!root.empty()) {
    config.documentRoot = root;
  } else {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(
            std::filesystem::path{config.documentRoot} / "index.html", ec) &&
        std::filesystem::is_regular_file("index.html", ec)) {
      config.documentRoot.assign(1, '.');
    }
  }
  return config;
}

}  // namespace trpg
