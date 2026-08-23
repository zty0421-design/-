#pragma once

#include <json/json.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace trpg {
class CoreService;

// [功能備註｜世界擴充] 註冊規則書、頭像、程序地圖、通訊限制、隊伍邀請與離線 AI API。
void registerWorldExpansionRoutes(const std::shared_ptr<CoreService>& service);

// [功能備註｜自動化] 啟動元素自動生成與離線玩家 AI 接管背景循環。
void startWorldAutomation(const std::shared_ptr<CoreService>& service);

// [功能備註｜通訊] 取得同場景／通訊道具規則下可收到房間聊天的玩家 ID。
std::vector<std::int64_t> communicationAudience(
    const std::shared_ptr<CoreService>& service,
    std::int64_t roomId,
    std::int64_t senderUserId);

// [功能備註｜通訊] 判斷兩名玩家目前是否可互相通訊。
bool canUsersCommunicate(const std::shared_ptr<CoreService>& service,
                         std::int64_t roomId,
                         std::int64_t senderUserId,
                         std::int64_t receiverUserId);

// [功能備註｜通訊] 依觀看者過濾只應在特定場景／通訊網路出現的聊天事件。
Json::Value roomSnapshotForViewer(const std::shared_ptr<CoreService>& service,
                                  std::int64_t roomId,
                                  std::int64_t viewerUserId);

// [功能備註｜離線 AI] 手動執行一次離線 AI；測試與 DM 即時控制共用。
Json::Value runOfflineAiTick(const std::shared_ptr<CoreService>& service,
                             std::int64_t roomId = 0);

// [功能備註｜元素自生] 依技能／道具 effects 自動增加元素儲存量，但永不超過最終容量。
Json::Value runElementGenerationTick(const std::shared_ptr<CoreService>& service,
                                     std::int64_t userId = 0);

}  // namespace trpg
