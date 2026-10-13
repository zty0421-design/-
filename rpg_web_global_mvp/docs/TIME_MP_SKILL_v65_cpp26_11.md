# 65-cpp.26.11 時間技能／精神力系統

## 精神力 MP
- 精神力與「精神」屬性不同，是技能施放資源。
- 上限：`最終精神 × 2 + 10`。
- `current_mental_power` 保存目前 MP；舊角色欄位為空時視為滿值。
- 技能 `data.mental_power_cost` 設定施放消耗；不足時後端拒絕施放。

## 時間／拉條技能
`data.time_control`：
- `enabled`：是否啟用。
- `action_advance`：增加目標本回合剩餘行動數，作為拉條／行動提前。
- `negative_status_recovery_rounds`：縮短有期限負面狀態的剩餘回合。
- `mental_power_restore`：恢復玩家精神力。
- 目標沿用 `combat_target_mode`；常用 `self` 或 `ally_single`。
- `turn_cost=0` 代表使用後不消耗自身行動，可表現「縮短自身回合」。

## 攻擊加成／治療效率
- `data.attack_bonus_percent`：持有技能時的所有正常攻擊傷害百分比加成。
- `data.healing_efficiency_bonus_percent`：持有技能時的治療效率加成；基礎 100%。
- 角色卡會顯示最終攻擊加成、治療效率與 MP。

## 範例
時間加速：目標=友方單體、行動提前=1、恢復加速=1、MP 消耗=8。
自我超頻：目標=自己、turn_cost=0、行動提前=1、MP 消耗=12。
治療專精：被動技能，治療效率加成=25%、MP 消耗=0。
