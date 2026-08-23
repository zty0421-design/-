#pragma once

#include "trpg/Config.h"
#include "trpg/Security.h"

#include <drogon/drogon.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace trpg {

class CoreService : public std::enable_shared_from_this<CoreService> {
 public:
  static constexpr const char* kVersion = "60-cpp.21.3";
  static constexpr int kLegacyRouteCount = 244;

  explicit CoreService(Config config);

  void initializeDatabase();
  void registerRoutes();

  const Config& config() const noexcept { return config_; }
  drogon::orm::DbClientPtr database() const noexcept { return database_; }

  std::optional<JwtClaims> authenticateRequest(
      const drogon::HttpRequestPtr& request) const;
  std::optional<JwtClaims> authenticateToken(const std::string& token) const;
  bool userCanAccessRoom(std::int64_t userId, std::int64_t roomId) const;
  Json::Value roomSnapshot(std::int64_t roomId) const;
  Json::Value characterByUser(std::int64_t userId) const;
  void addEvent(std::int64_t roomId, std::int64_t userId,
                const std::string& type, const Json::Value& payload) const;

  static void setInstance(const std::shared_ptr<CoreService>& service);
  static std::shared_ptr<CoreService> instance();

 private:
  Config config_;
  drogon::orm::DbClientPtr database_;

  void bootstrapAdmin();
  std::optional<JwtClaims> currentUser(std::int64_t userId) const;
};

// [功能備註｜規則－戰鬥橋接] 系統／規則引擎可直接開始或結束戰鬥，與 DM 手動操作共用同一戰鬥核心。
Json::Value startBattleFromSystem(const std::shared_ptr<CoreService>& service,
                                  std::int64_t roomId,
                                  std::int64_t actorUserId,
                                  const std::string& reason,
                                  const Json::Value& sourceContext);
Json::Value endBattleFromSystem(const std::shared_ptr<CoreService>& service,
                                std::int64_t roomId,
                                std::int64_t actorUserId,
                                const std::string& reason,
                                const Json::Value& sourceContext);

}  // namespace trpg
