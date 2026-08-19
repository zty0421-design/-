#!/usr/bin/env python3
from __future__ import annotations
import collections, json, os, re, shutil, subprocess, sys, tempfile, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PASS=[]; FAIL=[]; WARN=[]
def ok(x): PASS.append(x)
def bad(x): FAIL.append(x)
def expect(c,a,b): ok(a) if c else bad(b)
def read(n): return (ROOT/n).read_text(encoding='utf-8', errors='ignore')
def run(cmd,cwd=ROOT): return subprocess.run(cmd,cwd=cwd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)

def helper_call_arity_errors(source: str):
    """Lightweight C++ call scanner for fixed-arity selectRows helpers.

    This intentionally ignores C++ types and only balances parentheses/quotes; it is
    enough to catch the exact Render failure where selectRows1 was called with two
    SQL bind parameters.
    """
    expected={"selectRows":2,"selectRows1":3,"selectRows2":4}
    errors=[]
    for name, argc_expected in expected.items():
        needle=name+"("
        pos=0
        while True:
            start=source.find(needle,pos)
            if start<0: break
            i=start+len(needle); depth=1; quote=None; escaped=False
            while i<len(source) and depth:
                ch=source[i]
                if quote is not None:
                    if escaped: escaped=False
                    elif ch=="\\": escaped=True
                    elif ch==quote: quote=None
                else:
                    if ch in ("\"", "'"): quote=ch
                    elif ch=="(": depth+=1
                    elif ch==")": depth-=1
                i+=1
            body=source[start+len(needle):i-1]
            level=0; quote=None; escaped=False; commas=0
            for ch in body:
                if quote is not None:
                    if escaped: escaped=False
                    elif ch=="\\": escaped=True
                    elif ch==quote: quote=None
                else:
                    if ch in ("\"", "'"): quote=ch
                    elif ch in "([{": level+=1
                    elif ch in ")]}": level=max(0,level-1)
                    elif ch=="," and level==0: commas+=1
            argc=0 if not body.strip() else commas+1
            if argc!=argc_expected:
                line=source.count("\n",0,start)+1
                errors.append(f"{name} line {line}: expected {argc_expected}, got {argc}")
            pos=max(i,start+len(needle))
    return errors

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
    world=read('WorldSystems.cpp')
    for m in re.finditer(r'\breg[0-2]\(\s*"([^"]+)"\s*,\s*drogon::(Get|Post|Put|Patch|Delete)',world):
        out.append(norm(m.group(2),m.group(1)))
    engine=read('RuleEngine.cpp')
    for m in re.finditer(r'\breg[01]\(\s*"([^"]+)"\s*,\s*drogon::(Get|Post|Put|Patch|Delete)',engine):
        out.append(norm(m.group(2),m.group(1)))
    bridge=read('RuleCombatBridge.cpp')
    for part in bridge.split('drogon::app().registerHandler(')[1:]:
        p=re.search(r'^\s*"([^"]+)"',part)
        m=re.search(r'\{drogon::(Get|Post|Put|Patch|Delete)\}\s*\)',part,re.S)
        if p and m: out.append(norm(m.group(1),p.group(1)))
    for m in re.finditer(r'registerAdminCrud0\(service\s*,\s*"([^"]+)"',legacy):
        out += [norm('GET',m.group(1)),norm('POST',m.group(1))]
    for m in re.finditer(r'registerAdminPatchDelete1\(service\s*,\s*"([^"]+)"',legacy):
        out += [norm('PATCH',m.group(1)),norm('DELETE',m.group(1))]
    return out

def main():
    required=['CMakeLists.txt','Dockerfile','render.yaml','.env.example','VERSION.txt','api_parity.json',
      'main.cpp','Config.cpp','Security.cpp','CoreService.cpp','LegacyCompat.cpp','RoomSocket.cpp','RuleEngine.cpp','RuleCombatBridge.cpp',
      'Config.h','Security.h','CoreService.h','LegacyCompat.h','LegacyRouteManifest.h','RoomSocket.h','RuleEngine.h','RuleCombatBridge.h',
      'security_test.cpp','legacy_v39_migrations.sql','index.html','native-socket.js','sw.js','manifest.webmanifest',
      'favicon-32.png','apple-touch-icon.png','icon-192.png','icon-512.png','integration_smoke.py','GITHUB_RENDER_UPLOAD.txt','FEATURE_MAP.md','ANNOTATION_BUILD.txt']
    miss=[x for x in required if not (ROOT/x).is_file()]
    expect(not miss,'平鋪部署必要檔案完整','缺少：'+', '.join(miss))
    expect(ROOT.name=='rpg_web_global_mvp','ZIP 根資料夾名稱正確','根資料夾不是 rpg_web_global_mvp')
    dirs=[p.name for p in ROOT.iterdir() if p.is_dir()]
    expect(not dirs,'GitHub 上傳內容為真正根目錄平鋪','仍有子資料夾：'+', '.join(dirs))

    version=read('VERSION.txt').strip()
    expect(version=='52-cpp.13' and 'kVersion = "52-cpp.13"' in read('CoreService.h') and
           'C++ 相容版 52-cpp.13' in read('index.html') and 'version": "52-cpp.13"' in read('api_parity.json'),
           '52-cpp.13 版本資訊在後端/前端/api_parity 一致',
           '版本資訊不同步')

    docker=read('Dockerfile'); cmake=read('CMakeLists.txt'); render=read('render.yaml'); legacy=read('LegacyCompat.cpp')
    core_h=read('CoreService.h'); core_cpp=read('CoreService.cpp'); security_cpp=read('Security.cpp'); config_cpp=read('Config.cpp')
    helper_arity_errors=helper_call_arity_errors(legacy)
    expect(not helper_arity_errors,
           'SQL selectRows helper 參數數量一致（避免 Render too many arguments）',
           'SQL helper 參數數量錯誤：'+ '; '.join(helper_arity_errors))
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
    expect('v52-cpp-13-avatar-upload-1' in sw_js,
           'Service Worker cache 已換版，避免舊預覽頁殘留',
           'Service Worker cache 尚未換版')


    # Shop category system regression checks.
    migrations=read('legacy_v39_migrations.sql')
    expect('CREATE TABLE IF NOT EXISTS shop_categories' in migrations and
           'category_id BIGINT REFERENCES shop_categories' in migrations and
           'idx_shop_items_category_active' in migrations,
           '商店分類 PostgreSQL migration 完整',
           '商店分類資料表／category_id／索引 migration 不完整')
    expect('/api/admin/shop/categories' in legacy and 'shopCategoryList' in legacy and
           'syncShopItemCategory' in legacy and 'shopItemList' in legacy,
           '商店分類 C++ API／驗證／排序查詢存在',
           '商店分類 C++ 後端不完整')
    expect('shopCategoryButtonsHTML' in index_html and 'applyShopFilters' in index_html and
           'save-shop-item-category' in index_html and 'shopCategoryForm' in index_html,
           '商店分類／搜尋／商品改分類前端完整',
           '商店分類前端控制缺失')

    # 52-cpp.13 DM avatar management regression checks.
    world_cpp=read('WorldSystems.cpp')
    expect('/api/admin/players/{1}/avatar' in world_cpp and
           'requireUser(service,req,true)' in world_cpp and
           'DM 修改玩家頭像' in world_cpp and
           'UPDATE character_cards SET avatar_url=$1' in world_cpp,
           'DM 修改其他玩家頭像由 C++ 權限與資料庫更新保護',
           'DM 玩家頭像 C++ API／權限檢查不完整')
    expect('openAdminAvatarEditor' in index_html and 'class="btn secondary edit-avatar"' in index_html and
           'id="edit_avatar_url"' in index_html and 'id="edit_avatar_file"' in index_html and
           'adminAvatarFile' in index_html and 'avatar/upload' in index_html,
           'DM 玩家列表快捷頭像按鈕、URL 與直接上傳入口存在',
           'DM 玩家頭像前端入口缺失')
    expect('/api/admin/players/{player_id_for_study}/avatar' not in read('integration_smoke.py') and
           '/api/admin/players/{me[\'user\'][\'id\']}/avatar' in read('integration_smoke.py'),
           'PostgreSQL smoke test 會驗證 DM 修改其他玩家頭像後玩家可讀回',
           'DM 玩家頭像 integration smoke 缺失')

    # 46-cpp.7 productivity suite regression checks (features 1-14 + bulk import).
    efficiency_tables=['content_tags','entity_tags','user_favorites','user_notifications','codex_entries','codex_unlocks','shop_transactions']
    expect(all(f'CREATE TABLE IF NOT EXISTS {t}' in migrations for t in efficiency_tables) and
           'custom_resources JSONB' in migrations and 'prerequisite_skill_ids JSONB' in migrations,
           '效率套件 PostgreSQL migration 完整（搜尋/收藏/通知/百科/資源/技能樹）',
           '效率套件 migration 缺表或欄位')
    expect(all(x in legacy for x in ['/api/search','/api/tags','/api/favorites','/api/notifications','/api/codex',
                                     '/api/character/custom-resources','/api/rooms/{1}/timeline','/api/admin/template-center']),
           '全站搜尋、標籤、收藏、通知、百科、自訂資源、時間線與模板中心 C++ API 完整',
           '效率套件主要 C++ API 缺失')
    expect('productivityToolsHTML' in index_html and 'runGlobalSearch' in index_html and
           'mobileCombatModeHTML' in index_html and 'itemEffectSummary' in index_html and
           '任務追蹤器' in index_html and '通知中心' in index_html,
           '效率中心前端含搜尋/收藏/通知/百科/任務追蹤/裝備比較/手機戰鬥',
           '效率中心前端功能不完整')
    expect('/api/shop/sell' in legacy and 'purchase_limit' in legacy and 'discount_percent' in legacy and
           'shop_transactions' in legacy and 'shopTransactions' in index_html,
           '進階商店含庫存、限購、折扣、出售與交易紀錄',
           '進階商店功能不完整')
    expect('prerequisite_skill_ids' in legacy and 'tree_group' in index_html and 'editSkillPrerequisites' in index_html,
           '技能樹前置條件由 C++ 驗證且 DM 可編輯',
           '技能樹／前置技能功能不完整')
    expect('processRoundStatuses' in core_cpp and 'effects["per_round"]' in core_cpp and
           'expires_by_round' in core_cpp and 'status_tick' in core_cpp,
           '狀態效果會在 C++ 新回合自動結算、倒數與到期',
           '狀態效果自動化缺失')
    expect('INSERT INTO user_notifications' in core_cpp and "'turn','輪到你行動'" in core_cpp,
           '戰鬥輪到玩家時由 C++ 自動產生通知',
           '輪到玩家通知自動化缺失')
    expect('/api/admin/bulk-import/preview' in legacy and '/api/admin/bulk-import/commit' in legacy and
           'rows.size()>5000' in legacy and 'kind!="magics"' in legacy and 'kind!="rituals"' in legacy and
           'parseBulkImportText' in index_html and 'bulkImportExample' in index_html and
           '<option value="magics">魔法學</option>' in index_html and '<option value="rituals">儀式學</option>' in index_html,
           '技能/道具/材料/魔法/儀式批量匯入支援預覽、驗證與最多 5000 筆',
           '批量匯入功能不完整')
    expect('setClientMaxBodySize(32U * 1024U * 1024U)' in main_cpp,
           '批量匯入 HTTP body 上限提升至 32 MB',
           'HTTP body 上限不足以支援大量匯入')
    expect(re.search(r'std::array<const char\*,\s*\d+>', legacy) is not None and all(t in legacy for t in efficiency_tables) and all(t in legacy for t in ['magic_studies','ritual_studies','character_magics','character_rituals']),
           '全站備份已包含效率系統及魔法／儀式資料表',
           '全站備份未覆蓋新增資料表')

    # 47-cpp.8 magic / ritual studies + per-element storage regression checks.
    study_tables=['magic_studies','ritual_studies','character_magics','character_rituals']
    expect(all(f'CREATE TABLE IF NOT EXISTS {t}' in migrations for t in study_tables) and
           'magic_points BIGINT NOT NULL DEFAULT 0' in migrations and
           'ritual_points BIGINT NOT NULL DEFAULT 0' in migrations and
           'element_storage JSONB NOT NULL DEFAULT' in migrations and
           'element_storage_base_caps JSONB NOT NULL DEFAULT' in migrations and
           migrations.count('{"暗":0,"光":0,"金":0,"木":0,"水":0,"火":0,"土":0}') >= 2,
           '魔法學／儀式學與七元素儲存 migration 完整，初始上限/儲存量皆為 0',
           '魔法學／儀式學或元素儲存 migration 不完整')
    expect(all(route in legacy for route in ['/api/magic-studies','/api/ritual-studies',
           '/api/admin/magic-studies','/api/admin/ritual-studies','/api/admin/players/{1}/study-points',
           '/api/admin/players/{1}/element-storage']) and 'character_magics' in legacy and 'character_rituals' in legacy,
           '魔法學／儀式學玩家學習、DM 資料庫與點數管理 C++ API 完整',
           '魔法學／儀式學 C++ API 不完整')
    expect('kStoredElements={"暗","光","金","木","水","火","土"}' in legacy and
           'element_storage_cap_bonus' in legacy and 'element_storage_permanent_bonus' in legacy and
           'element_storage_caps' in legacy and 'element_storage_cap_sources' in legacy and
           'element_storage_cap_bonus' in core_cpp,
           '七元素容量由 C++ 即時計算，支援技能/道具/魔法/儀式與永久擴容',
           '七元素容量 C++ 計算或來源加成缺失')
    expect('itElementStorageCap' in index_html and 'itElementStoragePermanent' in index_html and
           'stElementStorageCap' in index_html and 'editSkillElementStorageCap' in index_html and
           '可只填單一元素' in index_html,
           'DM 可設定技能/道具單一元素上限與永久單元素擴容道具',
           '技能/道具單一元素上限設定 UI 缺失')
    expect('adminStudyHTML' in index_html and 'data-admin-view="magicStudies"' in index_html and 'data-admin-view="ritualStudies"' in index_html and
           'magicStudiesHTML' in index_html and 'ritualStudiesHTML' in index_html and
           'ElementAdjust' in index_html,
           '魔法學／儀式學各有獨立玩家頁與 DM 頁，並可管理元素儲存',
           '魔法學／儀式學 DM/玩家頁或元素管理 UI 缺失')
    expect('taskRewardMagicPoints' in index_html and 'taskRewardRitualPoints' in index_html and
           'magic_points=magic_points+$4' in legacy and 'ritual_points=ritual_points+$5' in legacy,
           '任務可發放魔法學／儀式學點數',
           '任務學習點數獎勵未完整串接')
    expect("SELECT 'magic'" in legacy and "SELECT 'ritual'" in legacy and
           '"magics","magic_studies"' in legacy and '"rituals","ritual_studies"' in legacy,
           '全站搜尋與模板中心已納入魔法學／儀式學',
           '搜尋或模板中心未納入魔法／儀式')


    # 48-cpp.9 世界／通訊／離線 AI／元素自生回歸檢查（手寫需求 1-10）。
    world_cpp=read('WorldSystems.cpp')
    room_socket=read('RoomSocket.cpp')
    cmake_text=read('CMakeLists.txt')
    docker_text=read('Dockerfile')
    v48_migration_tokens=['rulebook_entries','avatar_url','merchant_enabled','communication_blocked',
                          'offline_ai_enabled','room_team_invites','element_generation_last_at','uq_room_team_invites_pending']
    expect(all(token in migrations for token in v48_migration_tokens),
           'v48 規則書／頭像／商人／通訊／離線 AI／元素自生 migration 完整',
           'v48 世界系統 migration 缺表、欄位或邀請唯一索引')
    required_world_routes=['/api/rulebook','/api/admin/rulebook','/api/me/avatar',
                           '/api/admin/rooms/{1}/map/generate','/api/rooms/{1}/location','/api/rooms/{1}/communication',
                           '/api/rooms/{1}/team-invites','/api/rooms/{1}/team-invites/{2}/respond',
                           '/api/admin/rooms/{1}/offline-ai','/api/admin/rooms/{1}/offline-ai/{2}',
                           '/api/admin/rooms/{1}/offline-ai/tick','/api/me/elements/generation',
                           '/api/admin/elements/generation/tick']
    expect(all(route in world_cpp for route in required_world_routes),
           'v48 規則書、位置、通訊、邀請、離線 AI、元素自生 C++ API 完整',
           'v48 WorldSystems C++ API 缺失')
    expect('communicationAudience' in world_cpp and 'communication_device' in world_cpp and
           'communication_blocked' in world_cpp and 'roomSnapshotForViewer' in core_cpp and
           'roomSnapshotForViewer' in room_socket and 'audience_user_ids' in room_socket,
           '非同場景通訊限制／通訊道具／禁訊區會由 C++ 過濾聊天',
           '跨場景通訊規則或聊天受眾過濾缺失')
    expect('offline_auto_accept' in world_cpp and 'offline_ai_join' in world_cpp and
           'follow_team' in world_cpp and 'explore_team' in world_cpp and 'offline_ai_explore' in world_cpp and
           'RoomSocket::isUserOnline' in world_cpp and 'startWorldAutomation' in world_cpp,
           '離線 AI 可自動入隊、跟隊長、離線隊長帶隊探索、單人探索並在上線時交回控制',
           '離線 AI 規則不完整')
    expect('element_storage_generation' in world_cpp and 'runElementGenerationTick' in world_cpp and
           'fallbackTemplates' in world_cpp and 'itElementGeneration' in index_html and 'stElementGeneration' in index_html and
           'editSkillElementGeneration' in index_html,
           '技能／道具可自動生成七元素，避免技能模板重複計算，新增與修改 UI 均可設定',
           '元素自生 C++ 邏輯或技能／道具設定 UI 不完整')
    expect('rulebookHTML' in index_html and 'npcImage' in index_html and 'npcMerchant' in index_html and
           'my_avatar_url' in index_html and 'generateSceneMap' in index_html and
           'nodeCommunicationBlocked' in index_html and 'itCommunicationDevice' in index_html and
           'offline-ai-toggle' in index_html and 'teamInvite' in index_html,
           'v48 規則書、NPC/玩家頭像、商人、地圖生成、禁訊、通訊道具、離線 AI 前端完整',
           'v48 前端控制缺失')
    # 49-cpp.10 條件式規則怪談引擎。
    engine_cpp=read('RuleEngine.cpp')
    engine_h=read('RuleEngine.h')
    engine_tables=['rule_engine_rules','rule_engine_rule_state','rule_engine_player_state','rule_engine_logs']
    expect(all(f'CREATE TABLE IF NOT EXISTS {t}' in migrations for t in engine_tables),
           '規則引擎四張 PostgreSQL 狀態／紀錄表完整',
           '規則引擎 migration 缺表')
    expect(all(route in engine_cpp for route in ['/api/admin/rule-engine/rules','/api/admin/rule-engine/test','/api/admin/rule-engine/logs','/api/admin/rule-engine/state','/api/admin/rule-engine/state/reset']),
           '規則引擎 DM CRUD／測試／紀錄／狀態 API 完整',
           '規則引擎 API 缺失')
    expect(all(op in engine_cpp for op in ['"all"','"any"','"not"','"eq"','"contains"','"regex"']) and
           all(effect in engine_cpp for effect in ['add_flag','increment_counter','start_countdown','notify_dm','log_violation','block_action','add_status','set_location']),
           '規則引擎支援 AND/OR/NOT 條件樹與核心效果',
           '規則引擎條件或效果不完整')
    expect('evaluateRuleEvent(service, roomId, user.id, "PLAYER_ACTION"' in legacy and
           'RULE_COUNTDOWN_EXPIRED' in engine_cpp and 'runRuleEngineTick' in world_cpp,
           '玩家行動與規則倒數已接入 C++ 規則引擎',
           '規則引擎尚未接入玩家行動或倒數自動化')
    expect('adminRuleEngineHTML' in index_html and 'data-admin-view="ruleEngine"' in index_html and
           'reConditions' in index_html and 'reEffects' in index_html and 'reTestBtn' in index_html,
           'DM 有獨立規則引擎頁、JSON 條件/效果編輯與 dry-run 測試器',
           '規則引擎 DM 前端缺失')
    expect('RuleEngine.cpp' in cmake_text and 'RuleEngine.cpp' in docker_text and 'RuleEngine.h' in cmake_text and 'RuleEngine.h' in docker_text,
           'RuleEngine 已加入 CMake 與 GitHub 平鋪 Docker 重建',
           'RuleEngine 未加入編譯／部署檔')
    expect(all(t in legacy for t in engine_tables) and re.search(r'std::array<const char\*,\s*\d+>', legacy) is not None,
           '全站備份已包含規則引擎資料表',
           '全站備份未納入規則引擎')
    expect(all(token in legacy for token in ['checkpoint["rule_engine_rules"]','checkpoint["rule_engine_player_state"]','checkpoint["rule_engine_rule_state"]','checkpoint["rule_engine_logs"]']) and
           'DELETE FROM rule_engine_rules WHERE room_id=$1' in legacy,
           '房間存檔／還原會保留規則引擎進度與觸發紀錄',
           '房間備份未完整保存規則引擎狀態')
    expect('if (userId <= 0) return;' in engine_cpp and 'explicitDmNotification' in engine_cpp and
           '目標地圖節點不屬於此房間' in engine_cpp,
           '規則引擎邊界防護：無玩家測試、通知去重、同房傳送驗證完整',
           '規則引擎邊界防護缺失')
    expect(all(token in (legacy + world_cpp + room_socket) for token in [
               'ITEM_USE_ATTEMPT','ITEM_USED','SHOP_PURCHASE_ATTEMPT','SHOP_PURCHASED',
               'NPC_DIALOGUE_ATTEMPT','NPC_DIALOGUE','PLAYER_MOVE_ATTEMPT','PLAYER_MOVED',
               'TEAM_JOINED','TEAM_LEFT','CHAT_MESSAGE','PLAYER_ROOM_JOINED']) and
           '此行動被規則阻止' in legacy and '前往此地點的行動被規則阻止' in world_cpp,
           '移動／商店／道具／NPC／隊伍／聊天已自動接線到 C++ 規則引擎',
           '常用世界事件尚未完整接入規則引擎')
    expect('SHOP_PURCHASE_ATTEMPT' in legacy and 'room_id:Number(state.room?.room?.id)||0' in index_html,
           '一般商店會帶房間情境進入規則引擎',
           '一般商店缺少房間規則情境')
    expect('PLAYER_SCENE_TICK' in engine_cpp and 'background_30s' in engine_cpp and 'startWorldAutomation' in world_cpp,
           '持續場景規則每 30 秒自動檢查，可處理獨處／久留條件',
           'PLAYER_SCENE_TICK 持續場景檢查缺失')
    expect('offline_ai_follow' in world_cpp and 'offline_ai_team_explore' in world_cpp and
           'PLAYER_MOVE_ATTEMPT' in world_cpp and 'PLAYER_MOVED' in world_cpp and 'rule_blocked' in world_cpp and
           '隊伍中有成員受到規則限制' in legacy,
           '真人隊伍與離線 AI 移動都受同一套規則引擎約束',
           '離線 AI 或隊伍移動可能繞過規則引擎')
    guide=read('RULE_ENGINE_GUIDE.md')
    expect('PLAYER_SCENE_TICK' in guide and 'SHOP_PURCHASE_ATTEMPT' in guide and
           'context.nearby_ally_count' in guide and 'block_action' in guide and 'Z 市規則示範' in guide,
           'RULE_ENGINE_GUIDE 提供事件／標籤／條件／效果與怪談規則範例',
           '規則引擎使用指南缺失或不完整')

    expect('WorldSystems.cpp' in cmake_text and 'WorldSystems.cpp' in docker_text and
           'WorldSystems.h' in cmake_text and 'WorldSystems.h' in docker_text,
           'WorldSystems 已納入 CMake 與 Render 平鋪 Docker 建置',
           'WorldSystems 未完整納入 CMake/Docker')
    expect('updated_at=CURRENT_TIMESTAMP WHERE room_id=$2 AND user_id=$3' not in world_cpp and
           'UPDATE room_members SET current_map_node_id=$1,updated_at=CURRENT_TIMESTAMP' not in legacy,
           '新位置／隊伍 SQL 不會寫入不存在的 room_members.updated_at',
           '新位置／隊伍 SQL 仍引用不存在的 room_members.updated_at')
    expect("ON CONFLICT(room_id,team_id,target_user_id) WHERE status='pending'" in world_cpp and
           'uq_room_team_invites_pending' in migrations,
           '離線組隊邀請使用 pending 部分唯一索引，可重複歷史入隊',
           '隊伍邀請 ON CONFLICT 與唯一索引不一致')

    # 52-cpp.13 規則怪談 × 戰鬥雙向橋接。
    bridge_cpp=read('RuleCombatBridge.cpp')
    bridge_h=read('RuleCombatBridge.h')
    expect(all(token in migrations for token in ['combat_victory_condition','rule_combat_state','rule_boss_phase','rule_modifiers']),
           '規則戰鬥 bridge migration：勝利條件、戰鬥狀態、Boss 階段與修正完整',
           '規則戰鬥 bridge migration 缺失')
    expect(all(effect in bridge_cpp for effect in ['spawn_monster','start_battle','end_battle','force_encounter','modify_monster','set_boss_phase','add_combat_status','set_victory_condition','enable_rule','disable_rule','set_combat_flag','modify_combat_value']),
           '規則可直接生成怪物／開戰／Boss 階段／狀態／勝利條件／開關規則',
           '規則－戰鬥效果類型不完整')
    expect(all(event in (core_cpp+legacy+bridge_cpp) for event in ['COMBAT_STARTED','COMBAT_ROUND_STARTED','COMBAT_TURN_STARTED','COMBAT_ATTACK_RESOLVED','COMBAT_ENTITY_DEFEATED','COMBAT_SKILL_USED','COMBAT_ENDED','COMBAT_VICTORY_CONDITION_MET']),
           '戰鬥開始／回合／攻擊／擊敗／技能／結束會反向送入規則引擎',
           '戰鬥→規則事件接線不完整')
    expect('applyRuleCombatEffect' in engine_cpp and 'root["combat"]["enemy_count"]' in engine_cpp and
           'combat_victory_condition' in engine_cpp and 'rule_combat_state' in engine_cpp,
           '規則 context 可讀 combat.* 且未知效果會交給 C++ 戰鬥橋接層',
           'RuleEngine 未完整接入戰鬥 bridge')
    expect('/api/admin/rooms/{1}/rule-combat' in bridge_cpp and
           'add-rule-combat-effect' in index_html and 'reCombatVictory' in index_html and
           '規則－戰鬥橋接' in index_html,
           'DM 規則頁有戰鬥效果模板與特殊勝利條件管理',
           '規則－戰鬥 DM UI/API 缺失')
    expect('RuleCombatBridge.cpp' in cmake_text and 'RuleCombatBridge.cpp' in docker_text and
           'RuleCombatBridge.h' in cmake_text and 'RuleCombatBridge.h' in docker_text,
           'RuleCombatBridge 已加入 CMake 與 Render 平鋪 Docker 建置',
           'RuleCombatBridge 未加入編譯／部署檔')
    guide=read('RULE_ENGINE_GUIDE.md')
    expect('規則－戰鬥橋接（52-cpp.13）' in guide and 'COMBAT_ATTACK_RESOLVED' in guide and
           'set_boss_phase' in guide and '特殊勝利條件' in guide,
           '規則引擎指南已補戰鬥事件、效果與勝利條件範例',
           'RULE_ENGINE_GUIDE 缺少戰鬥 bridge 說明')

    parity=json.loads(read('api_parity.json'))
    expected=collections.Counter(norm(*x.split(' ',1)) for x in parity.get('ported_http_routes',[]))
    actual=collections.Counter(routes())
    missing=list((expected-actual).elements())
    expect(len(parity.get('ported_http_routes',[]))==244 and not missing,'244/244 v39 HTTP handler 仍完整','API handler 缺失：'+', '.join(missing[:20]))
    extra_expected=collections.Counter(norm(*x.split(' ',1)) for x in parity.get('extra_cpp_routes',[]))
    extra_actual=actual-expected
    extra_missing=list((extra_expected-extra_actual).elements())
    extra_unlisted=list((extra_actual-extra_expected).elements())
    expect(not extra_missing and not extra_unlisted,'C++ 額外功能 API 與 api_parity.json 完全一致',
           '額外 API 差異：缺 '+', '.join(extra_missing[:10])+'；未登錄 '+', '.join(extra_unlisted[:10]))
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
        for n in ['main.cpp','Config.cpp','Security.cpp','CoreService.cpp','LegacyCompat.cpp','RoomSocket.cpp','RuleEngine.cpp','RuleCombatBridge.cpp','WorldSystems.cpp']: shutil.copy2(t/n,t/'src'/n)
        for n in ['Config.h','CoreService.h','LegacyCompat.h','LegacyRouteManifest.h','RoomSocket.h','Security.h','WorldSystems.h','RuleEngine.h','RuleCombatBridge.h']: shutil.copy2(t/n,t/'include/trpg'/n)
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

    # Full CoreService/LegacyCompat compilation needs real Drogon headers, and DB E2E needs DATABASE_URL.
    if not (Path('/usr/local/include/drogon/drogon.h').is_file() or Path('/usr/include/drogon/drogon.h').is_file()):
        WARN.append('目前工作容器沒有完整 Drogon headers；CoreService/LegacyCompat/WorldSystems/RuleEngine/RuleCombatBridge 需由 Render/Docker 做完整編譯驗證')

    # 52-cpp.13 direct avatar upload regression checks.
    world = read('WorldSystems.cpp')
    html = read('index.html')
    migrations = read('legacy_v39_migrations.sql')
    expect('POST /api/me/avatar/upload' in read('FEATURE_MAP.md') and '/api/me/avatar/upload' in world and
           '/api/admin/players/{1}/avatar/upload' in world and '/api/avatar-images/{1}' in world,
           '玩家／DM 直接上傳頭像 C++ API 與圖片讀取路由存在',
           '直接上傳頭像 API 或圖片讀取路由缺失')
    expect('CREATE TABLE IF NOT EXISTS avatar_images' in migrations and 'image_base64 TEXT NOT NULL' in migrations,
           '頭像圖片 PostgreSQL 持久化 migration 存在',
           'avatar_images migration 缺失')
    expect('detectedAvatarMime' in world and 'kAvatarMaxBytes' in world and 'image/png' in world and 'image/jpeg' in world and 'image/webp' in world,
           'C++ 頭像格式與 5 MB 上限驗證存在',
           'C++ 頭像格式/大小驗證缺失')
    expect('avatarFilePayload' in html and 'uploadAvatarFile' in html and 'type="file"' in html and 'image/jpeg,image/png,image/webp' in html,
           '玩家與 DM 前端直接選圖流程存在',
           '前端直接選圖流程缺失')

    if not os.environ.get('DATABASE_URL'):
        WARN.append('目前工作容器未提供 DATABASE_URL；v52 頭像直接上傳與規則戰鬥橋接 API 尚未在真實 PostgreSQL 做端到端回歸')

    print('TRPG Render flat-upload self check')
    for x in PASS: print('PASS',x)
    for x in FAIL: print('FAIL',x)
    for x in WARN: print('WARN',x)
    print(f'SUMMARY pass={len(PASS)} fail={len(FAIL)} warn={len(WARN)}')
    return 1 if FAIL else 0
if __name__=='__main__': raise SystemExit(main())
