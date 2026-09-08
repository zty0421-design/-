# 技能系統總覽（62-cpp.23.1）

## 技能類型
- 主動技能：玩家／怪物主動施放。
- 被動技能：由戰鬥事件自動觸發。
- 反應技能：受攻擊、隊友受攻擊、閃避／防禦成功等事件觸發。
- 多段技能：一個技能可有多個 Hit，各段可使用不同倍率／公式／命中加成。
- 狀態／標記技能：可新增、移除、消耗、疊加狀態；支援來源獨立標記。
- 場地技能：效果掛在整個戰鬥，不佔角色狀態槽；可在進戰鬥時自動展開。
- 大道／怪物／Boss 技能：共用同一個事件與多段執行核心。

## 多段攻擊
- `hit_count`：簡單重複相同攻擊 N 段。
- `hit_sequence`：每段自訂傷害倍率、公式與判定設定。
- 每段依序結算，因此前一段新增的標記可影響下一段。

## Proc Mode
- `per_hit`：每段可觸發。
- `once_per_skill`：整次技能最多一次。
- `once_per_target`：每個目標最多一次。
- `per_event`：每個獨立事件一次。
- `final_hit_only`：只在最後一段觸發。

## Trigger 事件
目前事件技能核心可使用：
- `ON_SKILL_CAST / COMBAT_SKILL_USED`
- `ON_HIT`
- `ON_DAMAGE_DEALT`
- `ON_DAMAGE_TAKEN`
- `ON_HEAL_DONE / ON_HEAL_RECEIVED`
- `ON_POSITIVE_EFFECT_APPLIED`
- `ON_NEGATIVE_EFFECT_APPLIED`
- `ON_STATUS_ADDED`
- `ON_STATUS_REMOVED`
- `ON_STATUS_STACK_CHANGED`
- `ON_ENTITY_DEFEATED`
- `ON_ROUND_START`
- `ON_TURN_START`
- `ON_HIT_SEQUENCE_START / END`
- `ON_COMBAT_START`
- `ON_COMBAT_END`

## Condition
支援 AND／OR／NOT 組合，並包含：
- owner 是否為攻擊者／目標／被擊敗者
- 武器類型
- 目標狀態與最低層數
- 同節點／附近
- HP 百分比
- 機率
- context 路徑比較：eq/neq/gt/gte/lt/lte/contains 等
- `field_exists / field_active / field_not_exists`

## Effect
目前可執行：
- 新增／移除／消耗狀態
- 額外傷害
- 治療
- 增減行動數
- 發送事件
- 建立／移除場地效果

## Status
- 可疊層、設定最大層數。
- 可設定持續回合或永久到戰鬥結束／主動移除。
- 正面／負面／中性極性。
- 可有自己的 Trigger。
- 可依來源分開保存，例如不同繪師在同一敵人上的【畫像標記•攻】互不共用。
- 可修正造成傷害、受到傷害、治療量／治療效率。

## FieldEffect
- `priority`：場地優先度。
- `stack_policy`：`coexist / replace_same / replace_lower_priority / exclusive`。
- `duration_rounds=0`：持續到戰鬥結束。
- 場地可有全場傷害／治療修正與自己的 Trigger。
- 戰鬥面板可看到目前有效場地。

## 已建立的正式示例
### 畫兵成真
- 主動技能。
- `1 + 精神 × 2`。
- 3 Hit，每段獨立結算。

### 速寫標記
- 造成傷害：目標【畫像標記•攻】+1。
- 施加負面效果：目標【畫像標記•攻】+1。
- 附加正面效果：自身【畫像標記•療】+2。
- 受到傷害：自身【畫像標記•療】+2。
- 受到治療：本次治療完成後自身【畫像標記•療】+2。
- 攻標上限 20，每層僅提高施加者對該目標的傷害 10%。
- 療標上限 20，每層治療效率 +10%。

## 墨界類技能的設定方式
- 技能設為被動／特殊皆可。
- `activation_mode = auto_combat_start`。
- `field_definition` 設定名稱、優先度、覆蓋政策、持續時間、effects、triggers。
- 進入戰鬥時自動展開；戰鬥結束自動清除。
