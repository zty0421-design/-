# TRPG Online C++ 44-cpp.5 — Render 根目錄平鋪版檢查

日期：2026-08-18

這個發版包是針對 GitHub Repo **所有程式檔直接放在根目錄**的部署方式製作。

## 結果

- 平鋪版專用檢查：**16 PASS / 0 FAIL / 0 WARN**
- v39 HTTP handler：**244 / 244**
- Dockerfile：可從根目錄平鋪檔案重建 `src/`、`include/trpg/`、`tests/`、`db/`、`public/`
- Docker runtime：從 builder 的 `/src/public` 與 `/src/db` 複製，不再要求 GitHub build context 原本就有子資料夾
- CMake：支援標準資料夾與根目錄平鋪兩種結構
- migration：同時支援 `db/legacy_v39_migrations.sql` 與根目錄 `legacy_v39_migrations.sql`
- Render Blueprint：沒有 `rootDir`，Dockerfile 為 `./Dockerfile`，context 為 `.`
- Security C++：實際編譯與 JWT/bcrypt 測試通過
- 前端 JavaScript：語法檢查通過
- CMake 平鋪模式：configure 通過

## Render 必須設定

```text
Root Directory: 留空
Dockerfile Path: ./Dockerfile
Docker Build Context Directory: .
```

這個包解壓後的唯一最外層資料夾仍固定為 `rpg_web_global_mvp/`；但上傳 GitHub 時要上傳**該資料夾內的檔案**，讓 `Dockerfile` 位於 Repo 根目錄。
