# TRPG Online C++ 61-cpp.22 自我檢查報告

## 本版主題

**平面地圖＋節點系統 MVP**。

### 61-cpp.22 新增

- 房間 `map_image_url` 作為 2D 平面圖底圖。
- `room_map_nodes` 新增：
  - `x_percent` / `y_percent`：0～100 百分比座標。
  - `node_type`：地點、房間、出入口、搜索點、戰鬥點、商店、劇情點、傳送點等。
  - `icon`：節點圖示。
- 前端 `planarMapHTML()`：
  - 在底圖上疊加節點。
  - SVG 顯示節點連線。
  - 標示玩家目前位置與 DM 場景節點。
- DM `bindPlanarMap()`：
  - 拖曳節點後直接保存座標。
  - 雙擊平面圖空白處新增節點。
  - 點節點可進入完整節點編輯器。
- 玩家：
  - 無隊伍時直接點相鄰節點移動。
  - 有隊伍時沿隊伍節點移動。
  - 規則引擎 `PLAYER_MOVE_ATTEMPT / PLAYER_MOVED` 保留。
- 手動節點連線會自動雙向同步，避免 A→B 可走但 B→A 不可走。
- 舊地圖若完全沒有 `connections`，維持自由移動，避免舊房間被新規則鎖死。
- 程序地圖生成會自動分配平面座標並建立雙向連線。

## 功能備註

可全文搜尋：

- `[功能備註｜61-cpp.22 平面地圖核心 migration]`
- `[功能備註｜61-cpp.22 平面地圖讀取]`
- `[功能備註｜61-cpp.22 節點連線驗證]`
- `[功能備註｜61-cpp.22 隊伍節點連線驗證]`
- `[功能備註｜61-cpp.22 雙向節點連線]`
- `[功能備註｜61-cpp.22 玩家節點移動]`
- `[功能備註｜61-cpp.22 隊伍節點移動]`
- `[前端功能備註｜61-cpp.22 平面地圖]`
- `[前端功能備註｜61-cpp.22 平面地圖節點]`
- `[前端功能備註｜61-cpp.22 DM 地圖編輯器／玩家移動]`

## 自動檢查

```text
111 PASS
0 FAIL
2 WARN
```

主要通過項目：

- v39 HTTP API 244 / 244 仍有 C++ handler。
- HTTP handler 無重複。
- `index.html` 內嵌 JavaScript 語法通過。
- `native-socket.js` 語法通過。
- Security C++ 真編譯及 JWT/bcrypt 單元測試通過。
- CMake 標準目錄 configure 通過。
- 61-cpp.22 C++ 平面地圖 migration 存在。
- map API 回傳有效玩家／隊伍目前位置。
- DM 拖曳、雙擊新增、節點座標與 SVG 連線存在。
- 玩家與隊伍移動都會驗證節點連線。
- DM 手動連線會雙向同步。
- Render integration smoke 改為沿相鄰節點驗證移動與規則阻止。
- 60-cpp.21.3 建房／開團、60-cpp.21.4 提交行動、60-cpp.21.2 轉盤回歸檢查仍通過。

## WARN

1. 目前執行環境沒有 Docker/Podman，因此完整 Render Docker build 仍需部署環境驗證。
2. 目前沒有實際 `DATABASE_URL`，因此真 PostgreSQL integration smoke 仍由 Render 部署環境執行。
