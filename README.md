# TRPG Online C++ — GitHub / Render 根目錄平鋪版

版本：`52-cpp.13`

這個包專門對應目前的 GitHub 上傳方式：**Repo 根目錄直接放所有檔案**。不要求 GitHub 裡存在 `src/`、`include/`、`public/`、`db/` 子資料夾。

## 上傳到 GitHub

1. 解壓 ZIP。
2. 打開最外層的 `rpg_web_global_mvp`。
3. 把**裡面的所有檔案**上傳到 GitHub Repo 根目錄。
4. GitHub Repo 裡不要再建立 `rpg_web_global_mvp/` 子資料夾。

Repo 根目錄應直接看到：

```text
Dockerfile
CMakeLists.txt
render.yaml
main.cpp
CoreService.cpp
LegacyCompat.cpp
Config.h
CoreService.h
index.html
legacy_v39_migrations.sql
...
```

## Render 設定

```text
Branch: main
Root Directory: 留空
Dockerfile Path: ./Dockerfile
Docker Build Context Directory: .
Health Check Path: /api/health
```

必要環境變數：

```text
DATABASE_URL
JWT_SECRET
ADMIN_USERNAME
ADMIN_PASSWORD
```

`JWT_SECRET` 至少 32 個字元。

Dockerfile 會在 builder 階段自動把根目錄平鋪檔案重建成程式需要的 `src/`、`include/trpg/`、`public/`、`db/`、`tests/` 結構，因此 Render 不需要設定任何子資料夾 Root Directory。

## 相容範圍

- v39 HTTP API：244 / 244 C++ handler。
- 原生 WebSocket 核心事件：3 個。
- PostgreSQL v39 相容 migration。
- 帳號、角色卡、職業、技能、裝備、商店、房間、怪物/NPC、副本、戰鬥、任務、調查、地圖、隊伍、追逐、備份、大道/信仰、勢力聲望與 NPC 社交/交易等 C++ 相容層。
- 商店分類系統：DM 可新增／改名／排序／刪除分類；玩家與 DM 都可用分類＋關鍵字搜尋商品。

精確清單見 `api_parity.json`。

## 本包自我檢查

```bash
python3 release_check.py
```

此檢查會驗證平鋪結構、Render/Docker 路徑、244 API handler、前端 JavaScript、Security C++ 測試、Docker 目錄重建模擬與 CMake 平鋪模式 configure。

## 功能備註與修改索引

為了方便後續修改，本版已在主要程式加入中文功能備註：

- C++ API：搜尋 `[` + `功能備註｜`
- 前端函式：搜尋 `[` + `前端功能備註｜`
- WebSocket：搜尋 `[` + `WebSocket功能備註｜`
- 完整對照表：`FEATURE_MAP.md`（功能 → API → 後端檔案 → 前端函式）

只改註解／索引不會改變 API 行為；修改功能後仍請執行 `python3 release_check.py`。


## 46-cpp.7 效率功能與大量匯入

本版加入全站搜尋/標籤、收藏、戰鬥快捷、裝備比較、商店進階、技能樹、狀態回合自動化、自訂資源、時間線、模板中心、世界百科、任務追蹤、通知中心與手機戰鬥模式。核心資料/規則由 C++ + PostgreSQL 處理，前端只負責顯示與送出操作。

### 批量匯入

DM 後台 → **批量／模板中心**。可貼 JSON、CSV 或 TSV，一次處理最多 5000 筆技能／道具／材料。請先按「預覽」，確認錯誤列與整理後內容，再按確認匯入。大量資料的 request body 上限已提高到 32 MB。

範例 TSV：

```text
name	category	skill_type	level	description	damage_formula
火球術	戰鬥	active	1	投射火球	2D6+3
冰牆	防禦	active	1	建立冰牆	
```

```text
name	category	item_type	description	consume_on_use
治療藥水	消耗品	normal	恢復生命	true
鐵礦	材料	material	鍛造材料	false
```


## 52-cpp.13 魔法學／儀式學／元素儲存

- 新增獨立「魔法學」與「儀式學」玩家頁與 DM 管理頁。
- 角色擁有 `magic_points`／`ritual_points`，學習時由 C++ 驗證前置條件、重複學習與點數並扣除。
- 七元素（暗／光／金／木／水／火／土）都有目前儲存量與上限；新角色兩者皆從 0 開始。
- 元素上限來源可疊加：永久基礎、技能、持有道具、已學魔法、已學儀式。
- 道具可只增加單一元素上限，例如 `effects.element_storage_cap_bonus={"火":3}`；永久消耗型擴容使用 `element_storage_permanent_bonus`。
- 技能可用 `data.element_storage_cap_bonus` 增加一個或多個元素上限。
- 批量匯入新增魔法與儀式；任務與道具可發放魔法學／儀式學點數。


## 52-cpp.13 世界／通訊／離線 AI

- 規則書、NPC/玩家頭像、商人標記。
- C++ 程序場景地圖生成，可由 DM 再編輯節點。
- 非同場景預設不能聊天；雙方持有通訊道具時可跨場景，禁訊區會封鎖遠端通訊。
- 技能／道具可設定每分鐘自動生成指定元素，且不超過角色元素儲存上限。
- 玩家離線時 C++ AI 接管：有隊伍跟隨隊伍／隊長位置；無隊伍依既有地圖、探查與線索規則探索；離線玩家收到隊伍邀請自動接受；重新連線後交回控制。


## 52-cpp.13 條件式規則引擎
DM 後台新增「規則引擎」頁。玩家看到的規則書文字可以透過 `rulebook_entry_id` 綁定到 C++ 結構化規則；系統以 trigger + AND/OR/NOT conditions + effects 執行，並提供 dry-run 測試、違規/觸發紀錄、旗標/計數器與倒數狀態。

### 52-cpp.13 規則引擎事件接線

規則引擎不只提供 DM 測試器；C++ 已自動接入玩家行動、真人/隊伍/離線 AI 移動、場景每 30 秒輪詢、一般/NPC 商店購買、道具使用、NPC 對話、隊伍加入/離開、聊天、進房與規則倒數。詳細事件與 JSON 格式見 `RULE_ENGINE_GUIDE.md`。

## 52-cpp.13 規則怪談 × 戰鬥

規則引擎與戰鬥引擎現在雙向連接。規則可以生成怪物、開始/結束戰鬥、切 Boss 階段、增加戰鬥狀態、改特殊勝利條件與開關其他規則；戰鬥開始、攻擊、擊敗、回合、技能與結束事件也會回送規則引擎。

## 52-cpp.13 DM 玩家頭像管理

全站 DM 現在可在「所有玩家角色卡」直接修改其他玩家頭像。玩家仍可修改自己的頭像；權限由 C++ API 驗證。DM 可使用角色卡上的 `🖼️ 頭像` 快捷按鈕，或在完整角色編輯器直接修改 `avatar_url`。


## 52-cpp.13 直接上傳頭像

玩家與 DM 都可直接選 JPG／PNG／WebP 作為角色頭像（最大 5 MB），也仍可貼外部圖片網址。上傳圖片由 C++ 驗證真實檔頭後存入 PostgreSQL `avatar_images`，不依賴 Render 本機磁碟，因此重新部署不會丟失。
