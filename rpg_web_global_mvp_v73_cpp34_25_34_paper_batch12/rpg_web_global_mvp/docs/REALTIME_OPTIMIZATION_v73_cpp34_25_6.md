# 73-cpp.34.25.6 即時連線優化 v2

- WebSocket `room:enter` 改為冪等：同一連線重複進同房不再重跑進房規則、DB 更新與完整快照。
- 客戶端重連時丟棄排隊中的舊 `room:enter`，由 connect handler 只送目前房間。
- 票證 API 失敗後加入 15~30 秒 fallback 冷卻，避免每次重連先卡一次 HTTP timeout。
- 非同步開連線加入 generation，舊票證請求完成後不能覆蓋較新的 reconnect。
- 房間 presence 只在在線 user 集合真的改變時廣播，並直接從該房連線表計算。
- 切房時立即從舊房連線表移除 socket，降低 stale weak_ptr。
- 同一 user 多分頁／多裝置只生成一次 viewer snapshot，再扇出給該 user 的連線。
- 前端對重複 room snapshot 做輕量 signature 去重，避免相同狀態重建整個 DOM。
- `/api/health` 探測增加 5 秒節流與 in-flight 合併，避免 reconnect storm 額外打爆 Render。
