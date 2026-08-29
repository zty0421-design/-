# 登入優化｜73-cpp.34.25.7

- 登入畫面會在使用者輸入帳密前預熱 `/api/health`，降低 Render 冷啟動等待感。
- 登入成功後先 render，再延後啟動 WebSocket。
- 角色、上次房間、全量模板／DM 資料依優先級錯峰載入。
- C++ `/api/auth/login` 移除每次登入都執行的 `character_cards INSERT ... ON CONFLICT`，減少一次 PostgreSQL round-trip。
- 登入等待時提供「驗證／喚醒伺服器」狀態，不再看起來像按鈕失效。
- 使用 generation 防止舊登入回應覆蓋新一次登入。
