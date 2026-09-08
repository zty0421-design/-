# 人格值／控制權搶奪｜73-cpp.34.25.22

## 規則
- 人格具有 `personality_value`，範圍 0～100。
- 人格 AI 每次成功產生一次自我意識回應，依 `personality_value_gain` 增加人格值，預設 +2。
- `takeover_threshold` 預設 80。人格值達門檻且開啟 `takeover_enabled` 時，可在玩家送出下一次互動時搶奪本次控制權。
- `takeover_chance` 預設 100%，DM 可自行調整。
- 搶奪時會用 `takeover_actions` 中的一條內容覆蓋玩家本次輸入；若玩家原本選擇待機，會轉成 action。
- 搶奪行動成功通過規則並寫入事件後，才依 `takeover_cost` 扣除人格值，預設 -25；若被 DM 規則阻止則不扣值。
- 搶奪後的行動仍會通過既有 C++ 規則引擎與房間限制，不可繞過 DM 規則。

## 顯示
- 角色卡可顯示人格值進度條與搶奪門檻。
- 達門檻時顯示警示。
- 控制權被搶奪時聊天室／互動資料使用 `speaker_kind=personality_takeover`，前端顯示「⚠ 人格控制」。

## 渲染／載入優化
- 每次互動只查詢一次人格設定，搶奪判定與自我意識 AI 共用 `PersonalityRuntimeContext`。
- 人格值直接沿用互動 API 回傳更新本機角色卡，不額外 GET 角色資料。
- 純說話且沒有規則反應時，不再每次重新抓取整套世界系統。
- 人格值 UI 為輕量 CSS 進度條，不增加圖片解碼或重型元件。
