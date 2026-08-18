#!/usr/bin/env python3
"""Portable release checks for the v39-compatible C++ release."""
from __future__ import annotations

import collections
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PASSED: list[str] = []
FAILED: list[str] = []
WARNINGS: list[str] = []


def ok(message: str) -> None: PASSED.append(message)
def fail(message: str) -> None: FAILED.append(message)
def warn(message: str) -> None: WARNINGS.append(message)
def expect(condition: bool, success: str, failure: str) -> None: (ok if condition else fail)(success if condition else failure)
def read(path: str) -> str: return (ROOT / path).read_text(encoding="utf-8")
def run(command: list[str], cwd: Path = ROOT) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)


def png_size(path: Path) -> tuple[int, int] | None:
    data = path.read_bytes()[:24]
    if len(data) != 24 or data[:8] != b"\x89PNG\r\n\x1a\n": return None
    return struct.unpack(">II", data[16:24])


def norm(method: str, path: str) -> str:
    parts = []
    for part in path.split("/"):
        if part.startswith(":") or re.fullmatch(r"\{\d+\}", part): parts.append(":")
        else: parts.append(part)
    return method.upper() + " " + "/".join(parts)


def actual_routes() -> list[str]:
    routes: list[tuple[str, str]] = []
    core = read("src/CoreService.cpp")
    for part in core.split("drogon::app().registerHandler(")[1:]:
        path = re.search(r'^\s*"([^"]+)"', part)
        method = re.search(r"\{drogon::(Get|Post|Put|Patch|Delete)\}\s*\)", part, re.S)
        if path and method: routes.append((method.group(1).upper(), path.group(1)))
    legacy = read("src/LegacyCompat.cpp")
    for m in re.finditer(r'\breg[0-3]\(\s*"([^"]+)"\s*,\s*drogon::(Get|Post|Put|Patch|Delete)', legacy):
        routes.append((m.group(2).upper(), m.group(1)))
    for m in re.finditer(r'registerAdminCrud0\(service\s*,\s*"([^"]+)"', legacy):
        routes += [("GET", m.group(1)), ("POST", m.group(1))]
    for m in re.finditer(r'registerAdminPatchDelete1\(service\s*,\s*"([^"]+)"', legacy):
        routes += [("PATCH", m.group(1)), ("DELETE", m.group(1))]
    return [norm(method, path) for method, path in routes]


def main() -> int:
    required = [
        "CMakeLists.txt","Dockerfile","render.yaml",".env.example","README.md","VERSION.txt",
        "api_parity.json","CPP_MIGRATION_STATUS.md","SELF_CHECK_REPORT.md",
        "include/trpg/Config.h","include/trpg/CoreService.h","include/trpg/LegacyCompat.h",
        "include/trpg/LegacyRouteManifest.h","include/trpg/RoomSocket.h","include/trpg/Security.h",
        "src/Config.cpp","src/CoreService.cpp","src/LegacyCompat.cpp","src/RoomSocket.cpp","src/Security.cpp","src/main.cpp",
        "db/legacy_v39_migrations.sql","tests/security_test.cpp","tools/integration_smoke.py",
        "public/index.html","public/native-socket.js","public/sw.js","public/manifest.webmanifest",
    ]
    missing = [x for x in required if not (ROOT/x).is_file()]
    expect(not missing, "必要檔案完整", "缺少必要檔案：" + ", ".join(missing))
    expect(ROOT.name == "rpg_web_global_mvp", "專案根資料夾為 rpg_web_global_mvp", f"專案根資料夾錯誤：{ROOT.name}")

    version = read("VERSION.txt").strip()
    version_files = ["include/trpg/CoreService.h","README.md","CPP_MIGRATION_STATUS.md","api_parity.json","public/index.html"]
    expect(version == "44-cpp.5" and all(version in read(x) for x in version_files), "版本號 44-cpp.5 一致", "版本號不一致")
    stale = []
    stale_tokens = ["43-cpp.4","trpg-online-v43-cpp-4","42-cpp.3","trpg-online-v42-cpp-3","41-cpp.2","trpg-online-v41-cpp-2"]
    for path in ROOT.rglob("*"):
        if not path.is_file() or path.name == "SELF_CHECK_REPORT.md" or "__pycache__" in path.parts: continue
        if path.suffix.lower() not in {".cpp",".h",".html",".js",".md",".txt",".json",".yaml",".yml"}: continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        if any(token in text for token in stale_tokens): stale.append(str(path.relative_to(ROOT)))
    expect(not stale, "無舊 C++ 版本／快取字串", "仍有舊版本字串：" + ", ".join(stale))

    parity = json.loads(read("api_parity.json"))
    expected_raw = parity.get("ported_http_routes", [])
    expected = collections.Counter(norm(*route.split(" ",1)) for route in expected_raw)
    actual = collections.Counter(actual_routes())
    missing_routes = list((expected-actual).elements())
    legacy_extra = list((actual-expected).elements())
    expected_extra = collections.Counter(norm(*route.split(" ",1)) for route in parity.get("extra_cpp_routes", []))
    expect(len(expected_raw)==244 and parity.get("ported_http_route_count")==244 and parity.get("legacy_node_http_route_count")==244,
           "API 對等清單為 244 / 244", "api_parity 路由數量不是 244 / 244")
    expect(not missing_routes, "244 條 v39 HTTP 路由都有實際 C++ handler", "缺少 handler：" + ", ".join(missing_routes[:20]))
    expect(collections.Counter(legacy_extra)==expected_extra, "C++ 額外 HTTP 路由與清單一致（6 條）", "未預期的 C++ 額外路由：" + ", ".join(legacy_extra[:20]))
    duplicates = [route for route,count in actual.items() if count > 1]
    expect(not duplicates, "HTTP handler 沒有重複註冊", "重複 HTTP handler：" + ", ".join(duplicates[:20]))

    manifest = read("include/trpg/LegacyRouteManifest.h")
    manifest_routes = re.findall(r'"((?:GET|POST|PUT|PATCH|DELETE) /api/[^"]+)"', manifest)
    expect(len(manifest_routes)==244 and manifest_routes==expected_raw, "LegacyRouteManifest 與 api_parity 244 條逐條一致", "LegacyRouteManifest 與 api_parity 不一致")
    core = read("src/CoreService.cpp")
    expect("kLegacyRouteManifest" in core and "ported_http_route_count\"] = kLegacyRouteCount" in core and 'body["code_complete"] = true' in core,
           "/api/cpp/status 直接使用 244 路由 manifest", "/api/cpp/status 未使用完整 manifest")
    expect(parity.get("code_complete") is True and parity.get("production_ready") is False,
           "code_complete=true 且 production_ready=false", "完成／正式環境狀態旗標錯誤")

    legacy = read("src/LegacyCompat.cpp")
    core_h = read("include/trpg/CoreService.h")
    cmake = read("CMakeLists.txt")
    docker = read("Dockerfile")
    ddl = read("db/legacy_v39_migrations.sql")
    expect("applyLegacyV39Migrations(shared_from_this())" in core and "registerLegacyV39Routes(self)" in core,
           "legacy migration 與 244 路由在啟動流程實際掛載", "legacy migration 或路由沒有掛到 CoreService")
    expect("src/LegacyCompat.cpp" in cmake and "project(trpg_cpp VERSION 44.5.0" in cmake,
           "CMake 已編入 LegacyCompat 並同步 44.5.0", "CMake 未正確編入相容層或版本")
    expect("COPY db ./db" in docker and "COPY db /app/db" in docker,
           "Docker builder/runtime 都包含 db migration", "Docker 映像缺少 migration 檔")
    tables = sorted(set(re.findall(r"CREATE TABLE IF NOT EXISTS\s+([a-z_][a-z0-9_]*)", ddl, re.I)))
    statements = [x for x in ddl.split(";") if x.strip()]
    key_tables = {"users","rooms","character_cards","skill_templates","room_monsters","tasks","room_savepoints","audit_logs","great_way_templates","faith_deities","npc_shop_items"}
    expect(len(tables)>=66 and len(statements)>=250 and key_tables.issubset(tables),
           f"v39 migration 覆蓋 {len(tables)} 張表／{len(statements)} 段 DDL", f"v39 migration 覆蓋不足：tables={len(tables)} statements={len(statements)}")

    expect("sha256Hex(compactJson(backupDigestPayload(backup)))" in legacy and "createRestoreToken" in legacy and "validRestoreToken" in legacy and "std::hash" not in legacy,
           "備份 checksum／10 分鐘 restore token 使用 SHA-256", "備份 checksum／restore token 不是穩定 SHA-256 契約")
    site_pos = legacy.find('/api/admin/backups/site/export')
    site_end = legacy.find('/api/admin/backups/rooms/{1}/export', site_pos)
    site_block = legacy[site_pos:site_end if site_end > site_pos else site_pos+10000]
    expect("array<const char*,65> names" in site_block and "password_hash" not in site_block and "SELECT id,username,is_admin,created_at FROM users" in site_block,
           "全站備份覆蓋其餘 65 張表且不匯出密碼雜湊", "全站備份表範圍或密碼安全契約不完整")
    expect(all(token in legacy for token in ["trpg-online-backup","format_version","confirm_text","restore_token","buildRoomBackupCheckpoint","restoreRoomBackupCheckpoint","buildCharacterBackupCheckpoint","restoreCharacterBackupCheckpoint"]),
           "備份中心具 v39 envelope／預覽／房間與角色兩階段還原契約", "備份中心 v39 還原契約不完整")
    expect("NULLIF($5,0)" in legacy and "summon_instances" in legacy,
           "未指定房間的召喚物不會寫入 room_id=0", "召喚物 room_id 外鍵處理不安全")
    stock_pos = legacy.find("const auto stock=intValue(item[\"stock\"],-1)")
    coin_pos = legacy.find("if(coins<price)", stock_pos)
    dec_pos = legacy.find("UPDATE npc_shop_stock SET remaining=remaining-1", stock_pos)
    expect(stock_pos>=0 and coin_pos>=0 and dec_pos>coin_pos,
           "NPC 商店先驗資金再扣有限庫存", "NPC 商店可能在餘額檢查前扣庫存")

    socket = read("src/RoomSocket.cpp")
    expect(all(event in socket for event in parity.get("ported_websocket_events", [])), "3 個原生 WebSocket 核心事件齊全", "WebSocket 核心事件缺漏")
    expect("socket.io" not in read("public/index.html").lower() and "/socket.io/" not in read("public/sw.js"), "前端沒有 Socket.IO 依賴", "前端仍含 Socket.IO")
    expect("trpg-online-v44-cpp-5" in read("public/sw.js"), "Service Worker 快取版本為 v44-cpp-5", "Service Worker 快取版本錯誤")

    manifest_json = json.loads(read("public/manifest.webmanifest"))
    expected_icons = {"public/icons/favicon-32.png":(32,32),"public/icons/apple-touch-icon.png":(180,180),"public/icons/icon-192.png":(192,192),"public/icons/icon-512.png":(512,512)}
    bad_icons=[name for name,size in expected_icons.items() if not (ROOT/name).is_file() or png_size(ROOT/name)!=size]
    expect(not bad_icons, "PWA 四個圖示尺寸正確", "PWA 圖示錯誤：" + ", ".join(bad_icons))
    expect(manifest_json.get("display")=="standalone" and len(manifest_json.get("icons",[]))>=2, "PWA manifest 可安裝", "PWA manifest 不完整")

    node=shutil.which("node")
    if node:
        r=run([node,"--check","public/native-socket.js"]); expect(r.returncode==0,"native-socket.js JavaScript 語法通過",r.stdout.strip())
        inline="\n".join(m.group(1) for m in re.finditer(r"<script(?:\s[^>]*)?>(.*?)</script>",read("public/index.html"),re.S))
        with tempfile.NamedTemporaryFile("w",suffix=".js",encoding="utf-8") as f:
            f.write(inline); f.flush(); r=run([node,"--check",f.name])
        expect(r.returncode==0,"index.html 內嵌 JavaScript 語法通過",r.stdout.strip())
    else: warn("找不到 Node；未執行前端 JavaScript 語法檢查")

    py=run([sys.executable,"-m","py_compile","tools/integration_smoke.py"])
    expect(py.returncode==0,"整合測試腳本 Python 語法通過",py.stdout.strip())

    compiler=shutil.which("g++")
    if compiler:
        with tempfile.TemporaryDirectory(prefix="trpg-security-") as td:
            binary=Path(td)/"security_test"
            r=run([compiler,"-std=c++20","-Wall","-Wextra","-Wpedantic","-Iinclude","src/Security.cpp","tests/security_test.cpp","-lcrypto","-lcrypt","-o",str(binary)])
            expect(r.returncode==0,"Security C++ 單元測試可編譯",r.stdout.strip())
            if r.returncode==0:
                t=run([str(binary)]); expect(t.returncode==0 and "security tests passed" in t.stdout,"JWT／bcrypt 單元測試通過",t.stdout.strip())
    else: warn("找不到 g++；未執行 Security C++ 單元測試")

    # A real Drogon build is only counted when an actual installed/built environment is supplied.
    build_dir=os.environ.get("TRPG_BUILD_DIR","").strip()
    if build_dir:
        build=Path(build_dir).resolve(); binary=build/"trpg_cpp"
        expect(binary.is_file(),"完整 CMake build 有 trpg_cpp 執行檔","TRPG_BUILD_DIR 找不到 trpg_cpp")
        ctest=shutil.which("ctest")
        if ctest:
            r=run([ctest,"--test-dir",str(build),"--output-on-failure"]); expect(r.returncode==0,"完整 CTest 通過",r.stdout.strip())
        else: warn("找不到 ctest；未重跑完整 CMake 測試")
    else: warn("未提供可用的完整 Drogon build；未宣稱完整 C++ 編譯／連結已驗證")

    smoke=os.environ.get("SMOKE_BASE_URL","").strip(); admin=os.environ.get("ADMIN_USERNAME","").strip(); password=os.environ.get("ADMIN_PASSWORD","")
    if smoke and admin and password:
        r=run([sys.executable,"tools/integration_smoke.py","--base-url",smoke,"--admin-username",admin,"--admin-password",password])
        expect(r.returncode==0,"真實 PostgreSQL API／WebSocket smoke test 通過",r.stdout.strip())
    else: warn("未提供正在執行的 PostgreSQL+C++ Server；未執行真實 API／WebSocket E2E")

    if shutil.which("docker"): warn("偵測到 Docker，但 release_check 不會修改主機；請在部署管線執行 docker build")
    else: warn("目前環境沒有 Docker；未執行映像建置")
    browsers=[x for x in ("chromium","chromium-browser","google-chrome","firefox") if shutil.which(x)]
    if browsers: warn("偵測到瀏覽器但未配置 E2E 測試案例；未宣稱桌面／手機 E2E 通過")
    else: warn("目前環境沒有可用瀏覽器；未執行桌面／手機 E2E")

    print(f"TRPG C++ {version} 自我檢查")
    for x in PASSED: print("PASS ",x)
    for x in FAILED: print("FAIL ",x)
    for x in WARNINGS: print("WARN ",x)
    print(f"SUMMARY pass={len(PASSED)} fail={len(FAILED)} warn={len(WARNINGS)}")
    return 1 if FAILED else 0

if __name__ == "__main__": sys.exit(main())
