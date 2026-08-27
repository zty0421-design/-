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


## 55-cpp.16 規則引擎

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

### 55-cpp.16 規則引擎自動事件接線

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

## 55-cpp.16 規則怪談 × 戰鬥橋接

- `RuleCombatBridge.cpp / .h`：規則效果與戰鬥核心的 C++ 橋接層。
- `RuleEngine.cpp`：未知效果會交給 `applyRuleCombatEffect`；規則 context 新增 `combat.*`。
- `CoreService.cpp`：戰鬥開始、回合、攻擊、擊敗、技能、戰鬥結束會反向送規則事件。
- `LegacyCompat.cpp`：戰鬥中使用技能會送 `COMBAT_SKILL_USED`。
- `index.html` → DM「規則引擎」：新增戰鬥效果快速模板與特殊勝利條件管理。
- API：`GET/PATCH /api/admin/rooms/:id/rule-combat`。

## 55-cpp.16 DM 修改其他玩家頭像

- **玩家本人**：`PATCH /api/me/avatar`，只能修改自己的角色頭像。
- **全站 DM**：`PATCH /api/admin/players/:id/avatar`，可以修改任意已建立角色卡的玩家頭像。
- **C++ 實作**：`WorldSystems.cpp`，後端再次驗證 `is_admin`，不是只靠前端按鈕隱藏。
- **前端位置**：DM → 所有玩家角色卡，每張卡新增 `🖼️ 頭像` 快捷按鈕；完整「修改角色卡」視窗也有頭像網址欄位。
- **修改位置**：搜尋 `[功能備註｜DM 玩家頭像管理]`、`openAdminAvatarEditor`、`.edit-avatar`。
- 頭像網址最長 2048 字元；留空即可移除頭像。


## 55-cpp.16 直接上傳玩家頭像

- 玩家：`POST /api/me/avatar/upload`，直接選 JPG／PNG／WebP，最大 5 MB。
- DM：`POST /api/admin/players/{id}/avatar/upload`，可替任意已有角色卡玩家上傳。
- 圖片：`GET /api/avatar-images/{id}`，只回傳圖片 bytes；`<img>` 可直接載入。
- PostgreSQL：`avatar_images`，以 `user_id` 一人一張，重新上傳直接覆寫。
- URL 模式仍保留；改成外部 URL 或清空時會刪除舊的資料庫圖片，避免垃圾資料。
- 前端：`avatarFilePayload`、`uploadAvatarFile`、`previewAvatarFile`、`openAdminAvatarEditor`。

## 55-cpp.16 GitHub 目錄整理＋批量匯入擴充

- GitHub 根目錄改為標準 C++ 專案結構：`src/`、`include/`、`public/`、`db/`、`tests/`、`tools/`、`docs/`，避免首頁攤開大量檔案。
- `POST /api/admin/bulk-import/preview`：新增 `recipes`、`shop`，並保留 `magics`。
- `POST /api/admin/bulk-import/commit`：合成配方同名更新／不存在新增；商店商品同名更新／不存在新增，分類名稱由 C++ 自動建立或復用正式 `shop_categories`。
- 前端 `parseBulkImportText`：配方 `materials` 支援 `鐵礦:3|木材:1` 簡寫，`output` 支援 `鐵劍:1` 簡寫。
- 前端 `bulkImportExample`：新增「合成配方」「商店商品」範例；魔法匯入範例保留。
- Render/PostgreSQL 整合測試新增：魔法／配方／商店商品的 preview → commit → 讀回 → 清理。

## 55-cpp.16 共用品階 + 遊戲間／書庫
- C++：`src/LegacyCompat.cpp` 搜尋 `共用品階`、`遊戲間／書庫`。
- DB：`db/legacy_v39_migrations.sql` 搜尋 `55-cpp.16`。
- 前端：`public/index.html` 搜尋 `knowledgeShop`、`CONTENT_RANKS`。
- 批量：`skills/items/materials/magics/rituals/recipes/shop/knowledge_shop` 全部支援品階。

## 55-cpp.16 房間／魔法樹／NPC／副本\n- 房間 500：`src/CoreService.cpp` 搜尋 `55-cpp.16`、`房間清單只查核心穩定欄位`。
- 七元素魔法樹：`src/LegacyCompat.cpp` 搜尋 `kMagicTreeElements`；`public/index.html` 搜尋 `magicTreeBoardHTML`。
- NPC 任務：`npc_task_offers`、`/api/rooms/{1}/npcs/{2}/tasks/{3}/accept`。
- NPC 圖片：`src/WorldSystems.cpp` 搜尋 `storeNpcAvatarUpload`。
- 副本等級：`dungeon_level`。


## 55-cpp.16 房間穩定與七元素魔法樹強化
- `/api/rooms` 只查核心穩定欄位；房間快照中的怪物／NPC 擴充資料若舊 schema 不完整會降級為空陣列，不再拖垮建房與房間清單。
- C++ 核心 migration 會補齊 `rooms` 的狀態／回合／存檔／關閉時間欄位。
- 魔法學分為暗、光、金、木、水、火、土七棵技能樹。
- DM 可自行設計節點名稱、圖示、品階、座標、魔法學點數、前置節點與效果。
- C++ 會驗證前置節點存在、不可指向自己，且必須和節點位於同一元素樹。
- NPC 互動畫面可直接接受 NPC 發布的任務；NPC 頭像可直接上傳圖片。


## 56-cpp.17 通用詞條／洗煉／裝備與手機版

- C++ 核心：`src/GearAffixSystem.cpp`。
- 資料表：`special_currency_types`、`character_special_currencies`、`equipment_slot_types`、`affix_definitions`、`affix_pools`、`affix_pool_entries`、`reforge_profiles`、`affix_targets`。
- 詞條核心：搜尋 `trait_core`。
- DM 機率管理：搜尋 `/api/admin/affix-pools`、`weight`、`probability`。
- 通用洗煉：搜尋 `/api/affix-targets/{1}/reroll` 與 `/targeted`。
- 玩家裝備配置：搜尋 `/api/character/loadout`、`playerGearSlotsHTML`。
- 裝備槽管理：搜尋 `/api/admin/equipment-slots`、`equipment_slot_types`。
- 玩家詞條頁：搜尋 `playerAffixTargetsHTML`。
- DM 詞條／詞條池／洗煉規則頁：搜尋 `adminAffixSystemHTML`。
- 手機優化：`public/index.html` 搜尋 `mobile-character-nav`、`gear-slot-grid`、`mobile-admin-stack`。

## 58-cpp.19 PWA／WebSocket／魔法樹／遊戲間整合
- 登入密碼頁新增「下載 App」按鈕，支援瀏覽器 PWA 安裝；Service Worker 會定期檢查更新並提示立即更新。
- WebSocket 改用 `/api/realtime-ticket` 取得 2 分鐘一次性票證，握手不再把完整 JWT 暴露在 URL；舊 token query 保留相容。
- 魔法學七元素技能樹新增大量圖示、節點類型、分支、搜尋／篩選、總進度與分支進度條；DM 儲存時真正送出 element/tree_x/tree_y/icon/node_style。
- 書庫移入遊戲間成為內部分頁，側邊欄／手機導覽不再單獨顯示書庫。
- 保留 55/56 的房間 500、PostgreSQL 啟動 seed、手機面板與通用詞條修正。


## 58-cpp.19 角色衍生值修復

- `CoreService::characterByUser()`：生命上限、耐力、理智上限、意志由 C++ 依最終體質／精神即時計算。
- `CoreService::initializeDatabase()`：自動修復舊 `room_members` 中明顯失真的 0/1 衍生值。
- `CoreService::roomSnapshot()`：舊房間尚未修復時，以角色卡屬性提供安全 fallback。
- 戰鬥開始：同步 `max_sanity/current_sanity` 與新的精神值。
- `public/index.html`：玩家角色卡再加一層前端公式 fallback，避免舊快取或舊房間資料把衍生值顯示成 0。

## 59-cpp.20 混合品階／額外特殊詞條

- C++ 核心：`src/GearAffixSystem.cpp`，搜尋 `[功能備註｜59-cpp.20`。
- 詞條分層：
  - `current_affixes` / `affixes`：一般詞條，只占一般詞條槽。
  - `special_affixes` / `special_affix_details`：額外特殊詞條，**不占一般詞條槽**。
  - `intrinsic_effects`：裝備／造物固有效果，獨立於兩種詞條。
- 詞條定義新增 `affix_kind = normal | special`；詞條池新增 `pool_kind = normal | special`，C++ 會阻止一般／特殊詞條放進錯誤的池。
- 混合品階池：`affix_pools.mixed_ranks` + `rank_weights`。抽取順序是「先抽品階 → 再依該品階詞條權重抽詞條」，避免池內詞條數量改變品階稀有度。
- DM 介面可建立混合品階池、設定各品階權重 JSON，並顯示「品階機率 × 品階內詞條機率 = 最終機率」。
- 額外特殊詞條規則：
  - PostgreSQL：`special_affix_rules`、`special_affix_roll_history`。
  - DM API：`GET/POST /api/admin/special-affix-rules`、`DELETE /api/admin/special-affix-rules/{1}`；詞條池另支援 `DELETE /api/admin/affix-pools/{1}` 供管理與測試清理。
  - 玩家／DM 判定：`POST /api/affix-targets/{1}/special-roll/{2}`。
  - 可指定來源類型、來源名稱、來源品階、接收目標類型、特殊詞條池、成功率、授予數量、特殊詞條上限與是否同一來源×目標只判定一次。
- 玩家角色頁會把 `★ 額外特殊詞條` 單獨顯示；特殊判定成功只更新 `special_affixes`，`current_affixes` 完全不變。
- 自訂人偶／遺物／造物可另外設定 `max_special_affixes` 與 `intrinsic_effects`。


## 60-cpp.21 怪物完整戰鬥實體

- **五維**：怪物模板的 `attributes` 正式使用 `strength / agility / constitution / spirit / luck`；DM 建立、修改與玩家已揭露怪物卡都會顯示五維。搜尋 `[功能備註｜60-cpp.21 怪物五維]`。
- **主動技能**：`monster_templates.skills` 支援結構化技能；可設定判定屬性、傷害式、傷害目標、命中加成、AI 優先度、行動消耗、AI 條件。舊字串技能仍可讀取顯示。
- **被動技能**：新增 `monster_templates.passive_skills`，和主動技能分離，AI 不會把被動當作主動技使用。
- **大道（選填）**：新增 `monster_templates.great_way`；可設定大道名稱、品階、資源上限與大道技能。只有 `enabled=true` 的怪物會初始化大道資源並讓 AI 使用大道技。搜尋 `[功能備註｜60-cpp.21 怪物大道]`。
- **Boss 階段**：新增正式 `monster_templates.boss_phases`，並同步保留 `config.boss_phases` 相容舊 Boss 流程。
- **怪物戰鬥 AI**：`CoreService.cpp` 的 `chooseMonsterCombatSkill()` 會依優先度與條件選擇技能；目前條件支援自身 HP、目標 HP、回合、Boss 階段與機率，大道技另驗證資源是否足夠。搜尋 `[功能備註｜60-cpp.21 怪物戰鬥 AI]`。
- **規則引擎**：AI 使用結構化怪物技能時會送出 `COMBAT_SKILL_USED`，並照原流程送 `COMBAT_ATTACK_RESOLVED`／擊敗／勝利條件事件。
- **大道資源**：手動放入房間與規則引擎 `spawn_monster` 都會依 `great_way.resource_max` 初始化 `current_fifth/max_fifth`；大道技能實際扣除資源。
- **修正舊怪物修改落庫問題**：POST/PATCH 都會把舊前端常用的頂層戰鬥欄位合併進 `config`，避免看似儲存但重新整理後遺失。
- **前端**：`public/index.html` 搜尋 `[前端功能備註｜60-cpp.21 怪物完整戰鬥實體]`、`monsterPassiveSkills`、`monsterGreatWayEnabled`、`monsterGreatWaySkills`。
- **Render 整合測試**：`tools/integration_smoke.py` 會實際建立五維怪物、主動／被動／大道技／Boss 階段，驗證 AI 選擇大道技與資源扣除。


## 60-cpp.21.1 Render 編譯 Hotfix

- **LegacyCompat.cpp 編譯修正**：`normalizeMagicNodeStyle()` 使用的 UTF-8 `clipped()` helper 現在於同一翻譯單元內正式定義；不再錯誤依賴 `CoreService.cpp` 匿名 namespace 內不可見的同名 helper。搜尋 `[功能備註｜60-cpp.21.1 Render 編譯修正]`。
- **編譯 warning 整理**：知識商店批量匯入的 `price / stock / purchase_limit / description / active` 預設值判斷拆成獨立 `if`，移除 Render GCC 的 `-Wmisleading-indentation` 警告。
- 本 hotfix 不改資料庫 schema、API 或怪物／詞條規則，只修正 Render C++ build。

- **Root Directory 文件統一**：README 與 `docs/GITHUB_RENDER_UPLOAD.txt` 現在一致要求 Repo 根目錄部署時 Render Root Directory 留空。


## 60-cpp.21.2 開團／轉盤緊急修正

- **開團關鍵事件優先綁定**：`public/index.html` 搜尋 `[前端功能備註｜60-cpp.21.2 開團／轉盤緊急修正]`。`bindView()` 先綁 `startCampaign` 與 `.spin-wheel`，再進入大型管理模組，避免其他 bind 例外拖垮關鍵按鈕。
- **開團後端降級保護**：`CoreService.cpp` 搜尋 `[功能備註｜60-cpp.21.2 開團穩定性]`。核心 `status/saved/round` 先成功更新，後期戰鬥欄位重置為 best-effort。
- **轉盤核心 migration**：`custom_wheels`、`wheel_spins` 與索引正式放進 `CoreService::initializeDatabase()`，不再只依賴 legacy migration。
- **Service Worker 強制換版**：快取名改為 `v60-cpp-21-2-room-wheel-hotfix-1`，避免手機 App 繼續使用舊前端。
- **Render smoke**：新增實際建立房間→開團，以及建立輪盤→玩家讀取→轉動→刪除的 PostgreSQL API 驗證。

## 60-cpp.21.3 建房／開團核心穩定性修正

- **建立房間優先綁定**：`public/index.html` 搜尋 `[前端功能備註｜60-cpp.21.3 建房／開團核心事件]`。`createRoom`、`joinRoom`、`startCampaign` 與轉盤都在 `bindView()` 最前段綁定，不再被後續大型管理模組的 JS 例外拖垮。
- **建立房間核心優先**：`CoreService.cpp` 搜尋 `[功能備註｜60-cpp.21.3 建立房間核心優先]`。房間與 GM 成員先建立；角色綁定、事件紀錄失敗只記 WARN，不再讓 `POST /api/rooms` 回 500。
- **安全房間快照**：`CoreService.cpp` 搜尋 `[功能備註｜60-cpp.21.3 開團安全快照]`。完整 `roomSnapshot()` 若因舊 schema 漂移失敗，會降級成只讀 `rooms / room_members / events` 核心欄位的可進房快照。
- **開始跑團核心優先**：`CoreService.cpp` 搜尋 `[功能備註｜60-cpp.21.3 開團核心優先]`。`status='active'` 先成功，戰鬥欄位重置、事件與完整快照皆可降級。
- **房間 GM 權限**：開始跑團不再只接受全站 DM；房主、房間角色為 `gm` 的成員、全站 DM 都可執行。
- **前端降級快照相容**：建房／開團收到 `degraded_snapshot=true` 仍會立即 `activateRoom()`，並用既有 `Promise.allSettled` 重新補載地圖、世界、隊伍、轉盤等擴充資料。
- **PWA 換版**：Service Worker cache 更新為 `trpg-online-v60-cpp-21-3-room-start-hardening-1`。
- **加入房間核心優先補強**：`POST /api/rooms/join` 只查 `character_cards.id/name` 核心欄位，不再先呼叫包含裝備、元素、學習等後期系統的 `characterByUser()`；角色綁定與事件若異常會降級記錄 WARN，回傳仍走安全快照。


## 60-cpp.21.4 提交行動核心穩定性

- **前端優先事件**：`public/index.html` 搜尋 `[前端功能備註｜60-cpp.21.4 提交行動核心事件]` / `submitActionCritical()`。`actionDeclareForm` 與建房、加入、開團、轉盤一起在 `bindView()` 最前段綁定。
- **後端核心優先**：`src/LegacyCompat.cpp` 搜尋 `[功能備註｜60-cpp.21.4 行動提交核心優先]`。`events` 行動紀錄是核心成功條件；規則引擎本身例外與完整房間快照例外只進入降級路徑。
- **規則阻止仍有效**：若 `evaluateRuleEvent()` 正常回傳 `blocked=true`，API 仍回 403，不會繞過規則怪談限制。
- **玩家可見性**：action 回傳快照改用 `roomSnapshotForViewer()`，不再直接回傳未過濾的 `roomSnapshot()`。
- **PWA 換版**：Service Worker cache `trpg-online-v60-cpp-21-4-action-submit-hotfix-1`。


## 61-cpp.22 平面地圖＋節點系統
- **C++ 核心 migration**：`src/CoreService.cpp` 搜尋 `[功能備註｜61-cpp.22 平面地圖核心 migration]`。
- **節點資料**：`room_map_nodes.x_percent / y_percent / node_type / icon / connections`。
- **讀取位置**：`src/LegacyCompat.cpp` 搜尋 `[功能備註｜61-cpp.22 平面地圖讀取]`，會回傳 `current_map_node_id`。
- **玩家移動**：`src/WorldSystems.cpp` 搜尋 `[功能備註｜61-cpp.22 玩家節點移動]`；若目前節點已有連線，只能移動到相鄰節點。
- **前端平面圖**：`public/index.html` 搜尋 `planarMapHTML`、`bindPlanarMap`。SVG 畫連線、百分比定位節點；DM 可拖曳與雙擊新增。
- **相容性**：舊地圖節點沒有座標時會自動使用前端 fallback 位置；舊節點沒有任何 connections 時仍允許自由移動，避免舊房間被鎖死。


## 61-cpp.22.1 地圖節點刪除修正
- **C++ 安全刪除**：`src/LegacyCompat.cpp` 搜尋 `[功能備註｜61-cpp.22.1 安全刪除地圖節點]`。
- 刪除前會清除房間場景、玩家／隊伍位置、怪物、NPC、副本、定時事件、搜索容器、線索與節點發現紀錄的引用。
- 其他節點的 `connections` 會同步移除被刪除節點，避免留下幽靈連線。
- **前端**：`deleteMapNodeCritical()`；節點清單與節點編輯視窗都可刪除，刪除後同步刷新地圖、世界單位、隊伍與時間資料。

## 62-cpp.23 事件型技能／多段攻擊
- **C++ 核心**：`src/EventSkillSystem.cpp`、`include/trpg/EventSkillSystem.h`，搜尋 `[功能備註｜62-cpp.23 事件型技能 migration]`。
- **多段 Hit Sequence**：`src/CoreService.cpp` 搜尋 `[功能備註｜62-cpp.23 多段攻擊]`；支援 `hit_count`、`hit_sequence`、逐段命中與逐段事件。
- **Proc Mode**：`per_hit`、`once_per_skill`、`once_per_target`、`final_hit_only`，由同一 `procState` 跨整個技能序列去重。
- **安全公式**：`CombatExpressionParser` 支援力量／敏捷／體質／精神／幸運／大道／信仰／耐力／意志／理智與四則運算，例如 `1+精神*2`。
- **玩家技能 API**：`src/LegacyCompat.cpp` 搜尋 `[功能備註｜62-cpp.23 多段攻擊／事件型技能執行]`；已學模板技能會即時 hydrate 最新事件設定。
- **怪物 AI**：`src/CoreService.cpp` 搜尋 `[功能備註｜62-cpp.23 怪物多段技能]`；怪物資料技能與玩家共用執行器。
- **DM 前端**：`public/index.html` 搜尋 `[前端功能備註｜62-cpp.23 事件型技能 JSON 編輯器]`；技能模板可設定段數、逐段 JSON 與事件觸發器 JSON。
- **可執行範例**：啟動 migration 自動補齊「畫兵成真」「速寫標記」「畫像標記•攻」「畫像標記•療」。
- **事件順序**：傷害／治療先結算，再發 `ON_DAMAGE_DEALT`／`ON_HEAL_RECEIVED`；因此新疊的標記只影響下一段／下一次治療。

## 62-cpp.23.1 場地技能／戰鬥開始自動觸發
- **場地資料**：`src/EventSkillSystem.cpp` 搜尋 `[功能備註｜62-cpp.23.1 場地技能 migration]`；`combat_field_effects` 保存整場戰鬥的領域，不佔任何角色狀態槽。
- **自動開場**：技能 `data.activation_mode=auto_combat_start` + `data.field_definition` 會在 `COMBAT_STARTED` 自動建立場地。
- **場地覆蓋**：`stack_policy` 支援 `coexist`、`replace_same`、`replace_lower_priority`、`exclusive`；另有 `priority`。
- **場地事件**：場地 `triggers` 沿用 Trigger → Condition → Effect，可監聽回合、傷害、治療、狀態、擊敗等既有 COMBAT_* 事件。
- **場地條件**：Condition 新增 `field_exists / field_active / field_not_exists`。
- **場地效果**：Effect 新增 `create_field / add_field / remove_field / clear_field`；場地 effects 可提供全場傷害／治療修正。
- **生命週期**：`duration_rounds=0` 持續到戰鬥結束；有期限場地隨新回合遞減；戰鬥結束先觸發 `ON_COMBAT_END` 再清場。
- **快照／備份**：房間快照回傳 `field_effects`；房間存檔／全站備份包含 `combat_field_effects`。
- **前端**：技能模板可選「進入戰鬥自動啟動」並編輯 Field JSON；戰鬥面板顯示目前場地與剩餘回合。


# 63-cpp.24｜儀式學雙面板
- [功能備註｜63-cpp.24 儀式學雙面板]：`ritual_studies` 新增佈置圖、材料／元素需求、步驟、成功／失敗效果。
- [功能備註｜63-cpp.24 儀式場]：`ritual_instances` 保存房間內正在佈置／引導／完成的儀式。
- 玩家第一頁 `ritualStudiesHTML()`：配方、學習、材料、如何佈置與平面節點預覽。
- 玩家第二頁 `ritualFieldHTML()`：建立儀式、點節點放置／移除內容、啟動、推進、中斷、撤除。
- C++ API：`GET/POST /api/rooms/:roomId/ritual-field`、`POST .../:id/place|remove|start|advance|fail`、`DELETE .../:id`。
- 儀式成功／失敗效果直接呼叫 `applyConfiguredEventSkillEffects()`，共用 62-cpp.23 的 Effect/Status/FieldEffect 核心。
- 規則事件：`RITUAL_CREATED`、`RITUAL_ITEM_PLACED`、`RITUAL_STARTED`、`RITUAL_STEP_COMPLETED`、`RITUAL_SUCCEEDED`、`RITUAL_FAILED`。


# 63-cpp.24.1｜儀式學點數／研究系統
- [功能備註｜63-cpp.24.1 儀式學點數／研究系統]：角色新增 `ritual_xp / ritual_level / ritual_points_earned / ritual_points_spent`。
- [功能備註｜63-cpp.24.1 未解析儀式]：`character_ritual_research` 保存每位玩家獨立的發現、解析、首次完成與失敗研究狀態。
- `POST /api/ritual-studies/:id/research`：玩家投入儀式學點數解析；達 100% 後才可學習需要研究的配方。
- `POST /api/admin/players/:id/ritual-research`：DM 可發現儀式、調整研究進度、發儀式學 XP。
- 首次成功／特殊失敗獎勵採一次性旗標；重複成功預設不給 XP，防止刷簡單儀式。
- 任務 `rewards.ritual_xp`、道具 `effects.ritual_xp` 已接入同一成長核心。
- 前端：`ritualProgression`、`.research-ritual-study`、`ritualStudyResearchApply`。


# 64-cpp.25｜角色正式等階／登階儀式
- 角色採 `0～10階`，每階 `1～4級`；9階為偽神、9階4級為半神、10階為真神。
- 大階固定基礎屬性總額：1階50、2階100、3階180、4階320、5階600、6階1100、7階2000、8階3600、9階6500、10階10000。
- 五項可分配基礎屬性：敏捷、力量、體質、精神、大道／神明；幸運為不可自由加點的特殊屬性。
- 0階→1階可由 DM 直接晉升；其餘大階必須先到本階4級並完成對應登階儀式。8→9、9→10 使用登神儀式。
- 登階成功後小級重置為1級，保留既有基礎屬性並依新大階額度補足剩餘屬性點。
- 儀式配方新增 `ascension_from_tier / ascension_to_tier`，可建立專用登階／登神配方。

## 64-cpp.25｜固定大階屬性額度相容修正
- 舊任務 `rewards.attribute_points` 不再增加角色基礎屬性總額。
- 任務完成後 C++ 會按目前大階額度重新計算剩餘基礎屬性點，避免舊任務把角色推到超額。
- DM 任務編輯器中的屬性點獎勵欄位已停用並標示由角色等階固定。
- Render integration smoke 已改成正式 0→1、四小級、登階儀式→下一階流程。


## 64-cpp.25.1 Render C++20 編譯 Hotfix
- `[功能備註｜64-cpp.25.1 Render C++20 編譯修正]`：`LegacyCompat.cpp` 不再以 C++20 保留字 `requires` 作為變數名，改用 `requiresResearch`。
- 清除 `syncBidirectionalMapConnections` 中未使用的 `hasNode` 變數，避免 Render 的 `-Wunused-but-set-variable` 警告。
- 發版檢查新增上述兩項回歸，避免同類編譯問題再次進入部署包。


## 64-cpp.25.2 玩家頭像恢復＋簡化角色卡
- `[功能備註｜64-cpp.25.2 玩家頭像自動恢復]`：`avatar_images` 仍有圖片時，自動補回 `/api/avatar-images/{userId}`。
- `[前端功能備註｜64-cpp.25.2 玩家卡簡化]`：DM 玩家列表預設只顯示頭像、姓名、等階、職業與五項核心屬性，其餘收進「更多資料」。


## 64-cpp.25.3 儀式編輯器簡易模式
- `[前端功能備註｜64-cpp.25.3 儀式編輯器簡易模式]`：DM 儀式資料庫預設改用一般文字／數字欄位，不再直接顯示大量 JSON。
- 材料需求：每行 `材料名稱 ×數量`。
- 佈置節點：每行 `位置名稱 | 要放的內容 | X | Y`。
- 七元素需求直接使用七個數字欄。
- 儀式步驟改為每行一步。
- 成功／失敗提供常用效果快捷設定；複雜效果仍可開啟「進階 JSON」。
- 登階欄位只在登階／登神儀式時顯示；研究與其他學習條件預設收合。


## 64-cpp.25.4 行動＋說話＋待機合併
- `[功能備註｜64-cpp.25.4 行動＋說話＋待機]`：`POST /api/rooms/{id}/actions` 支援 `action / speech / wait` 三種互動。
- 說話會觸發 `CHARACTER_SPOKE` 與 `SOUND_EMITTED`，預設不消耗戰鬥行動。
- 說話音量：悄聲 `noise=3`、普通 `noise=10`、大喊 `noise=25`；大喊可傳到相鄰平面圖節點。
- 聲音會增加目前節點 `noise_level`，並依怪物 `hearing_threshold` 提升 `room_monsters.alert_level`。
- 行動仍觸發 `PLAYER_ACTION`；有聲行動另外觸發 `SOUND_EMITTED`。
- 待機觸發 `PLAYER_WAITED`；戰鬥時前端再使用既有 `/combat/pass` 消耗本次行動。
- `[前端功能備註｜64-cpp.25.4 行動＋說話＋待機]`：房間只保留一個互動紀錄與輸入區，不再提供場外聊天 UI。
- 舊 WebSocket `chat:send` 僅作舊客戶端相容，語義已改成「普通音量角色說話」。

## 65-cpp.26 魔法學完整系統
- `[功能備註｜65-cpp.26 魔法學完整系統]`：七元素技能樹節點可設定前置、點數、元素成本、魔法書與技能模板。
- `[功能備註｜65-cpp.26 魔法書學習]`：學習時後端驗證背包魔法書，支援持有即可或學習後消耗。
- `[功能備註｜65-cpp.26 魔法施放｜POST /api/magic-studies/{1}/cast]`：施放前驗證元素儲存量，戰鬥內共用 EventSkillSystem／多段技能核心。
- `[功能備註｜65-cpp.26 元素親和／抗性／弱點]`：元素傷害倍率為 100% + 親和 - 抗性 + 弱點。
- `[前端功能備註｜65-cpp.26 魔法施放]`：已學可施放節點提供施放按鈕，DM 使用簡易欄位管理成本與技能連結。

## 65-cpp.26.2 角色立繪與手機版
- **玩家立繪** → `PATCH /api/me/portrait` / `POST /api/me/portrait/upload` → `WorldSystems.cpp` → `portrait_images` + `character_cards.portrait_url`。
- **DM 立繪管理** → `PATCH /api/admin/players/{1}/portrait` / `POST /api/admin/players/{1}/portrait/upload` → `openAdminPortraitEditor`。
- **公開圖片** → `GET /api/portrait-images/{1}`，供 `<img>` 直接載入。
- **角色頁顯示** → `characterPortraitURL`、`characterPortraitHTML`、`.character-profile-header`。
- **手機版** → 立繪單欄完整顯示、safe-area 邊距、表單 16px、防止 iOS 自動縮放、操作按鈕小螢幕單欄、平面圖 4:3。
- **備份** → 角色備份包含 `portrait_image`；全站備份包含 `portrait_images`。



## 65-cpp.26.3 房間立繪舞台 + 手機第二輪
- **房間立繪候選** → `roomPortraitCandidates()` → 房間成員 `portrait_url` + NPC `image_url`。
- **跟隨／固定角色** → `roomPortraitEntity()` / `roomPortraitStageHTML()` → 戰鬥目前角色或手動選定角色。
- **立繪操作** → `.portrait-stage-side` / `.portrait-stage-toggle` / `.portrait-target-chip` → 左右、隱藏、角色切換。
- **NPC 對話立繪** → `openNpcInteraction()` → `.npc-dialogue-stage`，沿用 NPC `image_url`。
- **手機快捷列** → `[data-mobile-jump]` + `#mobileMoreSheet` → 技能／道具直達角色頁區段，其餘功能收納。
- **DM 手機版** → `.admin-mobile-body` → 管理頁單欄卡片化與局部橫向捲動。

## 65-cpp.26.4 多立繪 + 戰鬥 UI 第一版
- **玩家多立繪 API** → `GET/POST /api/me/portraits`、`PATCH/DELETE /api/me/portraits/{id}`、`POST /api/me/portraits/upload` → `WorldSystems.cpp`。
- **NPC 多立繪 API** → `GET/POST /api/admin/npc-templates/{id}/portraits`、`PATCH/DELETE /api/admin/npc-portrait-variants/{id}` → `WorldSystems.cpp`。
- **圖片資料表** → `character_portrait_variants` / `npc_portrait_variants`；active 變體同步既有 `character_cards.portrait_url` / `npc_templates.portrait_url`。
- **多立繪 UI** → `openPortraitLibrary()` / `portraitVariantCardHTML()` / `.manage-my-portraits` / `.manage-npc-portraits`。
- **戰鬥主介面** → `combatDashboardHTML()` / `combatUnitCardHTML()` / `combatOrderUI()`。
- **目標預選** → `.combat-target-card` + `state.combatSelectedTarget`；既有 `chooseCombatTarget()` 會優先採用有效預選目標。
- **手機戰鬥快捷** → `.combat-mobile-dock`，共用既有基本攻擊、技能與 `/combat/pass` 後端，不建立平行戰鬥邏輯。
- **倒地／復甦** → 單位卡保留倒地角色並顯示狀態，DM 復甦仍呼叫既有 `/api/admin/rooms/{id}/combat/revive`。
- **備份** → 角色備份包含 `portrait_variants`；全站備份包含兩張多立繪資料表。

## 65-cpp.26.5 任務建立／DM 自由屬性點修正

- 任務建立：前後端同時修正 nullable FK 的 `0`／`NULL` 相容，公共任務不再因 `target_user_id=0` 或 `profession_id=0` 觸發 PostgreSQL 外鍵錯誤。
- DM 玩家管理：角色編輯器可直接「給予額外自由屬性點」。
- 玩家屬性：大階固定額度是最低額度；DM 額外給的點數形成額外可分配總額，玩家只能重新分配，不能自行增加。
- 保留規則：任務完成、服務重啟與登階皆不會清掉 DM 額外自由屬性點。


## 65-cpp.26.6 平面地圖／手機地圖優化

- `[前端功能備註｜65-cpp.26.6 地圖優化]`：節點、連線、可達提示、目前位置提示。
- `[前端功能備註｜65-cpp.26.6 地圖操作]`：滾輪縮放、拖曳、雙指縮放、定位、重置、全螢幕。
- DM 節點拖曳與雙擊新增改為明確編輯模式；玩家移動仍由 C++ 節點連線驗證作最終判定。


## 65-cpp.26.7 DM 管理工作區

- `[前端功能備註｜65-cpp.26.7 DM 面板優化]`：DM 功能依快速管理、角色與成長、世界與劇情、規則與物品、商店與工具分類。
- `[前端功能備註｜65-cpp.26.7 DM 面板搜尋]`：輸入關鍵字即時過濾管理功能，不呼叫後端。
- 桌機為固定側欄＋主工作區；手機為可折疊分類，保留單欄卡片與觸控尺寸。
- 快速統計卡可直接前往角色、房間、任務、怪物與 NPC。
- `rpg_admin_view` 保存上次管理頁，重新整理後可回到原工作位置。


## 65-cpp.26.8 登入體驗

- `[前端功能備註｜65-cpp.26.8 登入畫面優化]`：桌面雙欄品牌頁，<=900px 自動改單卡。
- 登入／註冊 tab 具有 `aria-selected`，標題／提示／密碼 autocomplete 同步。
- 密碼可顯示／隱藏；送出時按鈕 loading 並禁止重複請求。
- 成功登入只記住最後帳號，不保存密碼。
- PWA 安裝／更新入口保留在登入頁。


## 65-cpp.26.9 商店工作區
- `[前端功能備註｜65-cpp.26.9 商店 UX]`：購買／出售／交易紀錄、分類 chip、搜尋、狀態篩選、排序、手機卡片。
- DM 商品目錄：完整編輯、上下架、分類快捷修改、庫存／折扣狀態篩選。
- `[功能備註｜65-cpp.26.9 商店管理安全更新]`：C++ PATCH 僅清洗實際傳入欄位，避免部分更新重設品階。


## 65-cpp.26.10 玩家角色工作區
- `[前端功能備註｜65-cpp.26.10 玩家角色工作區]`：角色六區導航與手機單分頁。
- 角色總覽：HP／精神／理智／大道或信仰狀態條。
- 屬性：基礎／衍生／元素／職業特殊資源集中。
- 背包：搜尋、分類、名稱／數量／品階排序、道具詳情與原有使用／鑑定功能。
- 技能：搜尋、主動／被動／召喚／製作篩選、本機收藏與原有施放功能。
- 裝備：主副武器／自訂槽位選擇時先顯示草稿預覽，儲存仍交由 C++ `/api/character/loadout`。
- 狀態：調查手冊 Buff／Debuff、來源、層數、剩餘回合集中顯示。


## 65-cpp.26.11 任務／NPC／通知中心 2.0
- `[功能備註｜65-cpp.26.11 任務 2.0]`：C++ 驗證階級、前置任務、勢力聲望、互斥分支；玩家可搜尋、分類與追蹤任務。
- `[功能備註｜65-cpp.26.11 DM 任務流程]`：DM 直接看到接取玩家與角色名稱，可從下拉選單完成並發放獎勵，不再手輸玩家 ID。
- `[功能備註｜65-cpp.26.11 NPC 管理 2.0]`：NPC 搜尋、勢力／類型篩選、對話／商店／任務統計與模板一鍵複製。
- `[功能備註｜65-cpp.26.11 通知中心 2.0]`：未讀／任務篩選、全部已讀、任務指派／接取／完成自動通知。


## 65-cpp.26.12 戰鬥體驗／連線穩定 2.0
- `[功能備註｜65-cpp.26.12 連線穩定]`：C++ `/api/health` 回傳 ready／retry_after_ms；前端頂部顯示已同步／重新連線／伺服器啟動中／離線，GET API 遇到逾時、502、503、504 會有限次重試。
- `[前端功能備註｜65-cpp.26.12 WebSocket 重連]`：斷線後指數退避重連，網路恢復時立即重試並重新進入目前房間。
- `[前端功能備註｜65-cpp.26.12 戰鬥即時回饋]`：只比較 C++ `roomSnapshot` 結果顯示傷害、治療、狀態與新回合，不在前端重算戰鬥公式。
- `[前端功能備註｜65-cpp.26.12 戰鬥操作]`：目標可再次點擊取消、顯示選中標記、戰鬥紀錄補上來源與時間。

## 65-cpp.26.13 全介面優化
- `[前端功能備註｜65-cpp.26.13 全介面優化]`：建立統一 UI token，補上舊元件 `--border` fallback，統一背景、卡片、按鈕、輸入框、Badge、列表、Modal、Toast、表格與程式碼區塊。
- 桌面版：頂部列與左側導覽重新收斂，側欄固定且可獨立捲動，主工作區限制超寬螢幕閱讀寬度，選中頁面有清楚導覽指示。
- 玩家核心頁：角色卡／背包／技能、商店、地圖、立繪舞台、戰鬥、任務／通知全部套用一致的 surface、border、hover、焦點與資訊層級。
- DM：管理總覽、分類側欄、統計卡、功能標題與表單工作區統一視覺；保留既有 65-cpp.26.7 DM 分類與所有 API。
- 手機版：全站按鈕／分頁至少 44px 觸控高度；底部快捷列、更多選單、全螢幕 Modal、橫向工具列、表格、配方／任務材料編輯器避免溢出；保留 safe-area。
- 無障礙：全站 `:focus-visible`、較清楚表單焦點、disabled 狀態、`prefers-reduced-motion` 降低動畫，以及小螢幕 overflow 防護。
- 本版只調整介面層；65-cpp.26.12 的 C++ 戰鬥、連線重試、WebSocket、自動重連與所有既有資料流程不變。

## 65-cpp.26.14 可成長性裝備
- `[功能備註｜65-cpp.26.14 可成長性裝備]`：武器／裝備可啟用成長等級與 XP，進度保存在 `affix_targets.data`，同步角色卡不會重設。
- C++ 依等級計算有效品階、下一級 XP、一般詞條容量與裝備授予技能的成長傷害倍率。
- 新解鎖的一般詞條槽若有可用詞條池會自動生成新詞條，原有詞條不覆蓋。
- 裝備期間授予技能 `gear_<templateId>` 正式由 C++ 驗證目前裝備來源並接入戰鬥核心。
- DM 角色編輯器可設定最高等級、XP 曲線、每次使用 XP、品階成長、詞條槽成長、每級技能增傷，並直接給指定裝備成長 XP。
- `[前端功能備註｜65-cpp.26.14 可成長性裝備]`：玩家裝備／詞條卡顯示成長進度條、Lv、XP、有效品階、詞條槽與傷害加成，手機版同步適配。

## 65-cpp.26.15 可重複修改／品階合成／配方研究
- `[功能備註｜65-cpp.26.15 配方可重複修改]`：`PATCH /api/admin/recipes/{id}`，配方可反覆載入與儲存，不必刪除重建。
- `[功能備註｜65-cpp.26.15 道具／裝備模板可重複修改]`：`PATCH /api/admin/item-templates/{id}`；技能模板沿用既有 PATCH；DM 角色編輯器可直接修改已持有武器／裝備並保留 `gear_instance_id`、成長等級與 XP。
- `[功能備註｜65-cpp.26.15 合成研究／品階成功率]`：G/F/E/D/C/B/A/S/SS/SSS 預設基礎成功率為 100/95/90/82/74/65/55/45/35/25%，再由 C++ 加上製作技能成功率加成並限制 0～100%。
- 舊配方沒有 `success_rate_mode` 時沿用原 `success_rate`；新配方可選「依品階」或「手動指定」。
- 合成出的道具／武器／裝備若沒有另外指定 `rank`，C++ 會自動沿用配方品階。
- `[功能備註｜65-cpp.26.15 配方試驗｜POST /api/recipes/experiment]`：玩家以材料名稱＋類別＋數量進行順序無關的精確試驗；成功後才解鎖隱藏配方。
- `character_recipe_discoveries` 保存玩家發現進度；`recipe_experiment_attempts` 保存嘗試紀錄。

## 67-cpp.28 命隙／彌夜／塔羅占卜／命鬥場
- `[功能備註｜67-cpp.28 命隙／塔羅 API]`：新增 22 張大阿爾克那、56 張小阿爾克那牌庫骨架與正逆位資料。
- `POST /api/tarot/personal/{roomId}/{dungeonId}`：每玩家、每副本、每日一次個人命運（大阿爾）。
- `POST /api/tarot/omen/{roomId}/{dungeonId}`：每玩家、每副本、每日一次具體預兆（小阿爾，可被行動改變）。
- `POST /api/tarot/dungeon/{roomId}/{dungeonId}`：每個房間副本實例只允許一次副本命運（大阿爾）。
- `GET /api/tarot/status/{roomId}/{dungeonId}`、`GET /api/tarot/readings/{roomId}/{dungeonId}`：顯示使用狀態與紀錄。
- `POST /api/fate-gap/pvp`：建立命鬥場等待對局；命運模式由伺服器抽大阿爾作為規則牌。
- 彌夜自動 seed：無陣營、所屬命隙、遊戲間協力者，AI Lv.8，允許友軍參戰與特殊 Boss 劇情設定。
- 遊戲間新增「🔮 命隙」分頁；包含手機版塔羅洗牌／翻牌視覺、彌夜對話、占卜狀態、命運紀錄與命鬥場入口。
- 抽牌結果完全由 C++/PostgreSQL 決定，前端只播放動畫及顯示結果，避免刷新／修改前端刷牌。


## 67-cpp.28 命鬥場完整 PvP／彌夜 AI
- `POST /api/fate-gap/pvp` 建立 1v1。
- `GET /api/fate-gap/pvp/room/{room}` 公開對局列表。
- `GET /api/fate-gap/pvp/{id}` 對局狀態。
- `POST /api/fate-gap/pvp/{id}/join|leave|start|act|surrender` 完整非致死 PvP 流程。
- `GET /api/fate-gap/miyoi/{room}` 彌夜 AI／長期記憶狀態。
- 前端：`fateArenaHTML()`。

## 73-cpp.34 彌夜 × 無牌者特殊聯動

| 功能 | API | 主要後端位置 | 說明 |
|---|---|---|---|
| 逆命聯動狀態 | `GET /api/fate-gap/anti-fate/{roomId}/link-state` | `LegacyCompat.cpp` | 顯示無牌者階段、彌夜空白牌污染、修復進度與終結技解鎖。 |
| 修復塔羅 | `POST /api/fate-gap/miyoi/{roomId}/restore-tarot` | `LegacyCompat.cpp` | 每回合隊伍可協助一次，三段修復可解除命運雜訊／塔羅封鎖。 |
| 合作終結技 | `POST /api/fate-gap/miyoi/{roomId}/fate-finisher` | `LegacyCompat.cpp` | 第三階段以上且塔羅完全恢復後可使用一次【命運之外・世界重構】。 |
| 空白牌污染 | 自動 | `CoreService.cpp` | 無牌者的空白預言／吞食未來會把彌夜牌面抹成空白並壓制塔羅。 |
