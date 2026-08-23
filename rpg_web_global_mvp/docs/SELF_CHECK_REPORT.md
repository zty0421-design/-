# TRPG Online C++ 60-cpp.21.3 自我檢查報告

## 本版主題

**建房／加入／開團核心穩定性修正**。

使用者回報 `60-cpp.21.2` 仍「無法開團」後，重新檢查完整鏈路：

`建立房間按鈕 → 前端事件 → POST /api/rooms → 成員／角色綁定 → 事件 → roomSnapshot → 開始跑團按鈕 → POST /api/rooms/:id/start → 快照／WebSocket`。

## 找到的回歸來源

1. `createRoom` 的事件綁定仍位於 `bindView()` 後段；前面任一大型管理模組若拋出 JS 例外，建立房間按鈕可能完全沒有事件。
2. `POST /api/rooms` 在房間已建立後，仍會呼叫完整 `characterByUser()`、角色綁定、事件與 `roomSnapshot()`；後期裝備／元素／學習／怪物等 schema 任一漂移都可能讓 API 最後回 500。
3. `POST /api/rooms/join` 有相同問題，而且原本先呼叫完整 `characterByUser()`，玩家加入也可能被非核心系統拖垮。
4. `POST /api/rooms/:id/start` 雖已在 60-cpp.21.2 把戰鬥欄位改為 best-effort，但事件或完整快照仍可能在核心 status 已成功後造成 500。
5. 開團後端原本仍綁定全站 DM 權限，不接受房主／房間 `gm` 身分。

## 60-cpp.21.3 修正

### 前端

- `[前端功能備註｜60-cpp.21.3 建房／開團核心事件]`
- `createRoom`、`joinRoom`、`startCampaign`、`.spin-wheel` 全部在 `bindView()` 最前段綁定。
- 新增 `createRoomCritical()`、`joinRoomCritical()`、`startCampaignCritical()`。
- 建房／開團收到 `degraded_snapshot=true` 仍可 `activateRoom()`，再以既有 `Promise.allSettled` 補載擴充系統。
- 房主／房間 `gm` 也會顯示「開始跑團」按鈕。
- Service Worker cache：`trpg-online-v60-cpp-21-3-room-start-hardening-1`。

### C++ 後端

- `[功能備註｜60-cpp.21.3 開團安全快照]`
  - 新增 `resilientRoomSnapshot()`。
  - 先嘗試完整 `roomSnapshot()`；失敗時只用 `rooms / room_members / events` 核心欄位回傳可進房快照。
- `[功能備註｜60-cpp.21.3 建立房間核心優先]`
  - 房間與 GM 成員先成功建立。
  - 完整 `characterByUser()`／角色綁定與事件改為附加流程；異常只記 WARN。
  - 最終回傳改用 `resilientRoomSnapshot()`。
- `[功能備註｜60-cpp.21.3 加入房間核心優先]`
  - join 只查 `character_cards.id,name`，不再先呼叫完整 `characterByUser()`。
  - 角色綁定／事件異常可降級。
  - 最終回傳改用安全快照。
- `[功能備註｜60-cpp.21.3 開團核心優先]`
  - `rooms.status='active'` 為核心成功條件。
  - 戰鬥欄位重置、事件、完整快照皆可降級。
  - 權限支援：房主／房間 `gm`／全站 DM。

## 自動檢查

```text
100 PASS
0 FAIL
2 WARN
```

主要新增回歸項目：

- 建房／開團安全快照存在。
- 建房不再被角色綁定或事件拖垮。
- join 不再依賴完整 `characterByUser()`。
- 開團支援房主／房間 GM／全站 DM。
- 建房／加入／開團／轉盤都在大型模組前優先綁定。
- 前端可接受 `degraded_snapshot` 並重新補載房間資料。
- 既有 244 / 244 v39 HTTP API handler 覆蓋仍通過。
- `index.html` 內嵌 JavaScript 語法通過。
- Security C++ 真編譯與 JWT/bcrypt 單元測試通過。
- CMake configure 通過。

## WARN 說明

1. 目前工作環境沒有 Docker/Podman，也沒有 Drogon headers，因此本回合無法在本機重跑與 Render 完全相同的完整 `trpg_cpp` Docker 編譯。
2. 目前工作環境沒有實際 `DATABASE_URL`，因此真 PostgreSQL `integration_smoke.py` 仍由 Render 部署環境驗證。

這兩項為環境限制，不是 release check FAIL；新版本仍保留 Render integration smoke 的建房、加入、開團與轉盤實際 API 驗證流程。
