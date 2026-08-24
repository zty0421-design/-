# TRPG Online C++ 64-cpp.25 自我檢查報告

## 本版主題

**角色正式等階／四小級／固定屬性總額／登階與登神儀式**。本版以既有 63-cpp.24.1 儀式學系統為基礎，把角色成長恢復成正式世界觀規則。

## 64-cpp.25 新增與修正

- 角色採 0～10階，每階1～4級。
- 9階=偽神、9階4級=半神、10階=真神。
- 0→1 不需要儀式；其他跨大階必須在本階4級完成登階／登神儀式。
- 大階固定總屬性額度：0、50、100、180、320、600、1100、2000、3600、6500、10000。
- 五項可加點：敏捷、力量、體質、精神、大道／神明。
- 幸運改回不可自由加點的特殊屬性。
- 玩家角色 PATCH 強制驗證「五項已分配 + 剩餘 = 當前大階總額」。
- 生命、耐力、理智、意志維持正式衍生公式。
- DM 新增 `POST /api/admin/players/:id/character-rank/advance`，只處理0→1與同階小級。
- 儀式配方新增 `ascension_from_tier / ascension_to_tier`。
- 登階儀式成功才真正跨大階；跨階後保留已分配屬性並依新總額補足剩餘點。
- 進9階、進10階必須使用「登神儀式」。
- 舊任務屬性點獎勵不再突破固定大階總額；任務完成後重新校正剩餘基礎屬性點。
- 角色卡、DM角色編輯器、儀式配方編輯器與功能備註同步更新。

## Render integration smoke 更新

實際部署環境會驗證：

1. 新角色由0階直接升到1階1級，取得50點總額。
2. 玩家在1階配置40點基礎屬性，剩餘10點。
3. DM依序推進1階2級、3級、4級。
4. 1階4級再次普通推進必須回409，不能直接跨階。
5. 建立「1→2登階儀式」並由玩家學習、建立儀式場與成功啟動。
6. 驗證角色變成2階1級。
7. 驗證2階總額100，原已分配40保留，剩餘自動變60。

## 發版檢查

```text
152 PASS
0 FAIL
2 WARN
```

2 個 WARN：

- 目前工作環境沒有 Docker/Podman；完整 Render Docker build 需部署後驗證。
- 目前工作環境沒有實際 `DATABASE_URL`；真 PostgreSQL integration smoke 由 Render 驗證。

補充：目前環境缺少完整 Drogon 開發套件，因此無法本地完成與 Render 完全相同的 Drogon target build；前端 JavaScript、Python、Security C++、API parity、migration／路由／功能回歸均由發版檢查覆蓋。

## 部署

- ZIP 根目錄：`rpg_web_global_mvp/`
- GitHub：上傳 `rpg_web_global_mvp/` 裡面的內容到 Repo 根目錄。
- Render Root Directory：留空。
- 建議：`Clear build cache & deploy`。

## ZIP 重解壓一致性

```text
來源檔案：53
ZIP 解壓：53
SHA-256：53 / 53 完全一致
解壓後 release_check：152 PASS / 0 FAIL / 2 WARN
```
