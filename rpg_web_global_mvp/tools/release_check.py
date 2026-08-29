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
    gear=read('src/GearAffixSystem.cpp')
    for m in re.finditer(r'\bregister[0-2]\(\s*"([^"]+)"\s*,\s*drogon::(Get|Post|Put|Patch|Delete)',gear):
        out.append(norm(m.group(2),m.group(1)))
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
        'src/main.cpp','src/Config.cpp','src/Security.cpp','src/CoreService.cpp','src/LegacyCompat.cpp','src/RoomSocket.cpp','src/WorldSystems.cpp','src/RuleEngine.cpp','src/RuleCombatBridge.cpp','src/GearAffixSystem.cpp','src/EventSkillSystem.cpp',
        'include/trpg/Config.h','include/trpg/Security.h','include/trpg/CoreService.h','include/trpg/LegacyCompat.h','include/trpg/LegacyRouteManifest.h','include/trpg/RoomSocket.h','include/trpg/WorldSystems.h','include/trpg/RuleEngine.h','include/trpg/RuleCombatBridge.h','include/trpg/GearAffixSystem.h','include/trpg/EventSkillSystem.h',
        'public/index.html','public/native-socket.js','public/sw.js','public/manifest.webmanifest','public/icons/favicon-32.png','public/icons/apple-touch-icon.png','public/icons/icon-192.png','public/icons/icon-512.png',
        'db/legacy_v39_migrations.sql','tests/security_test.cpp','tools/integration_smoke.py','docs/VERSION.txt','docs/api_parity.json','docs/FEATURE_MAP.md','docs/RULE_ENGINE_GUIDE.md','docs/SKILL_SYSTEM_OVERVIEW.md','docs/RITUAL_SYSTEM_OVERVIEW.md','docs/MAGIC_SYSTEM_OVERVIEW.md','docs/CRAFT_DISCOVERY_OVERVIEW_v65_cpp26_15.md'
    ]
    missing=[x for x in required if not (ROOT/x).is_file()]
    expect(not missing,'必要檔案完整','缺少：'+', '.join(missing))
    expect(ROOT.name=='rpg_web_global_mvp','專案根資料夾名稱正確','根資料夾不是 rpg_web_global_mvp')
    root_files=[p.name for p in ROOT.iterdir() if p.is_file()]
    expect(len(root_files)<=4,'GitHub 根目錄只保留少量部署檔','根目錄檔案仍過多：'+', '.join(root_files))
    expect(not list(ROOT.glob('*.cpp')) and not list(ROOT.glob('*.h')),'C++ 原始碼已整理到 src/include','根目錄仍殘留 .cpp/.h')
    expect(all((ROOT/d).is_dir() for d in ['src','include/trpg','public','db','tests','tools','docs']),'標準 C++ 目錄結構完整','標準目錄缺失')

    version=read('docs/VERSION.txt').strip()
    expect(version=='73-cpp.34.25.10' and 'kVersion = "73-cpp.34.25.10"' in read('include/trpg/CoreService.h') and 'C++ 相容版 73-cpp.34.25.10' in read('public/index.html') and '"version": "73-cpp.34.25.10"' in read('docs/api_parity.json'),
           '67-cpp.28 版本資訊一致','版本資訊不同步')
    expect('v73-cpp-34-25-10-unresponsive-fix' in read('public/sw.js'),'Service Worker 快取版本已更新','Service Worker 仍使用舊快取')
    legacy=read('src/LegacyCompat.cpp'); html=read('public/index.html'); mig=read('db/legacy_v39_migrations.sql')
    expect(all(x in html for x in ['loadPostLoginDataInBackground','loadCharacterBootstrap','restoreLastRoomBackground']), '73-cpp.34.25.10 登入後非必要資料背景載入完整', '登入仍可能被大量資料載入阻塞')
    expect('後續資料依優先級背景載入' in html and 'setAuthBusy(false);\n    showApp();' in html, '73-cpp.34.25.10 認證成功立即進入主畫面並解鎖操作', '登入成功後仍可能停留在鎖定畫面')
    expect('for(let attempt=0;attempt<3;attempt++)' not in html and 'for(let loginTry=0;loginTry<2;loginTry++)' in html and 'setTimeout(()=>controller.abort(),75000)' in html, '73-cpp.34.25.10 登入僅在網路／gateway 錯誤重試一次且不會 28 秒過早取消', '登入重試策略或冷啟動等待時間不正確')
    expect(all(x in html for x in ['commitContentHTML(content,html)','content.__trpgRenderedHTML===next','if(changed){\n    bindView();']), '73-cpp.34.25.10 相同主內容不重建 DOM／不重綁事件', '主內容仍可能在相同畫面時重複重建')
    expect("queueSafeRender('background-loader')" not in html and 'runLoadersLimited(optionalLoaders,4)' in html, '73-cpp.34.25.10 DM 背景載入改為批次完成後統一渲染', 'DM 背景 API 完成時仍逐支重畫')
    expect('content-visibility:auto' in html and '#content{contain:layout style' in html and 'lastRenderedNavigationView' in html, '73-cpp.34.25.10 長頁面延遲排版／主內容 containment／導覽去重完整', '渲染層級優化缺失')
    expect(html.count('observe(document.body,{childList:true,subtree:true})') == 0 and 'function scheduleDmEnhancements()' in html, '73-cpp.34.25.10 移除全域 MutationObserver storm', '仍存在 document.body subtree observer，可能造成 Chrome 無回應')
    expect(all(x in html for x in ["state.view!=='admin'",'dmEnhanceRunning','scheduleDmEnhancements();']), '73-cpp.34.25.10 DM 增強器只在提交 DM 畫面後單次執行', 'DM 增強掃描仍可能在登入頁或非 DM 頁持續執行')
    socketjs=read('public/native-socket.js'); roomsocket=read('src/RoomSocket.cpp')
    expect(all(x in socketjs for x in ['client:ping','heartbeat timeout','visibilitychange','scheduleReconnect','2200']), '73-cpp.34.25.10 WebSocket 心跳／快速重連／前景恢復完整', '即時連線心跳或重連保護缺失')
    expect('server:pong' in roomsocket and 'client:ping' in roomsocket, '73-cpp.34.25.10 C++ 即時心跳回應存在', 'C++ WebSocket 心跳回應缺失')
    expect('pendingSnapshotRooms' in roomsocket and 'runAfter(0.035' in roomsocket and 'broadcastSnapshotNow' in roomsocket, '73-cpp.34.25.10 房間快照廣播具 35ms 合併', '房間快照仍可能重複密集廣播')
    expect('scheduleRealtimeSupplementRefresh' in html and "queueSafeRender('room:snapshot')" in html and 'Promise.allSettled([loadTimeView(),loadRoomWorld()])' in html, '73-cpp.34.25.10 即時快照立即渲染且附加 API 節流', '即時快照仍會被重型 HTTP 載入阻塞')
    expect(all(x in socketjs for x in ['ticketFallbackUntil','openGeneration','droppedRoomEnter']), '73-cpp.34.25.10 WebSocket 票證失敗冷卻／競態保護／舊進房佇列清理完整', 'WebSocket 重連仍可能被票證逾時或舊 room:enter 拖慢')
    expect(all(x in roomsocket for x in ['sameRoom','presenceCache','recipientsByUser']), '73-cpp.34.25.10 C++ 進房冪等／presence 去重／同 user 快照共用完整', 'C++ 即時同步仍存在重複進房或重複快照成本')
    expect('backendHealthProbePromise' in html and 'backendHealthProbeLastAt' in html and 'realtimeSnapshotSignature' in html and 'Math.imul(hash,16777619)' in html, '73-cpp.34.25.10 前端健康探測與重複快照去重完整', '前端 reconnect storm 或重複快照仍可能造成額外負載')

    expect(all(x in legacy for x in ['/api/fate-gap/pvp/{1}/join','/api/fate-gap/pvp/{1}/start','/api/fate-gap/pvp/{1}/act','/api/fate-gap/pvp/{1}/surrender']), '67-cpp.28 命鬥場 1v1 完整流程 API 存在','命鬥場完整流程 API 缺失')
    expect('/api/fate-gap/miyoi/{1}' in legacy and 'appendMiyoiMemory' in legacy, '67-cpp.28 彌夜 AI 狀態與長期記憶接線完整','彌夜 AI 記憶接線缺失')
    expect('fateArenaHTML' in html and 'join-fate-match' in html and 'fate-act' in html, '67-cpp.28 命鬥場玩家 UI 完整','命鬥場玩家 UI 缺失')
    expect('idx_fate_gap_pvp_room_status' in mig, '67-cpp.28 命鬥場查詢索引存在','命鬥場索引缺失')
    expect('fateMajorRule' in legacy and 'case 21:' in legacy and 'case 0:' in legacy, '73-cpp.34.25.10 22 張大阿爾克那命鬥規則接線存在','大阿爾克那完整規則缺失')
    expect('/api/fate-gap/miyoi/{1}/combat-decision' in legacy and '/api/fate-gap/miyoi/{1}/join-combat' in legacy, '73-cpp.34.25.10 彌夜協戰 AI API 存在','彌夜協戰 AI API 缺失')
    expect('/api/fate-gap/miyoi/{1}/combat-act' in legacy and 'last_combat_act_round' in legacy and 'fate_change_used' in legacy, '73-cpp.34.25.10 彌夜四技能實際協戰回合存在','彌夜實際協戰回合缺失')
    expect('/api/fate-gap/miyoi/{1}/dialogue' in legacy and 'anti_fate' in legacy and 'fate_interference' in legacy, '73-cpp.34.25.10 彌夜情境對話與逆命干擾存在','彌夜情境對話／逆命干擾缺失')
    expect('case 21:' in legacy and 'card[\"card_no\"]' in legacy, '73-cpp.34.25.10 22 張大阿爾協戰牌號接線完整','大阿爾協戰牌號或完整效果缺失')
    expect('/api/fate-gap/anti-fate/{1}/boss-state' in legacy and '/api/fate-gap/anti-fate/{1}/boss-act' in legacy and '逆寫終局' in legacy, '73-cpp.34.25.10 逆命 Boss 四階段 API 存在','逆命 Boss 階段接線缺失')
    expect('命運雜訊' in legacy and 'miyoi_fate_exposed' in legacy and 'tower_chip' in legacy, '73-cpp.34.25.10 塔羅協戰差異效果存在','塔羅協戰差異效果缺失')
    core=read('src/CoreService.cpp')
    expect('executeAntiFateBossAiAction' in core and 'isAntiFateBoss(attacker)' in core and 'anti_fate_ai' in core, '73-cpp.34.25.10 逆命 Boss 已接入正式怪物 AI 回合','逆命 Boss 仍未接入正式怪物 AI')
    expect('miyoi_silenced_until_round' in core and 'major_rule_flip' in core and 'anti_fate_phase' in core, '73-cpp.34.25.10 四階段命運污染會寫入實際戰鬥狀態','逆命 Boss 階段污染狀態接線缺失')
    expect(all(x in legacy for x in ['/api/fate-gap/anti-fate/{1}/link-state','/api/fate-gap/miyoi/{1}/restore-tarot','/api/fate-gap/miyoi/{1}/fate-finisher']), '73-cpp.34.25.10 彌夜逆命聯動 API 完整','彌夜逆命聯動 API 缺失')
    expect(all(x in html for x in ['被抹去的命運','restoreMiyoiTarot','miyoiFateFinisher','命運之外・世界重構']), '73-cpp.34.25.10 命隙聯動 UI 完整','命隙特殊聯動 UI 缺失')
    expect(all(x in core for x in ['blank_card_stacks','tarot_restored','fate_effect_suppressed']), '73-cpp.34.25.10 空白牌污染接入逆命 Boss AI','空白牌污染未接入逆命 Boss AI')
    expect("['library','fateGap'].includes(section)" in html, '73-cpp.34.25.10 命隙分頁可正確進入','命隙分頁仍被導回遊戲間大廳')
    expect('max-width:100vw !important' in html, '73-cpp.34.25.10 全站手機版寬度 hotfix 保留','全站手機版寬度 hotfix 缺失')
    expect('viewer room snapshot degraded for room' in read('src/WorldSystems.cpp') and 'snapshot[\"degraded\"] = true' in read('src/WorldSystems.cpp'), '73-cpp.34.25.10 進房 viewer 快照具降級保護','進房 viewer 快照缺少降級保護')
    expect('GET room viewer snapshot degraded for room' in core and 'resilientRoomSnapshot(self, roomId)' in core, '73-cpp.34.25.10 GET 房間具二次降級保護','GET 房間缺少二次降級保護')
    expect('部分房間附加資料載入失敗，已保留核心房間畫面' in html, '73-cpp.34.25.10 前端附加載入失敗不阻止進房','前端進房仍可能被附加載入阻斷')
    expect(all((ROOT / p).exists() for p in ['public/assets/miyoi/miyoi_avatar.jpg','public/assets/miyoi/miyoi_fullbody.jpg','public/assets/miyoi/miyoi_dialogue.jpg','public/assets/miyoi/miyoi_battle.jpg']), '73-cpp.34.25.10 彌夜正式美術素材完整', '彌夜正式美術素材缺失')
    expect('/assets/miyoi/miyoi_fullbody.jpg' in html and '/assets/miyoi/miyoi_avatar.jpg' in html, '73-cpp.34.25.10 命隙 UI 已接入彌夜正式立繪', '命隙 UI 尚未接入彌夜正式立繪')
    expect('wheelSpinState=new Map()' in html and '__wheelDelegatedBinding' in html and '輪盤轉動中…' in html, '73-cpp.34.25.10 輪盤具立即動畫與委派點擊保護', '輪盤手機點擊／動畫保護缺失')
    expect('if(options.isString())options=parseJson(options.asString()' in legacy, '73-cpp.34.25.10 後端輪盤選項相容 JSON 字串', '後端輪盤選項格式相容保護缺失')
    expect('chat-avatar' in html and 'chat-speaker' in html and 'data-chat-mention' in html, '73-cpp.34.25.10 聊天顯示頭像名字並可 @ 交流', '聊天室頭像／名字／交流 UI 缺失')
    expect('RoomSocket::broadcastSnapshotForRoom(roomId)' in legacy, '73-cpp.34.25.10 HTTP 說話會即時同步給房內玩家', 'HTTP 互動未即時廣播房間快照')
    expect("'avatar_url'" in core and "'portrait_url'" in core and 'character_room_bindings crb' in core, '73-cpp.34.25.10 聊天事件包含房間角色頭像與角色名', '聊天事件缺少角色頭像或房間角色綁定')
    expect('runAutonomousNpcSpeech' in legacy and 'npcJudgementLine' in legacy and 'autonomous_speech_chance' in legacy, '73-cpp.34.25.10 NPC 自主發言與情境判斷核心存在', 'NPC 自主發言核心缺失')
    expect('speaker_kind' in legacy and 'npc_avatar_url' in legacy and 'ai_judgement' in legacy, '73-cpp.34.25.10 NPC 聊天事件包含身份、頭像與判斷標記', 'NPC 聊天事件資料不完整')
    expect('npcAutonomousSpeech' in html and 'npcAutonomousSpeechChance' in html and 'NPC 判斷' in html, '73-cpp.34.25.10 DM 可控制 NPC 自主發言且聊天室可識別 NPC', 'NPC 自主交流前端設定或顯示缺失')
    expect('config:{...(state.npcTemplates||[]).find' in html and 'combat_side' in html and 'autonomous_speech_chance' in html, '73-cpp.34.25.10 NPC 表單會把 AI／戰鬥擴充欄位保存進 config', 'NPC 擴充欄位仍可能被 generic CRUD 丟棄')

    expect('monsterSkillBuilderHTML' in html and 'serializeMonsterSkillBuilder' in html and 'add-monster-skill-row' in html, '73-cpp.34.25.10 怪物卡可視化技能新增器存在', '怪物卡技能仍需手動輸入格式')
    expect(all(x in html for x in ['快速建立：','data-preset=\"normal\"','data-preset=\"elite\"','data-preset=\"boss\"','monster-form-section']), '73-cpp.34.25.10 怪物卡快速模板與分區表單存在', '怪物卡簡易建立 UI 缺失')
    expect("serializeMonsterSkillBuilder('active')" in html and "serializeMonsterSkillBuilder('passive')" in html and "serializeMonsterSkillBuilder('great')" in html, '73-cpp.34.25.10 主動／被動／大道技能皆可額外新增並保存', '怪物技能新增器未完整接入保存流程')


    cmake=read('CMakeLists.txt'); docker=read('Dockerfile'); render=read('render.yaml'); legacy=read('src/LegacyCompat.cpp')
    expect('src/main.cpp' in cmake and 'flat_include' in cmake and 'tests/security_test.cpp' in cmake and 'GearAffixSystem.cpp' in cmake and 'GearAffixSystem.h' in cmake and 'EventSkillSystem.cpp' in cmake and 'EventSkillSystem.h' in cmake,'CMake 以標準目錄為主並保留詞條模組／平鋪 fallback','CMake 目錄或詞條模組支援不完整')
    expect('COPY . .' in docker and 'if [ ! -f src/main.cpp ]' in docker,'Dockerfile 以標準目錄為主並保留平鋪 fallback','Dockerfile 目錄相容不完整')
    expect('COPY --from=builder /src/public /app/public' in docker and 'COPY --from=builder /src/db /app/db' in docker,'Docker runtime 正確帶入 public/db','Docker runtime public/db 路徑錯誤')
    expect('rootDir:' not in render and 'dockerfilePath: ./Dockerfile' in render and 'dockerContext: .' in render,'Render 使用 Repo 根目錄','Render root/context 設定錯誤')
    expect('Root Directory: 留空' in read('README.md') and 'Render Root Directory 保持空白' in read('docs/GITHUB_RENDER_UPLOAD.txt'),
           'Render Root Directory 文件已統一為 Repo 根目錄留空',
           'README 與 GitHub/Render 上傳說明的 Root Directory 互相矛盾')
    expect('std::filesystem::path{"db/legacy_v39_migrations.sql"}' in legacy,'C++ migration 使用 db/ 正式路徑','migration 找不到標準 db 路徑')

    arity=helper_call_arity_errors(legacy)
    expect(not arity,'SQL selectRows helper 參數數量一致','SQL helper 參數錯誤：'+'; '.join(arity))
    backup_array=re.search(r'std::array<const char\*,(\d+)> names=\{([^;]+)\};',legacy)
    if backup_array:
        declared=int(backup_array.group(1)); actual=len(re.findall(r'"[^"]+"',backup_array.group(2)))
        expect(declared==actual,'全站備份資料表陣列大小一致',f'備份資料表陣列宣告 {declared} 但實際 {actual}')
    else:
        bad('找不到全站備份資料表陣列')

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
    expect('60-cpp.21 怪物完整戰鬥實體' in read('docs/FEATURE_MAP.md'),'FEATURE_MAP 已更新本版功能','FEATURE_MAP 未更新')

    gear_cpp=read('src/GearAffixSystem.cpp')
    expect(all(x in migrations for x in ['CREATE TABLE IF NOT EXISTS special_currency_types','CREATE TABLE IF NOT EXISTS equipment_slot_types','CREATE TABLE IF NOT EXISTS affix_definitions','CREATE TABLE IF NOT EXISTS affix_pools','CREATE TABLE IF NOT EXISTS reforge_profiles','CREATE TABLE IF NOT EXISTS affix_targets']), '通用詞條／洗煉 migration 完整', '通用詞條／洗煉 migration 缺失')
    expect("'trait_core','詞條核心'" in migrations and "VALUES('puppet','G',1,2,'trait_core',1,'trait_core',5,FALSE,TRUE)" in migrations, '詞條核心與 G 級預設洗煉規則存在', '詞條核心或 G 級預設洗煉規則缺失')
    expect('/api/admin/affix-pools/{1}/entries/{2}' in gear_cpp and 'probability' in gear_cpp and 'weight' in gear_cpp, 'DM 詞條權重／機率管理由 C++ 提供', '詞條權重／機率 C++ 接線缺失')
    expect('/api/character/loadout' in gear_cpp and 'weapon_slots' in gear_cpp and 'equipment_slot_types' in gear_cpp and '雙手' in gear_cpp, '主副欄位與 DM 自訂裝備槽由 C++ 驗證', '裝備配置 C++ 驗證缺失')
    expect('/api/affix-targets/{1}/reroll' in gear_cpp and '/api/affix-targets/{1}/targeted' in gear_cpp and 'target_type' in gear_cpp, '通用洗煉支援隨機／指定且不綁單一目標類型', '通用洗煉 API 缺失')
    expect('special_currency' in legacy and 'character_special_currencies' in legacy, '合成配方可產出特殊貨幣', '配方尚未接特殊貨幣')
    expect('playerGearSlotsHTML' in html and 'playerAffixTargetsHTML' in html and 'adminAffixSystemHTML' in html, '玩家裝備／詞條與 DM 洗煉介面存在', '裝備或詞條前端介面缺失')
    expect(all(x in html for x in ['mobile-character-nav','gear-slot-grid','mobile-admin-stack','mobile-collapsible']), '手機角色／裝備／DM 面板優化存在', '手機面板優化缺失')
    expect('GearAffixSystem.cpp' in docker and 'GearAffixSystem.h' in docker, 'Docker 平鋪 fallback 包含詞條模組', 'Docker fallback 遺漏詞條模組')
    expect('defaultSlotSeedSql' in gear_cpp and 'VALUES($1,$2,$3,TRUE) ON CONFLICT(code) DO NOTHING' not in gear_cpp,
           '啟動預設裝備槽 seed 不再使用 PostgreSQL $3 binary bind',
           '啟動預設裝備槽仍使用 $3 參數，可能觸發 insufficient data left in message')

    # 59-cpp.20 mixed-rank + separate special-affix regression checks.
    expect(all(x in migrations for x in [
        "ALTER TABLE affix_definitions ADD COLUMN IF NOT EXISTS affix_kind",
        "ALTER TABLE affix_pools ADD COLUMN IF NOT EXISTS pool_kind",
        "ALTER TABLE affix_pools ADD COLUMN IF NOT EXISTS mixed_ranks",
        "ALTER TABLE affix_pools ADD COLUMN IF NOT EXISTS rank_weights",
        "ALTER TABLE affix_targets ADD COLUMN IF NOT EXISTS special_affixes",
        "ALTER TABLE affix_targets ADD COLUMN IF NOT EXISTS intrinsic_effects",
        "CREATE TABLE IF NOT EXISTS special_affix_rules",
        "CREATE TABLE IF NOT EXISTS special_affix_roll_history",
    ]), '59-cpp.20 詞條分層／特殊詞條 migration 完整', '59-cpp.20 詞條分層 migration 缺失')
    expect('pickAffixFromPool' in gear_cpp and 'mixed_ranks' in gear_cpp and 'rank_weights' in gear_cpp and 'weightedRankPick' in gear_cpp,
           '混合品階池採先品階後詞條兩層抽取', '混合品階兩層抽取 C++ 接線缺失')
    expect('special_affix_details' in gear_cpp and 'intrinsic_effects' in gear_cpp and 'current_affixes 完全不修改' in gear_cpp,
           '一般／特殊詞條／固有效果三層資料完全分離', '詞條三層資料分離不完整')
    expect('/api/admin/special-affix-rules' in gear_cpp and '/api/affix-targets/{1}/special-roll/{2}' in gear_cpp and 'once_per_pair' in gear_cpp,
           '裝備→人偶特殊詞條機率規則與一次判定 API 存在', '特殊詞條機率規則 API 缺失')
    expect('/api/admin/affix-pools/{1}' in gear_cpp and 'drogon::Delete' in gear_cpp,
           '詞條池可刪除並供整合測試清理', '詞條池刪除 API 缺失')
    expect('mixed-rank pool and separate equipment-to-puppet special affix roll' in smoke and 'chance_percent": 100' in smoke,
           'Render integration_smoke 實際驗證混池與特殊詞條不佔一般槽', '59-cpp.20 尚未納入 Render 整合測試')
    expect('affixPoolMixed' in html and 'pool-rank-weights' in html and 'specialAffixRuleForm' in html and 'roll-special-affix' in html,
           'DM 混合品階／特殊詞條規則與玩家判定 UI 完整', '59-cpp.20 詞條前端介面不完整')
    expect('[功能備註｜59-cpp.20 混合品階詞條池]' in gear_cpp and '[前端功能備註｜59-cpp.20 詞條／洗煉]' in html,
           '59-cpp.20 C++／前端功能備註存在', '59-cpp.20 功能備註缺失')

    # 60-cpp.21 monster combat entity regression checks.
    core=read('src/CoreService.cpp')
    expect(all(x in migrations for x in [
        "ALTER TABLE monster_templates ADD COLUMN IF NOT EXISTS passive_skills",
        "ALTER TABLE monster_templates ADD COLUMN IF NOT EXISTS great_way",
        "ALTER TABLE monster_templates ADD COLUMN IF NOT EXISTS boss_phases",
    ]), '60-cpp.21 怪物技能／大道／Boss migration 完整', '60-cpp.21 怪物 migration 缺失')
    expect('[功能備註｜60-cpp.21 怪物五維]' in core and '{"strength","agility","constitution","spirit","luck"}' in core and 'monsterSkillConditionMatches' in core and 'chooseMonsterCombatSkill' in core,
           'C++ 怪物五維與 AI 技能判定完整', '怪物五維或 AI 技能判定 C++ 接線缺失')
    expect('passive_skills' in core and 'great_way' in core and 'boss_phases' in core and 'COMBAT_SKILL_USED' in core and 'resource_cost' in core,
           'C++ 怪物主動／被動／大道技／Boss／規則事件已接戰鬥', '怪物完整戰鬥資料未接入 C++')
    expect('[功能備註｜60-cpp.21 怪物完整戰鬥實體]' in legacy and 'combat_accuracy_bonus' in legacy and 'great_way' in legacy and 'boss_phases' in legacy,
           '怪物修改 API 可保存完整戰鬥資料與頂層相容欄位', '怪物 PATCH 完整資料保存不完整')
    expect(all(x in html for x in ['monsterPassiveSkills','monsterGreatWayEnabled','monsterGreatWaySkills','parseMonsterCombatSkillsUI','formatMonsterCombatSkillsUI','luck']),
           'DM 怪物五維／技能／大道前端完整', '怪物完整戰鬥前端不完整')
    expect("if(!line.includes('|'))return line" in html and 'if (!raw.isObject()) continue' in core,
           '舊字串怪物技能維持顯示相容且不會被 AI 誤施放', '舊字串怪物技能相容保護缺失')
    expect('attributes_text' in legacy and 'PATCH 可只改一維，不會把其他四維清成 0' in legacy and '"attributes": {"luck": 18}' in smoke,
           '怪物五維 PATCH 支援局部修改且有整合回歸', '怪物五維局部 PATCH 可能覆寫其他屬性')
    expect('monster five attributes + structured skills + optional Great Way' in smoke and '測試大道技' in smoke and 'current_fifth' in smoke and 'boss_phases' in smoke,
           'Render integration_smoke 實際驗證怪物五維／技能／大道／Boss', '60-cpp.21 尚未納入 Render 整合測試')
    expect(smoke.find('def passed(name: str)') < smoke.find('passed("PWA version endpoint")') and 'status.get("version") == "65-cpp.26.15"' in smoke,
           'Render integration_smoke 初始化與版本檢查順序正確', 'integration_smoke 可能在 helper 定義前呼叫或仍檢查舊版本')
    expect('[前端功能備註｜60-cpp.21 怪物技能]' in html and '[前端功能備註｜60-cpp.21 怪物完整戰鬥實體]' in html,
           '60-cpp.21 C++／前端功能備註存在', '60-cpp.21 功能備註缺失')
    expect('[功能備註｜60-cpp.21.1 Render 編譯修正]' in legacy and
           'std::string clipped(std::string value, std::size_t bytes)' in legacy and
           'style["branch"] = clipped(branch, 80);' in legacy,
           '60-cpp.21.1 LegacyCompat UTF-8 clipped helper 已就地定義',
           '60-cpp.21.1 clipped helper 缺失，Render 會在 LegacyCompat.cpp 編譯失敗')
    expect('if(!x.isMember("price"))x["price"]' not in legacy and
           'if(!x.isMember("stock"))x["stock"]=-1;if' not in legacy,
           '60-cpp.21.1 knowledge_shop misleading-indentation warning 已整理',
           '60-cpp.21.1 knowledge_shop 仍有一行多條 if 的 misleading indentation')

    socket_cpp=read('src/RoomSocket.cpp'); native=read('public/native-socket.js')
    expect('CREATE TABLE IF NOT EXISTS realtime_tickets' in core and '/api/realtime-ticket' in core and '/api/version' in core,
           'PWA 版本 API 與 WebSocket 一次性票證 API 已建立','缺少版本或即時連線票證 API')
    expect('realtime_tickets' in socket_cpp and 'ticket' in native and 'createTicket' in native,
           'WebSocket 握手改用短效 ticket 並保留相容模式','WebSocket ticket 接線不完整')
    expect('/api/realtime-ticket' in smoke and 'WebSocketClient(args.base_url, ticket, "ticket")' in smoke,
           'Render integration_smoke 會實際驗證 WebSocket ticket 握手','WebSocket ticket 尚未納入整合測試')
    expect('id="installAppButton"' in html and 'beforeinstallprompt' in html and 'checkAppVersion' in html and 'SKIP_WAITING' in read('public/sw.js'),
           '登入頁下載 App 與 PWA 自動更新流程完整','PWA 下載／更新流程缺失')
    expect('data-view="library"' not in html and 'game-room-section' in html and '書庫位於遊戲間內' in html,
           '書庫已移入遊戲間且不再獨立顯示','書庫仍是獨立入口或未嵌入遊戲間')
    expect('MAGIC_NODE_ICONS' in html and 'MAGIC_NODE_TYPES' in html and 'magicTreeProgressHTML' in html and 'magicTreeToolbarHTML' in html,
           '魔法技能樹含圖示庫、節點類型、搜尋與進度條','魔法技能樹 UI 強化不完整')
    expect('body.element=' in html and 'body.node_style=' in html and 'normalizeMagicNodeStyle' in legacy,
           'DM 魔法節點的元素／位置／圖示／類型會真正寫入 C++','魔法節點表單仍未完整送到後端')
    expect('0,40' in legacy and '0,60' in legacy,
           '魔法技能樹可用座標範圍已擴充','魔法樹座標仍使用舊上限')

    # 58-cpp.19 derived-stat regression checks.
    expect('const auto derivedMaxHp = derivedConstitution * 2' in core and 'const auto derivedEndurance = (derivedConstitution + derivedSpirit) / 2' in core and 'const auto derivedWill = derivedSpirit * 8 / 10' in core and 'const auto derivedSanity = derivedSpirit * 12 / 10' in core,
           'C++ 角色衍生值公式完整','C++ 衍生值公式缺失或退回舊欄位')
    expect('WHERE cc.user_id=rm.user_id AND (rm.max_hp<=1 OR rm.max_spirit<=0 OR rm.max_sanity<=1)' in core,
           '舊房間衍生值修復 migration 存在','舊房間衍生值修復 migration 缺失')
    expect('derivedConstitution' in html and 'derivedEndurance' in html and 'derivedSanity' in html and 'derivedWill' in html,
           '角色卡前端衍生值 fallback 存在','角色卡仍完全依賴舊衍生欄位')
    expect("'max_sanity',CASE WHEN COALESCE(rm.max_sanity,0)<=1" in core,
           '房間快照對舊理智上限有 fallback','房間快照仍可能顯示 0/1 理智')

    if not shutil.which('docker') and not shutil.which('podman'):
        WARN.append('目前環境沒有 Docker/Podman；完整 Render Docker build 需部署後驗證')
    if not os.environ.get('DATABASE_URL'):
        WARN.append('目前環境沒有 DATABASE_URL；批量匯入真實 PostgreSQL 整合由 Render integration_smoke 驗證')


    expect(all(x in read('db/legacy_v39_migrations.sql') for x in ['ALTER TABLE skill_templates ADD COLUMN IF NOT EXISTS rank','ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS rank','ALTER TABLE recipes ADD COLUMN IF NOT EXISTS rank','CREATE TABLE IF NOT EXISTS knowledge_shop_items','CREATE TABLE IF NOT EXISTS knowledge_shop_purchases']), '共用品階／遊戲間／書庫 migration 完整', '缺少品階或知識商店 migration')
    expect('normalizeContentRank' in read('src/LegacyCompat.cpp') and '/api/knowledge-shop/{1}' in read('src/LegacyCompat.cpp') and 'knowledge_shop' in read('src/LegacyCompat.cpp'), 'C++ 品階驗證／知識商店／批量匯入已接線', 'C++ 品階或知識商店接線不完整')
    expect('function libraryHTML()' in read('public/index.html') and 'adminKnowledgeShopHTML' in read('public/index.html') and 'CONTENT_RANKS' in read('public/index.html'), '前端書庫／遊戲間／品階介面存在', '前端書庫或品階介面缺失')

    expect(all(x in read('db/legacy_v39_migrations.sql') for x in ['ALTER TABLE magic_studies ADD COLUMN IF NOT EXISTS element','CREATE TABLE IF NOT EXISTS npc_task_offers','CREATE TABLE IF NOT EXISTS npc_avatar_images','ALTER TABLE dungeon_templates ADD COLUMN IF NOT EXISTS dungeon_level']), '55-cpp.16 migration：魔法樹／NPC 任務頭像／副本等級完整', '55-cpp.16 migration 缺失')
    expect('房間清單只查核心穩定欄位' in read('src/CoreService.cpp') and "to_jsonb(rm)->'current_map_node_id'" in read('src/CoreService.cpp'), '房間列表／快照採向後相容查詢', '房間 500 相容修復不完整')
    expect('normalizeMagicElement' in read('src/LegacyCompat.cpp') and 'magicTreeBoardHTML' in read('public/index.html') and 'TreeX' in read('public/index.html') and '${prefix}Element' in read('public/index.html'), '七元素魔法技能樹 C++／前端完整', '魔法技能樹接線不完整')
    expect('validateMagicTreePrerequisites' in read('src/LegacyCompat.cpp') and '前置魔法必須位於同一元素技能樹' in read('src/LegacyCompat.cpp'), '魔法技能樹前置由 C++ 驗證同元素', '魔法技能樹前置驗證缺失')
    expect('room snapshot monsters degraded' in read('src/CoreService.cpp') and 'ALTER TABLE rooms ADD COLUMN IF NOT EXISTS closed_at' in read('src/CoreService.cpp'), '房間快照擴充區塊可降級且核心欄位自我修復', '房間快照降級／核心 migration 不完整')
    expect('/api/admin/npc-templates/{1}/avatar/upload' in read('src/WorldSystems.cpp') and '/tasks/{3}/accept' in read('src/LegacyCompat.cpp'), 'NPC 頭像上傳／任務發布已接線', 'NPC 頭像或任務接線不完整')
    expect('avatarFilePayload(file)' in read('public/index.html') and 'npc-task-accept' in read('public/index.html') and 'taskOffers=data.task_offers' in read('public/index.html'), 'NPC 頭像直接上傳與玩家接受 NPC 任務前端已接線', 'NPC 頭像 helper 或 NPC 任務玩家介面不完整')
    expect('dungeon_level' in read('src/LegacyCompat.cpp') and 'dungeonLevel' in read('public/index.html'), '副本 1~10／神 等級已接線', '副本等級接線不完整')
    expect('SELECT id,rank FROM \\\""+table+"\\\" WHERE name=$1' in read('src/LegacyCompat.cpp'), '知識商店批量 content_name 動態 SQL 已修正', '知識商店動態 SQL 仍可能錯誤')
    # 60-cpp.21.3 room creation/start hardening + retained wheel regression checks.
    expect('[功能備註｜60-cpp.21.3 開團安全快照]' in core and
           '[功能備註｜60-cpp.21.3 建立房間核心優先]' in core and
           '[功能備註｜60-cpp.21.3 開團核心優先]' in core and
           'resilientRoomSnapshot(self, roomId)' in core and
           'userCanManageRoom(self, user, roomId)' in core,
           '60-cpp.21.3 建房／開團後端核心優先與安全快照存在', '60-cpp.21.3 建房／開團後端保護缺失')
    expect('room character binding degraded for room' in core and
           'room create event degraded for room' in core and
           'reply(callback, resilientRoomSnapshot(self, roomId));' in core,
           '60-cpp.21.3 建房附加功能失敗不再拖垮房間建立', '60-cpp.21.3 建房仍可能被角色綁定／事件拖垮')
    expect('[功能備註｜60-cpp.21.3 加入房間核心優先]' in core and
           'SELECT id,name FROM character_cards WHERE user_id=$1 LIMIT 1' in core and
           'room join character binding degraded for room' in core and
           'room join event degraded for room' in core,
           '60-cpp.21.3 加入房間不再依賴完整 characterByUser', '60-cpp.21.3 加入房間仍可能被後期角色系統拖垮')
    expect('const auto user = requireCurrentUser(self, request);' in core[core.find('/api/rooms/{1}/start'):core.find('/api/rooms/{1}/battle/start')] and
           '只有房主／房間 GM／全站 DM 可以開團' in core,
           '60-cpp.21.3 開團權限支援房主／房間 GM／全站 DM', '60-cpp.21.3 開團仍被全站 DM 權限綁死')
    expect('[功能備註｜60-cpp.21.2 轉盤核心 migration]' in core and
           'CREATE TABLE IF NOT EXISTS custom_wheels' in core and
           'CREATE TABLE IF NOT EXISTS wheel_spins' in core,
           '60-cpp.21.2 轉盤核心 migration 完整', '60-cpp.21.2 轉盤核心 migration 缺失')
    expect('[前端功能備註｜60-cpp.21.3 建房／開團核心事件]' in html and
           'bindCriticalRoomWheelEvents();' in html and
           'if(create)create.onclick=createRoomCritical;' in html and
           'if(join)join.onclick=joinRoomCritical;' in html and
           "if(start)start.onclick=()=>startCampaignCritical(start);" in html and
           "if(interactionForm)interactionForm.onsubmit=submitInteractionCritical;" in html and
           html.find('bindCriticalRoomWheelEvents();') < html.find('try{bindGearAffixEvents();}'),
           '60-cpp.21.4 核心房間事件＋64-cpp.25.4 統一互動優先綁定存在', '核心房間／互動事件仍可能被其他 bind 阻斷')
    expect('await activateRoom(data);' in html[html.find('async function createRoomCritical'):html.find('async function spinWheelCritical')] and
           "data?.degraded_snapshot?'房間已建立" in html and
           "data?.degraded_snapshot?'跑團已開始" in html,
           '60-cpp.21.3 前端可接受安全降級快照並重新載入房間資料', '60-cpp.21.3 前端未處理降級快照')
    # 60-cpp.21.4 action hardening retained + 64-cpp.25.4 unified interaction.
    action_start=legacy.find('[功能備註｜60-cpp.21.4 行動提交核心優先]')
    action_end=legacy.find('// [功能備註｜調查／線索｜GET /api/journal]', action_start)
    action_block=legacy[action_start:action_end] if action_start>=0 and action_end>action_start else ''
    expect(action_start>=0 and 'ruleEngineDegraded' in action_block and
           'service->addEvent(roomId, user.id, "action", payload);' in action_block and
           'roomSnapshotForViewer(service, roomId, user.id)' in action_block and
           'out["snapshot_degraded"] = true;' in action_block,
           '60-cpp.21.4 action 核心持久化＋規則／快照降級仍保留', 'action 核心穩定性保護缺失')
    expect('service->roomSnapshot(roomId)' not in action_block and
           'runRule("PLAYER_ACTION", true);' in action_block,
           'action 使用 viewer 快照且保留規則阻止', 'action 可能回傳未過濾快照或繞過規則')
    expect('[功能備註｜64-cpp.25.4 行動＋說話＋待機]' in legacy and
           'interactionType != "action"' in action_block and
           'runRule("CHARACTER_SPOKE", true);' in action_block and
           'runRule("SOUND_EMITTED", true);' in action_block and
           'runRule("PLAYER_WAITED", true);' in action_block and
           'service->addEvent(roomId, user.id, "speech", payload);' in action_block and
           'service->addEvent(roomId, user.id, "wait", payload);' in action_block,
           '64-cpp.25.4 說話／行動／待機共用互動 API 完整', '互動 API 三模式接線不完整')
    expect('physicalSpeechAudience' in legacy and 'applyInteractionNoise' in legacy and
           'UPDATE room_map_nodes SET noise_level=' in legacy and
           'UPDATE room_monsters rm' in legacy and 'hearing_threshold' in legacy,
           '64-cpp.25.4 說話會產生節點噪音並提高可聽見怪物警戒', '說話聲音尚未接到地圖／怪物 AI')
    expect('[前端功能備註｜64-cpp.25.4 行動＋說話＋待機]' in html and
           'async function submitInteractionCritical(event)' in html and
           'interactionComposerHTML()' in html and
           "if(interactionForm)interactionForm.onsubmit=submitInteractionCritical;" in html and
           html.find("if(interactionForm)interactionForm.onsubmit=submitInteractionCritical;") < html.find('try{bindGearAffixEvents();}'),
           '64-cpp.25.4 統一互動區前端優先綁定存在', '互動區可能被後續 JS 綁定錯誤拖垮')
    expect('id="chatForm"' not in html and '房間聊天' not in html and
           '💬 說話' in html and '⚙️ 行動' in html and '⏸️ 待機' in html,
           '64-cpp.25.4 已移除場外聊天 UI，只保留說話／行動／待機', '仍存在場外聊天入口或三模式缺失')
    world=read('src/WorldSystems.cpp')
    expect('eventType != "chat" && eventType != "speech"' in world,
           'speech audience 會套用 viewer 可見性過濾', 'speech 可能繞過 audience 可見性')
    expect('action.get("rule_engine_degraded") is False' in smoke and
           'PLAYER_ACTION viewer snapshot missing' in smoke and
           'speech interaction did not persist' in smoke and
           'wait interaction did not persist' in smoke,
           'Render integration_smoke 驗證 action／speech／wait', '統一互動尚未納入 Render integration smoke')


    # 61-cpp.22 planar map regression.
    expect('[功能備註｜61-cpp.22 平面地圖核心 migration]' in core and
           'x_percent NUMERIC(6,3)' in core and 'node_type VARCHAR(40)' in core,
           '61-cpp.22 C++ 平面地圖 migration 完整', '平面地圖座標尚未納入 C++ 核心 migration')
    expect('[功能備註｜61-cpp.22 平面地圖讀取]' in legacy and 'out["current_map_node_id"]' in legacy,
           '61-cpp.22 map API 回傳有效玩家位置', 'map API 沒有回傳玩家目前節點')
    expect('function planarMapHTML()' in html and 'function bindPlanarMap()' in html and
           'class="planar-map' in html and 'x_percent' in html and 'y_percent' in html,
           '61-cpp.22 前端平面圖／節點拖曳存在', '平面圖 UI 或拖曳節點功能缺失')
    world=read('src/WorldSystems.cpp')
    expect('[功能備註｜61-cpp.22 玩家節點移動]' in world and '此節點與目前位置沒有連線' in world and
           world.find('std::int64_t intValue') < world.find('[功能備註｜61-cpp.22 節點連線驗證]'),
           '61-cpp.22 玩家節點移動會驗證連線且 helper 宣告順序正確', '玩家移動未依節點連線限制或 helper 宣告順序錯誤')
    expect('[功能備註｜61-cpp.22 隊伍節點移動' in legacy and '此節點與隊伍目前位置沒有連線' in legacy,
           '61-cpp.22 隊伍移動同樣遵守節點連線', '隊伍仍可跨未連線節點移動')
    expect('[功能備註｜61-cpp.22 雙向節點連線]' in legacy and 'syncBidirectionalMapConnections' in legacy,
           '61-cpp.22 DM 節點連線會雙向同步', '手動節點連線可能只剩單向')
    expect('player_node = int(node_ids[1])' in smoke and 'blocked_node = int(node_ids[2])' in smoke,
           'Render integration_smoke 依相鄰節點驗證移動／阻止', 'Render 地圖整合測試仍使用跨節點瞬移')

    # 61-cpp.22.1 node deletion regression.
    expect('[功能備註｜61-cpp.22.1 安全刪除地圖節點]' in legacy and
           'prepareMapNodeDeletion' in legacy and
           'clearMapNodeReference(db, "room_members", "current_map_node_id"' in legacy and
           'clearMapNodeReference(db, "room_teams", "current_map_node_id"' in legacy and
           'clearMapNodeReference(db, "scheduled_world_events", "map_node_id"' in legacy,
           '61-cpp.22.1 節點刪除會清理所有核心引用', '節點刪除仍可能被舊 FK／目前位置引用阻止')
    expect('[前端功能備註｜61-cpp.22.1 安全刪除地圖節點]' in html and
           'async function deleteMapNodeCritical' in html and 'deleteNodeFromEditor' in html and
           "$$('.delete-map-node').forEach(button=>button.onclick=()=>deleteMapNodeCritical" in html,
           '61-cpp.22.1 節點清單與編輯器共用安全刪除', '節點刪除前端入口或刷新流程缺失')
    expect('passed("planar map node deletion cleanup")' in smoke and
           'deleted_node_id' in smoke,
           'Render integration_smoke 實際驗證節點刪除清理', '節點刪除尚未納入 Render integration smoke')

    # 62-cpp.23 event skills / multi-hit regression.
    event_skill=read('src/EventSkillSystem.cpp')
    event_header=read('include/trpg/EventSkillSystem.h')
    expect('[功能備註｜62-cpp.23 事件型技能 migration]' in event_skill and
           'combat_status_instances' in event_skill and 'status_templates ADD COLUMN IF NOT EXISTS stack_mode' in event_skill,
           '62-cpp.23 事件型技能／疊層狀態 migration 完整', '事件技能狀態 migration 缺失')
    expect('畫兵成真' in event_skill and '速寫標記' in event_skill and '畫像標記•攻' in event_skill and '畫像標記•療' in event_skill and
           '"hit_count":3' in event_skill and 'damage_taken_from_source_percent_per_stack' in event_skill and 'healing_efficiency_percent_per_stack' in event_skill,
           '62-cpp.23 畫兵成真／速寫標記可執行範例存在', '繪師技能範例或標記效果缺失')
    expect('executeConfiguredCombatSkill' in core and 'COMBAT_HIT_SEQUENCE_STARTED' in core and 'COMBAT_HIT_SEQUENCE_ENDED' in core and
           'hit_index' in core and 'configured_hits' in core and 'applyEventSkillDamageModifiers' in core,
           '62-cpp.23 C++ 多段 Hit Sequence 已接戰鬥核心', '多段技能仍未接入正式戰鬥核心')
    expect('per_hit' in event_skill and 'once_per_skill' in event_skill and 'once_per_target' in event_skill and 'final_hit_only' in event_skill,
           '62-cpp.23 Proc Mode 支援每段／每技能／每目標／最後一段', 'Proc Mode 支援不完整')
    expect('CombatExpressionParser' in core and '精神' in core and 'std::stold' in core,
           '62-cpp.23 傷害公式支援 1+精神*2 等安全算式', '精神公式解析器缺失')
    expect('editSkillHitCount' in html and 'editSkillHitSequence' in html and 'editSkillEventTriggers' in html and
           'stHitCount' in html and 'stEventTriggers' in html and 'parseSkillJsonUI' in html,
           '62-cpp.23 DM 技能編輯器可設定多段與事件觸發器', '前端技能編輯器缺多段／Trigger 欄位')
    expect('[功能備註｜62-cpp.23 多段攻擊／事件型技能執行]' in legacy and 'executeConfiguredCombatSkill' in legacy and
           'roomSnapshotForViewer' in legacy,
           '62-cpp.23 玩家技能 API 執行多段技能並回傳 viewer 快照', '玩家技能 API 尚未接入多段 C++ 執行器')
    expect('[功能備註｜62-cpp.23 怪物多段技能]' in core and 'executeConfiguredCombatSkill(self, roomId, actorType, actorId, selectedSkill' in core,
           '62-cpp.23 怪物 AI 共用多段技能執行器', '怪物 AI 仍只會執行單段技能')
    expect('[功能備註｜62-cpp.23 每個衍生事件獨立判定]' in event_skill and
           'ctx.removeMember("event_instance_id")' in event_skill,
           '62-cpp.23 同一技能多個正／負面效果各自計算事件次數', '同一技能多個狀態事件可能被錯誤合併')
    expect('[功能備註｜62-cpp.23 戰鬥狀態快照]' in core and
           'combat_status_instances s' in core and 'combat_statuses' in core,
           '62-cpp.23 泛型戰鬥狀態會回到房間快照', '事件技能狀態未回傳到房間快照')
    expect('checkpoint["event_skill_statuses"]' in legacy and
           'DELETE FROM combat_status_instances WHERE room_id=$1' in legacy and
           '"combat_status_instances"' in legacy,
           '62-cpp.23 房間／全站備份包含事件技能狀態', '事件技能狀態未納入備份還原')
    expect('passed("event skill templates and portrait marks")' in smoke and '畫兵成真' in smoke and '速寫標記' in smoke,
           'Render integration_smoke 驗證事件技能模板與雙標記 migration', '事件技能尚未納入 Render integration smoke')

    # 62-cpp.23.1 field effect / auto combat-start regression.
    expect('[功能備註｜62-cpp.23.1 場地技能 migration]' in event_skill and 'combat_field_effects' in event_skill and
           'stack_policy' in event_skill and 'replace_lower_priority' in event_skill and 'exclusive' in event_skill,
           '62-cpp.23.1 場地效果 migration／覆蓋規則完整', '場地效果資料表或覆蓋規則缺失')
    expect('[功能備註｜62-cpp.23.1 戰鬥開始自動觸發]' in event_skill and 'auto_combat_start' in event_skill and
           'ON_COMBAT_START' in event_skill and 'create_field' in event_skill,
           '62-cpp.23.1 戰鬥開始自動場地技能存在', 'AUTO_COMBAT_START 尚未接到場地效果')
    expect('[功能備註｜62-cpp.23.1 戰鬥開始自動技能]' in core and 'COMBAT_STARTED' in core and
           'clearCombatFieldEffects' in core and '[功能備註｜62-cpp.23.1 場地快照]' in core,
           '62-cpp.23.1 戰鬥開場／結束／快照接線完整', '場地未接入正式戰鬥生命週期')
    expect('stActivationMode' in html and 'stFieldDefinition' in html and 'editSkillActivationMode' in html and
           'editSkillFieldDefinition' in html and '場地效果' in html,
           '62-cpp.23.1 DM 場地技能編輯器與玩家場地顯示存在', '前端缺少場地技能設定或顯示')
    expect('checkpoint["combat_field_effects"]' in legacy and 'DELETE FROM combat_field_effects WHERE room_id=$1' in legacy and
           '"combat_field_effects"' in legacy,
           '62-cpp.23.1 房間／全站備份包含場地效果', '場地效果未納入備份還原')
    expect('passed("auto combat-start field activation")' in smoke and 'field_effects' in smoke and
           'combat field effects were not cleared at battle end' in smoke,
           'Render integration_smoke 實際驗證場地自動展開與戰鬥結束清理', '場地技能尚未納入 Render integration smoke')

    overview=read('docs/SKILL_SYSTEM_OVERVIEW.md')
    expect('多段攻擊' in overview and 'Proc Mode' in overview and 'Trigger 事件' in overview and 'FieldEffect' in overview and '畫兵成真' in overview and '速寫標記' in overview,
           '62-cpp.23.1 技能系統總覽文件完整', '技能系統總覽缺少核心章節或示例')

    expect('passed("wheel create, list and spin")' in smoke and
           '/api/admin/wheels' in smoke and '/spin' in smoke,
           'Render integration_smoke 實際驗證輪盤建立／讀取／轉動', '輪盤尚未納入 Render integration smoke')


    # 63-cpp.24 ritual dual-panel regression checks.
    expect(all(x in migrations for x in [
        "ALTER TABLE ritual_studies ADD COLUMN IF NOT EXISTS layout_definition",
        "ALTER TABLE ritual_studies ADD COLUMN IF NOT EXISTS material_requirements",
        "ALTER TABLE ritual_studies ADD COLUMN IF NOT EXISTS ritual_steps",
        "CREATE TABLE IF NOT EXISTS ritual_instances",
    ]), '63-cpp.24 儀式配方／實例 migration 完整', '63-cpp.24 儀式 migration 缺失')
    expect(all(x in legacy for x in [
        '/api/rooms/{1}/ritual-field', '/api/rooms/{1}/ritual-field/{2}/place',
        '/api/rooms/{1}/ritual-field/{2}/start', '/api/rooms/{1}/ritual-field/{2}/advance',
        'RITUAL_SUCCEEDED', 'applyConfiguredEventSkillEffects',
    ]), 'C++ 儀式場生命週期 API 與技能效果共用核心存在', 'C++ 儀式場 API／效果接線缺失')
    expect('ritualStudiesHTML' in html and 'ritualFieldHTML' in html and 'data-view="ritualField"' in html and 'ritualLayoutPreviewHTML' in html,
           '前端儀式配方／儀式場兩個獨立面板存在', '儀式雙面板前端缺失')
    expect('material_requirements' in legacy and 'element_requirements' in legacy and 'inventoryQuantity' in legacy and 'element_storage' in legacy,
           '儀式啟動會驗證並扣除材料／元素', '儀式成本驗證／消耗缺失')
    expect('ritual recipe layout, room placement, channeling and success lifecycle' in smoke,
           'Render integration smoke 覆蓋儀式放置→引導→成功', '整合測試未涵蓋儀式生命週期')
    expect('[功能備註｜63-cpp.24 儀式學雙面板]' in migrations and '[功能備註｜63-cpp.24 儀式場]' in legacy and '[前端功能備註｜63-cpp.24 儀式場]' in html,
           '63-cpp.24 C++／DB／前端功能備註存在', '63-cpp.24 功能備註缺失')
    expect('checkpoint["ritual_instances"]' in legacy and 'snapshot["ritual_instances"]' in core and '"ritual_instances"' in legacy,
           '儀式實例納入房間快照與備份', '儀式實例快照／備份缺失')


    # 63-cpp.24.1 ritual progression / unknown research regression checks.
    expect(all(x in migrations for x in [
        "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS ritual_xp",
        "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS ritual_points_earned",
        "ALTER TABLE ritual_studies ADD COLUMN IF NOT EXISTS requires_research",
        "CREATE TABLE IF NOT EXISTS character_ritual_research",
    ]), '63-cpp.24.1 儀式 XP／研究 migration 完整', '63-cpp.24.1 儀式研究 migration 缺失')
    expect('/api/ritual-studies/{1}/research' in legacy and '/api/admin/players/{1}/ritual-research' in legacy and
           'ritualStudyForViewer' in legacy and 'grantRitualXp' in legacy and 'rewardRitualSuccess' in legacy,
           'C++ 儀式研究／XP／首次完成防刷核心存在', 'C++ 儀式研究核心缺失')
    expect('research-ritual-study' in html and 'ritualProgression' in html and 'ResearchProgressPerPoint' in html and
           'FailureResearchXp' in html and 'ritualStudyResearchApply' in html,
           '前端儀式研究進度／DM 研究管理存在', '前端儀式研究 UI 缺失')
    expect('ritual_xp' in legacy and 'ritual_xp' in html and 'taskRewardRitualXp' in html and 'itRitualXp' in html,
           '任務／道具可發放儀式學 XP', '儀式學 XP 獎勵來源未接線')
    expect('ritual XP, level-up points, unknown-recipe research and first-success anti-farm reward' in smoke,
           'Render integration smoke 覆蓋研究→學習→首次成功升級', '整合測試未涵蓋儀式研究進程')
    expect('[功能備註｜63-cpp.24.1 儀式學點數／研究系統]' in migrations and
           '[功能備註｜63-cpp.24.1 未解析儀式]' in legacy and
           '[前端功能備註｜63-cpp.24.1 未解析儀式]' in html,
           '63-cpp.24.1 C++／DB／前端功能備註存在', '63-cpp.24.1 功能備註缺失')

    # 64-cpp.25 character tier / ascension ritual regression.
    expect('[功能備註｜64-cpp.25 角色正式等階' in core and 'characterTierBudget' in core and
           'attribute_budget' in core and 'tier_title' in core and 'ascension_ready' in core,
           '64-cpp.25 角色 0～10 階／1～4 級與屬性額度已接核心', '64-cpp.25 角色等階核心缺失')
    expect(all(x in migrations for x in ['64-cpp.25 角色正式等階／登階儀式','ascension_from_tier','ascension_to_tier']),
           '64-cpp.25 migration 與登階儀式欄位完整', '64-cpp.25 migration／登階欄位缺失')
    expect('validateAscensionRitual' in legacy and 'applyAscensionRitual' in legacy and
           '/api/admin/players/{1}/character-rank/advance' in legacy and '登神儀式' in legacy,
           '64-cpp.25 小級推進／登階／登神 C++ 流程完整', '64-cpp.25 晉階流程缺失')
    expect('advanceCharacterRank' in html and 'AscensionFrom' in html and 'AscensionTo' in html and
           '第五基礎屬性' in html and '幸運（特殊屬性）' in html,
           '64-cpp.25 前端角色等階與登階儀式管理完整', '64-cpp.25 前端等階／儀式欄位缺失')
    expect('[功能備註｜64-cpp.25 固定等階屬性額度]' in legacy and
           '舊任務屬性點獎勵不再突破大階總額' in legacy and
           'taskRewardAttribute' in html and '屬性點（由角色等階固定）' in html,
           '64-cpp.25 舊任務屬性點不會突破固定大階額度', '舊任務仍可能額外灌入基礎屬性點')
    expect('version_info.get("version") == "65-cpp.26.15"' in smoke and
           'character tier 0 -> 1 and fixed tier-1 attribute budget' in smoke and
           'ascension ritual advances big tier and preserves DM extra attribute budget' in smoke,
           'Render integration_smoke 覆蓋0→1／四小級／登階儀式', '64-cpp.25 真實晉階流程尚未納入 Render integration smoke')


    # 64-cpp.25.2 avatar recovery + compact admin character cards.
    expect('64-cpp.25.2 玩家頭像自動恢復' in core and 'SELECT 1 FROM avatar_images WHERE user_id=$1 LIMIT 1' in core,
           '64-cpp.25.2 頭像資料庫 fallback 已接核心', '頭像 fallback 核心缺失')
    expect('characterAvatarURL' in html and 'compactCharacterAvatarHTML' in html and 'compact-player-card' in html and '更多資料' in html,
           '64-cpp.25.2 玩家角色卡已簡化並有頭像容錯', '玩家角色卡簡化或頭像容錯缺失')
    # 65-cpp.26.2 portrait + mobile regression.
    expect(all(x in migrations for x in ['65-cpp.26.2：角色立繪','character_cards ADD COLUMN IF NOT EXISTS portrait_url','CREATE TABLE IF NOT EXISTS portrait_images']),
           '65-cpp.26.2 立繪 migration 完整', '65-cpp.26.2 立繪 migration 缺失')
    world=read('src/WorldSystems.cpp')
    expect(all(x in world for x in ['/api/me/portrait','/api/me/portrait/upload','/api/admin/players/{1}/portrait','/api/admin/players/{1}/portrait/upload','/api/portrait-images/{1}','storePortraitUpload']),
           '65-cpp.26.2 C++ 立繪 API 完整', '65-cpp.26.2 C++ 立繪 API 缺失')
    expect('65-cpp.26.2 角色立繪自動恢復' in core and 'SELECT 1 FROM portrait_images WHERE user_id=$1 LIMIT 1' in core and
           'related["portrait_image"]' in legacy and '"avatar_images","portrait_images"' in legacy,
           '65-cpp.26.2 立繪 fallback／備份完整', '65-cpp.26.2 立繪 fallback 或備份缺失')
    expect(all(x in html for x in ['characterPortraitURL','characterPortraitHTML','my_portrait_file','openAdminPortraitEditor','edit-portrait','character-profile-header','65-cpp.26.2 CHARACTER PORTRAIT + MOBILE POLISH']),
           '65-cpp.26.2 立繪與手機 UI 完整', '65-cpp.26.2 立繪或手機 UI 缺失')
    expect('/api/me/portrait/upload' in smoke and 'portrait upload persistence' in smoke and 'player portrait and DM portrait override' in smoke,
           '65-cpp.26.2 Render smoke 覆蓋立繪', '65-cpp.26.2 立繪尚未納入 integration smoke')
    # 65-cpp.26.3 room portrait stage + mobile second-pass regression.
    expect(all(x in html for x in ['roomPortraitCandidates','roomPortraitEntity','roomPortraitStageHTML','portrait-stage-side','portrait-target-chip','npc-dialogue-stage']),
           '65-cpp.26.3 房間／NPC 立繪舞台完整', '65-cpp.26.3 房間或 NPC 立繪舞台缺失')
    expect(all(x in html for x in ['mobileMoreSheet','data-mobile-jump="char-skills"','data-mobile-jump="char-inventory"','admin-mobile-body','rpg_room_portrait_hidden','rpg_room_portrait_side','rpg_npc_portrait_hidden','rpg_npc_portrait_side']),
           '65-cpp.26.3 手機固定快捷列與 DM 卡片化完整', '65-cpp.26.3 手機第二輪 UI 缺失')
    expect('65-cpp.26.3 PORTRAIT STAGE + MOBILE SECOND PASS' in html and '65-cpp.26.3 手機固定快捷列' in html,
           '65-cpp.26.3 前端功能備註完整', '65-cpp.26.3 前端功能備註缺失')
    # 65-cpp.26.5 task creation + DM free-attribute regression.
    expect('65-cpp.26.5 任務建立修正' in legacy and 'body["target_user_id"] = Json::Value{Json::nullValue}' in legacy and 'body["profession_id"] = Json::Value{Json::nullValue}' in legacy and "target_user_id:$('#taskTargetUser').value?Number($('#taskTargetUser').value):null" in html,
           '65-cpp.26.5 任務建立 nullable FK 修正完整', '任務建立仍可能把空選項寫成 0 外鍵')
    expect('/api/admin/players/{1}/attribute-points/grant' in legacy and '65-cpp.26.5 DM 自由屬性點' in core and 'grantAttributePointsBtn' in html and 'attribute_bonus' in core,
           '65-cpp.26.5 DM 自由屬性點完整', 'DM 自由屬性點後端／角色總額／前端接線不完整')
    expect('task creation normalizes nullable foreign keys' in smoke and 'DM grants persistent extra free attribute points' in smoke and 'tier 2 effective budget must preserve the +7 DM bonus' in smoke,
           '65-cpp.26.5 Render smoke 覆蓋任務與自由屬性點', '65-cpp.26.5 新修正尚未納入 integration smoke')

    # 65-cpp.26.6 planar-map UX + mobile regression.
    expect(all(x in html for x in ['65-cpp.26.6 PLANAR MAP UX + MOBILE','map-toolbar','planar-map-surface','mapZoomIn','mapZoomOut','mapLocate','mapFullscreen','mapViewState','zoomTo','centerNode']),
           '65-cpp.26.6 地圖縮放／定位／全螢幕 UI 完整', '65-cpp.26.6 地圖工具列或縮放定位功能缺失')
    expect(all(x in html for x in ['mapReachableIds','reachable','unreachable','mapEditMode','雙指縮放','這個地點與目前位置沒有直接連線','map-fullscreen-open']),
           '65-cpp.26.6 可達路線／手機手勢／DM 安全編輯完整', '65-cpp.26.6 地圖路線提示、手機手勢或 DM 編輯保護缺失')

    # 65-cpp.26.7 DM workspace UX regression.
    expect(all(x in html for x in ['65-cpp.26.7 DM WORKSPACE UX','ADMIN_NAV_GROUPS','adminNavigationHTML','admin-nav-search','admin-workspace','admin-section-head','admin-stat-strip']),
           '65-cpp.26.7 DM 分類導航／搜尋／統計 UI 完整', '65-cpp.26.7 DM 面板分類、搜尋或快速統計缺失')
    expect(all(x in html for x in ["localStorage.setItem('rpg_admin_view',state.adminView)",'adminNavSearch.oninput','admin-nav-item','admin-nav-group']),
           '65-cpp.26.7 DM 分頁記憶與手機導航完整', '65-cpp.26.7 DM 分頁記憶或手機導航接線缺失')

    # 65-cpp.26.8 login UX regression.
    expect(all(x in html for x in ['65-cpp.26.8 登入畫面優化','auth-frame','auth-showcase','auth-version-badge','togglePassword','authSubmitSpinner']),
           '65-cpp.26.8 登入頁 responsive／密碼顯示／loading UI 完整', '65-cpp.26.8 登入頁 UI 元件缺失')
    expect(all(x in html for x in ['function setAuthBusy','function setAuthMode','rpg_last_username','aria-selected','autocomplete=registering']),
           '65-cpp.26.8 登入模式／送出狀態／帳號記憶接線完整', '65-cpp.26.8 登入互動接線缺失')

    # 65-cpp.26.9 shop UX + safe DM catalog editing regression.
    expect(all(x in html for x in ['65-cpp.26.9 商店 UX','shop-view-tabs','shop-toolbar-shell','shopProductGrid','shopSellGrid','shopHistoryHTML','adminShopStatusFilter','edit-shop-item']),
           '65-cpp.26.9 玩家購買／出售／紀錄與 DM 商品目錄 UI 完整', '65-cpp.26.9 商店工作區 UI 缺失')
    expect('id="shopRank"' in html and 'adminShopEditId' in html and 'toggle-shop-item' in html and "state.shopTransactions=tx.transactions||[]" in html,
           '65-cpp.26.9 DM 商品完整編輯／上下架與玩家交易紀錄接線完整', '65-cpp.26.9 商品編輯或交易紀錄接線缺失')
    expect('65-cpp.26.9 商店管理安全更新' in legacy and 'normalizeShopItemWrite(body, false)' in legacy and
           'if (body.isMember("rank")) body["rank"] = normalizeContentRank' in legacy,
           '65-cpp.26.9 C++ 商店 PATCH 不再重設未送出的品階', '商店部分 PATCH 仍可能重設商品品階')
    expect('shop partial PATCH preserves rank' in smoke and 'partial shop PATCH reset rank' in smoke,
           '65-cpp.26.9 Render smoke 覆蓋商店部分 PATCH 品階保留', '65-cpp.26.9 商店 PATCH 尚未納入 integration smoke')

    # 65-cpp.26.10 player character / inventory / skill workspace regression.
    expect(all(x in html for x in ['65-cpp.26.10 PLAYER CHARACTER / INVENTORY / SKILL WORKSPACE','character-workspace-tabs','character-status-strip','character-pane-anchor','characterInventorySearch','characterInventoryCategory','inspect-inventory-item','characterSkillSearch','skill-filter-pill','skill-favorite','gearLoadoutPreview','attribute-stepper','char-status']),
           '65-cpp.26.10 角色卡／背包／技能工作區 UI 完整', '65-cpp.26.10 玩家工作區 UI 缺失')
    expect(all(x in html for x in ['setCharacterPaneUI','applyCharacterInventoryFiltersUI','applyCharacterSkillFiltersUI','toggleCharacterSkillFavoriteUI','gearLoadoutDraftPreviewHTML','attr-step','rpg_character_pane','rpg_skill_favorites']),
           '65-cpp.26.10 手機分頁／篩選／收藏／裝備預覽／屬性步進接線完整', '65-cpp.26.10 玩家工作區互動接線缺失')

    # 65-cpp.26.4 multi-portrait + combat UI regression.
    expect(all(x in migrations for x in ['65-cpp.26.4：多立繪／表情立繪','CREATE TABLE IF NOT EXISTS character_portrait_variants','CREATE TABLE IF NOT EXISTS npc_portrait_variants','npc_templates ADD COLUMN IF NOT EXISTS portrait_url']),
           '65-cpp.26.4 多立繪 migration 完整', '65-cpp.26.4 多立繪 migration 缺失')
    npc_create_pos=core.find('CREATE TABLE IF NOT EXISTS npc_templates')
    npc_portrait_pos=core.find('CREATE TABLE IF NOT EXISTS npc_portrait_variants')
    expect(npc_create_pos>=0 and npc_portrait_pos>npc_create_pos,
           '65-cpp.26.4 全新資料庫先建 NPC 再建 NPC 多立繪', '65-cpp.26.4 NPC 多立繪 migration 順序可能導致全新資料庫啟動失敗')
    expect(all(x in world for x in ['/api/me/portraits','/api/character-portrait-variants/{1}/image','/api/admin/npc-templates/{1}/portraits','/api/admin/npc-portrait-variants/{1}','refreshCharacterPortraitSelection','refreshNpcPortraitSelection']),
           '65-cpp.26.4 玩家／NPC 多立繪 C++ API 完整', '65-cpp.26.4 多立繪 C++ API 缺失')
    expect('related["portrait_variants"]' in legacy and '65-cpp.26.4 多立繪備份還原' in legacy and '"character_portrait_variants","npc_portrait_variants"' in legacy and 'nt.portrait_url' in legacy and "'portrait_url',COALESCE(to_jsonb(nt)->>'portrait_url','')" in core,
           '65-cpp.26.4 多立繪備份／房間快照完整', '65-cpp.26.4 多立繪備份或房間快照缺失')
    expect(all(x in html for x in ['openPortraitLibrary','portraitVariantCardHTML','manage-my-portraits','manage-npc-portraits','portrait-library-grid','65-cpp.26.4 多立繪']),
           '65-cpp.26.4 多立繪前端管理完整', '65-cpp.26.4 多立繪前端管理缺失')
    expect(all(x in html for x in ['combatDashboardHTML','combatUnitCardHTML','combat-order-strip','combat-target-card','combat-mobile-dock','combat-mobile-basic','combat-mobile-pass','revive-combat-target']),
           '65-cpp.26.4 戰鬥卡片／手機快捷 UI 完整', '65-cpp.26.4 戰鬥 UI 缺失')
    expect('multi portrait player and NPC lifecycle' in smoke and '/api/me/portraits' in smoke and '/api/admin/npc-portrait-variants/' in smoke,
           '65-cpp.26.4 Render smoke 覆蓋玩家／NPC 多立繪', '65-cpp.26.4 多立繪尚未納入 integration smoke')
    # 64-cpp.25.3 simplified ritual editor regression.
    expect('64-cpp.25.3 儀式編輯器簡易模式' in html and 'ritualAdminFieldsHTML' in html and '⚙️ 進階 JSON' in html,
           '64-cpp.25.3 儀式編輯器預設簡易模式', '儀式編輯器仍直接暴露大量 JSON')
    expect('ritualSimpleMaterialsFromText' in html and 'ritualSimpleSlotsFromText' in html and 'ritualSimpleStepsFromText' in html and 'ElementRequirements' in html,
           '64-cpp.25.3 材料／佈置／元素／步驟可用簡易欄位設定', '儀式簡易欄位轉換缺失')
    expect("const rawMode=Boolean($(`#${prefix}AdvancedJson`)?.open)" in html and 'syncRitualSimpleToJson(prefix)' in html,
           '64-cpp.25.3 簡易／進階 JSON 儲存模式可共存', '儀式簡易模式可能覆蓋進階資料')
    # 65-cpp.26 complete magic-study regression.
    expect(all(x in migrations for x in [
        '65-cpp.26：魔法學完整系統',
        'magic_studies ADD COLUMN IF NOT EXISTS element_cost',
        'magic_studies ADD COLUMN IF NOT EXISTS linked_skill_template_id',
        'magic_studies ADD COLUMN IF NOT EXISTS required_book_item',
        'character_cards ADD COLUMN IF NOT EXISTS element_resistance',
        'character_cards ADD COLUMN IF NOT EXISTS element_weakness',
        'monster_templates ADD COLUMN IF NOT EXISTS element_resistance',
        'npc_templates ADD COLUMN IF NOT EXISTS element_weakness',
    ]), '65-cpp.26 魔法學 migration 完整', '65-cpp.26 魔法學 migration 缺失')
    expect('[功能備註｜65-cpp.26 魔法學完整系統]' in legacy and
           '[功能備註｜65-cpp.26 魔法書學習]' in legacy and
           '/api/magic-studies/{1}/cast' in legacy and
           'magicElementCostStatus' in legacy and 'spendMagicElements' in legacy and
           'executeConfiguredCombatSkill' in legacy,
           '65-cpp.26 C++ 魔法學習／元素成本／施放核心完整', '65-cpp.26 C++ 魔法施放接線缺失')
    expect('[功能備註｜65-cpp.26 元素親和／抗性／弱點]' in core and
           'elementalDamageModifier' in core and 'element_resistance' in core and 'element_weakness' in core,
           '65-cpp.26 元素親和／抗性／弱點已接傷害核心', '65-cpp.26 元素傷害核心缺失')
    expect('[前端功能備註｜65-cpp.26 魔法施放]' in html and 'cast-magic-study' in html and
           'ElementCost' in html and 'LinkedSkill' in html and 'ConsumeBook' in html and
           'required_book_item' in html,
           '65-cpp.26 魔法技能樹／DM 簡易管理前端完整', '65-cpp.26 魔法前端接線缺失')
    expect('魔法學系統總覽｜65-cpp.26' in read('docs/MAGIC_SYSTEM_OVERVIEW.md') and
           '100% + 攻擊者對該元素親和 - 目標對該元素抗性 + 目標對該元素弱點' in read('docs/MAGIC_SYSTEM_OVERVIEW.md'),
           '65-cpp.26 魔法學系統文件完整', '65-cpp.26 魔法學文件缺失')

    # 64-cpp.25.1 Render C++20 compile hotfix regression.
    legacy_cpp = read('src/LegacyCompat.cpp')
    expect('const bool requiresResearch=' in legacy_cpp and 'const bool requires=' not in legacy_cpp,
           '64-cpp.25.1 C++20 requires 保留字衝突已修正', 'LegacyCompat.cpp 仍把 C++20 requires 當變數名')
    expect('bool hasNode = false;' not in legacy_cpp,
           '64-cpp.25.1 地圖連線未使用變數 warning 已清除', 'LegacyCompat.cpp 仍保留 hasNode 未使用變數')

    # 65-cpp.26.11 task / NPC / notification center regression.
    expect('taskStatusUI' in html and 'participant_status' in html and 'trackedTaskId' in html and 'track-task' in html,
           '65-cpp.26.11 任務狀態／追蹤 UI 完整', '65-cpp.26.11 任務狀態或追蹤 UI 缺失')
    expect('尚未完成必要的前置任務' in legacy and '勢力聲望不足' in legacy and 'exclusive_group' in legacy,
           '65-cpp.26.11 C++ 任務接取條件完整', '65-cpp.26.11 任務前置／聲望／互斥驗證缺失')
    expect('complete-task-user' in html and 'participants' in legacy and 'task-participant-chip' in html,
           '65-cpp.26.11 DM 任務參與者流程完整', '65-cpp.26.11 DM 任務參與者流程缺失')
    expect('adminNpcFilterForm' in html and 'duplicate-npc' in html and '/api/admin/npc-templates/{1}/clone' in legacy,
           '65-cpp.26.11 NPC 篩選／複製完整', '65-cpp.26.11 NPC 管理 2.0 缺失')
    expect('/api/notifications/read-all' in legacy and 'readAllNotifications' in html and 'notification-filter' in html and "'task','已接受任務'" in legacy,
           '65-cpp.26.11 通知中心／任務自動通知完整', '65-cpp.26.11 通知中心 2.0 缺失')
    expect('const tasks=(state.myTasks||[]).filter(taskIsActiveUI);' in html and 'if(tracked && !taskIsActiveUI(tracked)) setTrackedTaskUI(0);' in html,
           '65-cpp.26.11 任務追蹤只保留進行中任務', '65-cpp.26.11 已完成任務仍可能留在效率中心追蹤')
    expect('imageUrl.rfind("/api/npc-avatar-images/", 0) == 0 ? "" : imageUrl' in legacy and
           'portraitUrl.rfind("/api/npc-portrait-variants/", 0) == 0 ? "" : portraitUrl' in legacy,
           '65-cpp.26.11 NPC 複製不共用原 NPC 內部圖片記錄', '65-cpp.26.11 NPC 複製可能錯誤引用原 NPC 內部圖片')

    # 65-cpp.26.12 combat UX / connection stability regression.
    native=read('public/native-socket.js')
    expect('connectionPill' in html and 'probeBackendHealth' in html and "setConnectionStatus('waking'" in html and '/api/health' in html,
           '65-cpp.26.12 連線狀態與 Render 冷啟動提示完整', '65-cpp.26.12 連線狀態 UI 或健康檢查缺失')
    expect('body["ready"] = true' in core and 'body["retry_after_ms"] = 2500' in core and 'waiting_for_database' in core,
           '65-cpp.26.12 C++ 健康檢查提供就緒／重試狀態', '65-cpp.26.12 C++ 健康檢查缺少冷啟動狀態')
    expect("state.socket.on('reconnecting'" in html and "this.dispatch('reconnecting'" in native and 'reconnect(){' in native and "this.dispatch('disconnect'" in native,
           '65-cpp.26.12 WebSocket 自動重連狀態完整', '65-cpp.26.12 WebSocket 重連事件缺失')
    expect('retryable=method===\'GET\'' in html and 'AbortController' in html and '[502,503,504]' in html,
           '65-cpp.26.12 GET 逾時／重試保護完整', '65-cpp.26.12 API 韌性處理缺失')
    expect('updateCombatFeedbackFromSnapshot' in html and 'combatSnapshotUnits' in html and 'combat-feedback-layer' in html and 'combat-live-feed' in html,
           '65-cpp.26.12 C++ 快照驅動戰鬥即時回饋完整', '65-cpp.26.12 戰鬥傷害／治療／狀態回饋缺失')
    expect('clear-combat-target' in html and "state.combatSelectedTarget=String(state.combatSelectedTarget||'')===key?'':key" in html and 'combatEventTime' in html,
           '65-cpp.26.12 目標取消／戰鬥紀錄時間完整', '65-cpp.26.12 戰鬥目標或紀錄 UX 缺失')

    # 65-cpp.26.13 global interface optimization regression.
    expect('[前端功能備註｜65-cpp.26.13 全介面優化]' in html and '--border:var(--line)' in html and '--topbar-height:64px' in html,
           '65-cpp.26.13 全站設計 token／舊 border 相容完整', '65-cpp.26.13 全站 UI token 或 border fallback 缺失')
    expect('.sidebar{' in html and 'position:sticky' in html and '.nav button.active::before' in html and 'max-width:1760px' in html,
           '65-cpp.26.13 桌面導覽／內容工作區完整', '65-cpp.26.13 桌面導覽或內容寬度優化缺失')
    expect('@media(max-width:760px)' in html and '至少 44px 觸控面積' in html and '.mobile-more-sheet{' in html and '.modal-card{' in html,
           '65-cpp.26.13 手機觸控／更多選單／Modal 完整', '65-cpp.26.13 手機全域介面優化缺失')
    expect('.character-workspace-tabs{' in html and '.shop-card:hover' in html and '.planar-map{' in html and '.combat-turn-banner{' in html,
           '65-cpp.26.13 角色／商店／地圖／戰鬥介面一致化完整', '65-cpp.26.13 核心玩家介面樣式缺失')
    expect('.admin-overview-bar{' in html and '.admin-sidebar{' in html and '.notification-row.unread' in html and '.quest-tracker{' in html,
           '65-cpp.26.13 DM／通知／任務介面一致化完整', '65-cpp.26.13 DM 或任務通知介面樣式缺失')
    expect('@media(prefers-reduced-motion:reduce)' in html and 'table{display:block;max-width:100%;overflow-x:auto' in html,
           '65-cpp.26.13 無障礙動態／手機表格溢出保護完整', '65-cpp.26.13 reduced-motion 或 responsive table 缺失')

    # 65-cpp.26.14 growable equipment regression.
    gear=read('src/GearAffixSystem.cpp'); gear_h=read('include/trpg/GearAffixSystem.h')
    expect('[功能備註｜65-cpp.26.14 可成長性裝備]' in gear and 'grantGearGrowthXp' in gear_h and 'growth_effective_rank' in gear and 'growth_affix_bonus' in gear,
           '65-cpp.26.14 C++ 裝備成長等級／品階／詞條容量核心完整', '65-cpp.26.14 裝備成長核心缺失')
    expect('/api/admin/players/{1}/gear-growth' in gear and 'grantHeldGearGrowthXpByIndex' in gear and 'current_affixes=$4::jsonb' in gear,
           '65-cpp.26.14 DM 成長 XP／新詞條槽成長完整', '65-cpp.26.14 DM XP 或成長詞條槽缺失')
    expect('gear_damage_multiplier' in core and 'equipment_granted_skill_template_ids' in core and 'growthTargetId' in legacy and 'skillId.rfind("gear_", 0)' in legacy,
           '65-cpp.26.14 裝備授予技能與 C++ 成長傷害完整', '65-cpp.26.14 裝備技能成長戰鬥接線缺失')
    expect('[前端功能備註｜65-cpp.26.14 可成長性裝備]' in html and 'gearGrowthPanelHTML' in html and 'edit_equipment_growth_enabled' in html and 'edit_weapon_growth_enabled' in html and 'grant-gear-growth' in html,
           '65-cpp.26.14 玩家／DM 可成長裝備介面完整', '65-cpp.26.14 成長裝備前端缺失')


    # 65-cpp.26.15 repeated editing / ranked crafting / recipe discovery regression.
    expect(all(x in migrations for x in ['65-cpp.26.15：可重複修改核心資料 + 品階合成成功率 + 試驗發現配方','CREATE TABLE IF NOT EXISTS character_recipe_discoveries','CREATE TABLE IF NOT EXISTS recipe_experiment_attempts']),
           '65-cpp.26.15 配方發現／試驗 migration 完整', '65-cpp.26.15 配方研究 migration 缺失')
    expect('defaultRecipeRankSuccessRate' in legacy and '{"G",100}' in legacy and '{"SSS",25}' in legacy and 'recipeBaseSuccessRate' in legacy and 'characterCraftSuccessBonusCpp' in legacy,
           '65-cpp.26.15 C++ 品階成功率／製作技能加成完整', '65-cpp.26.15 合成成功率核心缺失')
    expect('craftedOutput["rank"] = normalizeContentRank(stringValue(recipe["rank"], "G"))' in legacy and 'craftedOutput["rank"] = normalizeContentRank(stringValue(matched["rank"], "G"))' in legacy and '配方／成品品階' in html,
           '65-cpp.26.15 合成成品預設繼承配方品階', '65-cpp.26.15 合成成品品階接線缺失')
    expect('/api/recipes/experiment' in legacy and 'recipeMaterialSignature' in legacy and 'recordRecipeExperiment' in legacy and 'hidden_until_discovered' in legacy and 'recipeIsDiscovered' in legacy,
           '65-cpp.26.15 未知配方試驗／隱藏解鎖 C++ 完整', '65-cpp.26.15 配方試驗或發現流程缺失')
    expect('/api/admin/recipes/{1}' in legacy and '修改合成配方失敗' in legacy and '/api/admin/item-templates/{1}' in legacy and '修改道具／裝備模板失敗' in legacy,
           '65-cpp.26.15 配方／道具模板 PATCH 可重複修改', '65-cpp.26.15 配方或道具 PATCH 缺失')
    expect('adminRecipeEditId' in html and 'edit-recipe' in html and 'adminItemTemplateEditId' in html and 'load-item-template' in html and '可反覆儲存同一筆資料' in html,
           '65-cpp.26.15 配方／道具重複修改 UI 完整', '65-cpp.26.15 配方或道具修改 UI 缺失')
    expect('recipeExperimentForm' in html and 'experiment-material-row' in html and "api('/api/recipes/experiment'" in html and 'RECIPE_RANK_SUCCESS' in html,
           '65-cpp.26.15 玩家未知配方試驗／品階成功率 UI 完整', '65-cpp.26.15 玩家配方試驗 UI 缺失')
    expect('populateGearForm' in html and 'edit-inventory' in html and 'gear_instance_id' in html and 'growth_total_xp' in html,
           '65-cpp.26.15 已持有武器／裝備可反覆修改並保留成長身份', '65-cpp.26.15 裝備重複修改或成長保留缺失')
    expect('/api/admin/skill-templates/{1}' in legacy and 'edit-skill-template' in html and "method:'PATCH'" in html,
           '65-cpp.26.15 技能模板維持可重複修改', '65-cpp.26.15 技能模板修改流程缺失')
    expect('repeated content edits and experimental recipe discovery' in smoke and '/api/recipes/experiment' in smoke and 'hidden experimental recipe leaked before discovery' in smoke,
           '65-cpp.26.15 Render smoke 覆蓋重複修改／配方試驗', '65-cpp.26.15 尚未納入 integration smoke')
    expect('related["recipe_discoveries"]' in legacy and '{"recipe_discoveries","character_recipe_discoveries"}' in legacy and '"character_recipe_discoveries","recipe_experiment_attempts"' in legacy,
           '65-cpp.26.15 配方發現進度納入角色／全站備份', '65-cpp.26.15 配方發現備份缺失')
    expect('可重複修改與配方研究總覽｜65-cpp.26.15' in read('docs/CRAFT_DISCOVERY_OVERVIEW_v65_cpp26_15.md') and 'G | F | E | D | C | B | A | S | SS | SSS' in read('docs/CRAFT_DISCOVERY_OVERVIEW_v65_cpp26_15.md'),
           '65-cpp.26.15 配方研究文件完整', '65-cpp.26.15 配方研究文件缺失')


    expect('universalDerivedStats' in html and '體質 × 2' in html, '73-cpp.34.25.10 全域衍生屬性 UI 存在', '全域衍生屬性 UI 缺失')
    expect(all(x in html for x in ['monsterUseEndurance','monsterUseWill','monsterUseSanity']), '73-cpp.34.25.10 怪物可個別取消耐力／意志／理智', '怪物特殊屬性取消開關缺失')
    expect('const auto maxHp = constitution * 2' in core and 'derived_stats' in core, '73-cpp.34.25.10 怪物生命與衍生值由 C++ 強制計算', '怪物衍生值後端規則缺失')
    expect('body[\"max_hp\"]=Json::Int64(con*2)' in legacy and '修改 NPC 失敗' in legacy, '73-cpp.34.25.10 NPC 生命與衍生值由 C++ 強制計算', 'NPC 衍生值後端規則缺失')
    expect('dmEnhanceCreationPanels' in html and 'dmApplyEasyMode' in html and 'dm-easy-mode' in html, '73-cpp.34.25.10 DM 技能／配方／裝備簡易模式存在', 'DM 建立面板簡易模式缺失')
    expect(all(x in html for x in ['data-skill-preset=\"attack\"','data-eq-preset=\"growth\"','顯示進階設定']), '73-cpp.34.25.10 快速模板與進階切換存在', 'DM 快速模板或進階切換缺失')
    expect(all(x in migrations for x in ['CREATE TABLE IF NOT EXISTS admin_content_versions','CREATE TABLE IF NOT EXISTS admin_content_drafts']), '73-cpp.34.25.10 DM 版本歷史／草稿 migration 完整', 'DM 版本歷史或草稿 migration 缺失')
    expect(all(x in legacy for x in ['/api/admin/content/bulk-update','/api/admin/content/{1}/{2}/versions','/api/admin/content/{1}/{2}/dependencies','/api/admin/content-drafts/{1}']), '73-cpp.34.25.10 C++ 批量／版本／依賴／草稿 API 完整', 'DM 內容工作流 C++ API 缺失')
    expect(all(x in html for x in ['dmBulkToolbarHTML','dmBindServerDraft','dmOpenVersions','dmOpenDependencies','dm-content-history','dm-bulk-select']), '73-cpp.34.25.10 DM 內容工作流前端完整', 'DM 批量／草稿／版本／依賴 UI 缺失')
    expect(all(x in html for x in ['dmAdminStartPanelHTML','dmOpenCommandPalette','DM_FAVORITES_KEY','dm-mobile-dock']), '73-cpp.34.25.10 DM 常用／最近／快捷指令／手機快捷列完整', 'DM 編輯器快捷工作區缺失')
    expect(all(x in html for x in ['dmMakeSortable','dm-sort-up','dm-sort-down','#recipeMaterialList']), '73-cpp.34.25.10 技能／效果／配方可排序編輯器存在', 'DM 編輯器拖曳／上下排序缺失')

    expect(all(x in html for x in ['dmContentHealthPanelHTML','dmContentHealthIssues','dm-balance-inline','同品階屬性明顯離群']), '73-cpp.34.25.10 DM 內容健康與平衡檢查器存在', 'DM 內容健康或平衡檢查器缺失')
    expect(all(x in html for x in ['dmSmartCreateCommand','dmSmartSearchCommand','建立 怪物 名稱']), '73-cpp.34.25.10 DM 智慧指令可建立／搜尋內容', 'DM 智慧指令缺失')
    expect('/api/admin/editor-dashboard' in legacy and 'COUNT(DISTINCT rm.user_id)' in legacy and 'recent_versions' in legacy and 'recent_drafts' in legacy, '73-cpp.34.25.10 C++ DM 工作儀表板 API 完整', 'DM 工作儀表板 API 缺失')
    expect(all(x in html for x in ['dmEditorDashboardHTML','refreshDmDashboard','dm-dashboard-grid','state.dmEditorDashboard']), '73-cpp.34.25.10 DM 工作儀表板 UI 完整', 'DM 工作儀表板 UI 缺失')
    expect(all(x in html for x in ['bulk-preview-table','bulkImportValidOnlyBtn','只匯入有效資料']), '73-cpp.34.25.10 批量匯入逐筆預覽／有效資料匯入完整', '批量匯入逐筆預覽或部分匯入缺失')
    expect('CREATE TABLE IF NOT EXISTS admin_effect_templates' in migrations, '73-cpp.34.25.10 共用效果模板 migration 完整', '共用效果模板 migration 缺失')
    expect(all(x in legacy for x in ['/api/admin/effect-templates','/rename-sync','/safe-delete','/combat-center','/gm-command']), '73-cpp.34.25.10 C++ 效果模板／引用保護／戰鬥中心 API 完整', '34.20 C++ API 缺失')
    expect(all(x in html for x in ['dmEffectTemplatesHTML','dmCombatCenterHTML','dmRunGmCommand','dmSafeDelete','dmReferenceRename']), '73-cpp.34.25.10 DM 戰鬥中心／GM 指令／引用安全 UI 完整', '34.20 DM UI 缺失')
    expect(all(x in html for x in ['dmBossPhaseRowHTML','dmEnhanceBossBuilder','三階段 Boss 模板']), '73-cpp.34.25.10 Boss 視覺階段編輯器完整', 'Boss 視覺階段編輯器缺失')
    expect('CREATE TABLE IF NOT EXISTS admin_drop_pools' in migrations and '/api/admin/drop-pools' in legacy, '73-cpp.34.25.10 共用掉落池 C++／migration 完整', '共用掉落池後端或 migration 缺失')
    expect(all(x in html for x in ['dmDropPoolsHTML','dm-monster-drop-pool','drop_pool_ids']), '73-cpp.34.25.10 共用掉落池 DM／怪物引用 UI 完整', '共用掉落池前端引用缺失')
    expect(all(x in html for x in ['副本內容套組','dm-dungeon-monster','dm-dungeon-boss','packageCfg']), '73-cpp.34.25.10 副本快速內容套組完整', '副本內容套組缺失')
    expect(all(x in html for x in ['dmTaskFlowHTML','任務流程圖','dm-task-node']), '73-cpp.34.25.10 任務流程視覺總覽完整', '任務流程總覽缺失')
    expect(all(x in migrations for x in ['ALTER TABLE tasks ADD COLUMN IF NOT EXISTS task_steps','ALTER TABLE task_participants ADD COLUMN IF NOT EXISTS progress','CREATE TABLE IF NOT EXISTS room_task_anchors','CREATE TABLE IF NOT EXISTS task_step_actions']), '73-cpp.34.25.10 結構化任務步驟／錨點 migration 完整', '結構化任務步驟 migration 缺失')
    expect(all(x in legacy for x in ['normalizeTaskSteps','/api/rooms/{1}/tasks/{2}/steps/{3}/progress','cooperative_seal','place_anchor',"status='sealed'",'/api/rooms/{1}/task-anchors','/api/admin/tasks/{1}/step-progress']), '73-cpp.34.25.10 C++ NPC 協力封印／錨點／DM 判定完整', '任務步驟 C++ 執行核心缺失')
    expect(all(x in html for x in ['TASK_STEP_TYPES','taskStepRowHTML','taskStepBuilder','NPC 協力封印','安插錨點','progress-task-step','admin-task-step-manage']), '73-cpp.34.25.10 任務環節編輯／玩家進度／DM 校正 UI 完整', '任務環節前端缺失')
    expect('task-anchor-badge' in html and 'roomTaskAnchors' in html and '/task-anchors' in html, '73-cpp.34.25.10 任務錨點顯示於地圖節點', '任務錨點未接入地圖')
    expect('任務步驟尚未全部完成；DM 可選擇強制完成' in legacy and 'force:true' in html, '73-cpp.34.25.10 任務完成前步驟驗證／DM 強制完成完整', '任務步驟完成保護缺失')
    expect(all(x in migrations for x in ['task_variables JSONB','task_failure_conditions JSONB']), '73-cpp.34.25.10 任務變數／失敗條件 migration 完整', '任務變數或失敗條件 migration 缺失')
    expect(all(x in legacy for x in ['taskStepActive','taskFailureReason','applyTaskStepVariableChange','/api/admin/tasks/{1}/debug/{2}','/api/admin/tasks/{1}/variables']), '73-cpp.34.25.10 C++ 任務分支／失敗／變數／除錯器完整', '任務分支／失敗／變數／除錯器 C++ 缺失')
    expect(all(x in html for x in ['taskVariables','taskFailureConditions','task-step-activation-var','task-step-change-var','admin-task-debug','openAdminTaskDebugger']), '73-cpp.34.25.10 任務分支／失敗／變數／除錯 UI 完整', '任務分支／失敗／變數／除錯 UI 缺失')

    expect(all(x in legacy for x in ['advanceStructuredTaskStepsForEvent','taskStepDependenciesComplete','required_anchor_count','BOSS_PHASE_CHANGED']), '73-cpp.34.25.10 任務事件自動推進／前置依賴 C++ 完整', '任務事件自動推進或前置依賴缺失')
    expect('advanceStructuredTaskStepsForEvent(service, roomId, actorUserId, eventType, eventContext)' in read('src/RuleEngine.cpp'), '73-cpp.34.25.10 規則引擎事件自動接入任務', '規則事件未接入任務自動推進')
    expect(all(x in html for x in ['Boss 階段','規則事件名稱','封印前需要錨點數','由遊戲事件自動判定','task-step-auto-progress']), '73-cpp.34.25.10 任務自動事件／Boss／封印前置 UI 完整', '任務自動事件或封印前置 UI 缺失')
    expect("status='success' ORDER BY completed_at DESC LIMIT 1" in legacy and 'ev["ritual_id"]=inst["ritual_id"]' in legacy, '73-cpp.34.25.10 儀式成功任務判定修正完整', '儀式成功任務判定仍有缺口')

    expect(all(x in html for x in ['prewarmAuthServer','schedulePostLoginBootstrap','authProgress','postLoginBootstrapGeneration']), '73-cpp.34.25.10 登入預熱／錯峰 bootstrap 完整', '登入預熱或錯峰載入缺失')
    expect('不再每次登入額外 INSERT ... ON CONFLICT' in core and '/api/auth/login' in core, '73-cpp.34.25.10 C++ 登入熱路徑移除角色卡寫入', '登入熱路徑仍有不必要 DB 寫入')
    expect('requestAnimationFrame(()=>setTimeout' in html and 'connectSocket();' in html, '73-cpp.34.25.10 首屏先 render 再啟動即時連線', '登入首屏與 WebSocket 啟動順序未優化')
    expect('setTimeout(()=>controller.abort(),75000)' in html and 'for(let loginTry=0;loginTry<2;loginTry++)' in html, '73-cpp.34.25.10 登入冷啟動長等待／網路重試完整', '登入仍可能過早逾時或缺少重試')
    expect('不要在使用者按登入的同時再額外打一支 health' in html and 'Promise.race([authWarmPromise,sleep(1500)])' in html, '73-cpp.34.25.10 登入避免 health/login 冷啟動競爭', '登入仍可能與 health 預熱競爭')

    print(f'PASS={len(PASS)} FAIL={len(FAIL)} WARN={len(WARN)}')
    for x in PASS: print('PASS',x)
    for x in FAIL: print('FAIL',x)
    for x in WARN: print('WARN',x)
    return 1 if FAIL else 0

if __name__=='__main__':
    raise SystemExit(main())
