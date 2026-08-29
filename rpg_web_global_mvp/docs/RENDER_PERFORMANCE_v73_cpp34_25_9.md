# 73-cpp.34.25.9 渲染效能優化

- 主內容 HTML 與上一幀相同時，不再執行 `innerHTML` 與 `bindView()`。
- DM 背景 API 批次載入期間只更新 state，批次完成後才統一 render，避免 20 支 API 各重畫一次。
- 長頁面使用 `content-visibility:auto`，讓瀏覽器優先排版目前可視區。
- `#content` 使用 layout/style containment，降低局部內容變更造成的全站 layout 範圍。
- 相同畫面被重複觸發 render 時會保留既有 DOM，因此也可降低滾動位置與輸入焦點被重設的情況。
