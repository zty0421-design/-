# 65-cpp.26.12 渲染／載入效能修正

- WebSocket presence、room snapshot、team update 改為 requestAnimationFrame 合併重繪。
- 分頁在背景時暫停非必要 DOM 重畫，回到頁面再補一次。
- 房間 time/world/world-systems/containers/team/map 使用 single-flight，避免同一資料並行重複請求。
- room snapshot 附加資料刷新加入 180ms 節流，依目前 view 只刷新必要資料。
- `loadRoomWorld(false)` 可避免進房流程重複載入 world-systems / containers。
- 登入／自動登入改為先顯示首屏，再背景載入大型遊戲與 DM 資料。
- 長列表卡片使用 `content-visibility:auto` + `contain-intrinsic-size`，降低畫面外 layout/paint。
- 動態圖片套用 lazy loading、async decode 與低優先抓取提示。
- 行動列導覽避免每次 render 都重做 active 導覽與 scrollIntoView。
- 隊伍切頁合併資料載入，移除重複 `loadTeamData()`。
