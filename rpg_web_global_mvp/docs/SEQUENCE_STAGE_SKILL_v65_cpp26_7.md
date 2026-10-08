# 65-cpp.26.7 通用序列階段技能

## 核心規則

- `sequence_mechanic.stages[]` 依陣列順序視為第 1、2、3…階，顯示名稱完全自訂。
- 每場戰鬥本場進度從 0 開始；第一個新階段必須是第 1 階。
- `no_skip=true` 時，未使用的新階段只能首次使用 `highest_reached + 1`。
- `allow_repeat_reached=true` 時，已使用過的階段可以任意重複。
- `one_per_round=true` 時，同一戰鬥回合最多使用一次此序列技能。
- 每個角色的永久解鎖進度保存在 `class_resources.skill_sequence_<templateId>.current`；戰鬥重置不會清除。
- 本場 `highest_reached / used_stages / cycle / last_round` 存在該資源的 `sequence_mechanic` 物件。
- 完成全部階段後可依 `max_cycles` 進入下一輪，並用 `cycle_trigger_name` 顯示特殊觸發名稱。

## 血之樂章範例

預設只解鎖第一階；DM 可逐步把永久解鎖提高到第 2～5 階。第一輪完整走完五階後觸發「血之樂章 第零章 返響輪迴」，再從第 1 階推進第二輪。

顯示刀數與實際判定段數分開。為避免單次 100～250 次判定拖慢房間同步，現有戰鬥核心實際判定段數維持最多 20；例如可顯示「250刀」，但用自訂傷害公式／20 段判定表現總傷害。
