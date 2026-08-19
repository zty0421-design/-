#!/usr/bin/env python3
from __future__ import annotations
import collections, json, os, re, shutil, subprocess, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PASS: list[str] = []
FAIL: list[str] = []
WARN: list[str] = []

def ok(msg): PASS.append(msg)
def bad(msg): FAIL.append(msg)
def expect(cond, good, bad_msg): ok(good) if cond else bad(bad_msg)
def read(rel): return (ROOT / rel).read_text(encoding='utf-8', errors='ignore')
def run(cmd, cwd=ROOT): return subprocess.run(cmd, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

def norm(method, path):
    parts=[]
    for p in path.split('/'):
        parts.append(':' if p.startswith(':') or re.fullmatch(r'\{\d+\}',p) else p)
    return method.upper()+' '+'/'.join(parts)

def actual_routes():
    out=[]
    core=read('src/CoreService.cpp')
    for part in core.split('drogon::app().registerHandler(')[1:]:
        p=re.search(r'^\s*"([^"]+)"',part)
        m=re.search(r'\{drogon::(Get|Post|Put|Patch|Delete)\}\s*\)',part,re.S)
        if p and m: out.append(norm(m.group(1),p.group(1)))
    legacy=read('src/LegacyCompat.cpp')
    for m in re.finditer(r'\breg[0-3]\(\s*"([^"]+)"\s*,\s*drogon::(Get|Post|Put|Patch|Delete)',legacy):
        out.append(norm(m.group(2),m.group(1)))
    world=read('src/WorldSystems.cpp')
    for m in re.finditer(r'\breg[0-2]\(\s*"([^"]+)"\s*,\s*drogon::(Get|Post|Put|Patch|Delete)',world):
        out.append(norm(m.group(2),m.group(1)))
    engine=read('src/RuleEngine.cpp')
    for m in re.finditer(r'\breg[01]\(\s*"([^"]+)"\s*,\s*drogon::(Get|Post|Put|Patch|Delete)',engine):
        out.append(norm(m.group(2),m.group(1)))
    bridge=read('src/RuleCombatBridge.cpp')
    for part in bridge.split('drogon::app().registerHandler(')[1:]:
        p=re.search(r'^\s*"([^"]+)"',part)
        m=re.search(r'\{drogon::(Get|Post|Put|Patch|Delete)\}\s*\)',part,re.S)
        if p and m: out.append(norm(m.group(1),p.group(1)))
    for m in re.finditer(r'registerAdminCrud0\(service\s*,\s*"([^"]+)"',legacy):
        out += [norm('GET',m.group(1)),norm('POST',m.group(1))]
    for m in re.finditer(r'registerAdminPatchDelete1\(service\s*,\s*"([^"]+)"',legacy):
        out += [norm('PATCH',m.group(1)),norm('DELETE',m.group(1))]
    return out

def helper_call_arity_errors(source: str):
    expected={"selectRows":2,"selectRows1":3,"selectRows2":4}
    errors=[]
    for name, argc_expected in expected.items():
        needle=name+'('; pos=0
        while True:
            start=source.find(needle,pos)
            if start<0: break
            # Skip definitions.
            prefix=source[max(0,start-80):start]
            if re.search(r'Json::Value\s*$', prefix):
                pos=start+len(needle); continue
            i=start+len(needle); depth=1; quote=None; escaped=False
            while i<len(source) and depth:
                ch=source[i]
                if quote:
                    if escaped: escaped=False
                    elif ch=='\\': escaped=True
                    elif ch==quote: quote=None
                else:
                    if ch in ('"',"'"): quote=ch
                    elif ch=='(': depth+=1
                    elif ch==')': depth-=1
                i+=1
            body=source[start+len(needle):i-1]
            level=0; quote=None; escaped=False; commas=0
            for ch in body:
                if quote:
                    if escaped: escaped=False
                    elif ch=='\\': escaped=True
                    elif ch==quote: quote=None
                else:
                    if ch in ('"',"'"): quote=ch
                    elif ch in '([{': level+=1
                    elif ch in ')]}': level=max(0,level-1)
                    elif ch==',' and level==0: commas+=1
            argc=0 if not body.strip() else commas+1
            if argc!=argc_expected:
                line=source.count('\n',0,start)+1
                errors.append(f'{name} line {line}: expected {argc_expected}, got {argc}')
            pos=max(i,start+len(needle))
    return errors

def main():
    required=[
        'CMakeLists.txt','Dockerfile','render.yaml','README.md',
        'src/main.cpp','src/Config.cpp','src/Security.cpp','src/CoreService.cpp','src/LegacyCompat.cpp','src/RoomSocket.cpp','src/WorldSystems.cpp','src/RuleEngine.cpp','src/RuleCombatBridge.cpp',
        'include/trpg/Config.h','include/trpg/Security.h','include/trpg/CoreService.h','include/trpg/LegacyCompat.h','include/trpg/LegacyRouteManifest.h','include/trpg/RoomSocket.h','include/trpg/WorldSystems.h','include/trpg/RuleEngine.h','include/trpg/RuleCombatBridge.h',
        'public/index.html','public/native-socket.js','public/sw.js','public/manifest.webmanifest','public/icons/favicon-32.png','public/icons/apple-touch-icon.png','public/icons/icon-192.png','public/icons/icon-512.png',
        'db/legacy_v39_migrations.sql','tests/security_test.cpp','tools/integration_smoke.py','docs/VERSION.txt','docs/api_parity.json','docs/FEATURE_MAP.md','docs/RULE_ENGINE_GUIDE.md'
    ]
    missing=[x for x in required if not (ROOT/x).is_file()]
    expect(not missing,'必要檔案完整','缺少：'+', '.join(missing))
    expect(ROOT.name=='rpg_web_global_mvp','專案根資料夾名稱正確','根資料夾不是 rpg_web_global_mvp')
    root_files=[p.name for p in ROOT.iterdir() if p.is_file()]
    expect(len(root_files)<=4,'GitHub 根目錄只保留少量部署檔','根目錄檔案仍過多：'+', '.join(root_files))
    expect(not list(ROOT.glob('*.cpp')) and not list(ROOT.glob('*.h')),'C++ 原始碼已整理到 src/include','根目錄仍殘留 .cpp/.h')
    expect(all((ROOT/d).is_dir() for d in ['src','include/trpg','public','db','tests','tools','docs']),'標準 C++ 目錄結構完整','標準目錄缺失')

    version=read('docs/VERSION.txt').strip()
    expect(version=='53-cpp.14' and 'kVersion = "53-cpp.14"' in read('include/trpg/CoreService.h') and 'C++ 相容版 53-cpp.14' in read('public/index.html') and '"version": "53-cpp.14"' in read('docs/api_parity.json'),
           '53-cpp.14 版本資訊一致','版本資訊不同步')
    expect('v53-cpp-14-bulk-organized-1' in read('public/sw.js'),'Service Worker 快取版本已更新','Service Worker 仍使用舊快取')

    cmake=read('CMakeLists.txt'); docker=read('Dockerfile'); render=read('render.yaml'); legacy=read('src/LegacyCompat.cpp')
    expect('src/main.cpp' in cmake and 'flat_include' in cmake and 'tests/security_test.cpp' in cmake,'CMake 以標準目錄為主並保留平鋪 fallback','CMake 目錄支援不完整')
    expect('COPY . .' in docker and 'if [ ! -f src/main.cpp ]' in docker,'Dockerfile 以標準目錄為主並保留平鋪 fallback','Dockerfile 目錄相容不完整')
    expect('COPY --from=builder /src/public /app/public' in docker and 'COPY --from=builder /src/db /app/db' in docker,'Docker runtime 正確帶入 public/db','Docker runtime public/db 路徑錯誤')
    expect('rootDir:' not in render and 'dockerfilePath: ./Dockerfile' in render and 'dockerContext: .' in render,'Render 使用 Repo 根目錄','Render root/context 設定錯誤')
    expect('std::filesystem::path{"db/legacy_v39_migrations.sql"}' in legacy,'C++ migration 使用 db/ 正式路徑','migration 找不到標準 db 路徑')

    arity=helper_call_arity_errors(legacy)
    expect(not arity,'SQL selectRows helper 參數數量一致','SQL helper 參數錯誤：'+'; '.join(arity))

    # Bulk import regression checks.
    expect('"recipes","shop"' in legacy and 'kind=="recipes"?"recipes"' in legacy and 'kind=="shop"?"shop_items"' in legacy,'C++ 批量匯入支援配方與商店','C++ 批量匯入缺 recipes/shop')
    expect('syncShopItemCategory(service->database(),row)' in legacy,'商店批量匯入使用正式分類同步','商店批量匯入未同步分類')
    expect('materials 必須是陣列' in legacy and 'output 必須是物件' in legacy and 'config 必須是物件' in legacy,'配方批量匯入有結構驗證','配方批量驗證不完整')
    html=read('public/index.html')
    expect('<option value="recipes">合成配方</option>' in html and '<option value="shop">商店商品</option>' in html and '<option value="magics">魔法學</option>' in html,'批量中心有配方／商店／魔法選項','批量中心選項缺失')
    expect("if(kind==='recipes')" in html and "if(kind==='shop')" in html and 'parseRecipeMaterials' in html and 'parseRecipeOutput' in html,'前端有配方／商店批量範例與簡寫解析','前端批量配方/商店支援不完整')
    expect('loadRecipes(),loadShop()' in html,'批量匯入後自動刷新配方與商店','匯入後未刷新配方/商店')
    smoke=read('tools/integration_smoke.py')
    expect('bulk import magic, recipes and shop items' in smoke and '("recipes"' in smoke and '("shop"' in smoke,'Render 整合測試涵蓋魔法／配方／商店批量匯入','整合測試未涵蓋批量匯入')

    migrations=read('db/legacy_v39_migrations.sql')
    expect('CREATE TABLE IF NOT EXISTS recipes' in migrations and 'CREATE TABLE IF NOT EXISTS shop_items' in migrations and 'CREATE TABLE IF NOT EXISTS magic_studies' in migrations,'配方／商店／魔法資料表存在','必要資料表缺失')

    # Route parity.
    routes=actual_routes(); counts=collections.Counter(routes)
    expect(not [r for r,c in counts.items() if c>1],'HTTP handler 無重複','有重複 handler：'+', '.join(r for r,c in counts.items() if c>1))
    parity=json.loads(read('docs/api_parity.json'))
    expected_routes=set()
    # Support either list of strings or objects from older parity formats.
    for key in ['legacy_http_routes','ported_http_routes','routes']:
        value=parity.get(key)
        if isinstance(value,list):
            for item in value:
                if isinstance(item,str): expected_routes.add(norm(*item.split(' ',1))) if ' ' in item else None
                elif isinstance(item,dict) and item.get('method') and item.get('path'): expected_routes.add(norm(item['method'],item['path']))
    if not expected_routes:
        # Core status still promises 244 legacy routes; actual list may live in a different field.
        for item in parity.get('http_routes',[]):
            if isinstance(item,dict): expected_routes.add(norm(item.get('method','GET'),item.get('path','/')))
    if expected_routes:
        missing_routes=sorted(expected_routes-set(routes))
        expect(len(expected_routes)==244,'api_parity 記錄 244 條 v39 API',f'api_parity 路由數不是 244：{len(expected_routes)}')
        expect(not missing_routes,'244 條 v39 API 均有 C++ handler','缺少路由：'+', '.join(missing_routes[:20]))
    else:
        expect('244' in read('include/trpg/CoreService.h') or '244' in read('src/CoreService.cpp'),'C++ 狀態仍宣告 244 條 v39 API','無法確認 244 API')

    # Front-end syntax.
    node=shutil.which('node')
    if node:
        r=run([node,'--check',str(ROOT/'public/native-socket.js')]); expect(r.returncode==0,'native-socket.js 語法通過',r.stdout.strip())
        inline='\n'.join(m.group(1) for m in re.finditer(r'<script(?:\s[^>]*)?>(.*?)</script>',html,re.S))
        with tempfile.NamedTemporaryFile('w',suffix='.js',encoding='utf-8',delete=False) as f:
            f.write(inline); tmp=f.name
        try:
            r=run([node,'--check',tmp]); expect(r.returncode==0,'index.html 內嵌 JavaScript 語法通過',r.stdout.strip())
        finally:
            Path(tmp).unlink(missing_ok=True)
    else: WARN.append('找不到 Node，未執行前端 JavaScript 語法檢查')

    # Compile security test independent of Drogon.
    gpp=shutil.which('g++')
    if gpp:
        with tempfile.TemporaryDirectory() as td:
            binary=Path(td)/'security_test'
            r=run([gpp,'-std=c++20','-Wall','-Wextra','-Wpedantic','-I'+str(ROOT/'include'),str(ROOT/'src/Security.cpp'),str(ROOT/'tests/security_test.cpp'),'-lcrypto','-lcrypt','-o',str(binary)])
            expect(r.returncode==0,'Security C++ 真編譯通過',r.stdout.strip())
            if r.returncode==0:
                t=run([str(binary)]); expect(t.returncode==0,'JWT/bcrypt 單元測試通過',t.stdout.strip())
    else: WARN.append('找不到 g++，未執行 Security C++ 編譯')

    # CMake parse/configure using a fake imported Drogon target.
    cmake_bin=shutil.which('cmake')
    if cmake_bin:
        with tempfile.TemporaryDirectory() as td:
            td=Path(td); pkg=td/'fake'; pkg.mkdir(); (pkg/'DrogonConfig.cmake').write_text('add_library(Drogon::Drogon INTERFACE IMPORTED)\n',encoding='utf-8')
            r=run([cmake_bin,'-S',str(ROOT),'-B',str(td/'build'),'-DDrogon_DIR='+str(pkg),'-DBUILD_TESTING=OFF'])
            expect(r.returncode==0,'CMake 標準目錄 configure 通過',r.stdout.strip())
    else: WARN.append('找不到 cmake')

    # PWA and repository cleanliness.
    for icon in ['favicon-32.png','apple-touch-icon.png','icon-192.png','icon-512.png']:
        data=(ROOT/'public/icons'/icon).read_bytes(); expect(data.startswith(b'\x89PNG\r\n\x1a\n'),f'{icon} PNG 簽名正確',f'{icon} 不是有效 PNG')
    expect('/manifest.webmanifest' in html and 'manifest.webmanifest' in read('public/sw.js'),'PWA manifest 已接入','PWA manifest 連結缺失')
    junk=[p for p in ROOT.rglob('*') if p.is_dir() and p.name in {'build','__pycache__'}]
    junk += [p for p in ROOT.rglob('*.pyc')]
    expect(not junk,'專案無 build/__pycache__/pyc 垃圾','發現測試產物：'+', '.join(str(p.relative_to(ROOT)) for p in junk[:20]))
    expect('53-cpp.14 GitHub 目錄整理＋批量匯入擴充' in read('docs/FEATURE_MAP.md'),'FEATURE_MAP 已更新本版功能','FEATURE_MAP 未更新')

    if not shutil.which('docker') and not shutil.which('podman'):
        WARN.append('目前環境沒有 Docker/Podman；完整 Render Docker build 需部署後驗證')
    if not os.environ.get('DATABASE_URL'):
        WARN.append('目前環境沒有 DATABASE_URL；批量匯入真實 PostgreSQL 整合由 Render integration_smoke 驗證')

    print(f'PASS={len(PASS)} FAIL={len(FAIL)} WARN={len(WARN)}')
    for x in PASS: print('PASS',x)
    for x in FAIL: print('FAIL',x)
    for x in WARN: print('WARN',x)
    return 1 if FAIL else 0

if __name__=='__main__':
    raise SystemExit(main())
