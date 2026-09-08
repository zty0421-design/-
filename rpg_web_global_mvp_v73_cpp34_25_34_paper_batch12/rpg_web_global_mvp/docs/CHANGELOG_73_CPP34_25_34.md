# 73-cpp.34.25.34｜紙本清單第十二批

- 學術道具分類：item_templates.study_domains 正式接上 DM 編輯與玩家背包，可標記魔法學／儀式學／神秘學。
- 背包增加學術／特殊用途篩選，完全使用已載入道具模板，不新增 API。
- 特殊職業道具增加 profession_requirement_mode：any / main / sub，C++ 後端實際驗證主職業或副職業。
- 職業名稱比對改為大小寫不敏感；前端只提供提示，最終權限仍由後端判斷。
- 修正角色背包「使用」按鈕只送 name、後端卻要求 template_id 的舊接線不一致；新版優先送 template_id，後端保留 name fallback 相容舊客戶端。
- 道具詳情顯示學術用途與職業限制／目前是否符合。
