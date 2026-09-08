#pragma once

#include <json/json.h>

#include <cstdint>
#include <memory>
#include <string>

namespace trpg {
class CoreService;

// [功能備註｜規則－戰鬥橋接] 由規則引擎呼叫，將規則效果映射到戰鬥系統。
Json::Value applyRuleCombatEffect(const std::shared_ptr<CoreService>& service,
                                  const Json::Value& rule,
                                  std::int64_t roomId,
                                  std::int64_t actorUserId,
                                  const Json::Value& context,
                                  const Json::Value& effect,
                                  bool dryRun);

// [功能備註｜規則－戰鬥橋接] 檢查房間自訂勝利條件，必要時觸發規則事件／結束戰鬥。
Json::Value evaluateRuleCombatVictory(const std::shared_ptr<CoreService>& service,
                                      std::int64_t roomId,
                                      std::int64_t actorUserId,
                                      const std::string& sourceEvent);

// [功能備註｜規則－戰鬥橋接] DM 查看／修改房間的規則戰鬥狀態與勝利條件。
void registerRuleCombatBridgeRoutes(const std::shared_ptr<CoreService>& service);

}  // namespace trpg
