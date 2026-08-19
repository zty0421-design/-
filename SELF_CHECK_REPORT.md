# TRPG Online C++ 52-cpp.13 自我檢查報告

## 本版重點

- 玩家與 DM 都可直接選 JPG／PNG／WebP 頭像圖片上傳。
- 單檔最大 5 MB；C++ 會解碼 base64 並用真實檔頭驗證格式。
- 圖片持久化於 PostgreSQL `avatar_images`，不依賴 Render 本機檔案系統。
- 保留原本圖片 URL 模式；切換到外部 URL 或清空時會刪除舊的資料庫圖片。
- DM 仍可修改任意玩家頭像；玩家只能改自己的。
- 全站備份與角色備份已納入上傳型頭像。

## 自動檢查

```text
TRPG Render flat-upload self check
PASS 平鋪部署必要檔案完整
PASS ZIP 根資料夾名稱正確
PASS GitHub 上傳內容為真正根目錄平鋪
PASS 52-cpp.13 版本資訊在後端/前端/api_parity 一致
PASS Dockerfile 可從根目錄平鋪重建標準結構
PASS runtime 從 builder 取得 public/db，不依賴 GitHub 子資料夾
PASS CMake 可直接讀根目錄 .cpp/.h
PASS render.yaml 使用 Repo 根目錄
PASS migration 可從平鋪根目錄定位
PASS characterByUser 對戰鬥 helper 為 public，可通過 Render 編譯
PASS 職業 role 使用 std::string 比較，不再比較字串指標
PASS GCC 12 -Wrestrict 已針對 Security/Config 修正
PASS 功能備註索引存在且覆蓋主要 API/前端函式
PASS index.html 每個具名 function 都有前端功能備註
PASS 完整 v39 前端介面已恢復，不再被 C++ preview 隱藏
PASS Drogon 可直接提供 manifest.webmanifest
PASS Service Worker cache 已換版，避免舊預覽頁殘留
PASS 商店分類 PostgreSQL migration 完整
PASS 商店分類 C++ API／驗證／排序查詢存在
PASS 商店分類／搜尋／商品改分類前端完整
PASS DM 修改其他玩家頭像由 C++ 權限與資料庫更新保護
PASS DM 玩家列表快捷頭像按鈕、URL 與直接上傳入口存在
PASS PostgreSQL smoke test 會驗證 DM 修改其他玩家頭像後玩家可讀回
PASS 效率套件 PostgreSQL migration 完整（搜尋/收藏/通知/百科/資源/技能樹）
PASS 全站搜尋、標籤、收藏、通知、百科、自訂資源、時間線與模板中心 C++ API 完整
PASS 效率中心前端含搜尋/收藏/通知/百科/任務追蹤/裝備比較/手機戰鬥
PASS 進階商店含庫存、限購、折扣、出售與交易紀錄
PASS 技能樹前置條件由 C++ 驗證且 DM 可編輯
PASS 狀態效果會在 C++ 新回合自動結算、倒數與到期
PASS 戰鬥輪到玩家時由 C++ 自動產生通知
PASS 技能/道具/材料/魔法/儀式批量匯入支援預覽、驗證與最多 5000 筆
PASS 批量匯入 HTTP body 上限提升至 32 MB
PASS 全站備份已包含效率系統及魔法／儀式資料表
PASS 魔法學／儀式學與七元素儲存 migration 完整，初始上限/儲存量皆為 0
PASS 魔法學／儀式學玩家學習、DM 資料庫與點數管理 C++ API 完整
PASS 七元素容量由 C++ 即時計算，支援技能/道具/魔法/儀式與永久擴容
PASS DM 可設定技能/道具單一元素上限與永久單元素擴容道具
PASS 魔法學／儀式學各有獨立玩家頁與 DM 頁，並可管理元素儲存
PASS 任務可發放魔法學／儀式學點數
PASS 全站搜尋與模板中心已納入魔法學／儀式學
PASS v48 規則書／頭像／商人／通訊／離線 AI／元素自生 migration 完整
PASS v48 規則書、位置、通訊、邀請、離線 AI、元素自生 C++ API 完整
PASS 非同場景通訊限制／通訊道具／禁訊區會由 C++ 過濾聊天
PASS 離線 AI 可自動入隊、跟隊長、離線隊長帶隊探索、單人探索並在上線時交回控制
PASS 技能／道具可自動生成七元素，避免技能模板重複計算，新增與修改 UI 均可設定
PASS v48 規則書、NPC/玩家頭像、商人、地圖生成、禁訊、通訊道具、離線 AI 前端完整
PASS 規則引擎四張 PostgreSQL 狀態／紀錄表完整
PASS 規則引擎 DM CRUD／測試／紀錄／狀態 API 完整
PASS 規則引擎支援 AND/OR/NOT 條件樹與核心效果
PASS 玩家行動與規則倒數已接入 C++ 規則引擎
PASS DM 有獨立規則引擎頁、JSON 條件/效果編輯與 dry-run 測試器
PASS RuleEngine 已加入 CMake 與 GitHub 平鋪 Docker 重建
PASS 全站備份已包含規則引擎資料表
PASS 房間存檔／還原會保留規則引擎進度與觸發紀錄
PASS 規則引擎邊界防護：無玩家測試、通知去重、同房傳送驗證完整
PASS 移動／商店／道具／NPC／隊伍／聊天已自動接線到 C++ 規則引擎
PASS 一般商店會帶房間情境進入規則引擎
PASS 持續場景規則每 30 秒自動檢查，可處理獨處／久留條件
PASS 真人隊伍與離線 AI 移動都受同一套規則引擎約束
PASS RULE_ENGINE_GUIDE 提供事件／標籤／條件／效果與怪談規則範例
PASS WorldSystems 已納入 CMake 與 Render 平鋪 Docker 建置
PASS 新位置／隊伍 SQL 不會寫入不存在的 room_members.updated_at
PASS 離線組隊邀請使用 pending 部分唯一索引，可重複歷史入隊
PASS 規則戰鬥 bridge migration：勝利條件、戰鬥狀態、Boss 階段與修正完整
PASS 規則可直接生成怪物／開戰／Boss 階段／狀態／勝利條件／開關規則
PASS 戰鬥開始／回合／攻擊／擊敗／技能／結束會反向送入規則引擎
PASS 規則 context 可讀 combat.* 且未知效果會交給 C++ 戰鬥橋接層
PASS DM 規則頁有戰鬥效果模板與特殊勝利條件管理
PASS RuleCombatBridge 已加入 CMake 與 Render 平鋪 Docker 建置
PASS 規則引擎指南已補戰鬥事件、效果與勝利條件範例
PASS 244/244 v39 HTTP handler 仍完整
PASS C++ 額外功能 API 與 api_parity.json 完全一致
PASS HTTP handler 無重複
PASS native-socket.js 語法通過
PASS index.html 內嵌 JavaScript 語法通過
PASS 平鋪 Security C++ 可編譯
PASS JWT/bcrypt 測試通過
PASS Docker 平鋪→標準目錄重建模擬通過
PASS CMake 平鋪模式 configure 通過
PASS 玩家／DM 直接上傳頭像 C++ API 與圖片讀取路由存在
PASS 頭像圖片 PostgreSQL 持久化 migration 存在
PASS C++ 頭像格式與 5 MB 上限驗證存在
PASS 玩家與 DM 前端直接選圖流程存在
WARN 目前工作容器沒有完整 Drogon headers；CoreService/LegacyCompat/WorldSystems/RuleEngine/RuleCombatBridge 需由 Render/Docker 做完整編譯驗證
WARN 目前工作容器未提供 DATABASE_URL；v52 頭像直接上傳與規則戰鬥橋接 API 尚未在真實 PostgreSQL 做端到端回歸
SUMMARY pass=83 fail=0 warn=2
```

## 尚需 Render 驗證

本地工作容器沒有完整 Drogon headers 與真實 `DATABASE_URL`，因此新增的 WorldSystems C++ 路由仍需由 Render/Docker 完整編譯並在 PostgreSQL 上執行 integration smoke。
