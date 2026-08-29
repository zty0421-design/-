# 73-cpp.34.25.10 — Chrome「網頁無回應」修正

## 問題
DM 前端累積了多個 `MutationObserver(document.body, {subtree:true})`。大型 `innerHTML` 更新時，每個 observer 都會掃描 DOM；部分增強器又會插入 DOM，造成下一輪 observer，容易形成主執行緒長任務與 observer storm。

## 修正
- 移除 9 個全域 document.body subtree observer。
- 改成 `scheduleDmEnhancements()`：只在真正提交新的 DM HTML 後執行一次。
- 使用 requestAnimationFrame 合併同一幀的增強工作。
- 非 DM 畫面、登入畫面完全不執行 DM 增強掃描。
- 每個增強器獨立 try/catch，單一模組失敗不拖垮整頁。
- 保留局部 sortable 容器 observer；它只監看自己的小容器，不掃描整個 document.body。
