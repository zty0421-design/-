# 互動系統總覽｜64-cpp.25.4

## 三種互動

### 說話 `speech`
- 角色實際開口。
- `CHARACTER_SPOKE`。
- `SOUND_EMITTED`。
- 不消耗戰鬥行動。
- 音量：悄聲 / 普通 / 大喊。

### 行動 `action`
- 正式角色行動。
- `PLAYER_ACTION`。
- 可設定動作聲量；有聲時再送 `SOUND_EMITTED`。
- 戰鬥時由前端在成功提交後呼叫 `/combat/pass` 消耗一次行動。

### 待機 `wait`
- 本輪不做動作。
- `PLAYER_WAITED`。
- 戰鬥時由 `/combat/pass` 消耗一次行動。

## 說話可聽範圍
- 悄聲：目前節點。
- 普通：目前節點。
- 大喊：目前節點 + 相鄰節點。
- 無地圖節點的舊房間：視為同一場景。

## 聲音與怪物
- 節點 `noise_level` 會增加。
- 怪物卡 `hearing_threshold` 判斷是否聽見。
- 聽見後提高 `room_monsters.alert_level`。

## 共用 API
`POST /api/rooms/{roomId}/actions`

核心欄位：
- `interaction_type`: `speech | action | wait`
- `text` / `action`
- `volume`: `whisper | normal | shout`
- `noise`: 0~100（行動使用）

舊 action API 欄位與回傳仍保留相容。
