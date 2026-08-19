# TRPG Online C++ 56-cpp.17 自我檢查報告

## 本版重點

- 通用詞條／洗煉系統：不綁死人偶，可套用到任何啟用詞條的遊戲物件或 DM 自訂目標。
- 詞條核心改為特殊貨幣，可由合成配方產出；DM 可在遊戲內調整詞條池權重，系統即時計算百分比。
- 遊戲內角色配置增加固定主／副欄位與 DM 自訂裝備槽；C++ 驗證所有權、重複配置及雙手類型衝突。
- 玩家角色頁拆出裝備與詞條區，手機版加入快速導覽、卡片式槽位、可收合面板與戰鬥／DM 行動版排版。
- 全站備份新增特殊貨幣、裝備槽、詞條庫、詞條池、洗煉規則與詞條目標資料。
- 合成配方支援輸出特殊貨幣，包括 `trait_core` 詞條核心。

## 可攜式檢查結果

`58 PASS / 0 FAIL / 2 WARN`

主要通過項目：

- 244 / 244 v39 HTTP API 仍有 C++ handler。
- HTTP handler 無重複。
- `index.html` 內嵌 JavaScript 與 `native-socket.js` 語法通過。
- Security C++ 真編譯與 JWT/bcrypt 單元測試通過。
- CMake 標準目錄 configure 通過。
- SQL helper 參數數量檢查通過。
- 全站備份資料表陣列宣告數與實際項目數一致。
- 通用詞條／洗煉 migration、詞條核心、詞條池權重、特殊貨幣配方與裝備槽接線均存在。
- `GearAffixSystem.cpp` 另外以本地介面 stub 執行 C++20 `-fsyntax-only`，結果為 0 error；這只能驗證語法，不取代真實 Drogon 編譯。
- Python 發版與整合測試腳本語法通過。
- 專案沒有 build、`__pycache__` 或 `.pyc` 測試垃圾。

## 仍需 Render 驗證的 2 個項目

1. 目前工作環境沒有 Docker/Podman，因此無法在本地執行完整 Drogon Docker build。部署包已包含 `Dockerfile`、CMake 與 Render 設定。
2. 目前工作環境沒有真實 `DATABASE_URL`，因此新增 migration 與詞條／特殊貨幣 API 的 PostgreSQL 整合需由 Render 部署後的 `tools/integration_smoke.py` 驗證。

## Render 建議設定

若 GitHub Repo 根目錄內有 `rpg_web_global_mvp/` 這一層：

```text
Root Directory: rpg_web_global_mvp
Dockerfile Path: ./Dockerfile
Docker Build Context Directory: .
```

部署時建議使用 Clear build cache & deploy。
