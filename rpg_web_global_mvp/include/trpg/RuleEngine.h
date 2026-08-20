#pragma once

#include <json/json.h>

#include <cstdint>
#include <memory>
#include <string>

namespace trpg {
class CoreService;

// [功能備註｜規則引擎] 註冊條件式世界規則的 DM 管理、測試、紀錄與狀態 API。
void registerRuleEngineRoutes(const std::shared_ptr<CoreService>& service);

// [功能備註｜規則引擎倒數] 結算已到期的規則倒數，並送出 RULE_COUNTDOWN_EXPIRED 事件。
Json::Value runRuleEngineTick(const std::shared_ptr<CoreService>& service,
                              std::int64_t roomId = 0);

// [功能備註｜規則引擎] 所有世界系統都可呼叫這個入口觸發規則。
// eventContext 會放在 context.event.payload；dryRun=true 時只判斷、不改資料。
Json::Value evaluateRuleEvent(const std::shared_ptr<CoreService>& service,
                              std::int64_t roomId,
                              std::int64_t actorUserId,
                              const std::string& eventType,
                              const Json::Value& eventContext,
                              bool dryRun = false);

// [功能備註｜規則引擎地圖事件] 建立統一的目標地圖節點 payload，供真人／隊伍／離線 AI 共用。
Json::Value ruleMapNodePayload(const std::shared_ptr<CoreService>& service,
                               std::int64_t roomId,
                               std::int64_t nodeId);

}  // namespace trpg
