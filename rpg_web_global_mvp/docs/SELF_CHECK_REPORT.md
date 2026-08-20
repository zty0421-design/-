# TRPG Online C++ 60-cpp.21 自我檢查報告

檢查日期：2026-08-20

## 本版主題

**怪物完整戰鬥實體：五維 + 主動／被動技能 + 可選大道／大道技 + Boss 階段 + AI 技能條件。**

## 本版完成

- 怪物五維正式統一為：力量 `strength`、敏捷 `agility`、體質 `constitution`、精神 `spirit`、幸運 `luck`。
- `monster_templates` 新增正式欄位：
  - `passive_skills`
  - `great_way`
  - `boss_phases`
- `skills` 支援結構化主動技能；舊純文字技能維持相容，只顯示、不會被 AI 誤當成可執行攻擊。
- 大道為**選填**：只有 `great_way.enabled=true` 的怪物會初始化大道資源與使用大道技。
- 手動放入房間、規則引擎 `spawn_monster` 都會初始化 `current_fifth/max_fifth`。
- 大道技能可實際消耗怪物大道資源。
- 怪物 AI 技能條件支援：自身 HP、目標 HP、回合、Boss 階段、機率、資源足夠性。
- AI 依 `priority` 選擇目前可用的最高優先度技能，沒有符合條件的技能才回退基本攻擊。
- AI 使用結構化怪物技能時會送 `COMBAT_SKILL_USED`，並繼續送原有戰鬥規則事件。
- Boss `boss_phase_index` 的 0-based 舊資料已在 AI 判斷轉成玩家看到的 1-based `phase=1/2/...`。
- 修正舊怪物建立／修改戰鬥欄位可能「畫面看似保存、實際沒有落到 config」的相容問題。
- 怪物五維 PATCH 支援局部修改；只改一維不會把其他四維覆寫成 0。
- 房間快照會攜帶怪物主動技能、被動技能、大道、Boss 階段、精神與大道資源。
- DM 怪物管理頁新增五維、主動技能、被動技能、大道、大道技能與 AI 條件輸入。
- 玩家看到已揭露怪物時會顯示五維、技能與大道資訊。
- C++ 快速怪物表單也加入幸運。

## 功能備註

本版新增／修改位置均保留可搜尋備註，主要標記：

- `[功能備註｜60-cpp.21 怪物完整戰鬥實體]`
- `[功能備註｜60-cpp.21 怪物五維]`
- `[功能備註｜60-cpp.21 怪物技能]`
- `[功能備註｜60-cpp.21 怪物大道]`
- `[功能備註｜60-cpp.21 怪物戰鬥 AI]`
- `[前端功能備註｜60-cpp.21 怪物技能]`
- `[前端功能備註｜60-cpp.21 怪物完整戰鬥實體]`

同步更新：`FEATURE_MAP.md`、`ANNOTATION_BUILD.txt`、`CPP_MIGRATION_STATUS.md`、`api_parity.json`。

## Render / PostgreSQL 整合測試新增

`tools/integration_smoke.py` 現在會在真實部署環境：

1. 建立一隻五維怪物（含幸運）。
2. 建立結構化主動技與被動技。
3. 建立啟用的大道與大道技。
4. 建立 Boss 階段。
5. 驗證頂層戰鬥欄位真正合併進 `config`。
6. 只 PATCH 幸運並驗證敏捷等其他屬性不被覆寫。
7. 放入房間並驗證大道資源初始化為 9/9。
8. 開始戰鬥後讓 AI 依條件選出最高優先度的「測試大道技」。
9. 驗證使用後大道資源由 9 扣為 8。
10. 驗證戰鬥行動數仍正常消耗與輪轉。

另外修正 integration smoke 原本可能在 `passed()` helper 定義前呼叫的順序問題，並同步檢查 `60-cpp.21` 版本。

## 自我檢查結果

```text
92 PASS
0 FAIL
2 WARN
```

主要通過項目包含：

- 244 / 244 v39 HTTP API handler 仍存在。
- HTTP handler 無重複。
- `index.html` 內嵌 JavaScript 語法通過。
- `native-socket.js` 語法通過。
- Security C++ 真編譯通過。
- JWT / bcrypt 單元測試通過。
- CMake 標準目錄 configure 通過。
- PostgreSQL migration 含怪物被動技能／大道／Boss 正式欄位。
- C++ 怪物五維與 AI 技能判定接線完整。
- 怪物技能會接規則引擎 `COMBAT_SKILL_USED`。
- 怪物修改 API 可持久化完整戰鬥資料。
- 舊純文字技能相容保護存在。
- 怪物五維局部 PATCH 保護存在。
- DM 怪物五維／技能／大道前端完整。
- Service Worker 已切換 `60-cpp.21` 新快取。
- 專案無 build / `__pycache__` / `.pyc` 測試垃圾。
- 59-cpp.20 混合品階／特殊詞條功能全部保留並通過回歸。
- 58-cpp.19 衍生值修正全部保留並通過回歸。
- 60-cpp.21 Render 編譯 hotfix：`LegacyCompat.cpp` 本地 `clipped()` helper 回歸檢查通過。
- knowledge_shop `-Wmisleading-indentation` 兩段警告已整理。

## 2 個 WARN

1. 目前工作環境沒有 Docker / Podman，因此無法在本機執行完整 Render Docker build；部署後由 Render 驗證。
2. 目前工作環境沒有實際 `DATABASE_URL`，因此真 PostgreSQL 整合由更新後的 `tools/integration_smoke.py` 在 Render 執行。

本機沒有 Drogon headers，所以本地「真 C++ 編譯」只覆蓋可獨立編譯的 Security 模組；完整 CoreService / LegacyCompat / RuleCombatBridge 會由 Render Docker build 使用正式 Drogon 環境驗證。這一限制包含在 Docker WARN 內。


## 60-cpp.21.1 Render 編譯 Hotfix

Render 實際 build log 發現：

```text
LegacyCompat.cpp:359:21: error: 'clipped' was not declared in this scope
```

原因是 `clipped()` 原本只存在於 `CoreService.cpp` 的匿名 namespace，無法被另一個翻譯單元 `LegacyCompat.cpp` 呼叫。Hotfix 已在 `LegacyCompat.cpp` 就地加入同樣的 UTF-8 安全截斷 helper，並將同區 `knowledge_shop` 的一行多個 `if` 拆開，消除 `-Wmisleading-indentation`。

此工作環境沒有 Drogon 開發套件與 Docker/Podman，因此無法在本機重現完整 Render Drogon build；`tools/release_check.py` 已新增針對本次錯誤的結構回歸檢查，正式 Drogon 編譯仍由 Render 驗證。

- Root Directory 文件已統一：GitHub Repo 根目錄直接放 Dockerfile/CMakeLists.txt/src/... 時，Render Root Directory 保持空白。
