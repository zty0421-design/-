# TRPG Online C++

版本：`44-cpp.5`

這個專案是 `rpg_web_global_mvp` 的 C++20 / Drogon / PostgreSQL 後端版本。`44-cpp.5` 已把 v39 的 **244 條 HTTP API 路由介面全部註冊到 C++**，保留 3 個瀏覽器原生 WebSocket 核心事件，並加入 v39 PostgreSQL schema/migration 相容層。

## 目前完成範圍

- 帳號、JWT、DM 權限、角色卡與大型數值。
- 職業、技能、召喚物、配方、背包、裝備與商店。
- 房間建立／加入／離開／開始／存檔／重開／關閉與多人同步。
- 怪物、NPC、副本、地圖、線索、狀態、恐怖規則與調查。
- D100 基本戰鬥、敏捷行動序、多行動回合、玩家反應、怪物／NPC AI、傷害與倒地／擊敗。
- 任務、隊伍、私人幻象、世界事件、追逐、Boss 階段、搜索容器。
- 房間與角色存檔點、備份預覽／確認、稽核。
- 大道、信仰、輪盤、勢力、聲望、NPC 對話與 NPC 商店。
- `db/legacy_v39_migrations.sql`：v39 後期完整資料結構相容 migration。

精確路由清單見 `api_parity.json`。

## 重要狀態

`code_complete = true` 表示 **v39 的 244 條 HTTP 路由介面在 C++ 專案中都有實作入口**；這不等於已完成正式環境驗證，因此目前仍保持：

```text
production_ready = false
```

在公開部署前仍必須完成：正式 Drogon 1.9.13 全量編譯／連結、真實 PostgreSQL 資料遷移與 244 API 回歸、Docker/Render WebSocket 代理測試、桌面／手機 E2E，以及多人高併發壓力測試。

## 本機建置

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/trpg_cpp
```

必要環境變數請參考 `.env.example`。`JWT_SECRET` 必須由部署環境提供，不應使用固定預設值。

## 自我檢查

```bash
python3 tools/release_check.py
```

若已有可執行的完整 CMake build：

```bash
TRPG_BUILD_DIR=build python3 tools/release_check.py
```

若已啟動 C++ 伺服器與 PostgreSQL：

```bash
SMOKE_BASE_URL=http://127.0.0.1:10000 \
ADMIN_USERNAME=your-dm-account \
ADMIN_PASSWORD=your-dm-password \
python3 tools/release_check.py
```

## 部署

`Dockerfile` 固定 Drogon `v1.9.13`，並在 builder 階段執行 CTest。Render 使用 `render.yaml` 的 `/api/health` 作為健康檢查。
