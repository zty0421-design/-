# TRPG C++ 條件式規則引擎指南（60-cpp.21）

> 用途：把玩家看到的自然語言規則書，轉成可由 C++ 自動判定的「事件 + 標籤 + 條件 + 效果」。
> 玩家文字與系統判定分開保存；規則可以透過 `rulebook_entry_id` 綁定到規則書條目。

## 1. 自動事件目錄

| 事件 | 何時觸發 | 適合的規則 |
|---|---|---|
| `PLAYER_ACTION` | 玩家提交一般行動宣告前 | 餵食、觸摸、撕地圖、報告工作人員等自由行動 |
| `PLAYER_MOVE_ATTEMPT` | 真人、隊伍、離線 AI 移動前 | 禁止進入、特定人物同行時不可進入、路線限制 |
| `PLAYER_MOVED` | 移動完成後 | 進入場景即觸發、發現異常、增加計數 |
| `PLAYER_SCENE_TICK` | 活躍房間每位玩家約每 30 秒 | 不可獨處、不可久留、附近人數、持續區域效果 |
| `SHOP_PURCHASE_ATTEMPT` | 一般商店或 NPC 商店扣款前 | 禁止購買「兔子血」、年齡限制商品 |
| `SHOP_PURCHASED` | 購買成功後 | 購買後標記、詛咒、任務進度 |
| `ITEM_USE_ATTEMPT` | 房間內使用道具生效前 | 禁止使用、特定場景才能使用 |
| `ITEM_USED` | 道具成功使用後 | 使用地圖碎片、通訊器、特殊護符後的結果 |
| `NPC_DIALOGUE_ATTEMPT` | 玩家選擇 NPC 對話前 | 黑衣工作人員不可回應、敵對 NPC 禁止交談 |
| `NPC_DIALOGUE` | NPC 對話完成後 | 記錄已交談、改變旗標、開啟後續規則 |
| `TEAM_JOINED` | 玩家加入隊伍（含離線自動接受） | 同行者規則、隊伍身份條件 |
| `TEAM_LEFT` | 玩家離開隊伍 | 走散、獨處、隊伍狀態改變 |
| `CHAT_MESSAGE` | 房間 WebSocket 訊息送出前 | 禁止回應某 NPC／某暗號、特殊通訊限制 |
| `PLAYER_ROOM_JOINED` | WebSocket 進入房間並交回玩家控制 | 登入／回歸事件、初始化規則狀態 |
| `RULE_COUNTDOWN_EXPIRED` | 規則倒數到期 | 「15 分鐘內必須完成」等逾時後果 |

### `block_action` 使用原則

要真的阻止原本操作，優先把 `block_action` 放在 `*_ATTEMPT`、`PLAYER_ACTION` 或 `CHAT_MESSAGE`。
後置事件（例如 `PLAYER_MOVED`、`SHOP_PURCHASED`）已經發生完成，適合做後果，不適合阻止原操作。

## 2. 常用 Context 路徑

- `event.type`：目前事件名稱。
- `event.payload.*`：該事件帶入的資料。
- `actor.user.*`：帳號資料。
- `actor.character.*`：角色卡資料，例如年齡、職業、屬性。
- `actor.tags`：角色實體標籤。
- `room.*`：房間狀態。
- `location.*`：玩家目前所在地圖節點。
- `location.tags`：目前場景標籤。
- `team.*`：目前隊伍。
- `state.flags`：跨規則旗標。
- `state.counters.*`：跨規則計數器。
- `state.values.*`：自訂規則值。
- `context.nearby_ally_count`：目前同位置的其他友方人數。
- 移動事件額外有 `event.payload.target_node.*`、`event.payload.target_node_tags`。
- 商店事件額外有 `event.payload.item.*`、`event.payload.item_name`。
- NPC 對話事件可使用 `event.payload.npc.*`、`event.payload.dialogue.*`。

## 3. 條件語法

組合：

```json
{"all":[CONDITION_A, CONDITION_B]}
```

```json
{"any":[CONDITION_A, CONDITION_B]}
```

```json
{"not": CONDITION_A}
```

Leaf：

```json
{"path":"location.tags","op":"contains","value":"兔子園區"}
```

支援：`exists`、`not_exists`、`truthy`、`falsy`、`eq`、`neq`、`gt`、`gte`、`lt`、`lte`、`contains`、`not_contains`、`in`、`not_in`、`regex`。

## 4. 效果語法

- `add_flag` / `remove_flag`
- `increment_counter`
- `set_value`
- `start_countdown` / `clear_countdown`
- `notify_dm`
- `log_violation`
- `block_action`
- `add_event`
- `reveal_rule`
- `add_status`
- `set_location`（C++ 會驗證目標節點屬於同一房間）

範例：

```json
[
  {"type":"log_violation"},
  {"type":"add_flag","flag":"rabbit_zone_isolation"},
  {"type":"increment_counter","key":"zoo_rule_5_violation","amount":1}
]
```

## 5. Z 市規則示範

### 「不要獨自停留在兔子園區的樹蔭下」

事件：`PLAYER_SCENE_TICK`

```json
{
  "all": [
    {"path":"location.tags","op":"contains","value":"兔子園區"},
    {"path":"location.tags","op":"contains","value":"樹蔭"},
    {"path":"context.nearby_ally_count","op":"eq","value":0}
  ]
}
```

建議效果：`log_violation` + `add_flag` + `notify_dm`；建議設 `cooldown_seconds`，避免每 30 秒重複通知。

### 「飲料店不要購買兔子血」

事件：`SHOP_PURCHASE_ATTEMPT`

```json
{
  "all": [
    {"path":"event.payload.item_name","op":"eq","value":"兔子血"}
  ]
}
```

效果可以是 `log_violation`；若規則代表系統直接禁止購買，再加 `block_action`。若設定上允許玩家犯規，就不要加 `block_action`，讓購買完成後用 `SHOP_PURCHASED` 觸發後果。

### 「18 歲以上不能購買兔子玩具」

事件：`SHOP_PURCHASE_ATTEMPT`

```json
{
  "all": [
    {"path":"actor.character.age","op":"gte","value":18},
    {"path":"event.payload.item.tags","op":"contains","value":"兔子玩具"}
  ]
}
```

效果：`block_action` + `notify_dm`（是否算違規由 DM 決定）。

## 6. DM 建規則建議流程

1. 先建立玩家看到的規則書文字。
2. 建立規則引擎條目並綁定 `rulebook_entry_id`。
3. 選觸發事件。
4. 依自然語言標出「場景／NPC／生物／道具／玩家狀態／時間／計數」等標籤。
5. 寫 `conditions`。
6. 寫 `effects`。
7. 先用 DM `dry-run` 測試。
8. 確認 condition trace 正確後再啟用 automatic。

後續自然語言規則不需要使用者自己決定標籤；可直接交給 ChatGPT，按這個格式轉換。

## 規則－戰鬥橋接（55-cpp.16）

規則引擎不只處理探索；現在可直接與 C++ 戰鬥引擎雙向聯動。

### 戰鬥會送回規則引擎的事件

- `COMBAT_STARTED`：戰鬥開始。
- `COMBAT_ROUND_STARTED`：新戰鬥輪開始。
- `COMBAT_TURN_STARTED`：切到下一個行動者。
- `COMBAT_ATTACK_RESOLVED`：一次基本攻擊完成，context 會帶命中、傷害、目標與 damage_change。
- `COMBAT_ENTITY_DEFEATED`：攻擊造成目標倒地／擊敗。
- `COMBAT_SKILL_USED`：戰鬥中使用角色技能。
- `COMBAT_ENDED`：戰鬥結束。
- `COMBAT_VICTORY_CONDITION_MET`：房間設定的特殊勝利條件達成。

### 規則可執行的戰鬥效果

- `spawn_monster`：依怪物模板生成房間怪物，可直接 `status:"encountered"`。
- `force_encounter`：讓既有房間怪物進入遭遇。
- `start_battle` / `end_battle`：由規則直接開始／結束戰鬥。
- `modify_monster`：調整房間怪物 HP、治療／傷害、規則修正。
- `set_boss_phase`：切換怪物 Boss 階段，可一併寫入 modifiers。
- `add_combat_status`：對玩家／怪物／NPC 加戰鬥狀態。
- `set_victory_condition`：改變本場戰鬥勝利條件。
- `enable_rule` / `disable_rule`：戰鬥階段可開關其他世界規則。
- `set_combat_flag` / `clear_combat_flag`：保存戰鬥旗標。
- `set_combat_value` / `modify_combat_value`：保存戰鬥數值。

### 範例：違反規則後生成怪物並開戰

```json
[
  {"type":"log_violation"},
  {"type":"spawn_monster","template_id":12,"count":2,"status":"encountered"},
  {"type":"start_battle","reason":"違反園區規則，異常兔群出現"}
]
```

### 範例：Boss 低血量切第二階段

觸發事件可用 `COMBAT_ATTACK_RESOLVED`，條件可讀 `event.payload.target`、`event.payload.damage_change`，並用：

```json
[
  {
    "type":"set_boss_phase",
    "room_monster_id":31,
    "phase":2,
    "modifiers":{"damage_percent":25,"rule_immune":"ordinary_damage"}
  },
  {"type":"enable_rule","rule_code":"ZOO-BOSS-PHASE2"}
]
```

### 特殊勝利條件

目前內建：

- `eliminate_enemies`
- `survive_rounds`
- `monster_defeated`
- `combat_flag`

例如撐過 5 輪：

```json
{"type":"survive_rounds","rounds":5,"auto_end":true}
```
