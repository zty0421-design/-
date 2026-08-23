# TRPG Online C++ 60-cpp.21.2 自我檢查報告

## 本版主題

**開團／轉盤緊急修正**。保留 60-cpp.21 怪物完整戰鬥實體與 60-cpp.21.1 Render 編譯修正，針對實際部署後回報的「無法開團／無法使用轉盤」補強前後端關鍵路徑。

## 修正內容

### 1. 開團事件不再被其他前端模組拖垮

- `public/index.html` 新增 `[前端功能備註｜60-cpp.21.2 開團／轉盤緊急修正]`。
- `bindView()` 會先執行 `bindCriticalRoomWheelEvents()`，之後才進入裝備／詞條與其他大型管理模組事件綁定。
- 即使其他非關鍵 UI 的事件綁定發生例外，`#startCampaign` 與 `.spin-wheel` 仍已先取得可用 handler。
- `bindGearAffixEvents()` 增加隔離式 `try/catch`，不再讓詞條 UI 綁定錯誤中斷整個頁面。

### 2. 開團後端降級保護

- `CoreService.cpp` 新增 `[功能備註｜60-cpp.21.2 開團穩定性]`。
- `POST /api/rooms/{id}/start` 先只更新跑團必要核心欄位：`status / saved / round / updated_at / closed_at`。
- 戰鬥狀態欄位重置改為第二段 best-effort；舊資料庫若個別後期戰鬥欄位尚未補齊，不會因此讓「開團」整體失敗。

### 3. 轉盤升級為 C++ 核心 migration

- `custom_wheels`、`wheel_spins` 與索引正式加入 `CoreService::initializeDatabase()`。
- 不再只依賴 `legacy_v39_migrations.sql` 才能建立輪盤資料表。
- legacy migration 仍保留冪等相容版本，舊 PostgreSQL 可安全升級。

### 4. Service Worker 強制換版

- 新快取：`trpg-online-v60-cpp-21-2-room-wheel-hotfix-1`。
- 避免手機 App／PWA 繼續保留上一版開團／轉盤事件程式。

### 5. Render PostgreSQL 整合測試加強

`tools/integration_smoke.py` 現在會實際驗證：

1. 建立房間。
2. 玩家加入房間。
3. DM 開團並確認 `status=active`。
4. DM 建立房間輪盤。
5. 玩家讀取該輪盤。
6. 玩家實際轉動輪盤並取得合法結果。
7. DM 刪除測試輪盤。

## 本地發版檢查

```text
96 PASS
0 FAIL
2 WARN
```

兩個 WARN：

- 目前工作環境沒有 Docker/Podman，因此完整 Render Docker + Drogon 編譯仍由 Render 驗證。
- 目前工作環境沒有實際 `DATABASE_URL`，因此真 PostgreSQL integration smoke 仍由 Render 部署環境執行。

已通過項目包含：

- `index.html` 內嵌 JavaScript 語法檢查。
- `native-socket.js` 語法檢查。
- Security C++ 編譯與 JWT/bcrypt 單元測試。
- CMake 標準目錄 configure。
- 244 / 244 v39 HTTP API handler 對齊。
- HTTP handler 無重複。
- 60-cpp.21 怪物五維／主動／被動／大道／Boss／AI 回歸。
- 60-cpp.21.1 `LegacyCompat.cpp` clipped helper Render 編譯回歸。
- 60-cpp.21.2 開團降級保護、輪盤核心 migration、關鍵事件優先綁定、輪盤 integration smoke 靜態回歸。

