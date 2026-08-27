# TRPG Online C++ 69-cpp.30 自我檢查

## 本版
- 22 張大阿爾克那命鬥場規則全部接線，正／逆位均有規則描述。
- 彌夜新增協戰 AI 判斷與加入戰鬥 API。
- 詭異／命運異常敵人會提高彌夜主動介入優先度。
- 協戰狀態寫入既有 `room_npcs.state`，關係與事件寫入既有 NPC 長期記憶。

## 檢查
- `python3 tools/release_check.py`: 239 PASS / 0 FAIL / 2 WARN
- `public/index.html` 內嵌 JavaScript：`node --check` 通過。
- C++ 原始碼大括號平衡檢查通過。

## WARN
1. 工作環境沒有 Docker/Podman，完整 Render Docker build 需部署驗證。
2. 工作環境沒有正式 `DATABASE_URL`，真 PostgreSQL integration smoke 需部署驗證。
