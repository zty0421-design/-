#pragma once

#include <memory>

namespace trpg {
class CoreService;

// [功能備註｜59-cpp.20 裝備／詞條／洗煉] 建立通用詞條、特殊貨幣與裝備槽資料表。
// 一般詞條、額外特殊詞條、固有效果分層；詞條池支援混合品階。
void applyGearAffixMigrations(const std::shared_ptr<CoreService>& service);
// [功能備註｜59-cpp.20 裝備／詞條／洗煉] 註冊主副武器、裝備槽、詞條池、機率與洗煉 API。
// 特殊詞條規則可由裝備等來源按機率授予人偶／其他指定類型，且不消耗一般詞條槽。
void registerGearAffixRoutes(const std::shared_ptr<CoreService>& service);
}  // namespace trpg
