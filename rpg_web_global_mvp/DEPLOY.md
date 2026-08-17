# 全球部署指南（Render + PostgreSQL）

這個版本已經準備成「單一公開 Web Service + Managed PostgreSQL」架構。
Render Web Service 可接受來自公網的 WebSocket 連線，Postgres 可與同區域服務走內網連線。正式部署時建議讓網站與資料庫放在同一區域，以降低延遲。 

## 1. 把專案放到 GitHub

建立一個 GitHub repository，把這個專案的所有檔案放進去。

## 2. 在 Render 建立 Blueprint

在 Render Dashboard 選擇建立新的 Blueprint，連接 GitHub repository。根目錄已經有 `render.yaml`，它會建立：

- `trpg-online` Web Service
- `trpg-online-db` PostgreSQL
- `DATABASE_URL` 自動接到資料庫內網 connection string
- `JWT_SECRET` 自動產生隨機密鑰
- `PGSSL=require`
- `/api/health` 健康檢查

## 3. 部署完成後

Render 會提供公開網址，例如：

`https://trpg-online-xxxx.onrender.com`

把這個網址傳給不同國家的玩家即可。玩家只需要使用手機、平板或電腦瀏覽器。

## 4. 自訂網域

可在 Render 的 Web Service 綁定自己的網域。Render 會處理 HTTPS 憑證，因此玩家最後可以使用像 `https://trpg.example.com` 的網址。

## 5. 目前同步架構限制

這個 MVP 的 Socket.IO 在線狀態存在 Web Service 記憶體中，因此目前應使用「單一 Web Service instance」來跑多人房間。資料庫內容會持久保存。

若未來把 Web Service 擴展到多個 instance，需要加 Redis/Valkey + Socket.IO adapter，讓不同 instance 之間共享即時事件。

## 6. 本機測試

```bash
npm install
cp .env.example .env
npm start
```

需要一個本機 PostgreSQL，然後設定 `DATABASE_URL`。
