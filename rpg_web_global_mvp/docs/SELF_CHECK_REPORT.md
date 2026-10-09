# 65-cpp.26.11 時間／精神力／攻擊治療加成自檢

- 結果：221 PASS / 0 FAIL / 2 WARN。
- 新增精神力 MP：上限 = 最終精神 × 2 + 10；目前 MP 獨立保存。
- 技能可設定 MP 消耗；不足時由 C++ 後端拒絕施放，成功後扣除。
- 時間技能支援行動提前、負面狀態剩餘回合縮短與精神力恢復。
- `turn_cost=0` 可製作不消耗本回合行動的自我加速技能。
- 攻擊加成已接實際傷害計算；治療效率已接實際治療計算。
- `public/native-socket.js` 與 `public/index.html` inline JavaScript 通過 Node 語法檢查。
- WARN 僅為本地無 Docker/Podman 與無 DATABASE_URL，需 Render 部署後做真實整合 smoke。

# 65-cpp.26.10 更新進度條／傻瓜化介面自檢

- 全域登入／載入／進房進度條：PASS。
- PWA 更新下載／套用進度：PASS。
- 玩家簡易模式與完整功能切換：PASS。
- DM 管理中心常用／進階分層：PASS。
- 首頁快速開始：PASS。
- 常見錯誤訊息人話化：PASS。
- 原有房間、命隙、魔法學、成長裝備、序列技能、DM 助手回歸：PASS。

# 65-cpp.26.9 最終自檢結果

- 208 PASS / 0 FAIL / 2 WARN。
- DM 助手仍只有 3 個 create-only POST 白名單。
- DM 助手配方成品後端只接受 item / equipment / weapon。
- special_currency 在助手前端隱藏，手動呼叫 API 也會被 403 擋下。
- NPC／怪物卡／規則怪談與玩家資產發放權限仍不開放給助手。
- JavaScript inline script 與 native-socket.js 語法檢查通過。
- WARN 僅為本地無 Docker/Podman 與無真實 DATABASE_URL；需 Render 部署驗證完整 Docker/PostgreSQL。

# 65-cpp.26.9 DM 助手權限強化回歸檢查

- 驗證 DM 助手仍只有 3 個 create-only POST 白名單。
- 驗證配方輸出只允許 item/equipment/weapon。
- 驗證 special_currency 同時由前端隱藏與後端拒絕。
- 驗證 NPC／怪物搜尋仍只對正式 DM 開放。

# 65-cpp.26.8 DM 助手權限回歸檢查

最終靜態發版檢查：**205 PASS / 0 FAIL / 2 WARN**。兩個 WARN 為本地無 Docker/Podman 與無真實 `DATABASE_URL`。

- DM 助手仍以玩家帳號登入，不取得 `is_admin`。
- 正式 DM 可即時指定／撤銷 `is_dm_assistant`。
- 後端只提供 create-only 白名單：商店道具／裝備武器、技能、合成配方。
- 無 DM 助手 PATCH/DELETE 路由；玩家資產發放仍為正式 DM 專用。
- NPC／怪物模板不出現在非 DM 全站搜尋，DM 敏感管理頁不出現在助手資料中心。

# 65-cpp.26.7 序列階段技能回歸檢查

- 結果：197 PASS / 0 FAIL / 2 WARN。
- 任意階段名稱，不依賴「章」字樣。
- 永久解鎖與本場進度分離。
- 第一次推進禁止跳階；已用階段可重複。
- 一回合最多一次與循環規則由 C++ 後端強制。
- 戰鬥開始只重置本場進度，不重置永久解鎖。

# 65-cpp.26.6 成長裝備回歸檢查

最終靜態發版檢查：**192 PASS / 0 FAIL / 2 WARN**。兩個 WARN 為本地無 Docker/Podman 與無真實 `DATABASE_URL`。

- 檢查成長值、條件、材料資料格式。
- 檢查使用裝備增加成長值。
- 檢查玩家成長 API 同時驗證三項需求並在成功後扣材料。
- 檢查 DM 編輯器可建立與再次修改成長裝備。
- 保留 65-cpp.26.5 進階形態技能、命隙、房間 fallback 與魔法學。

# 65-cpp.26.5 進階形態／階段技能回歸檢查

- 結果：187 PASS / 0 FAIL / 2 WARN。
- WARN：本地無 Docker/Podman；無真實 `DATABASE_URL`，Render Docker build / PostgreSQL smoke 需部署後驗證。
- 編輯器：自訂資源、啟動門檻、區間階級、必要道具、形態名稱、屬性轉換與極限退出條件。
- 血淚值範例：40/120、80～99、100～119、120；雙魚玉佩；精神→力量 ×1.5；理智＝精神 ×1.2。
- 執行層：啟動／退出、開戰重置、區間刷新、資源歸 0、下一回合跳過與不可被選中。
- 相容性：舊技能快照於開戰時讀最新版模板；移除技能會清理對應資源且不能在形態啟用時直接刪除。

# 65-cpp.26.4 命隙副本自動選擇回歸檢查

- 命隙改用目前房間 `roomWorld.dungeons` 產生副本下拉，不再手動輸入資料庫 ID。
- 進入命隙時自動挑選第一個可用副本並讀取占卜狀態。
- 沒有房間／沒有副本時顯示明確空狀態並停用占卜。
- 塔羅狀態與紀錄改為 `Promise.all` 平行讀取，減少命隙載入等待。
- 保留 65-cpp.26.3 JSON helper 編譯修正、房間 snapshot fallback、WebSocket fallback 與魔法學。

# 65-cpp.26.3 Render 編譯 Hotfix

- 修正 `parseJsonText` 未宣告：改用既有 `parseJson`。
- 修正 `jsonString` 未宣告：改用既有 `compactJson`。
- 拆分魔法學更新處連續單行 `if`。
- 保留房間載入 fallback、命隙 UI/API/資料表與 65-cpp.26 魔法學。

# 65-cpp.26.2 命隙恢復回歸檢查

待 release_check 執行後確認。

# 65-cpp.26.2 房間載入 Hotfix 自檢

- 結果：171 PASS / 0 FAIL / 2 WARN。
- WebSocket `room:enter`：完整快照失敗時會回傳核心 rooms / room_members 安全快照。
- `GET /api/rooms/{id}`：viewer snapshot 失敗時會再降級到 resilientRoomSnapshot。
- 前端房間附加資料採 Promise.allSettled；單項失敗不阻止核心房間畫面。
- Service Worker 快取已升版，避免舊前端持續快取。
- WARN 僅為本地無 Docker/Podman 與無 DATABASE_URL，需 Render 部署後做真實整合 smoke。

# TRPG Online C++ 65-cpp.26 自我檢查報告

## 結果

- PASS: 168
- FAIL: 0
- WARN: 2

## 65-cpp.26 本版重點

- 七元素魔法技能樹保留並擴充施放元素成本。
- 魔法節點可連結既有技能模板，共用多段攻擊／事件型技能核心。
- 魔法節點可要求實體魔法書，並可選擇學習後消耗。
- 玩家／怪物／NPC 支援元素抗性與弱點。
- 元素傷害套用：`100% + 親和 - 抗性 + 弱點`。
- DM 魔法編輯器以簡單欄位管理元素成本、技能模板與魔法書。
- 64-cpp.25.4 行動／說話／待機、儀式學、角色登階、地圖、怪物與事件技能回歸均保留。

## 已執行

- `python3 tools/release_check.py`: 168 PASS / 0 FAIL / 2 WARN
- `node --check`：`native-socket.js` 與 `index.html` 內嵌 JavaScript 通過。
- Security C++：`g++ -std=c++20` 真編譯與 JWT/bcrypt 單元測試通過。
- CMake：使用 fake imported Drogon target 的標準目錄 configure 通過。
- Python：`release_check.py`、`integration_smoke.py` AST 語法解析通過。

## WARN

1. 本工作環境沒有 Docker/Podman，因此無法在此執行完整 Render Docker build。
2. 本工作環境沒有實際 `DATABASE_URL`，因此真實 PostgreSQL integration smoke 需由 Render 執行。

## 完整 Drogon Build 嘗試

已另外執行真實 `cmake -S . -B ...`。目前工作環境沒有 Drogon 開發套件，因此停止於：

```text
Could not find DrogonConfig.cmake / drogon-config.cmake
```

這是環境依賴缺失，不是本次 release check 的程式 FAIL；Render Dockerfile 會安裝／建置 Drogon 後再進行完整編譯與測試。

## 發版封裝驗證

- 專案來源檔案：55
- ZIP 全新解壓檔案：55
- 逐檔 SHA-256：55 / 55 一致
- 解壓版再次執行 `release_check.py`：168 PASS / 0 FAIL / 2 WARN
- ZIP 最上層資料夾：`rpg_web_global_mvp/`
