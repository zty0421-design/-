# 73-cpp.34.25.5 即時連線優化

- WebSocket 應用層心跳：20 秒一次，60 秒無任何伺服器活動視為失效並重連。
- 票證請求最多等待 3.2 秒，之後使用既有 JWT 相容握手，降低 Render 冷啟動等待。
- WebSocket CONNECTING 最多等待 10 秒。
- 指數退避加入 jitter，首輪約 300ms，最高約 10 秒，避免多人同時重連。
- App 回到前景時若連線失效或過久無活動會立即恢復。
- 斷線事件佇列中的 room:enter 只保留最後一筆，心跳不進佇列。
- C++ `client:ping` / `server:pong` 不查資料庫。
- C++ 房間 snapshot 廣播 35ms 合併，避免一次操作中的多個系統重複產生昂貴 viewer snapshot。
- 前端收到 room:snapshot 立即更新畫面；time/world/world-systems/containers 改為節流背景補齊，且不允許重疊載入。
- presence 僅在線名單真的變更時重繪。
