# 73-cpp.34.25.4 登入／DM 載入效能優化

- 同一時間相同 GET API 會共用同一個 in-flight Promise，避免初始化模組重複查詢。
- DM 背景 loader 改為最多 4 組並行，避免登入瞬間大量 API 同時壓到 Render 與 PostgreSQL。
- 大量背景完成事件透過 requestAnimationFrame 合併 render，降低巨大 DOM 重建次數。
- DM 起始面板與內容健康掃描只在「控制台」頁建立，切到怪物／NPC／技能等頁時不再每次重算。
- 非必要背景載入會先讓出瀏覽器主執行緒，再開始分批讀取。
- 保留 34.25.3 的單模組失敗隔離與重新載入按鈕。
