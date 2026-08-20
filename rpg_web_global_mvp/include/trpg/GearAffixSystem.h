#pragma once

#include <memory>

namespace trpg {
class CoreService;

// [功能備註｜裝備／詞條／洗煉] 建立通用詞條、特殊貨幣與裝備槽資料表。
void applyGearAffixMigrations(const std::shared_ptr<CoreService>& service);
// [功能備註｜裝備／詞條／洗煉] 註冊主副武器、裝備槽、詞條池、機率與洗煉 API。
void registerGearAffixRoutes(const std::shared_ptr<CoreService>& service);
}  // namespace trpg
