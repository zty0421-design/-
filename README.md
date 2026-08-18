# TRPG Online C++ — GitHub / Render 根目錄平鋪版

版本：`44-cpp.5`

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
