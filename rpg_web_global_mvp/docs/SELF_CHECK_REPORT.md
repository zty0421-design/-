# TRPG Online C++ 65-cpp.26.4 自我檢查報告

## 結果

- PASS: 183
- FAIL: 0
- WARN: 2

## 65-cpp.26.4 本版重點

- 玩家與 NPC 新增多立繪資料表，可保存多張普通／戰鬥／受傷／生氣等立繪。
- 玩家可自行新增、上傳、切換、設預設、改名與刪除；DM 可對 NPC 執行相同管理。
- active 立繪同步既有 `character_cards.portrait_url` / `npc_templates.portrait_url`，房間舞台與 NPC 對話直接讀取目前立繪。
- 角色備份／還原加入 `portrait_variants`；還原後會依新 ID 重建目前立繪 URL。全站備份加入玩家與 NPC 多立繪表。
- 戰鬥 UI 新增回合與目前行動者、橫向行動順序、敵我單位卡、HP／精神／理智／職業能量／元素資訊與 Buff／Debuff。
- 倒地角色仍保留在戰鬥單位卡中；DM 復甦沿用既有 C++ `/combat/revive`。
- 玩家可先點單位卡預選目標；基本攻擊與技能仍由既有 C++ 後端驗證。
- 手機戰鬥加入固定底部攻擊／技能／跳過快捷列，並在戰鬥時隱藏一般底部導覽避免重疊。
- 65-cpp.26.3 房間立繪舞台、手機第二輪 UI、65-cpp.26 魔法學與 64-cpp.25.4 行動／說話／待機均保留。

## 額外修正

- 已驗證全新 PostgreSQL migration 順序：先建立 `npc_templates`，再建立 NPC 多立繪資料表，避免空資料庫啟動失敗。

## 已執行

- `python3 tools/release_check.py`: 183 PASS / 0 FAIL / 2 WARN。
- `node --check`：`native-socket.js` 與 `index.html` 內嵌 JavaScript 通過（由 release check 執行）。
- Security C++：`g++ -std=c++20` 真編譯與 JWT/bcrypt 單元測試通過。
- CMake：使用 fake imported Drogon target 的標準目錄 configure 通過。
- Python：`release_check.py`、`integration_smoke.py` 語法檢查通過。
- 全站備份資料表陣列大小檢查通過，新增兩張多立繪表後仍一致。
- HTTP handler 重複檢查通過，244 條 v39 API parity 維持完整。

## WARN

1. 本工作環境沒有 Docker/Podman，因此無法在此執行完整 Render Docker build。
2. 本工作環境沒有實際 `DATABASE_URL`，新增多立繪 lifecycle 與其他 PostgreSQL integration smoke 需由 Render 執行。

## 完整 Drogon Build 嘗試

另執行真實 `cmake -S . -B ...`；目前環境缺少 Drogon 開發套件，停止於 `Could not find DrogonConfig.cmake / drogon-config.cmake`。這屬於工作環境依賴缺失；Render Dockerfile 仍負責安裝／建置 Drogon。
