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
