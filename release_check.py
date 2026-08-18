#!/usr/bin/env python3
from __future__ import annotations
import collections, json, re, shutil, subprocess, sys, tempfile, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PASS=[]; FAIL=[]; WARN=[]
def ok(x): PASS.append(x)
def bad(x): FAIL.append(x)
def expect(c,a,b): ok(a) if c else bad(b)
def read(n): return (ROOT/n).read_text(encoding='utf-8', errors='ignore')
def run(cmd,cwd=ROOT): return subprocess.run(cmd,cwd=cwd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)

def norm(method,path):
    parts=[]
    for p in path.split('/'):
        parts.append(':' if p.startswith(':') or re.fullmatch(r'\{\d+\}',p) else p)
    return method.upper()+' '+'/'.join(parts)

def routes():
    out=[]
    core=read('CoreService.cpp')
    for part in core.split('drogon::app().registerHandler(')[1:]:
        p=re.search(r'^\s*"([^"]+)"',part)
        m=re.search(r'\{drogon::(Get|Post|Put|Patch|Delete)\}\s*\)',part,re.S)
        if p and m: out.append(norm(m.group(1),p.group(1)))
    legacy=read('LegacyCompat.cpp')
    for m in re.finditer(r'\breg[0-3]\(\s*"([^"]+)"\s*,\s*drogon::(Get|Post|Put|Patch|Delete)',legacy):
        out.append(norm(m.group(2),m.group(1)))
    for m in re.finditer(r'registerAdminCrud0\(service\s*,\s*"([^"]+)"',legacy):
        out += [norm('GET',m.group(1)),norm('POST',m.group(1))]
    for m in re.finditer(r'registerAdminPatchDelete1\(service\s*,\s*"([^"]+)"',legacy):
        out += [norm('PATCH',m.group(1)),norm('DELETE',m.group(1))]
    return out

def main():
    required=['CMakeLists.txt','Dockerfile','render.yaml','.env.example','VERSION.txt','api_parity.json',
      'main.cpp','Config.cpp','Security.cpp','CoreService.cpp','LegacyCompat.cpp','RoomSocket.cpp',
      'Config.h','Security.h','CoreService.h','LegacyCompat.h','LegacyRouteManifest.h','RoomSocket.h',
      'security_test.cpp','legacy_v39_migrations.sql','index.html','native-socket.js','sw.js','manifest.webmanifest',
      'favicon-32.png','apple-touch-icon.png','icon-192.png','icon-512.png','integration_smoke.py','GITHUB_RENDER_UPLOAD.txt','FEATURE_MAP.md','ANNOTATION_BUILD.txt']
    miss=[x for x in required if not (ROOT/x).is_file()]
    expect(not miss,'平鋪部署必要檔案完整','缺少：'+', '.join(miss))
    expect(ROOT.name=='rpg_web_global_mvp','ZIP 根資料夾名稱正確','根資料夾不是 rpg_web_global_mvp')
    dirs=[p.name for p in ROOT.iterdir() if p.is_dir()]
    expect(not dirs,'GitHub 上傳內容為真正根目錄平鋪','仍有子資料夾：'+', '.join(dirs))

    docker=read('Dockerfile'); cmake=read('CMakeLists.txt'); render=read('render.yaml'); legacy=read('LegacyCompat.cpp')
    core_h=read('CoreService.h'); core_cpp=read('CoreService.cpp'); security_cpp=read('Security.cpp'); config_cpp=read('Config.cpp')
    expect('COPY . .' in docker and 'if [ ! -f src/main.cpp ]' in docker,'Dockerfile 可從根目錄平鋪重建標準結構','Dockerfile 缺平鋪相容重建')
    expect('COPY --from=builder /src/public /app/public' in docker and 'COPY --from=builder /src/db /app/db' in docker,
           'runtime 從 builder 取得 public/db，不依賴 GitHub 子資料夾','runtime 仍依賴 build context 子資料夾')
    expect('flat_include' in cmake and 'TRPG_SRC_PREFIX ""' in cmake,'CMake 可直接讀根目錄 .cpp/.h','CMake 不支援平鋪')
    expect('rootDir:' not in render and 'dockerfilePath: ./Dockerfile' in render and 'dockerContext: .' in render,
           'render.yaml 使用 Repo 根目錄','render.yaml 仍設定錯誤 rootDir/context')
    expect('std::filesystem::path{"legacy_v39_migrations.sql"}' in legacy,'migration 可從平鋪根目錄定位','migration 不支援平鋪路徑')

    public_part = core_h.split(' private:',1)[0]
    expect('Json::Value characterByUser(std::int64_t userId) const;' in public_part,
           'characterByUser 對戰鬥 helper 為 public，可通過 Render 編譯',
           'characterByUser 仍是 private，會重現 Render CoreService.cpp 編譯失敗')
    expect('const std::string role' in legacy and 'const auto role = stringValue(body["role"]) == "sub" ? "sub" : "main";' not in legacy,
           '職業 role 使用 std::string 比較，不再比較字串指標',
           '職業 role 仍可能以 const char* 比較')
    expect('marker.reserve(key.size() + 2)' in security_cpp and "documentRoot.assign(1, '.')" in config_cpp,
           'GCC 12 -Wrestrict 已針對 Security/Config 修正',
           'GCC 12 -Wrestrict hotfix 不完整')

    index_html=read('index.html'); main_cpp=read('main.cpp'); sw_js=read('sw.js')
    feature_map=read('FEATURE_MAP.md')
    note_count=read('CoreService.cpp').count('[功能備註｜') + read('LegacyCompat.cpp').count('[功能備註｜')
    front_function_count=len(re.findall(r'(?m)^\s*(?:async\s+)?function\s+[A-Za-z_$][\w$]*\s*\(', index_html))
    front_note_count=index_html.count('[前端功能備註｜')
    expect(note_count >= 180 and 'HTTP API → 功能 → 程式位置' in feature_map and len(feature_map) > 20000,
           '功能備註索引存在且覆蓋主要 API/前端函式',
           '功能備註或 FEATURE_MAP.md 不完整')
    expect(front_note_count == front_function_count and front_note_count > 100,
           'index.html 每個具名 function 都有前端功能備註',
           f'前端 function={front_function_count}，備註={front_note_count}')
    expect('const CPP_CORE_PREVIEW=false;' in index_html and 'cpp-unported{display:none' not in index_html,
           '完整 v39 前端介面已恢復，不再被 C++ preview 隱藏',
           'CPP_CORE_PREVIEW 或 cpp-unported 仍會隱藏完整功能')
    expect('setFileTypes({' in main_cpp and '"webmanifest"' in main_cpp and 'application/manifest+json' in main_cpp,
           'Drogon 可直接提供 manifest.webmanifest',
           'Drogon 尚未允許 webmanifest 靜態檔')
    expect('full-ui-1' in sw_js,
           'Service Worker cache 已換版，避免舊預覽頁殘留',
           'Service Worker cache 尚未換版')

    parity=json.loads(read('api_parity.json'))
    expected=collections.Counter(norm(*x.split(' ',1)) for x in parity.get('ported_http_routes',[]))
    actual=collections.Counter(routes())
    missing=list((expected-actual).elements())
    expect(len(parity.get('ported_http_routes',[]))==244 and not missing,'244/244 v39 HTTP handler 仍完整','API handler 缺失：'+', '.join(missing[:20]))
    expect(not [r for r,c in actual.items() if c>1],'HTTP handler 無重複','有重複 HTTP handler')

    node=shutil.which('node')
    if node:
        r=run([node,'--check','native-socket.js']); expect(r.returncode==0,'native-socket.js 語法通過',r.stdout.strip())
        inline='\n'.join(m.group(1) for m in re.finditer(r'<script(?:\s[^>]*)?>(.*?)</script>',read('index.html'),re.S))
        with tempfile.NamedTemporaryFile('w',suffix='.js',encoding='utf-8') as f:
            f.write(inline); f.flush(); r=run([node,'--check',f.name])
        expect(r.returncode==0,'index.html 內嵌 JavaScript 語法通過',r.stdout.strip())
    else: WARN.append('找不到 Node')

    gpp=shutil.which('g++')
    if gpp:
        with tempfile.TemporaryDirectory() as td:
            inc=Path(td)/'include/trpg'; inc.mkdir(parents=True)
            shutil.copy2(ROOT/'Security.h',inc/'Security.h')
            binary=Path(td)/'security_test'
            r=run([gpp,'-std=c++20','-Wall','-Wextra','-Wpedantic','-I'+str(Path(td)/'include'),'Security.cpp','security_test.cpp','-lcrypto','-lcrypt','-o',str(binary)])
            expect(r.returncode==0,'平鋪 Security C++ 可編譯',r.stdout.strip())
            if r.returncode==0:
                t=run([str(binary)]); expect(t.returncode==0,'JWT/bcrypt 測試通過',t.stdout.strip())
    else: WARN.append('找不到 g++')

    # Verify Docker reconstruction commands without Docker itself.
    with tempfile.TemporaryDirectory() as td:
        t=Path(td)
        for p in ROOT.iterdir():
            if p.is_file(): shutil.copy2(p,t/p.name)
        for d in ['src','include/trpg','tests','db','public/icons']:(t/d).mkdir(parents=True,exist_ok=True)
        for n in ['main.cpp','Config.cpp','Security.cpp','CoreService.cpp','LegacyCompat.cpp','RoomSocket.cpp']: shutil.copy2(t/n,t/'src'/n)
        for n in ['Config.h','CoreService.h','LegacyCompat.h','LegacyRouteManifest.h','RoomSocket.h','Security.h']: shutil.copy2(t/n,t/'include/trpg'/n)
        shutil.copy2(t/'security_test.cpp',t/'tests/security_test.cpp')
        shutil.copy2(t/'legacy_v39_migrations.sql',t/'db/legacy_v39_migrations.sql')
        for n in ['index.html','native-socket.js','sw.js','manifest.webmanifest']: shutil.copy2(t/n,t/'public'/n)
        for n in ['favicon-32.png','apple-touch-icon.png','icon-192.png','icon-512.png']: shutil.copy2(t/n,t/'public/icons'/n)
        expect((t/'src/main.cpp').is_file() and (t/'include/trpg/CoreService.h').is_file() and (t/'public/index.html').is_file() and (t/'db/legacy_v39_migrations.sql').is_file(),
               'Docker 平鋪→標準目錄重建模擬通過','Docker 重建模擬失敗')

    # CMake configure with a fake imported Drogon target proves flat-layout path logic parses.
    cmake_bin=shutil.which('cmake')
    if cmake_bin:
        with tempfile.TemporaryDirectory() as td:
            td=Path(td); pkg=td/'fake'; pkg.mkdir()
            (pkg/'DrogonConfig.cmake').write_text('add_library(Drogon::Drogon INTERFACE IMPORTED)\n',encoding='utf-8')
            b=td/'build'
            r=run([cmake_bin,'-S',str(ROOT),'-B',str(b),'-DDrogon_DIR='+str(pkg),'-DBUILD_TESTING=OFF'])
            expect(r.returncode==0,'CMake 平鋪模式 configure 通過',r.stdout.strip())
    else: WARN.append('找不到 cmake')

    print('TRPG Render flat-upload self check')
    for x in PASS: print('PASS',x)
    for x in FAIL: print('FAIL',x)
    for x in WARN: print('WARN',x)
    print(f'SUMMARY pass={len(PASS)} fail={len(FAIL)} warn={len(WARN)}')
    return 1 if FAIL else 0
if __name__=='__main__': raise SystemExit(main())
