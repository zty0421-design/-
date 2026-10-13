# 65-cpp.26.6 成長裝備

## 核心規則

每一件角色裝備／武器都可獨立保存成長資料：

- `growth_value`：目前成長值。
- `growth_value_required`：本次成長所需成長值。
- `growth_value_on_use`：成功使用裝備後自動增加的成長值。
- `growth_conditions`：自訂條件陣列，每條有名稱、目前進度、需求進度。
- `growth_materials`：成長材料陣列，包含名稱、類別與數量。
- `growth_stage` / `growth_max_stage`：目前／最高成長階段。
- `growth_rank_up` / `growth_max_rank`：是否成長時自動升一品階及最高品階。

成長按鈕由 C++ 驗證：成長值、全部條件、全部材料三項同時達標才成功。材料只在成功時扣除；失敗不扣材料。成長值會扣掉本階需求，超出的值保留。

成長條件採通用進度，不寫死事件種類。例如「擊殺詭異 10 次」可保存為 3/10，DM 可按跑團實際情況修改。
