# C++ 遷移狀態：61-cpp.22

## 結論

- v39 HTTP 路由：**244**
- C++ 已註冊 v39 HTTP 路由：**244 / 244**
- 額外 C++ 效率／管理 API：**128**
- 原生 WebSocket 核心事件：3
- v39 PostgreSQL schema/migration：已整合進 C++ 啟動流程
- `code_complete`：`true`
- `production_ready`：`false`（本版新增功能仍需 Render/PostgreSQL/瀏覽器完整回歸）

## 已整合系統

認證、角色、職業、技能、裝備、召喚、配方、商店、房間、戰鬥、怪物、NPC、副本、任務、恐怖、線索、狀態、道具、地圖、暗骰、隊伍、幻象、世界事件、追逐、Boss、容器、存檔點、備份、稽核、探索推理、大道、信仰、輪盤、勢力聲望與 NPC 社交／交易。

46-cpp.7 另外整合：全站搜尋/標籤、收藏、通知、百科、自訂資源、跑團時間線、進階商店、技能樹、狀態每回合自動化、DM 模板中心、任務追蹤、裝備比較、手機戰鬥模式，以及技能/道具/材料批量匯入。

## 已知外部驗證狀態

先前版本已在 Render 實際完成 Docker + Drogon 編譯、啟動 PostgreSQL 並成功登入。55-cpp.16 因新增條件式規則引擎、事件自動接線與規則狀態 migration，仍需重新部署以確認完整編譯及真實資料庫回歸；多人 WebSocket、高併發與桌面/手機全流程 E2E 仍屬正式上線前驗證項目。


55-cpp.16 新增：獨立魔法學／儀式學點數與學習資料庫、七元素儲存/容量、技能/道具/學習內容容量加成、永久單元素擴容道具、DM 元素調整、魔法/儀式批量匯入。


## 55-cpp.16 世界／通訊／離線 AI

- 規則書、NPC/玩家頭像、商人標記。
- C++ 程序場景地圖生成，可由 DM 再編輯節點。
- 非同場景預設不能聊天；雙方持有通訊道具時可跨場景，禁訊區會封鎖遠端通訊。
- 技能／道具可設定每分鐘自動生成指定元素，且不超過角色元素儲存上限。
- 玩家離線時 C++ AI 接管：有隊伍跟隨隊伍／隊長位置；無隊伍依既有地圖、探查與線索規則探索；離線玩家收到隊伍邀請自動接受；重新連線後交回控制。


## 55-cpp.16 條件式規則引擎

- 規則書文字可綁定結構化 C++ 規則。
- 支援 AND / OR / NOT 與比較、包含、正則等條件。
- 支援旗標、計數器、倒數、DM 通知、違規紀錄、阻止操作、狀態與同房傳送等效果。
- 已自動接入玩家行動、移動、隊伍、離線 AI、場景輪詢、商店、道具、NPC 對話、聊天、進房與倒數事件。
- 房間存檔／還原會保留規則、玩家規則狀態與觸發紀錄。
- 詳細格式：`RULE_ENGINE_GUIDE.md`。


## 55-cpp.16 規則怪談 × 戰鬥橋接

新增 RuleCombatBridge C++ 模組。世界規則可生成/遭遇怪物、開始/結束戰鬥、修改怪物與 Boss 階段、加戰鬥狀態、設定特殊勝利條件、開關其他規則；戰鬥開始/回合/攻擊/擊敗/技能/結束會反向送入 RuleEngine。

## 55-cpp.16 DM 玩家頭像管理

新增 `PATCH /api/admin/players/:id/avatar`。只有全站 DM 可修改其他玩家頭像，玩家自己的 `/api/me/avatar` 權限維持不變。角色卡列表與完整角色編輯器均可操作。


## 55-cpp.16 直接上傳頭像
- C++ base64 解碼 + JPG/PNG/WebP magic-byte 驗證。
- PostgreSQL `avatar_images` 持久化。
- 玩家與 DM 都可上傳；DM 權限在後端驗證。
- 外部 URL 模式仍保留。

## 55-cpp.16 目錄整理與批量匯入

- Repo 根目錄不再平鋪所有來源檔；改用標準 `src/include/public/db/tests/tools/docs` 結構。
- 批量匯入新增合成配方與商店商品；魔法批量匯入維持支援。
- 商店批量匯入由 C++ 驗證價格／庫存／限購／折扣／回收比例與分類，分類不存在時自動建立。
- 配方批量匯入由 C++ 驗證 `materials` 陣列、`output` 物件與 `config` 物件。

## 56-cpp.17 通用詞條／洗煉／裝備槽

- 新增 `GearAffixSystem.cpp` C++ 模組：特殊貨幣、可自訂裝備槽、詞條庫、詞條池權重、洗煉設定與通用詞條目標。
- `trait_core`（詞條核心）為特殊貨幣，可由合成配方輸出。
- 角色配置改為固定主／副欄位與 DM 自訂裝備槽；C++ 驗證持有權、重複配置與雙手類型衝突。
- 詞條池權重在遊戲內修改，API 回傳自動換算百分比。
- 行動版角色／裝備／戰鬥／DM 管理介面重新排版。

## 59-cpp.20 混合品階／特殊詞條分層

- `GearAffixSystem.cpp` 新增一般／特殊詞條類型與一般／特殊詞條池類型。
- 混合品階詞條池改為兩層抽取：先依 `rank_weights` 決定品階，再用該品階詞條權重抽實際詞條。
- `affix_targets` 將一般詞條、額外特殊詞條、固有效果拆成 `current_affixes`、`special_affixes`、`intrinsic_effects`。
- 新增 `special_affix_rules` 與 `special_affix_roll_history`；來源裝備可依名稱／品階／類型設定成功率，對指定人偶等目標額外授予特殊詞條。
- 特殊詞條判定不修改 `current_affixes`，因此不占一般詞條上限；可限制特殊詞條獨立上限。
- C++、前端、migration、FEATURE_MAP、release check 均加入 `59-cpp.20` 功能備註與回歸檢查。


## 60-cpp.21 怪物完整戰鬥實體

- 怪物五維統一為力量、敏捷、體質、精神、幸運；五維保留在模板 `attributes` JSONB。
- 新增 `passive_skills`、`great_way`、`boss_phases` JSONB 欄位；`skills` 升級為可執行的結構化主動技能。
- 怪物大道是選填；啟用後房間怪物的大道資源以 `resource_max` 初始化，技能可實際消耗。
- 怪物 AI 可依 HP、目標 HP、回合、Boss 階段、機率及資源條件挑選技能，否則回退基本攻擊。
- AI 怪物技能會送入規則引擎 `COMBAT_SKILL_USED`。
- 怪物建立與修改均把相容的頂層戰鬥欄位合併進正式 `config`，修正舊版部分欄位修改後未真正持久化。
- C++、SQL、前端、FEATURE_MAP、release check 與 Render integration smoke 均加入 `60-cpp.21` 備註與回歸。


## 60-cpp.21.2 開團／轉盤穩定性
- 開團與轉盤前端事件改成關鍵優先綁定。
- 轉盤資料表納入 C++ 核心 migration。
- 開團戰鬥欄位重置加入舊 schema 降級保護。
- Render integration smoke 新增輪盤建立／讀取／轉動驗證。

## 60-cpp.21.3 建房／開團穩定性

- `POST /api/rooms` 的角色綁定與事件寫入改為非核心 best-effort。
- `POST /api/rooms/:id/start` 支援房主／房間 GM／全站 DM。
- 建房與開團回傳使用 `resilientRoomSnapshot()`；完整快照失敗時仍能回傳核心房間資料。
- 前端核心事件優先綁定包含建立、加入、開始跑團與轉盤。


## 60-cpp.21.4 提交行動穩定性

- `POST /api/rooms/:id/actions`：規則引擎仍可正常阻止行動，但規則引擎本身異常時降級，不再拖垮 action event 持久化。
- action 成功後的房間快照改成 viewer-filtered `roomSnapshotForViewer()`，且快照異常不再把已成功的 action 回成 500。
- 前端 `submitActionCritical()` 在 `bindView()` 最前段綁定，避免後續大型 UI 模組例外造成「宣告行動」按鈕無反應。


## 61-cpp.22 平面地圖＋節點
- `room_map_nodes` 新增 `x_percent / y_percent / node_type / icon`。
- 房間 `map_image_url` 作為平面圖底圖，節點以百分比座標覆蓋。
- 前端 `planarMapHTML()` 繪製 SVG 連線；DM `bindPlanarMap()` 可拖曳節點、雙擊新增。
- 玩家點節點移動；有隊伍沿隊伍移動，無隊伍走 `/api/rooms/:id/location`。
