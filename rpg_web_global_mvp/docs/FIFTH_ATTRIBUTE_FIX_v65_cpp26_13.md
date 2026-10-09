# 65-cpp.26.13 第五基礎屬性角色編輯修正

問題：角色頁顯示的 `fifth_current` 已被上限夾限，但玩家 PATCH 驗證使用資料庫 raw current，舊資料若 raw current > max，會出現畫面顯示 0/0 卻報「不能低於目前剩餘值」。

修正：
- 驗證改用 `min(oldMax, rawCurrentValue)`，與角色快照一致。
- 僅修復原本已不合法的 raw current > max／負值／NULL 資料；正常資料仍禁止玩家把上限降到合法目前值以下。
- 角色編輯已分配與剩餘點數直接由五項輸入重算。
