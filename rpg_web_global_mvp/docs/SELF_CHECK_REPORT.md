# TRPG Online C++ 53-cpp.14 自我檢查報告

日期：2026-08-19

## 本版重點

- GitHub 根目錄整理為標準目錄結構，首頁只保留 `CMakeLists.txt`、`Dockerfile`、`README.md`、`render.yaml` 四個一般檔案。
- 批量匯入支援：技能、道具、材料、魔法、儀式、**合成配方、商店商品**。
- 合成配方支援 TSV/CSV 簡寫：`鐵礦:3|木材:1`，產物可寫 `鐵劍:1`。
- 商店商品批量匯入支援分類、價格、數量、庫存、限購、折扣、回收比例、標籤與啟用狀態；不存在的分類由 C++ 建立／復用。
- 保留 GitHub 平鋪 fallback，避免資料夾被意外攤平後 Render 完全無法建置。

## 檢查結果

```text
PASS=36 FAIL=0 WARN=2
PASS 必要檔案完整
PASS 專案根資料夾名稱正確
PASS GitHub 根目錄只保留少量部署檔
PASS C++ 原始碼已整理到 src/include
PASS 標準 C++ 目錄結構完整
PASS 53-cpp.14 版本資訊一致
PASS Service Worker 快取版本已更新
PASS CMake 以標準目錄為主並保留平鋪 fallback
PASS Dockerfile 以標準目錄為主並保留平鋪 fallback
PASS Docker runtime 正確帶入 public/db
PASS Render 使用 Repo 根目錄
PASS C++ migration 使用 db/ 正式路徑
PASS SQL selectRows helper 參數數量一致
PASS C++ 批量匯入支援配方與商店
PASS 商店批量匯入使用正式分類同步
PASS 配方批量匯入有結構驗證
PASS 批量中心有配方／商店／魔法選項
PASS 前端有配方／商店批量範例與簡寫解析
PASS 批量匯入後自動刷新配方與商店
PASS Render 整合測試涵蓋魔法／配方／商店批量匯入
PASS 配方／商店／魔法資料表存在
PASS HTTP handler 無重複
PASS api_parity 記錄 244 條 v39 API
PASS 244 條 v39 API 均有 C++ handler
PASS native-socket.js 語法通過
PASS index.html 內嵌 JavaScript 語法通過
PASS Security C++ 真編譯通過
PASS JWT/bcrypt 單元測試通過
PASS CMake 標準目錄 configure 通過
PASS favicon-32.png PNG 簽名正確
PASS apple-touch-icon.png PNG 簽名正確
PASS icon-192.png PNG 簽名正確
PASS icon-512.png PNG 簽名正確
PASS PWA manifest 已接入
PASS 專案無 build/__pycache__/pyc 垃圾
PASS FEATURE_MAP 已更新本版功能
WARN 目前環境沒有 Docker/Podman；完整 Render Docker build 需部署後驗證
WARN 目前環境沒有 DATABASE_URL；批量匯入真實 PostgreSQL 整合由 Render integration_smoke 驗證
```

## 尚需 Render 驗證

目前工作環境沒有 Docker/Podman 與真實 `DATABASE_URL`，因此完整 Drogon Docker build 與批量匯入 PostgreSQL E2E 留待 Render 驗證。`tools/integration_smoke.py` 已加入魔法／配方／商店的 preview → commit → 讀回 → 清理測試。

## 打包後驗證

- ZIP 唯一頂層：`rpg_web_global_mvp/`。
- ZIP 根目錄一般檔案只有 4 個：`CMakeLists.txt`、`Dockerfile`、`README.md`、`render.yaml`。
- 打包後全新解壓重新執行 `tools/release_check.py`：`36 PASS / 0 FAIL / 2 WARN`。
