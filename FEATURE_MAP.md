# TRPG C++ 功能修改索引

> 這份檔案是給後續修改用的導航。程式內也有 `[功能備註｜...]`、`[前端功能備註｜...]`、`[WebSocket功能備註｜...]` 可直接全文搜尋。

## 最快修改方式

1. 先在本檔搜尋功能名稱，例如「商店」「大道」「戰鬥」。
2. 找到對應 API 與後端檔案。
3. 前端要改顯示或按鈕時，在 `index.html` 搜尋對應的 `[前端功能備註｜分類]` 或函式名。
4. 改完執行 `python3 release_check.py`，避免 API、JavaScript 或平鋪部署結構出錯。

## HTTP API → 功能 → 程式位置

| 功能 | API | 主要後端位置 | 修改提醒 |
|---|---|---|---|
| 系統狀態 | `GET /api/health` | `CoreService.cpp` | Render 健康檢查與資料庫連線狀態。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 登入／帳號 | `POST /api/auth/register` | `CoreService.cpp` | 建立帳號並初始化角色卡。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 登入／帳號 | `POST /api/auth/login` | `CoreService.cpp` | 處理玩家／DM 登入與 JWT 簽發。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| DM 權限 | `GET /api/admin/status` | `CoreService.cpp` | 讀取/顯示DM 權限相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 登入／帳號 | `GET /api/me` | `CoreService.cpp` | 讀取/顯示登入／帳號相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 角色卡 | `GET /api/character` | `CoreService.cpp` | 讀取目前登入玩家的完整角色卡。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 角色卡 | `POST /api/character` | `CoreService.cpp` | 建立/執行角色卡相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 角色卡 | `PATCH /api/character` | `CoreService.cpp` | 玩家修改允許自行調整的角色資料與屬性上限。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 大道／信仰資源 | `PATCH /api/character/fifth-resource` | `LegacyCompat.cpp` | 修改大道／信仰資源相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 大道／信仰資源 | `PATCH /api/admin/players/:id/fifth-resource` | `LegacyCompat.cpp` | 修改大道／信仰資源相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 技能 | `POST /api/character/skill` | `LegacyCompat.cpp` | 建立/執行技能相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 職業 | `GET /api/professions` | `LegacyCompat.cpp` | 讀取/顯示職業相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 職業 | `POST /api/character/professions/acquire` | `LegacyCompat.cpp` | 建立/執行職業相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 技能 | `GET /api/skill-templates` | `LegacyCompat.cpp` | 讀取/顯示技能相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 召喚物 | `GET /api/summon-templates` | `LegacyCompat.cpp` | 讀取/顯示召喚物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 道具／裝備／背包 | `POST /api/character/gear/:kind/:index/use` | `LegacyCompat.cpp` | 建立/執行道具／裝備／背包相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 配方／製作 | `GET /api/recipes` | `LegacyCompat.cpp` | 讀取/顯示配方／製作相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 配方／製作 | `POST /api/recipes/:id/craft` | `LegacyCompat.cpp` | 建立/執行配方／製作相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 道具／裝備／背包 | `POST /api/character/inventory-slot` | `LegacyCompat.cpp` | 建立/執行道具／裝備／背包相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `GET /api/game-room` | `LegacyCompat.cpp` | 讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/game-room/enter` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 商店 | `GET /api/shop` | `LegacyCompat.cpp` | 讀取/顯示商店相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 商店 | `GET /api/admin/shop` | `LegacyCompat.cpp` | 讀取/顯示商店相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 商店 | `POST /api/admin/shop` | `LegacyCompat.cpp` | 建立/執行商店相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 商店 | `PATCH /api/admin/shop/:id` | `LegacyCompat.cpp` | 修改商店相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 商店 | `DELETE /api/admin/shop/:id` | `LegacyCompat.cpp` | 刪除商店相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 商店 | `POST /api/shop/:id/buy` | `LegacyCompat.cpp` | 建立/執行商店相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 技能 | `POST /api/character/skills/from-template` | `LegacyCompat.cpp` | 建立/執行技能相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 技能 | `DELETE /api/character/skills/:skillId` | `LegacyCompat.cpp` | 刪除技能相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 角色卡 | `PATCH /api/character/class-resource` | `LegacyCompat.cpp` | 修改角色卡相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 技能 | `POST /api/character/skills/:skillId/use` | `LegacyCompat.cpp` | 建立/執行技能相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 召喚物 | `GET /api/character/summons` | `LegacyCompat.cpp` | 讀取/顯示召喚物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 技能 | `POST /api/character/skills/:skillId/summon` | `LegacyCompat.cpp` | 建立/執行技能相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 技能 | `POST /api/character/summons/:id/skills/:skillTemplateId/use` | `LegacyCompat.cpp` | 建立/執行技能相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 召喚物 | `POST /api/character/summons/:id/dismiss` | `LegacyCompat.cpp` | 建立/執行召喚物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 其他系統 | `GET /api/admin/extra-skills` | `LegacyCompat.cpp` | 讀取/顯示其他系統相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 其他系統 | `POST /api/admin/extra-skills` | `LegacyCompat.cpp` | 建立/執行其他系統相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 召喚物 | `GET /api/admin/summon-templates` | `LegacyCompat.cpp` | 讀取/顯示召喚物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 召喚物 | `POST /api/admin/summon-templates` | `LegacyCompat.cpp` | 建立/執行召喚物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 召喚物 | `PATCH /api/admin/summon-templates/:id` | `LegacyCompat.cpp` | 修改召喚物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 召喚物 | `DELETE /api/admin/summon-templates/:id` | `LegacyCompat.cpp` | 刪除召喚物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 配方／製作 | `GET /api/admin/recipes` | `LegacyCompat.cpp` | 讀取/顯示配方／製作相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 配方／製作 | `POST /api/admin/recipes` | `LegacyCompat.cpp` | 建立/執行配方／製作相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 配方／製作 | `DELETE /api/admin/recipes/:id` | `LegacyCompat.cpp` | 刪除配方／製作相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 玩家管理 | `POST /api/admin/players/:id/professions/:professionId/level-up` | `LegacyCompat.cpp` | 建立/執行玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 血脈 | `GET /api/bloodlines` | `LegacyCompat.cpp` | 讀取/顯示血脈相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 血脈 | `GET /api/admin/bloodlines` | `LegacyCompat.cpp` | 讀取/顯示血脈相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 血脈 | `POST /api/admin/bloodlines` | `LegacyCompat.cpp` | 建立/執行血脈相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 血脈 | `PATCH /api/admin/bloodlines/:id` | `LegacyCompat.cpp` | 修改血脈相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 血脈 | `DELETE /api/admin/bloodlines/:id` | `LegacyCompat.cpp` | 刪除血脈相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 職業 | `POST /api/admin/professions` | `LegacyCompat.cpp` | 建立/執行職業相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 職業 | `PATCH /api/admin/professions/:id` | `LegacyCompat.cpp` | 修改職業相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 職業 | `DELETE /api/admin/professions/:id` | `LegacyCompat.cpp` | 刪除職業相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 技能 | `POST /api/admin/skill-templates` | `LegacyCompat.cpp` | 建立/執行技能相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 技能 | `PATCH /api/admin/skill-templates/:id` | `LegacyCompat.cpp` | 修改技能相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 技能 | `DELETE /api/admin/skill-templates/:id` | `LegacyCompat.cpp` | 刪除技能相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| DM 權限 | `GET /api/admin/me` | `LegacyCompat.cpp` | 讀取/顯示DM 權限相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 角色卡 | `GET /api/admin/characters` | `CoreService.cpp` | 讀取/顯示角色卡相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 角色卡 | `GET /api/admin/characters/:id` | `LegacyCompat.cpp` | 讀取/顯示角色卡相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 角色卡 | `PATCH /api/admin/characters/:id` | `LegacyCompat.cpp` | 修改角色卡相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 角色卡 | `DELETE /api/admin/characters/:id` | `LegacyCompat.cpp` | 刪除角色卡相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 其他系統 | `GET /api/admin/users` | `CoreService.cpp` | 讀取/顯示其他系統相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 玩家管理 | `GET /api/admin/players` | `LegacyCompat.cpp` | 讀取/顯示玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 玩家管理 | `GET /api/admin/players/:id` | `LegacyCompat.cpp` | 讀取/顯示玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 玩家管理 | `PATCH /api/admin/players/:id/character` | `LegacyCompat.cpp` | 修改玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 玩家管理 | `PATCH /api/admin/players/:id/class-resource` | `LegacyCompat.cpp` | 修改玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 玩家管理 | `DELETE /api/admin/players/:id/character` | `LegacyCompat.cpp` | 刪除玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 其他系統 | `DELETE /api/admin/users/:id` | `CoreService.cpp` | 刪除其他系統相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `GET /api/admin/rooms` | `CoreService.cpp` | 讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `GET /api/admin/rooms/:id` | `CoreService.cpp` | 讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `GET /api/admin/rooms/:id/characters` | `LegacyCompat.cpp` | 讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/admin/rooms/:id/close` | `CoreService.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/admin/rooms/:id/reopen` | `CoreService.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `DELETE /api/admin/rooms/:id` | `CoreService.cpp` | 刪除房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms` | `CoreService.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/join` | `CoreService.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `GET /api/rooms` | `CoreService.cpp` | 讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `GET /api/rooms/:id` | `CoreService.cpp` | 讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `GET /api/rooms/:id/characters` | `CoreService.cpp` | 讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/start` | `CoreService.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `PATCH /api/rooms/:id/scene` | `LegacyCompat.cpp` | 修改房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/time/advance` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `GET /api/rooms/:id/time-view` | `LegacyCompat.cpp` | 讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 戰鬥／回合 | `PATCH /api/rooms/:id/combat/reaction` | `CoreService.cpp` | 修改戰鬥／回合相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 戰鬥／回合 | `POST /api/rooms/:id/combat/pass` | `CoreService.cpp` | 建立/執行戰鬥／回合相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 戰鬥／回合 | `POST /api/rooms/:id/combat/basic-attack` | `CoreService.cpp` | 建立/執行戰鬥／回合相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 戰鬥／回合 | `POST /api/rooms/:id/combat/escape` | `LegacyCompat.cpp` | 建立/執行戰鬥／回合相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 怪物 | `POST /api/admin/rooms/:id/combat/monster-act` | `LegacyCompat.cpp` | 建立/執行怪物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 戰鬥／回合 | `POST /api/admin/rooms/:id/combat/ai-act` | `CoreService.cpp` | 建立/執行戰鬥／回合相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 戰鬥／回合 | `POST /api/admin/rooms/:id/combat/revive` | `LegacyCompat.cpp` | 建立/執行戰鬥／回合相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 戰鬥／回合 | `POST /api/rooms/:id/battle/start` | `CoreService.cpp` | 建立/執行戰鬥／回合相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 戰鬥／回合 | `POST /api/rooms/:id/battle/end` | `CoreService.cpp` | 建立/執行戰鬥／回合相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/resources/rest` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 戰鬥／回合 | `POST /api/rooms/:id/next-turn` | `CoreService.cpp` | 建立/執行戰鬥／回合相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/save` | `CoreService.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/leave` | `CoreService.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/reopen` | `CoreService.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `PATCH /api/rooms/:id/member/:userId` | `LegacyCompat.cpp` | 修改房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 怪物 | `GET /api/admin/monster-templates` | `CoreService.cpp` | 讀取/顯示怪物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| NPC | `GET /api/admin/npc-templates` | `LegacyCompat.cpp` | 讀取/顯示NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 副本 | `GET /api/admin/dungeon-templates` | `LegacyCompat.cpp` | 讀取/顯示副本相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 怪物 | `POST /api/admin/monster-templates` | `CoreService.cpp` | 建立/執行怪物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 怪物 | `PATCH /api/admin/monster-templates/:id` | `CoreService.cpp` | 修改怪物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 怪物 | `DELETE /api/admin/monster-templates/:id` | `CoreService.cpp` | 刪除怪物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| NPC | `POST /api/admin/npc-templates` | `LegacyCompat.cpp` | 建立/執行NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| NPC | `PATCH /api/admin/npc-templates/:id` | `LegacyCompat.cpp` | 修改NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| NPC | `DELETE /api/admin/npc-templates/:id` | `LegacyCompat.cpp` | 刪除NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 副本 | `POST /api/admin/dungeon-templates` | `LegacyCompat.cpp` | 建立/執行副本相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 副本 | `PATCH /api/admin/dungeon-templates/:id` | `LegacyCompat.cpp` | 修改副本相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 副本 | `DELETE /api/admin/dungeon-templates/:id` | `LegacyCompat.cpp` | 刪除副本相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `GET /api/rooms/:id/world` | `LegacyCompat.cpp` | 讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 怪物 | `POST /api/admin/rooms/:id/monsters` | `CoreService.cpp` | 建立/執行怪物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 怪物 | `POST /api/admin/rooms/:id/monsters/:monsterId/encounter` | `CoreService.cpp` | 建立/執行怪物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 怪物 | `DELETE /api/admin/rooms/:id/monsters/:monsterId` | `CoreService.cpp` | 刪除怪物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| NPC | `POST /api/admin/rooms/:id/npcs` | `LegacyCompat.cpp` | 建立/執行NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| NPC | `PATCH /api/admin/rooms/:id/npcs/:templateId/combat-side` | `LegacyCompat.cpp` | 修改NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| NPC | `DELETE /api/admin/rooms/:id/npcs/:templateId` | `LegacyCompat.cpp` | 刪除NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 副本 | `POST /api/admin/rooms/:id/dungeons` | `LegacyCompat.cpp` | 建立/執行副本相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 副本 | `DELETE /api/admin/rooms/:id/dungeons/:templateId` | `LegacyCompat.cpp` | 刪除副本相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 怪物 | `POST /api/rooms/:id/monsters/:monsterId/investigate` | `LegacyCompat.cpp` | 建立/執行怪物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/ai/step` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| NPC | `GET /api/game-room/npcs` | `LegacyCompat.cpp` | 讀取/顯示NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 召喚物 | `POST /api/rooms/:id/summons/:summonId/mount` | `LegacyCompat.cpp` | 建立/執行召喚物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 召喚物 | `POST /api/rooms/:id/summons/unmount` | `LegacyCompat.cpp` | 建立/執行召喚物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/charge/cancel` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 任務 | `GET /api/tasks/public` | `LegacyCompat.cpp` | 讀取/顯示任務相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 任務 | `GET /api/tasks/my` | `LegacyCompat.cpp` | 讀取/顯示任務相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 任務 | `POST /api/tasks/:id/accept` | `LegacyCompat.cpp` | 建立/執行任務相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 任務 | `GET /api/admin/tasks` | `LegacyCompat.cpp` | 讀取/顯示任務相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 任務 | `POST /api/admin/tasks` | `LegacyCompat.cpp` | 建立/執行任務相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 任務 | `PATCH /api/admin/tasks/:id` | `LegacyCompat.cpp` | 修改任務相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 任務 | `POST /api/admin/tasks/:id/complete` | `LegacyCompat.cpp` | 建立/執行任務相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 任務 | `DELETE /api/admin/tasks/:id` | `LegacyCompat.cpp` | 刪除任務相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 恐怖／理智規則 | `GET /api/admin/horror-rules` | `LegacyCompat.cpp` | 讀取/顯示恐怖／理智規則相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 恐怖／理智規則 | `POST /api/admin/horror-rules` | `LegacyCompat.cpp` | 建立/執行恐怖／理智規則相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 恐怖／理智規則 | `PATCH /api/admin/horror-rules/:id` | `LegacyCompat.cpp` | 修改恐怖／理智規則相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 恐怖／理智規則 | `DELETE /api/admin/horror-rules/:id` | `LegacyCompat.cpp` | 刪除恐怖／理智規則相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 戰鬥／回合 | `POST /api/rooms/:id/actions` | `LegacyCompat.cpp` | 建立/執行戰鬥／回合相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 調查／線索 | `GET /api/journal` | `LegacyCompat.cpp` | 讀取/顯示調查／線索相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 調查／線索 | `GET /api/admin/clues` | `LegacyCompat.cpp` | 讀取/顯示調查／線索相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 調查／線索 | `POST /api/admin/clues` | `LegacyCompat.cpp` | 建立/執行調查／線索相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 調查／線索 | `DELETE /api/admin/clues/:id` | `LegacyCompat.cpp` | 刪除調查／線索相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 玩家管理 | `POST /api/admin/players/:id/clues/:clueId` | `LegacyCompat.cpp` | 建立/執行玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 玩家管理 | `POST /api/admin/players/:id/rules/:ruleId/reveal` | `LegacyCompat.cpp` | 建立/執行玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| DM 權限 | `GET /api/admin/status-templates` | `LegacyCompat.cpp` | 讀取/顯示DM 權限相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| DM 權限 | `POST /api/admin/status-templates` | `LegacyCompat.cpp` | 建立/執行DM 權限相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| DM 權限 | `PATCH /api/admin/status-templates/:id` | `LegacyCompat.cpp` | 修改DM 權限相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| DM 權限 | `DELETE /api/admin/status-templates/:id` | `LegacyCompat.cpp` | 刪除DM 權限相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 玩家管理 | `POST /api/admin/players/:id/statuses` | `LegacyCompat.cpp` | 建立/執行玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 玩家管理 | `DELETE /api/admin/players/:id/statuses/:statusId` | `LegacyCompat.cpp` | 刪除玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 道具／裝備／背包 | `GET /api/item-templates` | `LegacyCompat.cpp` | 讀取/顯示道具／裝備／背包相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 道具／裝備／背包 | `GET /api/admin/item-templates` | `LegacyCompat.cpp` | 讀取/顯示道具／裝備／背包相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 道具／裝備／背包 | `POST /api/admin/item-templates` | `LegacyCompat.cpp` | 建立/執行道具／裝備／背包相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 道具／裝備／背包 | `DELETE /api/admin/item-templates/:id` | `LegacyCompat.cpp` | 刪除道具／裝備／背包相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 道具／裝備／背包 | `POST /api/character/items/use` | `LegacyCompat.cpp` | 建立/執行道具／裝備／背包相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `GET /api/rooms/:id/map-nodes` | `LegacyCompat.cpp` | 讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/admin/rooms/:id/map-nodes` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/admin/rooms/:id/map-nodes/:nodeId/enter` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `DELETE /api/admin/rooms/:id/map-nodes/:nodeId` | `LegacyCompat.cpp` | 刪除房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/hidden-roll` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `GET /api/admin/rooms/:id/hidden-rolls` | `LegacyCompat.cpp` | 讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `GET /api/rooms/:id/teams` | `LegacyCompat.cpp` | 讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/teams` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/teams/:teamId/join` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/teams/:teamId/leave` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/teams/:teamId/move` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/teams/:teamId/share-clue` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/teams/:teamId/share-rule` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/teams/:teamId/chat` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/admin/rooms/:id/visions` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/visions/:visionId/read` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/visions/:visionId/share` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `PATCH /api/admin/rooms/:id/map-nodes/:nodeId` | `LegacyCompat.cpp` | 修改房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `PATCH /api/admin/rooms/:id/world-location` | `LegacyCompat.cpp` | 修改房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `PATCH /api/admin/rooms/:id/map-nodes/:nodeId/noise` | `LegacyCompat.cpp` | 修改房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `PATCH /api/admin/rooms/:id/members/:userId/pollution` | `LegacyCompat.cpp` | 修改房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `GET /api/rooms/:id/world-systems` | `LegacyCompat.cpp` | 讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/admin/rooms/:id/scheduled-events` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `DELETE /api/admin/rooms/:id/scheduled-events/:eventId` | `LegacyCompat.cpp` | 刪除房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/admin/rooms/:id/chases` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/chases/:chaseId/action` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/admin/rooms/:id/chases/:chaseId/stop` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 怪物 | `POST /api/admin/rooms/:id/monsters/:monsterId/boss-next` | `LegacyCompat.cpp` | 建立/執行怪物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `GET /api/rooms/:id/containers` | `LegacyCompat.cpp` | 讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/admin/rooms/:id/containers` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `DELETE /api/admin/rooms/:id/containers/:containerId` | `LegacyCompat.cpp` | 刪除房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/containers/:containerId/search` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| NPC | `GET /api/rooms/:id/npc-relations` | `LegacyCompat.cpp` | 讀取/顯示NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| NPC | `PATCH /api/admin/rooms/:id/npc-relations` | `LegacyCompat.cpp` | 修改NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/admin/rooms/:id/control` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `GET /api/admin/rooms/:id/savepoints` | `LegacyCompat.cpp` | 讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/admin/rooms/:id/savepoints` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/admin/rooms/:id/savepoints/:savepointId/restore` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `DELETE /api/admin/rooms/:id/savepoints/:savepointId` | `LegacyCompat.cpp` | 刪除房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `GET /api/admin/rooms/:id/timeline` | `LegacyCompat.cpp` | 讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 存檔／備份／稽核 | `GET /api/admin/backups/site/export` | `LegacyCompat.cpp` | 讀取/顯示存檔／備份／稽核相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `GET /api/admin/backups/rooms/:id/export` | `LegacyCompat.cpp` | 讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 存檔／備份／稽核 | `GET /api/admin/backups/characters/:userId/export` | `LegacyCompat.cpp` | 讀取/顯示存檔／備份／稽核相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 存檔／備份／稽核 | `GET /api/admin/backups/character-savepoints` | `LegacyCompat.cpp` | 讀取/顯示存檔／備份／稽核相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 存檔／備份／稽核 | `GET /api/admin/backups/character-savepoints/:id/preview` | `LegacyCompat.cpp` | 讀取/顯示存檔／備份／稽核相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 存檔／備份／稽核 | `POST /api/admin/backups/preview` | `LegacyCompat.cpp` | 建立/執行存檔／備份／稽核相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 存檔／備份／稽核 | `POST /api/admin/backups/restore` | `LegacyCompat.cpp` | 建立/執行存檔／備份／稽核相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 存檔／備份／稽核 | `GET /api/admin/audit-logs` | `LegacyCompat.cpp` | 讀取/顯示存檔／備份／稽核相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/explore` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 調查／線索 | `GET /api/admin/clue-combinations` | `LegacyCompat.cpp` | 讀取/顯示調查／線索相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 調查／線索 | `POST /api/admin/clue-combinations` | `LegacyCompat.cpp` | 建立/執行調查／線索相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 調查／線索 | `DELETE /api/admin/clue-combinations/:id` | `LegacyCompat.cpp` | 刪除調查／線索相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/rooms/:id/inference` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 房間／跑團 | `POST /api/admin/rooms/:id/time-flow/reset` | `LegacyCompat.cpp` | 建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 大道 | `GET /api/great-ways` | `LegacyCompat.cpp` | 讀取/顯示大道相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 其他系統 | `GET /api/pantheon` | `LegacyCompat.cpp` | 讀取/顯示其他系統相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 大道 | `POST /api/admin/great-ways` | `LegacyCompat.cpp` | 建立/執行大道相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 大道 | `PATCH /api/admin/great-ways/:id` | `LegacyCompat.cpp` | 修改大道相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 大道 | `DELETE /api/admin/great-ways/:id` | `LegacyCompat.cpp` | 刪除大道相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 信仰／神祇 | `POST /api/admin/deities` | `LegacyCompat.cpp` | 建立/執行信仰／神祇相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 信仰／神祇 | `PATCH /api/admin/deities/:id` | `LegacyCompat.cpp` | 修改信仰／神祇相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 信仰／神祇 | `DELETE /api/admin/deities/:id` | `LegacyCompat.cpp` | 刪除信仰／神祇相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 大道 | `POST /api/admin/players/:userId/great-ways/:id` | `LegacyCompat.cpp` | 建立/執行大道相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 大道 | `DELETE /api/admin/players/:userId/great-ways/:id` | `LegacyCompat.cpp` | 刪除大道相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 信仰／神祇 | `POST /api/admin/players/:userId/faiths/:id` | `LegacyCompat.cpp` | 建立/執行信仰／神祇相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 信仰／神祇 | `DELETE /api/admin/players/:userId/faiths/:id` | `LegacyCompat.cpp` | 刪除信仰／神祇相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 輪盤 | `GET /api/wheels` | `LegacyCompat.cpp` | 讀取/顯示輪盤相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 輪盤 | `POST /api/admin/wheels` | `LegacyCompat.cpp` | 建立/執行輪盤相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 輪盤 | `PATCH /api/admin/wheels/:id` | `LegacyCompat.cpp` | 修改輪盤相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 輪盤 | `DELETE /api/admin/wheels/:id` | `LegacyCompat.cpp` | 刪除輪盤相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 輪盤 | `POST /api/wheels/:id/spin` | `LegacyCompat.cpp` | 建立/執行輪盤相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 勢力／聲望 | `GET /api/factions` | `LegacyCompat.cpp` | 讀取/顯示勢力／聲望相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 勢力／聲望 | `GET /api/admin/factions` | `LegacyCompat.cpp` | 讀取/顯示勢力／聲望相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 勢力／聲望 | `PUT /api/admin/faction-relations` | `LegacyCompat.cpp` | 覆寫/同步勢力／聲望相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 勢力／聲望 | `DELETE /api/admin/faction-relations/:source/:target` | `LegacyCompat.cpp` | 刪除勢力／聲望相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 勢力／聲望 | `POST /api/admin/factions` | `LegacyCompat.cpp` | 建立/執行勢力／聲望相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 勢力／聲望 | `PATCH /api/admin/factions/:id` | `LegacyCompat.cpp` | 修改勢力／聲望相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 勢力／聲望 | `DELETE /api/admin/factions/:id` | `LegacyCompat.cpp` | 刪除勢力／聲望相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 玩家管理 | `PATCH /api/admin/players/:userId/reputations/:factionId` | `LegacyCompat.cpp` | 修改玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| NPC | `GET /api/admin/npc-social` | `LegacyCompat.cpp` | 讀取/顯示NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| NPC | `POST /api/admin/npc-dialogues` | `LegacyCompat.cpp` | 建立/執行NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| NPC | `PATCH /api/admin/npc-dialogues/:id` | `LegacyCompat.cpp` | 修改NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| NPC | `DELETE /api/admin/npc-dialogues/:id` | `LegacyCompat.cpp` | 刪除NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| NPC | `POST /api/admin/npc-shop-items` | `LegacyCompat.cpp` | 建立/執行NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| NPC | `PATCH /api/admin/npc-shop-items/:id` | `LegacyCompat.cpp` | 修改NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| NPC | `DELETE /api/admin/npc-shop-items/:id` | `LegacyCompat.cpp` | 刪除NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| NPC | `GET /api/rooms/:id/npcs/:npcId/interact` | `LegacyCompat.cpp` | 讀取/顯示NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| NPC | `POST /api/rooms/:id/npcs/:npcId/dialogue/:entryId` | `LegacyCompat.cpp` | 建立/執行NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |
| 商店 | `POST /api/rooms/:id/npcs/:npcId/shop/:shopItemId/buy` | `LegacyCompat.cpp` | 建立/執行商店相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。 |

## C++ 額外 API

| 功能 | API |
|---|---|
| 系統狀態 | `GET /api/cpp/status` |
| 信仰／神祇 | `GET /api/admin/deities` |
| 大道 | `GET /api/admin/great-ways` |
| NPC | `GET /api/admin/npc-dialogues` |
| NPC | `GET /api/admin/npc-shop-items` |
| 輪盤 | `GET /api/admin/wheels` |

## WebSocket 即時同步

| 事件 | 功能 | 後端 | 前端 |
|---|---|---|---|
| `room:enter` | 進房、在線狀態、房間快照 | `RoomSocket.cpp` | `native-socket.js` / `index.html` |
| `chat:send` | 房間聊天 | `RoomSocket.cpp` | `native-socket.js` / `index.html` |
| `dice:roll` | 多人擲骰 | `RoomSocket.cpp` | `native-socket.js` / `index.html` |

## 前端函式索引

| 功能 | 函式 | 函式內直接出現的 API |
|---|---|---|
| 大道／信仰 | `esc()` | — |
| 大道／信仰 | `normalizeIdentityEntries()` | — |
| 大道／信仰 | `identitySummaryHTML()` | — |
| 大道／信仰 | `identityEditorRowHTML()` | — |
| 大道／信仰 | `bindIdentityEditor()` | — |
| 道具／裝備／背包 | `collectIdentityEditor()` | — |
| 道具／裝備／背包 | `inventoryDisplayName()` | — |
| 道具／裝備／背包 | `inventoryQuantityValue()` | — |
| 道具／裝備／背包 | `normalizedInventory()` | — |
| 道具／裝備／背包 | `gearEntry()` | — |
| 道具／裝備／背包 | `gearList()` | — |
| 道具／裝備／背包 | `gearLoadout()` | — |
| 道具／裝備／背包 | `gearCanUse()` | — |
| 道具／裝備／背包 | `gearSkillNameList()` | — |
| 其他系統 | `ammoOptionRowHTML()` | — |
| 其他系統 | `bindAmmoOptionBuilder()` | — |
| 道具／裝備／背包 | `collectAmmoOptions()` | — |
| 道具／裝備／背包 | `ammoInventoryCount()` | — |
| 道具／裝備／背包 | `gearSourceForSkillUI()` | — |
| 道具／裝備／背包 | `chooseAmmoForGear()` | — |
| 道具／裝備／背包 | `gearSkillChecksHTML()` | — |
| 道具／裝備／背包 | `collectGearSkillChecks()` | — |
| 道具／裝備／背包 | `gearRequirementSummary()` | — |
| 道具／裝備／背包 | `gearExtraSummary()` | — |
| 道具／裝備／背包 | `equipmentSkillModifierUI()` | — |
| 其他系統 | `effectiveSkillTurnCostUI()` | — |
| 其他系統 | `effectiveSkillChargeTurnsUI()` | — |
| 其他系統 | `equipmentGrantedSkillsUI()` | — |
| 其他系統 | `skillData()` | — |
| 其他系統 | `elementAffinityModifierMap()` | — |
| 其他系統 | `elementModifierInputsHTML()` | — |
| 其他系統 | `collectElementModifierInputs()` | — |
| 道具／裝備／背包 | `elementModifierSummary()` | — |
| 存檔／備份／稽核 | `normalizeEffectListUI()` | — |
| 其他系統 | `effectTypeLabel()` | — |
| 其他系統 | `effectTimingLabel()` | — |
| 其他系統 | `effectRowHTML()` | — |
| 其他系統 | `bindEffectBuilder()` | — |
| 其他系統 | `addEffectRow()` | — |
| 其他系統 | `collectEffectRows()` | — |
| 其他系統 | `effectListSummary()` | — |
| 其他系統 | `baseAttributeInputsHTML()` | — |
| 其他系統 | `collectBaseAttributeInputs()` | — |
| 其他系統 | `setBaseAttributeInputs()` | — |
| 其他系統 | `baseElementAffinityInputsHTML()` | — |
| 其他系統 | `collectBaseElementAffinityInputs()` | — |
| 其他系統 | `setBaseElementAffinityInputs()` | — |
| 其他系統 | `finalEntityAttributes()` | — |
| 其他系統 | `finalEntityAffinity()` | — |
| 其他系統 | `entityStatLine()` | — |
| 其他系統 | `entityAffinityLine()` | — |
| 其他系統 | `professionEffects()` | — |
| 其他系統 | `bloodlineOptions()` | — |
| 其他系統 | `adminBloodlinesHTML()` | — |
| 血脈 | `openBloodlineEditor()` | — |
| 其他系統 | `summonSkillIds()` | — |
| 其他系統 | `summonSkillTemplateById()` | — |
| 其他系統 | `summonSkillCheckboxes()` | — |
| 其他系統 | `summonSkillsHTML()` | — |
| 調查／線索 | `isCraftSkill()` | — |
| 調查／線索 | `isInvestigationSkill()` | — |
| 其他系統 | `isRidingSkill()` | — |
| 其他系統 | `skillTurnCostUI()` | — |
| 調查／線索 | `skillChargeTurnsUI()` | — |
| 調查／線索 | `investigationAttributeLabel()` | — |
| 道具／裝備／背包 | `unlocksGameCoin()` | — |
| 道具／裝備／背包 | `unlocksInventoryExpansion()` | — |
| 道具／裝備／背包 | `inventorySlotLimitUI()` | — |
| 道具／裝備／背包 | `inventoryUsedSlotsUI()` | — |
| 道具／裝備／背包 | `characterHasInventoryExpansion()` | — |
| 道具／裝備／背包 | `shopItemNeedsNewSlot()` | — |
| 其他系統 | `skillProfessionAutoRules()` | — |
| 其他系統 | `skillProfessionAutoRuleRowHTML()` | — |
| 其他系統 | `bindSkillProfessionAutoRows()` | — |
| 其他系統 | `collectSkillProfessionAutoRules()` | — |
| 職業 | `syncSkillProfessionAutoRules()` | — |
| 其他系統 | `skillLearnCost()` | — |
| 其他系統 | `professionById()` | — |
| 其他系統 | `professionByName()` | — |
| 其他系統 | `professionBookInfo()` | — |
| 其他系統 | `characterOwnsProfession()` | — |
| 其他系統 | `professionIdOptions()` | — |
| 其他系統 | `skillLearnCostText()` | — |
| 其他系統 | `skillCanManualLearn()` | — |
| 其他系統 | `professionAutoSkillRules()` | — |
| 其他系統 | `professionAutoSkillRowHTML()` | — |
| 其他系統 | `bindProfessionAutoSkillRows()` | — |
| 其他系統 | `collectProfessionAutoSkills()` | — |
| 其他系統 | `professionAutoSkillSummary()` | — |
| 其他系統 | `professionProgressMap()` | — |
| 其他系統 | `professionProgressFor()` | — |
| 其他系統 | `professionProgressForName()` | — |
| 其他系統 | `professionDisplay()` | — |
| 其他系統 | `recipeConfig()` | — |
| 其他系統 | `recipeOutput()` | — |
| 其他系統 | `recipeMaterials()` | — |
| 其他系統 | `characterCraftSuccessBonusUI()` | — |
| 其他系統 | `recipeSuccessRateFor()` | — |
| 其他系統 | `recipeRequiredSkill()` | — |
| 其他系統 | `characterOwnsTemplateSkill()` | — |
| 其他系統 | `professionSpecialResources()` | — |
| 其他系統 | `professionResourceMaxSummary()` | — |
| 其他系統 | `professionResourceKey()` | — |
| 其他系統 | `allProfessionResources()` | — |
| 其他系統 | `findProfessionResourceByKey()` | — |
| 其他系統 | `professionResourceOptions()` | — |
| 其他系統 | `skillClassResourceConfig()` | — |
| 其他系統 | `skillResourcePayload()` | — |
| 其他系統 | `skillResourceSummary()` | — |
| 其他系統 | `professionResourceRowHTML()` | — |
| 其他系統 | `bindProfessionResourceEditor()` | — |
| 其他系統 | `addProfessionResourceRow()` | — |
| 其他系統 | `collectProfessionResources()` | — |
| 其他系統 | `classResourcesList()` | — |
| 其他系統 | `classResourcePanelHTML()` | — |
| 其他系統 | `skillSummonTemplateIds()` | — |
| 其他系統 | `summonTemplateNameList()` | — |
| 其他系統 | `summonTemplateCheckboxes()` | — |
| 其他系統 | `toast()` | — |
| 其他系統 | `api()` | — |
| 其他系統 | `showAuth()` | — |
| 擲骰 | `showApp()` | — |
| 房間／跑團 | `boot()` | `/api/me` |
| 登入／帳號 | `submitAuth()` | `/api/auth/login`, `/api/auth/register` |
| 其他系統 | `connectSocket()` | — |
| 其他系統 | `activateRoom()` | — |
| 房間／跑團 | `enterRoomById()` | — |
| 角色卡 | `loadMyCharacter()` | `/api/character` |
| 職業 | `loadProfessions()` | `/api/professions` |
| 血脈 | `loadBloodlines()` | `/api/bloodlines` |
| 大道 | `loadIdentitySystems()` | `/api/great-ways`, `/api/pantheon` |
| 輪盤 | `loadWheels()` | — |
| 技能 | `loadSkillTemplates()` | `/api/skill-templates` |
| 召喚物 | `loadSummonTemplates()` | `/api/summon-templates` |
| 勢力／聲望 | `loadFactions()` | `/api/admin/factions`, `/api/factions` |
| NPC | `loadNpcSocialAdmin()` | `/api/admin/npc-social` |
| 怪物 | `loadWorldLibraries()` | `/api/admin/dungeon-templates`, `/api/admin/monster-templates`, `/api/admin/npc-templates`, `/api/game-room/npcs` |
| 房間／跑團 | `loadWorldSystems()` | — |
| 房間／跑團 | `loadRoomContainers()` | — |
| 房間／跑團 | `loadTimeView()` | — |
| 登入／帳號 | `loadDmOps()` | — |
| 房間／跑團 | `loadRoomWorld()` | — |
| 房間／跑團 | `loadTeamData()` | — |
| 配方／製作 | `loadRecipes()` | `/api/admin/recipes`, `/api/recipes` |
| 商店 | `loadShop()` | `/api/admin/shop`, `/api/shop` |
| 召喚物 | `loadMySummons()` | `/api/character/summons` |
| 角色卡 | `loadAdminCharacters()` | `/api/admin/characters` |
| 其他系統 | `loadAdminUsers()` | `/api/admin/users` |
| 房間／跑團 | `loadAdminRooms()` | `/api/admin/rooms` |
| 存檔／備份／稽核 | `loadBackupCenter()` | `/api/admin/audit-logs?limit=100`, `/api/admin/backups/character-savepoints?limit=30` |
| 存檔／備份／稽核 | `downloadBackupJson()` | — |
| 存檔／備份／稽核 | `exportAdminBackup()` | — |
| 任務 | `loadPublicTasks()` | `/api/tasks/public` |
| 任務 | `loadMyTasks()` | `/api/tasks/my` |
| 任務 | `loadAdminTasks()` | `/api/admin/tasks` |
| 恐怖／理智規則 | `loadHorrorRules()` | `/api/admin/horror-rules` |
| 調查／線索 | `loadJournal()` | `/api/journal` |
| 職業 | `loadItemTemplates()` | `/api/item-templates` |
| 大道 | `itemTemplateByName()` | — |
| DM 權限 | `loadAdminInvestigationData()` | `/api/admin/clue-combinations`, `/api/admin/clues`, `/api/admin/item-templates`, `/api/admin/status-templates` |
| 技能 | `loadMapNodes()` | — |
| 召喚物 | `minuteClockText()` | — |
| 怪物 | `loadCppMonsterTemplates()` | `/api/admin/monster-templates` |
| 調查／線索 | `loadAdminData()` | — |
| 怪物 | `render()` | — |
| 其他系統 | `renderNavigation()` | — |
| 恐怖／理智規則 | `renderContent()` | — |
| 怪物 | `cppAdminHTML()` | — |
| 擲骰 | `cppRoomHTML()` | — |
| 其他系統 | `cppCharacterHTML()` | — |
| 其他系統 | `dashboardHTML()` | — |
| 道具／裝備／背包 | `taskRewardHTML()` | — |
| 探索／推理 | `taskCardHTML()` | — |
| 其他系統 | `taskHallHTML()` | — |
| 大道／信仰 | `myTasksHTML()` | — |
| 大道／信仰 | `identityRankOptions()` | — |
| 大道／信仰 | `identitySkillNames()` | — |
| 大道／信仰 | `identitySkillChoicesHTML()` | — |
| 大道／信仰 | `greatWaysHTML()` | — |
| 大道／信仰 | `pantheonHTML()` | — |
| 其他系統 | `wheelGradient()` | — |
| 其他系統 | `wheelsHTML()` | — |
| 勢力／聲望 | `factionNameUI()` | — |
| 勢力／聲望 | `reputationTierUI()` | — |
| 其他系統 | `factionRelationLabelUI()` | — |
| 勢力／聲望 | `factionsHTML()` | — |
| 大道 | `adminGreatWaysHTML()` | — |
| 大道／信仰 | `adminPantheonHTML()` | — |
| 其他系統 | `adminWheelsHTML()` | — |
| 存檔／備份／稽核 | `adminHTML()` | — |
| 存檔／備份／稽核 | `backupScopeLabel()` | — |
| 大道 | `backupCountSummary()` | — |
| 存檔／備份／稽核 | `adminBackupsHTML()` | — |
| 存檔／備份／稽核 | `adminControlHTML()` | — |
| 其他系統 | `itemTypeLabel()` | — |
| 其他系統 | `adminStatusesHTML()` | — |
| 其他系統 | `adminItemsHTML()` | — |
| 其他系統 | `adminCluesHTML()` | — |
| DM 權限 | `openStatusTemplateEditor()` | — |
| 其他系統 | `adminHorrorRulesHTML()` | — |
| 調查／線索 | `adminSkillsHTML()` | — |
| 其他系統 | `adminProfessionsHTML()` | — |
| 職業 | `openProfessionEditor()` | — |
| 其他系統 | `adminSummonsHTML()` | — |
| 召喚物 | `openSummonTemplateEditor()` | — |
| 其他系統 | `recipeMaterialRowHTML()` | — |
| 其他系統 | `bindRecipeMaterialRows()` | — |
| 其他系統 | `collectRecipeMaterials()` | — |
| 其他系統 | `adminRecipesHTML()` | — |
| 其他系統 | `taskRewardItemRowHTML()` | — |
| 其他系統 | `bindTaskRewardItems()` | — |
| 其他系統 | `collectTaskRewardItems()` | — |
| 隊伍／分隊 | `adminTasksHTML()` | — |
| 其他系統 | `adminCharactersHTML()` | — |
| 大道 | `characterCardHTML()` | — |
| 其他系統 | `adminUsersHTML()` | — |
| 其他系統 | `adminRoomsHTML()` | — |
| 其他系統 | `professionOptions()` | — |
| 大道 | `openMyCharacterEditor()` | `/api/character` |
| 職業 | `openProfessionAcquire()` | `/api/character/professions/acquire` |
| 技能 | `openSkillPicker()` | `/api/character/skills/from-template` |
| 調查／線索 | `openSkillTemplateEditor()` | — |
| 技能 | `openSummonPicker()` | — |
| 大道 | `openCharacterEditor()` | — |
| 其他系統 | `deleteUser()` | — |
| 調查／線索 | `linesText()` | — |
| 調查／線索 | `investigationSkills()` | — |
| 其他系統 | `visionTypeLabel()` | — |
| 搜索／容器 | `teamHTML()` | — |
| 搜索／容器 | `searchContainerHTML()` | — |
| 調查／線索 | `worldMonsterHTML()` | — |
| 調查／線索 | `worldNpcHTML()` | — |
| 其他系統 | `worldDungeonHTML()` | — |
| 其他系統 | `activeChaseHTML()` | — |
| 其他系統 | `worldSystemsHTML()` | — |
| 其他系統 | `bossPhaseText()` | — |
| 調查／線索 | `explorationPanelHTML()` | — |
| 商店 | `openNpcInteraction()` | — |
| 搜索／容器 | `worldHTML()` | — |
| 其他系統 | `aiBehaviorOptions()` | — |
| 其他系統 | `parseBossPhasesUI()` | — |
| 其他系統 | `formatBossPhasesUI()` | — |
| 調查／線索 | `adminMonstersHTML()` | — |
| 其他系統 | `adminFactionsHTML()` | — |
| 勢力／聲望 | `adminNpcSocialHTML()` | — |
| 調查／線索 | `adminNpcsHTML()` | — |
| 大道／信仰 | `adminDungeonsHTML()` | — |
| 房間／跑團 | `openWorldAssign()` | — |
| 其他系統 | `gameTimeText()` | — |
| 其他系統 | `gameTimeHintHTML()` | — |
| 調查／線索 | `journalHTML()` | — |
| 其他系統 | `isCombatAttackSkillUI()` | — |
| 其他系統 | `isReactionSkillUI()` | — |
| 其他系統 | `reactionKindLabel()` | — |
| 其他系統 | `reactionTriggerLabel()` | — |
| 其他系統 | `reactionSkillOptionsHTML()` | — |
| 其他系統 | `currentCombatActorUI()` | — |
| 其他系統 | `combatVisibleMonsters()` | — |
| 其他系統 | `combatNpcs()` | — |
| 其他系統 | `combatDownedAllies()` | — |
| 其他系統 | `combatTargetPool()` | — |
| 其他系統 | `combatLogHTML()` | — |
| 道具／裝備／背包 | `combatSkillButtonsHTML()` | — |
| 其他系統 | `summonCombatButtonsHTML()` | — |
| 其他系統 | `combatTargetModeUI()` | — |
| 其他系統 | `combatEffectsCanReviveUI()` | — |
| 其他系統 | `chooseCombatTarget()` | — |
| 其他系統 | `chooseTargetForSkill()` | — |
| 調查／線索 | `roomHTML()` | — |
| 房間／跑團 | `openSearchContainerEditor()` | — |
| 房間／跑團 | `openMapNodeEditor()` | — |
| 房間／跑團 | `openSceneEditor()` | — |
| 大道 | `characterHTML()` | — |
| 道具／裝備／背包 | `shopHTML()` | — |
| 其他系統 | `adminShopHTML()` | — |
| 道具／裝備／背包 | `gameRoomHTML()` | — |
| 道具／裝備／背包 | `craftHTML()` | — |
| 擲骰 | `diceHTML()` | — |
| 其他系統 | `chatHTML()` | — |
| 其他系統 | `membersHTML()` | — |
| 其他系統 | `roomSummonsHTML()` | — |
| 房間／跑團 | `openWorldLocationEditor()` | — |
| 其他系統 | `openScheduledEventsManager()` | — |
| 房間／跑團 | `openChaseStarter()` | — |
| 大道 | `bindView()` | `/api/admin/deities`, `/api/admin/great-ways`, `/api/admin/wheels` |
| NPC | `closeModal()` | — |
| 其他系統 | `openPrompt()` | — |


## 商店分類系統（46-cpp.7）

- `GET /api/shop`：回傳啟用商品＋分類，C++ 依分類排序後提供前端。
- `GET /api/shop/categories`：玩家讀取啟用分類。
- `GET /api/admin/shop/categories`：DM 讀取全部分類。
- `POST /api/admin/shop/categories`：新增／重新啟用分類。
- `PATCH /api/admin/shop/categories/:id`：改名、排序、啟用狀態；同步商品相容文字分類。
- `DELETE /api/admin/shop/categories/:id`：刪除分類但保留商品，商品移到未分類。
- `PATCH /api/admin/shop/:id`：可直接重新指定既有商品分類。
- 前端 `shopCategoryButtonsHTML`：分類快捷按鈕。
- 前端 `applyShopFilters`：分類＋名稱／說明關鍵字即時篩選。
- 前端 `adminShopHTML`：DM 分類管理與商品分類調整。
- PostgreSQL：`shop_categories`、`shop_items.category_id`。舊 `item_category` 會自動轉成正式分類並保留相容。


## 46-cpp.7：效率中心＋大量匯入（功能 1～14）

| 編號 | 功能 | C++ / API 主要位置 | 前端入口 |
|---|---|---|---|
| 1 | 全站搜尋＋標籤 | `LegacyCompat.cpp`：`GET /api/search`、`/api/tags`、`/api/admin/entity-tags` | `runGlobalSearch()`、效率中心 |
| 2 | 收藏／常用 | `GET/POST/DELETE /api/favorites` | `productivityToolsHTML()` |
| 3 | 戰鬥快捷面板 | `CoreService.cpp` 戰鬥 API | 效率中心＋`mobileCombatModeHTML()` |
| 4 | 裝備／道具比較 | 資料來源 `item_templates` | `itemEffectSummary()`、比較面板 |
| 5 | 商店進階 | `shop_items` 庫存/限購/折扣/回收；`/api/shop/sell`、`/api/shop/transactions` | 玩家/DM 商店 |
| 6 | 技能分類＋技能樹 | `skill_templates.prerequisite_skill_ids/tree_group/tree_order/tags`；學習技能時 C++ 驗證前置 | 技能新增/編輯 |
| 7 | 狀態自動化 | `CoreService.cpp::processRoundStatuses()`；`effects.per_round` | 狀態頁/戰鬥時間線 |
| 8 | 自訂資源條 | `PATCH /api/character/custom-resources` | 效率中心資源條 |
| 9 | 操作時間線 | `GET /api/rooms/:id/timeline` | 效率中心時間線 |
| 10 | DM 模板中心 | `GET /api/admin/template-center` | DM「批量／模板中心」 |
| 11 | 世界百科／圖鑑 | `/api/codex`、`/api/admin/codex*` | 效率中心／DM 工具 |
| 12 | 任務追蹤 | 既有 C++ 任務 API | 效率中心任務追蹤器 |
| 13 | 通知中心 | `/api/notifications*`、`/api/admin/notifications`；戰鬥輪到玩家自動通知 | 效率中心通知 |
| 14 | 手機戰鬥模式 | 核心戰鬥仍由 C++ 驗證 | `mobileCombatModeHTML()` |
| + | 大量技能／道具／材料匯入 | `POST /api/admin/bulk-import/preview`、`POST /api/admin/bulk-import/commit` | DM「批量／模板中心」 |

### 批量匯入格式

支援 **JSON 陣列、CSV、TSV**。單次最多 **5000 筆**；HTTP body 上限為 **32 MB**。流程固定為「解析 → C++ 預覽驗證 → 顯示錯誤列 → 確認匯入」。同名資料會更新，沒有同名資料才新增。

技能 TSV 常用欄位：`name, category, skill_type, level, description, check_name, damage_formula, tree_group, tree_order, prerequisite_skill_ids, tags, resources, data`。

道具／材料 TSV 常用欄位：`name, category, item_type, description, consume_on_use, tags, effects`。選「材料」時 C++ 會強制 `category=材料` 並預設 `item_type=material`。JSON 欄位（`effects/resources/data`）可直接填合法 JSON。

### 狀態每回合效果格式

在狀態 `effects` 內使用：`{"per_round":{"hp":-5,"spirit":0,"sanity":0}}`。正數為恢復、負數為傷害；有 `remaining_rounds > 0` 的新狀態預設會隨戰鬥新回合自動倒數並在 0 時停用。


# 47-cpp.8｜魔法學／儀式學／元素儲存索引

## 魔法學
- C++ 玩家 API：`GET /api/magic-studies`、`POST /api/magic-studies/:id/learn`
- C++ DM API：`GET/POST /api/admin/magic-studies`、`PATCH/DELETE /api/admin/magic-studies/:id`
- 點數調整：`POST /api/admin/players/:id/study-points`（`kind=magic`）
- 前端：搜尋 `magicStudiesHTML`、`adminStudyHTML('magic')`。

## 儀式學
- C++ 玩家 API：`GET /api/ritual-studies`、`POST /api/ritual-studies/:id/learn`
- C++ DM API：`GET/POST /api/admin/ritual-studies`、`PATCH/DELETE /api/admin/ritual-studies/:id`
- 點數調整：`POST /api/admin/players/:id/study-points`（`kind=ritual`）
- 前端：搜尋 `ritualStudiesHTML`、`adminStudyHTML('ritual')`。

## 七元素儲存
- 元素：暗、光、金、木、水、火、土。
- 新角色 `element_storage` 與 `element_storage_base_caps` 全為 0。
- C++ 統一計算：搜尋 `enrichElementStorage`（LegacyCompat）與 `element_storage_caps`（CoreService）。
- DM 手動調整：`POST /api/admin/players/:id/element-storage`。
- 技能加成：`data.element_storage_cap_bonus`。
- 持有道具加成：`effects.element_storage_cap_bonus`。
- 永久消耗型擴容：`effects.element_storage_permanent_bonus`。
- 魔法／儀式加成：`effects.element_storage_cap_bonus`。
- 單一元素道具範例：`{"element_storage_cap_bonus":{"火":3}}`；其他六元素不填或為 0。

## 大量匯入
- `kind=magics`、`kind=rituals` 已加入預覽/確認匯入流程。
- 可直接使用 TSV/CSV/JSON；魔法/儀式支援 `point_cost`、`prerequisite_ids`、`effects`。


## 48-cpp.9 世界／通訊／離線 AI 功能索引

- **規則書** → `GET /api/rulebook`、`POST/PATCH/DELETE /api/admin/rulebook...` → `WorldSystems.cpp` → `rulebookHTML()`。
- **NPC 商人與頭像** → `npc_templates.image_url / merchant_enabled / merchant_title`、既有 NPC 商店 API → `LegacyCompat.cpp` → `adminWorldHTML()` / `worldNpcHTML()`。
- **玩家頭像** → `PATCH /api/me/avatar` → `WorldSystems.cpp` → 角色卡編輯器 `my_avatar_url`。
- **程序場景地圖** → `POST /api/admin/rooms/:id/map/generate` → `WorldSystems.cpp` → `generateSceneMap`。
- **場景通訊限制** → `communicationAudience()` / `canUsersCommunicate()` / `roomSnapshotForViewer()` → `WorldSystems.cpp` + `RoomSocket.cpp`。
- **可通訊道具** → `item_templates.effects.communication_device=true` → `WorldSystems.cpp` → DM 道具欄 `itCommunicationDevice`。
- **不可通訊區** → `room_map_nodes.communication_blocked` → `WorldSystems.cpp` → 地圖節點編輯 `nodeCommunicationBlocked`。
- **元素自動生成** → `element_storage_generation`（每分鐘）→ `WorldSystems.cpp::runElementGenerationTick()` → 技能／道具 DM 編輯器。
- **離線 AI 接管** → `runOfflineAiTick()` → 有隊伍跟隨隊伍位置、無隊伍依節點探索；離線邀請自動接受；上線後停止接管。
- **DM 離線 AI 控制** → `/api/admin/rooms/:id/offline-ai...` → `WorldSystems.cpp` → `teamHTML()`。


## 52-cpp.13 規則引擎

| 功能 | API / 入口 | C++ 位置 | 備註 |
|---|---|---|---|
| 結構化規則列表／建立 | `GET/POST /api/admin/rule-engine/rules` | `RuleEngine.cpp` | DM 建立 trigger、tags、conditions、effects。 |
| 修改／刪除規則 | `PATCH/DELETE /api/admin/rule-engine/rules/:id` | `RuleEngine.cpp` | 規則文字與隱藏判定分離，可綁 `rulebook_entries`。 |
| 規則測試器 | `POST /api/admin/rule-engine/test` | `RuleEngine.cpp` | 預設 dry-run；回傳完整 condition trace。 |
| 規則觸發紀錄 | `GET /api/admin/rule-engine/logs` | `RuleEngine.cpp` | 回看誰、何時、哪個 event 觸發。 |
| 玩家規則狀態 | `GET /api/admin/rule-engine/state` | `RuleEngine.cpp` | flags / counters / values / countdowns。 |
| 重設玩家規則狀態 | `POST /api/admin/rule-engine/state/reset` | `RuleEngine.cpp` | 方便重跑副本或重置測試。 |
| 玩家行動觸發 | `PLAYER_ACTION` | `LegacyCompat.cpp` → `evaluateRuleEvent()` | 行動宣告會先送進規則引擎。 |
| 倒數到期觸發 | `RULE_COUNTDOWN_EXPIRED` | `RuleEngine.cpp` / `WorldSystems.cpp` | 背景循環自動結算。 |

### 條件格式
- `all` = AND、`any` = OR、`not` = NOT。
- Leaf：`{"path":"location.tags","op":"contains","value":"兔子園區"}`。
- 可讀路徑包含 `event.*`、`actor.user.*`、`actor.character.*`、`actor.tags`、`room.*`、`location.*`、`location.tags`、`team.*`、`state.*`、`context.nearby_ally_count`。

### 效果類型
`notify_dm`、`add_flag`、`remove_flag`、`increment_counter`、`set_value`、`start_countdown`、`clear_countdown`、`log_violation`、`block_action`、`add_event`、`reveal_rule`、`add_status`、`set_location`。

### 52-cpp.13 規則引擎自動事件接線

- `PLAYER_ACTION`：一般玩家行動。
- `PLAYER_MOVE_ATTEMPT` / `PLAYER_MOVED`：單人、隊伍、離線 AI 共用；移動前可用 `block_action` 阻止。
- `PLAYER_SCENE_TICK`：每約 30 秒檢查持續場景條件。
- `SHOP_PURCHASE_ATTEMPT` / `SHOP_PURCHASED`：一般商店與 NPC 商店。
- `ITEM_USE_ATTEMPT` / `ITEM_USED`：房間內道具使用。
- `NPC_DIALOGUE_ATTEMPT` / `NPC_DIALOGUE`：NPC 對話。
- `TEAM_JOINED` / `TEAM_LEFT`：隊伍狀態。
- `CHAT_MESSAGE`：聊天送出前。
- `PLAYER_ROOM_JOINED`：玩家進房／重新上線。
- `RULE_COUNTDOWN_EXPIRED`：規則倒數到期。
- 詳細 Context、條件與效果範例：`RULE_ENGINE_GUIDE.md`。

## 52-cpp.13 規則怪談 × 戰鬥橋接

- `RuleCombatBridge.cpp / .h`：規則效果與戰鬥核心的 C++ 橋接層。
- `RuleEngine.cpp`：未知效果會交給 `applyRuleCombatEffect`；規則 context 新增 `combat.*`。
- `CoreService.cpp`：戰鬥開始、回合、攻擊、擊敗、技能、戰鬥結束會反向送規則事件。
- `LegacyCompat.cpp`：戰鬥中使用技能會送 `COMBAT_SKILL_USED`。
- `index.html` → DM「規則引擎」：新增戰鬥效果快速模板與特殊勝利條件管理。
- API：`GET/PATCH /api/admin/rooms/:id/rule-combat`。

## 52-cpp.13 DM 修改其他玩家頭像

- **玩家本人**：`PATCH /api/me/avatar`，只能修改自己的角色頭像。
- **全站 DM**：`PATCH /api/admin/players/:id/avatar`，可以修改任意已建立角色卡的玩家頭像。
- **C++ 實作**：`WorldSystems.cpp`，後端再次驗證 `is_admin`，不是只靠前端按鈕隱藏。
- **前端位置**：DM → 所有玩家角色卡，每張卡新增 `🖼️ 頭像` 快捷按鈕；完整「修改角色卡」視窗也有頭像網址欄位。
- **修改位置**：搜尋 `[功能備註｜DM 玩家頭像管理]`、`openAdminAvatarEditor`、`.edit-avatar`。
- 頭像網址最長 2048 字元；留空即可移除頭像。


## 52-cpp.13 直接上傳玩家頭像

- 玩家：`POST /api/me/avatar/upload`，直接選 JPG／PNG／WebP，最大 5 MB。
- DM：`POST /api/admin/players/{id}/avatar/upload`，可替任意已有角色卡玩家上傳。
- 圖片：`GET /api/avatar-images/{id}`，只回傳圖片 bytes；`<img>` 可直接載入。
- PostgreSQL：`avatar_images`，以 `user_id` 一人一張，重新上傳直接覆寫。
- URL 模式仍保留；改成外部 URL 或清空時會刪除舊的資料庫圖片，避免垃圾資料。
- 前端：`avatarFilePayload`、`uploadAvatarFile`、`previewAvatarFile`、`openAdminAvatarEditor`。
