# TRPG Online C++ 64-cpp.25.1 自我檢查報告

## 本版主題

**Render C++20 編譯 Hotfix**。

Render 真實編譯抓到：

```text
LegacyCompat.cpp:1164:14: error: expected unqualified-id before 'requires'
```

CMake 使用 `CMAKE_CXX_STANDARD 20`，而 `requires` 是 C++20 保留關鍵字；舊版 `ritualStudyForViewer()` 將其作為區域變數名稱，因此 GCC 直接拒絕編譯。

## 本版修正

- `[功能備註｜64-cpp.25.1 Render C++20 編譯修正]`
- `const bool requires` 改為 `const bool requiresResearch`。
- 同步修正 `research_complete` 與研究遮罩條件引用。
- 清除 `syncBidirectionalMapConnections()` 中未使用的 `hasNode`，消除 `-Wunused-but-set-variable` warning。
- 掃描 `src/`、`include/` 的 C++20 新保留字，未發現其他相同識別字衝突。
- `release_check.py` 新增回歸：禁止 `const bool requires=` 回歸、禁止 `hasNode` 未使用變數回歸。
- 版本與 Service Worker 更新至 `64-cpp.25.1`。

## 發版檢查

```text
154 PASS
0 FAIL
2 WARN
```

兩個 WARN：

1. 目前工作環境沒有 Docker/Podman，完整 Render Docker build 仍由部署環境驗證。
2. 目前工作環境沒有真實 `DATABASE_URL`，PostgreSQL integration smoke 由 Render 驗證。

本機可完成的 Security C++ 編譯、JWT/bcrypt 單元測試、CMake configure、前端 JavaScript 語法、244/244 v39 API 相容檢查均通過。

## 部署

GitHub Repo 根目錄放 `rpg_web_global_mvp/` 內的內容；Render Root Directory 留空，建議使用 **Clear build cache & deploy**。
