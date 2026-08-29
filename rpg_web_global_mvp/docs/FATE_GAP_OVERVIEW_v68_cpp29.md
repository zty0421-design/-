# 命隙 / 彌夜 / 塔羅系統 68-cpp.29

- 22 張大阿爾克那已全部具有命鬥場正逆位規則描述與戰鬥接線。
- 規則涵蓋命中、傷害、防禦、恢復、低生命、回合震盪、連擊、救贖與後期加速。
- `GET /api/fate-gap/miyoi/:roomId/combat-decision`：依信任、敵意、現存怪物與詭異/命運異常判斷協戰意願。
- `POST /api/fate-gap/miyoi/:roomId/join-combat`：彌夜加入房間戰鬥，寫入 room_npcs 協戰狀態與長期記憶。
- 彌夜對詭異/命運異常有較高介入優先度，即使一般信任不足也可能出手。
