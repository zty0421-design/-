# TRPG ONLINE — 多人跑團網站 MVP（PostgreSQL）

這是一個可部署到公開網路的多人即時跑團網站 MVP，包含：

- 帳號註冊／登入
- JWT 登入驗證
- **PostgreSQL 資料庫保存**
- 建立跑團與 6 碼房間代碼
- 多人加入同一房間
- GM / 玩家角色
- GM 開團與回合切換
- Socket.IO 即時同步
- 即時在線狀態
- 即時聊天
- 即時骰子
- 角色 HP／精神同步
- 手機、平板、電腦響應式網頁
- 跑團事件紀錄持久化

## 1. 環境

需要 Node.js 18+，建議 Node.js 20+，以及 PostgreSQL 14+。

## 2. 建立 PostgreSQL 資料庫

例如使用 psql：

```sql
CREATE DATABASE rpg;
```

如果是雲端 PostgreSQL（例如 Render、Railway、Supabase、Neon 等），直接使用平台提供的連線字串即可。

## 3. 安裝

```bash
npm install
```

## 4. 設定環境變數

複製 `.env.example` 成 `.env`，或直接在伺服器設定環境變數：

```text
PORT=3000
JWT_SECRET=請換成長且隨機的密鑰
DATABASE_URL=postgresql://postgres:password@localhost:5432/rpg
PGSSL=disable
```

雲端 PostgreSQL 常見設定：

```text
PGSSL=require
```

程式第一次啟動時會自動建立：

- users
- rooms
- room_members
- events

以及必要索引。

## 5. 啟動

```bash
npm start
```

瀏覽器開啟：

```text
http://localhost:3000
```

## 6. 全球多人連線

這個網站本身支援不同城市、不同國家的玩家一起連線；正式使用時，把 Node.js 伺服器部署到有公開網址的雲端服務，並使用 HTTPS。

玩家只需要用手機或電腦瀏覽器進入同一個網址，再輸入房間代碼即可。

## 7. 資料保存

PostgreSQL 會保存帳號、房間、房間成員、角色數值與事件紀錄。伺服器重新啟動後資料不會因程式重啟而消失。

## 8. 正式部署注意事項

- 一定要換掉 `JWT_SECRET`
- 啟用 HTTPS
- PostgreSQL 使用強密碼與限制來源
- 生產環境可使用託管 PostgreSQL
- Socket.IO 在單一 Node 實例下即可運作；之後如果擴充到多台應用伺服器，應加入 Socket.IO adapter（例如 Redis）做跨實例同步

## 9. 下一階段

- 完整角色卡
- 職業／副職業
- 大道資料庫
- 技能系統
- 裝備／背包
- 道具與鍛造
- NPC／怪物
- 地圖與戰鬥棋盤
- 狀態／BUFF／DEBUFF
- GM 隱藏資訊
- 劇本管理
- 房間邀請連結

## 全球部署

此專案另外提供 `render.yaml`、`Dockerfile`、`.dockerignore` 與 `DEPLOY.md`，可用 Render + PostgreSQL 部署成公開網站。
