#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace trpg {
class CoreService;

struct GearGrowthResult {
  bool enabled{};
  bool changed{};
  bool levelled{};
  bool rankChanged{};
  std::int64_t targetId{};
  std::int64_t level{1};
  std::int64_t xp{};
  std::int64_t xpNeeded{};
  std::int64_t totalXp{};
  std::int64_t xpGained{};
  std::int64_t maxAffixes{};
  std::string rank{"G"};
};

// [功能備註｜59-cpp.20 裝備／詞條／洗煉] 建立通用詞條、特殊貨幣與裝備槽資料表。
// 一般詞條、額外特殊詞條、固有效果分層；詞條池支援混合品階。
void applyGearAffixMigrations(const std::shared_ptr<CoreService>& service);
// [功能備註｜59-cpp.20 裝備／詞條／洗煉] 註冊主副武器、裝備槽、詞條池、機率與洗煉 API。
// 特殊詞條規則可由裝備等來源按機率授予人偶／其他指定類型，且不消耗一般詞條槽。
void registerGearAffixRoutes(const std::shared_ptr<CoreService>& service);

// [功能備註｜65-cpp.26.14 可成長性裝備]
// 成長進度保存在 affix_targets.data，由 C++ 統一計算等級、品階、詞條容量與技能傷害倍率。
void syncHeldGearTargets(const std::shared_ptr<CoreService>& service, std::int64_t userId);
GearGrowthResult grantGearGrowthXp(const std::shared_ptr<CoreService>& service,
                                   std::int64_t ownerUserId,
                                   std::int64_t targetId,
                                   std::int64_t amount);
GearGrowthResult grantHeldGearGrowthXpByIndex(const std::shared_ptr<CoreService>& service,
                                              std::int64_t ownerUserId,
                                              const std::string& targetType,
                                              std::int64_t index,
                                              std::int64_t amount);
}  // namespace trpg
