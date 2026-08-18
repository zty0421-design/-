# TRPG Render compile hotfix self-check

針對 Render 真實 C++ build log 修正：

- `CoreService::characterByUser()` 已公開給匿名 namespace 戰鬥 helper 使用，排除 private access 編譯錯誤。
- 職業取得 API 的 `role` 已改為 `std::string`，排除字串 literal 指標比較警告。
- `Security.cpp` 的 JSON key marker 改為 reserve/append，避免 GCC 12 對 chained string `operator+` 的 `-Wrestrict` 警告。
- `Config.cpp` 的 document root 使用 `assign()`，避免 GCC 12 的字串 literal assignment `-Wrestrict` 警告。
- `release_check.py` 已新增上述 Render 編譯 regression guards。

## 本地可執行檢查

結果：`19 PASS / 0 FAIL / 0 WARN`

包含：
- GitHub 根目錄平鋪結構
- Docker 平鋪 → canonical 目錄重建
- CMake flat layout configure
- 244/244 v39 HTTP handler
- HTTP route duplicate scan
- Render root/context 設定
- migration path
- `characterByUser` public visibility regression check
- profession role string regression check
- GCC 12 warning rewrite regression check
- native WebSocket JS syntax
- index inline JS syntax
- Security C++ compile
- JWT/bcrypt test

完整 Drogon 編譯仍以 Render 的下一次 Docker build 為最終驗證；本 hotfix 直接針對上一輪 Render 顯示的 compiler diagnostics 修正。
