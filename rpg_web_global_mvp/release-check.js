#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const crypto = require('crypto');
const childProcess = require('child_process');

const EXPECTED_VERSION = '39';
const ROOT = __dirname;

const options = {
  db: false,
  zip: null,
  report: null
};

for (let index = 2; index < process.argv.length; index += 1) {
  const argument = process.argv[index];

  if (argument === '--db') {
    options.db = true;
  } else if (argument === '--zip') {
    options.zip = process.argv[index + 1] || null;
    index += 1;
  } else if (argument === '--report') {
    options.report = process.argv[index + 1] || null;
    index += 1;
  } else if (argument === '--help' || argument === '-h') {
    console.log(`
TRPG Online 發版檢查

用法：
  node release-check.js
  node release-check.js --zip ../rpg_online_patch_v39.zip
  node release-check.js --db
  node release-check.js --zip <zip> --report <report.txt>

--db      對 DATABASE_URL 執行唯讀 schema 檢查
--zip     檢查 ZIP 完整性、路徑與來源檔案雜湊
--report  將本次結果另存成文字報告
`);
    process.exit(0);
  } else {
    console.error(`不明參數：${argument}`);
    process.exit(2);
  }
}

if (process.argv.includes('--zip') && !options.zip) {
  console.error('--zip 後面必須提供檔案路徑');
  process.exit(2);
}

if (process.argv.includes('--report') && !options.report) {
  console.error('--report 後面必須提供檔案路徑');
  process.exit(2);
}

const passed = [];
const warnings = [];
const failures = [];

function pass(name, detail = '') {
  passed.push({ name, detail });
}

function warn(name, detail) {
  warnings.push({ name, detail });
}

function fail(name, error) {
  failures.push({
    name,
    detail: error instanceof Error ? error.message : String(error)
  });
}

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

async function check(name, task) {
  try {
    const detail = await task();
    pass(name, detail || '');
  } catch (error) {
    fail(name, error);
  }
}

function absolute(relativePath) {
  return path.join(ROOT, relativePath);
}

function read(relativePath) {
  return fs.readFileSync(absolute(relativePath), 'utf8');
}

function countLine(source, offset) {
  return source.slice(0, offset).split('\n').length;
}

function sha256(buffer) {
  return crypto.createHash('sha256').update(buffer).digest('hex');
}

function escapeRegex(value) {
  return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

function assertBalancedBraces(source, label) {
  let depth = 0;
  let quote = null;
  let lineComment = false;
  let blockComment = false;

  for (let index = 0; index < source.length; index += 1) {
    const current = source[index];
    const next = source[index + 1];

    if (lineComment) {
      if (current === '\n') lineComment = false;
      continue;
    }

    if (blockComment) {
      if (current === '*' && next === '/') {
        blockComment = false;
        index += 1;
      }
      continue;
    }

    if (quote) {
      if (current === '\\') {
        index += 1;
      } else if (current === quote) {
        quote = null;
      }
      continue;
    }

    if (current === '/' && next === '/') {
      lineComment = true;
      index += 1;
      continue;
    }

    if (current === '/' && next === '*') {
      blockComment = true;
      index += 1;
      continue;
    }

    if (current === '"' || current === "'" || current === '`') {
      quote = current;
      continue;
    }

    if (current === '{') depth += 1;
    if (current === '}') depth -= 1;

    if (depth < 0) {
      throw new Error(`${label} 在第 ${countLine(source, index)} 行出現多餘的 }`);
    }
  }

  assert(depth === 0, `${label} 的大括號不平衡，差值 ${depth}`);
  assert(!quote, `${label} 存在未結束的字串`);
  assert(!blockComment, `${label} 存在未結束的註解`);
}

function extractRoutes(serverSource) {
  const routePattern = /\bapp\.(get|post|put|patch|delete)\s*\(\s*(['"])(\/api\/[^'"]+)\2/g;
  const routes = [];
  let match;

  while ((match = routePattern.exec(serverSource))) {
    const method = match[1].toUpperCase();
    const routePath = match[3];
    const middlewareWindow = serverSource
      .slice(match.index, match.index + 600)
      .split(/\basync\s+function\b|\basync\s*\(/)[0];

    routes.push({
      method,
      path: routePath,
      line: countLine(serverSource, match.index),
      middlewareWindow
    });
  }

  return routes;
}

function extractFrontendApiPaths(htmlSource) {
  const callPattern = /\bapi\s*\(\s*([`'"])(\/api\/[^`'"\r\n]*)/g;
  const results = [];
  let match;

  while ((match = callPattern.exec(htmlSource))) {
    const raw = match[2];
    const withoutQuery = raw.split('?')[0];
    const interpolationIndex = withoutQuery.indexOf('${');

    results.push({
      raw,
      line: countLine(htmlSource, match.index),
      prefix: interpolationIndex >= 0
        ? withoutQuery.slice(0, interpolationIndex)
        : null,
      path: interpolationIndex >= 0
        ? null
        : withoutQuery.replace(/\/+$/, '') || '/'
    });
  }

  return results;
}

function pathSegmentsCompatible(frontendPath, backendPath) {
  const frontSegments = frontendPath.split('/').filter(Boolean);
  const backSegments = backendPath.split('/').filter(Boolean);

  if (frontSegments.length !== backSegments.length) return false;

  return backSegments.every((segment, index) => {
    return segment.startsWith(':') || segment === frontSegments[index];
  });
}

function extractStringCalls(source, expression) {
  const values = new Set();
  let match;

  expression.lastIndex = 0;
  while ((match = expression.exec(source))) {
    values.add(match[1]);
  }
  return values;
}

function pngInfo(filePath) {
  const buffer = fs.readFileSync(filePath);
  const pngSignature = '89504e470d0a1a0a';

  assert(
    buffer.subarray(0, 8).toString('hex') === pngSignature,
    `${path.basename(filePath)} 不是有效 PNG`
  );
  assert(buffer.subarray(12, 16).toString('ascii') === 'IHDR', 'PNG 缺少 IHDR');

  return {
    width: buffer.readUInt32BE(16),
    height: buffer.readUInt32BE(20),
    colorType: buffer[25],
    hasTransparencyChunk: buffer.includes(Buffer.from('tRNS'))
  };
}

function commandAvailable(command) {
  const result = childProcess.spawnSync(command, ['-v'], {
    encoding: 'utf8',
    stdio: 'ignore'
  });
  return result.status === 0;
}

function runCommand(command, args, settings = {}) {
  const result = childProcess.spawnSync(command, args, {
    encoding: settings.encoding === null ? null : 'utf8',
    maxBuffer: 64 * 1024 * 1024
  });

  if (result.status !== 0) {
    const errorText = Buffer.isBuffer(result.stderr)
      ? result.stderr.toString('utf8')
      : String(result.stderr || '');
    throw new Error(`${command} ${args.join(' ')} 失敗：${errorText.trim()}`);
  }

  return result.stdout;
}

function formatResultLine(prefix, item) {
  return `${prefix} ${item.name}${item.detail ? ` — ${item.detail}` : ''}`;
}

async function checkDatabaseSchema() {
  assert(process.env.DATABASE_URL, '缺少 DATABASE_URL');

  let Pool;
  try {
    ({ Pool } = require('pg'));
  } catch {
    throw new Error('找不到 pg 套件，無法執行資料庫檢查');
  }

  const pool = new Pool({
    connectionString: process.env.DATABASE_URL,
    max: 1,
    connectionTimeoutMillis: 5000,
    ssl: process.env.PGSSL === 'disable'
      ? false
      : process.env.PGSSL === 'require'
        ? { rejectUnauthorized: false }
        : undefined
  });

  try {
    await pool.query('SELECT 1');

    const requiredColumns = [
      'gender',
      'age',
      'height_cm',
      'birthday',
      'great_way',
      'faith',
      'current_great_way',
      'current_faith'
    ];

    const columns = await pool.query(
      `SELECT column_name,data_type
       FROM information_schema.columns
       WHERE table_schema='public'
         AND table_name='character_cards'`
    );
    const byName = new Map(columns.rows.map(row => [row.column_name, row.data_type]));

    for (const column of requiredColumns) {
      assert(byName.has(column), `character_cards 缺少 ${column}`);
    }

    const requiredTables = ['room_savepoints', 'character_savepoints', 'audit_logs'];
    const tables = await pool.query(
      `SELECT table_name
       FROM information_schema.tables
       WHERE table_schema='public'
         AND table_type='BASE TABLE'
         AND table_name=ANY($1::text[])`,
      [requiredTables]
    );
    const tableNames = new Set(tables.rows.map(row => row.table_name));
    for (const table of requiredTables) {
      assert(tableNames.has(table), `public schema 缺少 ${table}`);
    }

    const remainingIntegers = await pool.query(
      `SELECT c.table_name,c.column_name
       FROM information_schema.columns c
       JOIN information_schema.tables t
         ON t.table_schema=c.table_schema
        AND t.table_name=c.table_name
       WHERE c.table_schema='public'
         AND t.table_type='BASE TABLE'
         AND c.data_type='integer'
         AND c.column_name<>'id'
         AND c.column_name NOT LIKE '%\\_id' ESCAPE '\\'
         AND COALESCE(c.column_default,'') NOT LIKE 'nextval(%'`
    );

    assert(
      remainingIntegers.rows.length === 0,
      `仍有未升級的 INTEGER：${remainingIntegers.rows
        .slice(0, 12)
        .map(row => `${row.table_name}.${row.column_name}`)
        .join('、')}`
    );

    return `連線正常；角色欄位 ${requiredColumns.length} 個；備份／稽核表 ${requiredTables.length} 個；大型數值 migration 正常`;
  } finally {
    await pool.end();
  }
}

async function main() {
  const requiredFiles = [
    'server.js',
    'release-check.js',
    'VERSION.txt',
    'PATCH_NOTES.txt',
    'APP_ICON_GUIDE.txt',
    'RELEASE_CHECKLIST.txt',
    'public/index.html',
    'public/manifest.webmanifest',
    'public/sw.js',
    'public/icons/favicon-32.png',
    'public/icons/apple-touch-icon.png',
    'public/icons/icon-192.png',
    'public/icons/icon-512.png'
  ];

  await check('必要檔案', () => {
    const missing = requiredFiles.filter(file => !fs.existsSync(absolute(file)));
    assert(!missing.length, `缺少：${missing.join('、')}`);
    return `${requiredFiles.length} 個檔案齊全`;
  });

  let serverSource = '';
  let htmlSource = '';
  let serviceWorkerSource = '';
  let manifest = null;

  await check('讀取核心檔案', () => {
    serverSource = read('server.js');
    htmlSource = read('public/index.html');
    serviceWorkerSource = read('public/sw.js');
    manifest = JSON.parse(read('public/manifest.webmanifest'));
    return 'server、HTML、Service Worker、manifest 已讀取';
  });

  await check('版本一致性', () => {
    const versionFile = read('VERSION.txt').trim();
    assert(versionFile === EXPECTED_VERSION, `VERSION.txt 是 ${versionFile}`);
    assert(
      new RegExp(`const\\s+APP_VERSION\\s*=\\s*['"]${escapeRegex(EXPECTED_VERSION)}['"]`).test(serverSource),
      'server.js APP_VERSION 不一致'
    );
    assert(
      new RegExp(`name=["']application-version["']\\s+content=["']${escapeRegex(EXPECTED_VERSION)}["']`).test(htmlSource),
      'index.html application-version 不一致'
    );
    assert(serviceWorkerSource.includes(`trpg-online-v${EXPECTED_VERSION}`), 'Service Worker 快取版本不一致');
    assert(serverSource.includes('version:\n          APP_VERSION'), '/api/health 未回傳 APP_VERSION');
    assert(read('PATCH_NOTES.txt').includes(`v${EXPECTED_VERSION}：`), '更新說明缺少目前版本');
    return `v${EXPECTED_VERSION}`;
  });

  await check('JavaScript 語法', () => {
    new vm.Script(serverSource, { filename: 'server.js' });
    new vm.Script(serviceWorkerSource, { filename: 'public/sw.js' });

    const scripts = [...htmlSource.matchAll(/<script(?:\s[^>]*)?>([\s\S]*?)<\/script>/gi)];
    assert(scripts.length > 0, '找不到前端內嵌腳本');
    scripts.forEach((script, index) => {
      new Function(script[1]);
    });
    return `server、Service Worker、${scripts.length} 段前端腳本`;
  });

  await check('HTML／CSS 結構', () => {
    const styles = [...htmlSource.matchAll(/<style(?:\s[^>]*)?>([\s\S]*?)<\/style>/gi)];
    assert(styles.length === 1, `style 區塊數量為 ${styles.length}`);
    assertBalancedBraces(styles[0][1], 'CSS');
    for (const tag of ['html', 'head', 'body']) {
      assert((htmlSource.match(new RegExp(`<${tag}\\b`, 'gi')) || []).length === 1, `${tag} 起始標籤數量不正確`);
      assert((htmlSource.match(new RegExp(`</${tag}>`, 'gi')) || []).length === 1, `${tag} 結束標籤數量不正確`);
    }
    return '主要標籤與 CSS 大括號正常';
  });

  await check('關鍵相容函式', () => {
    const requiredFunctions = [
      'gearLoadout',
      'itemTemplateByName',
      'itemTypeLabel',
      'wheelsHTML',
      'adminStatusesHTML',
      'adminItemsHTML',
      'adminBackupsHTML',
      'loadBackupCenter',
      'downloadBackupJson',
      'openMyCharacterEditor',
      'characterHTML',
      'renderContent',
      'render'
    ];
    const missing = [];
    const duplicates = [];

    for (const name of requiredFunctions) {
      const matches = htmlSource.match(new RegExp(`function\\s+${escapeRegex(name)}\\s*\\(`, 'g')) || [];
      if (!matches.length) missing.push(name);
      if (matches.length > 1) duplicates.push(`${name} ×${matches.length}`);
    }

    assert(!missing.length, `缺少：${missing.join('、')}`);
    assert(!duplicates.length, `重複定義：${duplicates.join('、')}`);
    return `${requiredFunctions.length} 個近期重要函式正常`;
  });

  await check('靜態 DOM 事件目標', () => {
    const declared = new Set(
      [...htmlSource.matchAll(/\bid\s*=\s*(['"])([^'"]+)\1/g)].map(match => match[2])
    );
    const used = new Set(
      [...htmlSource.matchAll(/\$\(\s*(['"])#([A-Za-z][\w:.-]*)\1\s*\)/g)].map(match => match[2])
    );
    const missing = [...used].filter(id => !declared.has(id));
    assert(!missing.length, `下列 #id 被使用但未建立：${missing.slice(0, 20).join('、')}`);
    return `${used.size} 個固定 ID 均能找到宣告`;
  });

  let routes = [];
  await check('後端 API 路由', () => {
    routes = extractRoutes(serverSource);
    assert(routes.length >= 200, `只找到 ${routes.length} 條路由，可能解析失敗或路由遺失`);

    const seen = new Map();
    for (const route of routes) {
      const key = `${route.method} ${route.path}`;
      if (!seen.has(key)) seen.set(key, []);
      seen.get(key).push(route.line);
    }
    const duplicates = [...seen.entries()].filter(([, lines]) => lines.length > 1);
    assert(
      !duplicates.length,
      `重複路由：${duplicates.map(([key, lines]) => `${key}（${lines.join(',')}）`).join('；')}`
    );
    return `${routes.length} 條路由，沒有重複`;
  });

  await check('API 權限中介層', () => {
    const publicRoutes = new Set([
      'GET /api/health',
      'POST /api/auth/register',
      'POST /api/auth/login'
    ]);
    const problems = [];

    for (const route of routes) {
      const key = `${route.method} ${route.path}`;
      if (route.path.startsWith('/api/admin/') && !/\badminAuth\b/.test(route.middlewareWindow)) {
        problems.push(`${key} 缺少 adminAuth（第 ${route.line} 行）`);
      }
      if (!publicRoutes.has(key) && !/\bauth\b/.test(route.middlewareWindow)) {
        problems.push(`${key} 缺少 auth（第 ${route.line} 行）`);
      }
    }

    assert(!problems.length, problems.slice(0, 20).join('；'));
    return '管理路由與登入保護正常';
  });

  await check('前後端 API 對照', () => {
    const frontendCalls = extractFrontendApiPaths(htmlSource);
    const unique = new Map();
    frontendCalls.forEach(call => unique.set(call.raw, call));
    const missing = [];

    for (const call of unique.values()) {
      const matched = call.prefix !== null
        ? routes.some(route => route.path.startsWith(call.prefix) || call.prefix.startsWith(route.path))
        : routes.some(route => pathSegmentsCompatible(call.path, route.path));

      if (!matched) {
        missing.push(`${call.raw}（前端第 ${call.line} 行）`);
      }
    }

    assert(!missing.length, `找不到後端路由：${missing.slice(0, 20).join('；')}`);
    return `${unique.size} 種可靜態辨識的 API 路徑均有後端對應`;
  });

  await check('Socket 事件對照', () => {
    const serverIncoming = extractStringCalls(serverSource, /\bsocket\.on\s*\(\s*['"]([^'"]+)['"]/g);
    const serverOutgoing = extractStringCalls(serverSource, /\.emit\s*\(\s*['"]([^'"]+)['"]/g);
    const clientIncoming = extractStringCalls(htmlSource, /\bstate\.socket\.on\s*\(\s*['"]([^'"]+)['"]/g);
    const clientOutgoing = extractStringCalls(htmlSource, /\bstate\.socket\.emit\s*\(\s*['"]([^'"]+)['"]/g);
    const builtIn = new Set(['connect', 'disconnect', 'connect_error']);
    const missingServer = [...clientOutgoing].filter(event => !serverIncoming.has(event));
    const missingEmitter = [...clientIncoming].filter(event => !builtIn.has(event) && !serverOutgoing.has(event));

    assert(!missingServer.length, `前端送出但後端未監聽：${missingServer.join('、')}`);
    assert(!missingEmitter.length, `前端監聽但後端未送出：${missingEmitter.join('、')}`);
    return `前端送出 ${clientOutgoing.size} 種、監聽 ${clientIncoming.size} 種事件`;
  });

  await check('玩家大道／信仰權限契約', () => {
    const updateStart = serverSource.indexOf('async function updatePlayerCharacter');
    const updateEnd = serverSource.indexOf('async function migrateTaskSystem', updateStart);
    assert(updateStart >= 0 && updateEnd > updateStart, '找不到 updatePlayerCharacter 區塊');
    const block = serverSource.slice(updateStart, updateEnd);

    assert(block.includes("current.faction==='西國'?'faith':'great_way'"), '陣營對應上限欄位遺失');
    assert(block.includes("current.faction==='西國'?'current_faith':'current_great_way'"), '目前值欄位對應遺失');
    assert(block.includes('wrongFifthField'), '錯誤陣營欄位拒絕邏輯遺失');
    assert(block.includes('if(nextMax<currentValue)'), '上限不可低於目前值的保護遺失');

    const allowedMatch = block.match(/const allowed = new Set\(\[([\s\S]*?)\]\);/);
    assert(allowedMatch, '找不到玩家允許欄位清單');
    assert(!/current_great_way|current_faith/.test(allowedMatch[1]), '玩家允許欄位誤包含目前剩餘值');
    assert(htmlSource.includes('目前剩餘（不可修改）'), '前端未標示目前值不可修改');
    assert(/目前剩餘（不可修改）[\s\S]{0,180}disabled/.test(htmlSource), '前端目前值輸入框未停用');
    return '玩家只能改正確陣營上限，不能提交目前剩餘值';
  });

  await check('角色資料與大型數值 migration', () => {
    for (const column of ['gender', 'age', 'height_cm', 'birthday']) {
      assert(
        new RegExp(`ADD COLUMN IF NOT EXISTS ${escapeRegex(column)}\\b`).test(serverSource),
        `缺少 character_cards.${column} migration`
      );
    }
    assert(serverSource.includes("c.data_type='integer'"), '找不到 INTEGER 掃描');
    assert(serverSource.includes("c.column_name<>'id'"), 'BIGINT migration 未排除主鍵');
    assert(serverSource.includes("c.column_name NOT LIKE '%\\\\_id'"), 'BIGINT migration 未排除外鍵');
    assert(serverSource.includes('ALTER TABLE %I.%I ALTER COLUMN %I TYPE BIGINT'), '找不到 BIGINT 轉換');
    return '角色基本資料與 BIGINT migration 契約存在';
  });

  await check('備份／還原與稽核安全契約', () => {
    for (const table of ['room_savepoints', 'character_savepoints', 'audit_logs']) {
      assert(
        serverSource.includes(`CREATE TABLE IF NOT EXISTS ${table}`),
        `缺少 ${table} migration`
      );
    }

    const routeKeys = new Set(routes.map(route => `${route.method} ${route.path}`));
    const requiredRoutes = [
      'GET /api/admin/backups/site/export',
      'GET /api/admin/backups/rooms/:id/export',
      'GET /api/admin/backups/characters/:userId/export',
      'GET /api/admin/backups/character-savepoints',
      'GET /api/admin/backups/character-savepoints/:id/preview',
      'POST /api/admin/backups/preview',
      'POST /api/admin/backups/restore',
      'GET /api/admin/audit-logs'
    ];
    for (const route of requiredRoutes) {
      assert(routeKeys.has(route), `缺少 ${route}`);
    }

    assert(serverSource.includes("const BACKUP_SCHEMA='trpg-online-backup'"), '缺少備份 schema 識別');
    assert(serverSource.includes('crypto.createHash(\'sha256\')'), '備份沒有 SHA-256 校驗碼');
    assert(serverSource.includes('crypto.timingSafeEqual'), '校驗碼比較未使用固定時間比較');
    assert(serverSource.includes("purpose:'backup_restore'"), '缺少短效還原許可用途綁定');
    assert(serverSource.includes("expiresIn:'10m'"), '還原許可不是 10 分鐘短效 Token');
    assert(serverSource.includes("String(req.body?.confirm_text||'')!==preview.confirm_text"), '後端未驗證指定確認文字');
    assert(serverSource.includes("if(backup.scope==='site')return res.status(400)"), '後端未禁止網頁全站還原');
    assert(serverSource.includes('INSERT INTO room_savepoints(room_id,label,snapshot,created_by)'), '房間還原前沒有安全存檔');
    assert(serverSource.includes('INSERT INTO character_savepoints(user_id,label,snapshot,created_by)'), '角色還原前沒有安全存檔');
    assert(serverSource.includes("await client.query('ROLLBACK').catch"), '角色還原缺少交易回滾');

    const siteStart = serverSource.indexOf('async function buildSiteArchive');
    const siteEnd = serverSource.indexOf('async function getBackupTableColumns', siteStart);
    assert(siteStart >= 0 && siteEnd > siteStart, '找不到全站封存函式');
    const siteBlock = serverSource.slice(siteStart, siteEnd);
    assert(siteBlock.includes('SELECT id,username,is_admin,created_at FROM users'), '全站封存未使用帳號欄位白名單');
    assert(!siteBlock.includes('password_hash'), '全站封存函式碰觸密碼雜湊');

    const auditStart = serverSource.indexOf('function auditAdminRequests');
    const auditEnd = serverSource.indexOf('/* =========================================================\n   HEALTH', auditStart);
    assert(auditStart >= 0 && auditEnd > auditStart, '找不到管理操作稽核 middleware');
    const auditBlock = serverSource.slice(auditStart, auditEnd);
    assert(!auditBlock.includes('req.body'), '管理稽核不應保存表單原文');
    assert(serverSource.includes('function isSensitiveBackupKey'), '缺少敏感憑證欄位排除');
    for (const key of ['password_hash', 'access_token', 'jwt_secret', 'database_url', 'api_key']) {
      assert(serverSource.includes(key), `敏感欄位規則缺少 ${key}`);
    }

    for (const id of ['exportSiteBackup', 'exportRoomBackup', 'exportCharacterBackup', 'backupImportFile', 'restoreImportedBackup', 'refreshBackupCenter']) {
      assert(htmlSource.includes(`id="${id}"`), `備份介面缺少 #${id}`);
    }
    assert(htmlSource.includes("data-admin-view=\"backups\""), '管理中心缺少備份分頁');
    assert(htmlSource.includes('file.size>8*1024*1024'), '前端未限制匯入檔案大小');
    assert(htmlSource.includes('還原許可有效 10 分鐘'), '前端未提示還原許可期限');
    assert(htmlSource.includes('preview-character-savepoint'), '前端無法載入角色自動安全存檔');
    return `${requiredRoutes.length} 條端點；校驗、兩階段確認、自動安全存檔、憑證排除與操作稽核正常`;
  });

  await check('備份校驗故障測試', () => {
    const start = serverSource.indexOf("const BACKUP_SCHEMA='trpg-online-backup'");
    const end = serverSource.indexOf('function safeBackupTableName', start);
    assert(start >= 0 && end > start, '找不到可執行的備份校驗核心');
    const backupCore = serverSource.slice(start, end);
    const result = vm.runInNewContext(
      backupCore + `
        (() => {
          const passed=[];
          const expect=(name,task)=>{task();passed.push(name);};
          const expectThrow=(name,task,pattern)=>{
            let caught=null;
            try{task();}catch(error){caught=error;}
            if(!caught)throw new Error(name+' 沒有拒絕');
            if(pattern&&!pattern.test(caught.message))throw new Error(name+' 錯誤訊息不符：'+caught.message);
            passed.push(name);
          };
          const backup=createBackupEnvelope('character',{id:7,name:'測試角色'},{checkpoint:{character:{user_id:7,name:'甲',password:'不可輸出'},related:{tokens:[{access_token:'不可輸出',safe:'保留'}]}}});
          expect('敏感欄位排除',()=>{if(JSON.stringify(backup).includes('不可輸出'))throw new Error('敏感值仍存在');});
          expect('正常校驗',()=>validateBackupEnvelope(backup));
          expect('鍵順序穩定',()=>{const reordered={...backup,target:{name:'測試角色',id:7},data:{checkpoint:backup.data.checkpoint}};if(backupChecksum(reordered)!==backup.checksum)throw new Error('鍵順序影響校驗');});
          expectThrow('內容篡改',()=>validateBackupEnvelope({...backup,data:{checkpoint:{character:{user_id:7,name:'被改過'}}}}),/校驗失敗/);
          expectThrow('偽造憑證欄位',()=>{const forged={...backup,data:{...backup.data,password_hash:'hash'}};forged.checksum=backupChecksum(forged);validateBackupEnvelope(forged);},/登入憑證/);
          expectThrow('錯誤範圍',()=>{const bad={...backup,scope:'unknown'};bad.checksum=backupChecksum(bad);validateBackupEnvelope(bad);},/備份範圍/);
          expectThrow('超大檔案',()=>validateBackupEnvelope(createBackupEnvelope('site',{id:'site'},{text:'x'.repeat(8*1024*1024)})),/8 MB/);
          return passed;
        })()
      `,
      { APP_VERSION: EXPECTED_VERSION, crypto, Buffer }
    );
    assert(Array.isArray(result) && result.length === 7, '故障測試沒有完整執行');
    return `${result.length} 種正常／故障案例通過`;
  });

  await check('備份中心輸出測試', () => {
    const escStart = htmlSource.indexOf('function esc(value)');
    const escEnd = htmlSource.indexOf('const IDENTITY_RANKS', escStart);
    const backupStart = htmlSource.indexOf('function backupScopeLabel');
    const backupEnd = htmlSource.indexOf('function adminControlHTML', backupStart);
    assert(escStart >= 0 && escEnd > escStart && backupStart >= 0 && backupEnd > backupStart, '找不到備份中心輸出函式');
    const state = {
      adminRooms: [{ id: 1, name: '<房間>', code: 'A&B' }],
      adminCharacters: [{ user_id: 2, name: '<script>alert(1)</script>', username: 'p' }],
      backupCenter: {
        logs: [{ action: 'backup.restore.character', method: 'POST', path: '/api/admin/backups/restore', status_code: 200, actor_display: 'dm', target_type: 'character', target_id: '2', created_at: '2026-08-17T00:00:00Z' }],
        savepoints: [{ id: 3, user_id: 2, character_name: '角色', label: '安全', created_by_username: 'dm', created_at: '2026-08-17T00:00:00Z' }],
        fileName: 'sample.json',
        preview: { valid: true, restorable: true, scope: 'character', target: { id: 2, name: '角色' }, counts: { character: 1, statuses: 2 }, confirm_text: '還原角色 #2', warning: '警告' }
      }
    };
    const output = vm.runInNewContext(
      htmlSource.slice(escStart, escEnd) + htmlSource.slice(backupStart, backupEnd) + '\nadminBackupsHTML();',
      { state }
    );
    for (const id of ['exportSiteBackup', 'exportRoomBackup', 'exportCharacterBackup', 'backupImportFile', 'restoreImportedBackup']) {
      assert(output.includes(`id="${id}"`), `模擬輸出缺少 #${id}`);
    }
    assert(output.includes('preview-character-savepoint'), '模擬輸出缺少角色安全存檔按鈕');
    assert(output.includes('&lt;script&gt;alert(1)&lt;/script&gt;'), '備份中心未跳脫角色名稱');
    assert(!output.includes('<script>alert(1)</script>'), '備份中心輸出未跳脫的 HTML');
    assert(!output.includes('undefined'), '備份中心模擬輸出含 undefined');
    assert(htmlSource.includes('.backup-warning{') && htmlSource.includes('#backupImportFile{'), '備份警告／檔案輸入缺少版面規則');
    return '匯出、匯入、還原、安全存檔與 HTML 跳脫正常';
  });

  await check('PWA 與圖標', () => {
    assert(manifest.name && manifest.short_name, 'manifest 缺少名稱');
    assert(manifest.start_url === '/' && manifest.scope === '/', 'manifest start_url／scope 不正確');
    assert(manifest.display === 'standalone', 'manifest display 不是 standalone');

    const icons = [
      ['public/icons/favicon-32.png', 32],
      ['public/icons/apple-touch-icon.png', 180],
      ['public/icons/icon-192.png', 192],
      ['public/icons/icon-512.png', 512]
    ];

    for (const [relativePath, expectedSize] of icons) {
      const info = pngInfo(absolute(relativePath));
      assert(
        info.width === expectedSize && info.height === expectedSize,
        `${relativePath} 是 ${info.width}×${info.height}`
      );
      assert(![4, 6].includes(info.colorType), `${relativePath} 含 alpha 色彩類型`);
      assert(!info.hasTransparencyChunk, `${relativePath} 含透明色塊`);
      const url = `/${relativePath.replace(/^public\//, '')}`;
      assert(
        htmlSource.includes(url) || serviceWorkerSource.includes(url) ||
          manifest.icons.some(icon => icon.src === url),
        `${url} 沒有被引用`
      );
    }

    for (const size of ['192x192', '512x512']) {
      const icon = manifest.icons.find(item => item.sizes === size && item.type === 'image/png');
      assert(icon, `manifest 缺少 ${size}`);
      assert(String(icon.purpose || '').split(/\s+/).includes('maskable'), `${size} 未標記 maskable`);
    }

    assert(htmlSource.includes("register('/sw.js')"), '前端未註冊 Service Worker');
    assert(!fs.existsSync(absolute('public/icons/icon.svg')), '舊 icon.svg 仍存在');
    assert(!/icon\.svg/.test(htmlSource + serviceWorkerSource + JSON.stringify(manifest)), '仍引用舊 icon.svg');
    return 'manifest、Service Worker 與 4 個不透明 PNG 正常';
  });

  await check('手機版防呆契約', () => {
    const contracts = [
      ['viewport-fit=cover', '缺少安全區 viewport'],
      ['env(safe-area-inset-bottom)', '缺少底部安全區'],
      ['scroll-snap-type:x mandatory', '底部導覽未設定滑動吸附'],
      ['activeMobileButton.scrollIntoView', '目前分頁不會自動移入可視範圍'],
      ['font-size:16px', '手機輸入框未防止自動放大'],
      ['height:100dvh', '手機彈窗未使用動態視窗高度']
    ];
    for (const [token, message] of contracts) {
      assert(htmlSource.includes(token), message);
    }
    return `${contracts.length} 個手機版契約存在`;
  });

  if (options.zip) {
    await check('ZIP 完整性與來源雜湊', () => {
      assert(commandAvailable('unzip'), '系統沒有 unzip，無法檢查 ZIP');
      const zipPath = path.resolve(process.cwd(), options.zip);
      assert(fs.existsSync(zipPath), `找不到 ${zipPath}`);
      runCommand('unzip', ['-t', zipPath]);

      const entries = String(runCommand('unzip', ['-Z1', zipPath]))
        .split(/\r?\n/)
        .filter(Boolean);
      const fileEntries = entries.filter(entry => !entry.endsWith('/'));

      for (const entry of entries) {
        assert(!path.isAbsolute(entry), `ZIP 含絕對路徑：${entry}`);
        assert(!entry.split('/').includes('..'), `ZIP 含路徑跳脫：${entry}`);
        assert(!entry.includes('\\'), `ZIP 含非標準反斜線路徑：${entry}`);
      }

      const expected = new Set(requiredFiles);
      const actual = new Set(fileEntries);
      const missing = [...expected].filter(file => !actual.has(file));
      const unexpected = [...actual].filter(file => !expected.has(file));
      assert(!missing.length, `ZIP 缺少：${missing.join('、')}`);
      assert(!unexpected.length, `ZIP 多出：${unexpected.join('、')}`);

      for (const relativePath of requiredFiles) {
        const sourceBuffer = fs.readFileSync(absolute(relativePath));
        const archivedBuffer = runCommand('unzip', ['-p', zipPath, relativePath], { encoding: null });
        assert(
          sha256(sourceBuffer) === sha256(archivedBuffer),
          `${relativePath} 與來源雜湊不同`
        );
      }

      return `${fileEntries.length} 個檔案完整且與來源一致`;
    });
  } else {
    warn('ZIP 檢查', '未提供 --zip；完成打包後必須再跑一次');
  }

  if (options.db) {
    await check('PostgreSQL 唯讀 schema', checkDatabaseSchema);
  } else {
    warn('PostgreSQL 實際 schema', '未提供 --db；本次只檢查 migration 程式碼');
  }

  warn('瀏覽器／手機實機', '靜態檢查不能取代正式部署後的登入、多人同步與安裝測試');

  const status = failures.length ? 'FAIL' : 'PASS';
  const lines = [
    `TRPG Online v${EXPECTED_VERSION} 發版檢查`,
    `時間：${new Date().toISOString()}`,
    `結果：${status}`,
    '',
    ...passed.map(item => formatResultLine('PASS', item)),
    ...warnings.map(item => formatResultLine('WARN', item)),
    ...failures.map(item => formatResultLine('FAIL', item)),
    '',
    `統計：${passed.length} 通過／${warnings.length} 警告／${failures.length} 失敗`
  ];

  const report = lines.join('\n') + '\n';
  console.log(report);

  if (options.report) {
    const reportPath = path.resolve(process.cwd(), options.report);
    fs.writeFileSync(reportPath, report, 'utf8');
    console.log(`報告已寫入：${reportPath}`);
  }

  process.exitCode = failures.length ? 1 : 0;
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
