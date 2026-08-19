# TRPG Online C++ 44-cpp.5 自我檢查報告

檢查日期：2026-08-18

## 結論

本版完成 v39 HTTP API 的 **244 / 244 C++ 路由介面移植**，另保留 6 條 C++ 專用 GET；實際 handler 掃描結果為 **250 條、0 缺失、0 重複**。v39 PostgreSQL 相容 migration 已掛入啟動流程，包含 **66 張表／285 段 DDL**。

可攜式 release check 結果：

- **30 PASS**
- **0 FAIL**
- **4 WARN**

`code_complete=true` 代表 v39 的 HTTP 相容介面在 C++ 專案中已有實作入口；`production_ready=false` 仍保留，因本次執行環境不能完成真正的 Drogon/PostgreSQL/Docker/瀏覽器全鏈路驗證。

## 本版完成與修正

- v39 244 條 HTTP 路由全部有 C++ handler，`LegacyRouteManifest`、`api_parity.json`、`/api/cpp/status` 逐條一致。
- 3 個原生 WebSocket 核心事件保留：`room:enter`、`chat:send`、`dice:roll`。
- `db/legacy_v39_migrations.sql` 已由 `CoreService::initializeDatabase()` 實際套用，Docker builder/runtime 均包含 `db/`。
- 職業、技能、召喚、配方、商店、房間、戰鬥、怪物/NPC、副本、任務、恐怖、線索、狀態、地圖、隊伍、幻象、追逐、容器、存檔、稽核、大道/信仰、輪盤、勢力聲望與 NPC 社交/交易均有 C++ PostgreSQL 實作。
- 大道/信仰授予會驗證等階上限、授予關聯技能並同步身份來源效果。
- 召喚物未指定房間時使用 `NULLIF(room_id,0)`，避免外鍵寫入 `room_id=0`。
- NPC 商店改為先驗證貨幣，再原子扣減有限庫存，避免餘額不足卻先扣庫存。
- 全站備份匯出覆蓋 `users` 安全欄位 + 其餘 65 張表，明確排除密碼雜湊與伺服器密鑰。
- 備份中心改成 v39 envelope：schema/version/checksum/scope/target/checkpoint；房間與角色支援預覽、10 分鐘 restore token、確認文字與還原前自動安全存檔；全站封存只允許匯出，不允許網頁整站覆寫。
- 備份 checksum 與 restore token 使用 SHA-256；不使用平台不穩定的 `std::hash`。
- Service Worker cache 已更新為 `trpg-online-v44-cpp-5`。
- ZIP 專案根資料夾固定為 `rpg_web_global_mvp`。

## 30 個通過項目

1. 必要檔案完整。
2. 專案根資料夾為 `rpg_web_global_mvp`。
3. 版本號 `44-cpp.5` 一致。
4. 無舊 C++ 版本／快取字串。
5. API 對等清單為 244 / 244。
6. 244 條 v39 HTTP 路由都有實際 C++ handler。
7. C++ 額外 HTTP 路由與清單一致（6 條）。
8. HTTP handler 沒有重複註冊。
9. `LegacyRouteManifest` 與 `api_parity.json` 244 條逐條一致。
10. `/api/cpp/status` 直接使用 244 路由 manifest。
11. `code_complete=true` 且 `production_ready=false`。
12. legacy migration 與 244 路由在啟動流程實際掛載。
13. CMake 已編入 `LegacyCompat.cpp` 並同步 44.5.0。
14. Docker builder/runtime 都包含 DB migration。
15. v39 migration 覆蓋 66 張表／285 段 DDL。
16. 備份 checksum／10 分鐘 restore token 使用 SHA-256。
17. 全站備份覆蓋其餘 65 張表且不匯出密碼雜湊。
18. 備份中心具 v39 envelope／預覽／房間與角色兩階段還原契約。
19. 未指定房間的召喚物不會寫入 `room_id=0`。
20. NPC 商店先驗資金再扣有限庫存。
21. 3 個原生 WebSocket 核心事件齊全。
22. 前端沒有 Socket.IO 依賴。
23. Service Worker 快取版本為 v44-cpp-5。
24. PWA 四個圖示尺寸正確。
25. PWA manifest 可安裝。
26. `native-socket.js` JavaScript 語法通過。
27. `index.html` 內嵌 JavaScript 語法通過。
28. 整合測試腳本 Python 語法通過。
29. Security C++ 單元測試可編譯。
30. JWT／bcrypt 單元測試通過。

另外，`LegacyCompat.cpp` 已用 `g++ -std=c++20 -fsyntax-only` 搭配最小 Drogon/Json API stub 做純 C++ 語法與型別檢查，結果為 0 error；**這不能替代真正 Drogon 編譯/連結**，因此沒有算進上述 30 PASS。

## 4 個環境警告

1. 本次環境沒有可用的完整 Drogon build，因此沒有宣稱完整 C++ 編譯／連結已驗證。
2. 沒有正在執行的 PostgreSQL + C++ Server，因此沒有執行真實 244 API／WebSocket E2E。
3. 本次環境沒有 Docker，因此沒有執行 `docker build`。
4. 雖偵測到瀏覽器執行檔，但沒有配置完整桌面／手機 E2E 測試案例，因此沒有宣稱瀏覽器回歸通過。

## 重跑方式

```bash
python3 tools/release_check.py
```

若已有正式 Drogon build：

```bash
TRPG_BUILD_DIR=build python3 tools/release_check.py
```

若 C++ Server 與 PostgreSQL 已啟動：

```bash
SMOKE_BASE_URL=http://127.0.0.1:10000 \
ADMIN_USERNAME=your-dm-account \
ADMIN_PASSWORD=your-dm-password \
python3 tools/release_check.py
```

## ZIP 打包後驗證

最終候選 ZIP 已經執行一次「打包 → 全新目錄解壓 → 逐檔 SHA-256 → release check → C++ syntax-only」驗證：

- ZIP 唯一頂層目錄：`rpg_web_global_mvp/`
- 來源檔案：34 個
- 解壓檔案：34 個
- 檔名集合：完全一致
- SHA-256：34 / 34 完全一致
- 解壓後 `tools/release_check.py`：**30 PASS / 0 FAIL / 4 WARN**
- 解壓後 `LegacyCompat.cpp` syntax-only：**0 error**

因此本報告所附版本就是以固定根資料夾 `rpg_web_global_mvp` 打包的最終 `44-cpp.5` 候選檔。
