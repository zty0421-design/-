# 儀式學系統總覽｜63-cpp.24.1

## 兩個玩家面板

### 📕 儀式配方
用於「知道怎麼做」：查閱／學習儀式、材料、元素、步驟、佈置說明與 2D 佈置圖。

### ⭕ 儀式場
用於「真的做」：建立房間儀式實例、依配方放置節點、檢查配置、消耗材料／元素、開始引導、逐步推進並結算成功／失敗效果。

## 核心資料

- `ritual_studies`：儀式配方。
- `ritual_instances`：房間內正在進行的儀式實例。
- `layout_definition.slots`：佈置節點。
- `layout_definition.connections`：佈置連線。
- `material_requirements`：背包材料需求。
- `element_requirements`：七元素需求。
- `ritual_steps`：引導步驟。
- `success_effects / failure_effects`：直接共用事件技能 Effect 核心。

## 儀式狀態

`placing → ready → channeling → success / failed`

## 事件

- `RITUAL_CREATED`
- `RITUAL_ITEM_PLACED`
- `RITUAL_STARTED`
- `RITUAL_STEP_COMPLETED`
- `RITUAL_SUCCEEDED`
- `RITUAL_FAILED`

## 與技能系統共用

儀式效果可直接使用既有的傷害、治療、狀態、標記、場地效果與自訂事件，不需要每種儀式另外寫角色專用 C++。


## 63-cpp.24.1 儀式學成長／研究

### 角色成長欄位
- `ritual_xp`：累計儀式學經驗。
- `ritual_level`：`1 + floor(ritual_xp / 100)`。
- `ritual_points`：目前可花費儀式學點數。
- `ritual_points_earned`：累計獲得。
- `ritual_points_spent`：累計花費。
- 每跨 100 XP 自動升 1 級並取得 1 點 `ritual_points`。
- 儀式學點數只用於「解析／學習」，不作為施放儀式的消耗；施放仍消耗配方材料、元素與其他實際成本。

### 未解析儀式
`ritual_studies` 可設定：
- `requires_research`：是否必須解析到 100% 才能學習。
- `hidden_until_discovered`：未被發現前是否完全不顯示。
- `mystery_name`：低研究進度時顯示的未知名稱。
- `research_progress_per_point`：每投入 1 點提升多少研究進度。
- `research_xp_per_point`：每投入 1 點研究時取得多少儀式學 XP（後端上限 20，避免形成刷點循環）。
- `research_reveal_rules`：名稱、說明、材料、元素、佈置、步驟、成功效果、失敗效果的揭露百分比門檻。

每位玩家的進度保存在 `character_ritual_research`，包含：
`discovered / progress / points_invested / successful_completions / first_success_rewarded / failure_rewarded`。
因此 A 玩家解析 80% 不會讓 B 玩家同時知道答案。

### 研究／學習 API
- `GET /api/ritual-studies`：回傳玩家自己的研究遮罩與 `progression`。
- `POST /api/ritual-studies/:id/research`：投入可用儀式學點數研究。
- `POST /api/ritual-studies/:id/learn`：若配方要求研究，C++ 會強制驗證 100% 才能學。
- `POST /api/admin/players/:id/ritual-research`：DM／任務／副本劇情可標記發現、推進研究或發 XP。
- `POST /api/admin/players/:id/study-points`：DM 直接調整點數時同步維護累計獲得／已花費帳本。

### 防刷
- 首次成功完成儀式：依 `first_success_xp` 發 XP；0 代表依品階使用預設值。
- 重複成功：預設 `repeat_success_xp = 0`，DM 可明確開啟少量重複獎勵。
- 特殊失敗：`failure_research_xp / failure_research_progress` 只對每位玩家／每個儀式發一次。
- 研究本身可給少量 XP，但 `research_xp_per_point` 被限制，不會靠投入點數原地無限生點。

### 其他獎勵來源
- 任務 `rewards.ritual_xp` 可直接發儀式學 XP。
- 道具 `effects.ritual_xp` 可作為研究書／殘卷等消耗品；使用後由 C++ 發 XP。
- 任務／道具直接發 `ritual_points` 時也會同步累計獲得。
