# TRPG C++ 功能備註版自我檢查

日期：2026-08-18
基準：44-cpp.5 full-ui Render hotfix

## 本次修改

- 不變更遊戲規則與 API 行為，只新增中文功能備註與修改索引。
- CoreService.cpp：38 個核心 HTTP handler 備註。
- LegacyCompat.cpp：190 個 v39 相容／管理路由註冊備註。
- index.html：287 個具名前端 function 全部加入功能備註。
- RoomSocket.cpp：room:enter、chat:send、dice:roll 三個 WebSocket 事件備註。
- main.cpp：環境設定、資料庫 migration、HTTP API、前端靜態檔、安全標頭備註。
- FEATURE_MAP.md：244 條 v39 HTTP API 的功能 → API → 主要後端檔案索引，以及前端函式索引。

## 自我檢查結果

- 24 PASS
- 0 FAIL
- 0 WARN
- 244/244 v39 HTTP handler 保持完整。
- HTTP handler 無重複。
- index.html 287/287 具名 function 具有前端功能備註。
- native-socket.js 與 index.html 內嵌 JavaScript 語法通過。
- Security C++ 測試可編譯且 JWT/bcrypt 測試通過。
- CMake 平鋪模式 configure 通過。
- Docker 平鋪 → 標準目錄重建模擬通過。

## 修改時建議

先開啟 `FEATURE_MAP.md` 搜尋功能名稱；再依 API 前往 `CoreService.cpp` 或 `LegacyCompat.cpp`。前端可在 `index.html` 搜尋 `[前端功能備註｜功能分類]`。

修改完成後執行：

```bash
python3 release_check.py
```
