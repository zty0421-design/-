const path = require('path');
const http = require('http');
const express = require('express');
const { Server } = require('socket.io');
const { Pool } = require('pg');
const bcrypt = require('bcryptjs');
const jwt = require('jsonwebtoken');

const PORT = Number(process.env.PORT || 3000);
const HOST = process.env.HOST || '0.0.0.0';
const JWT_SECRET = process.env.JWT_SECRET || 'dev-only-change-me';
if (process.env.NODE_ENV === 'production' && JWT_SECRET === 'dev-only-change-me') {
  throw new Error('JWT_SECRET must be set in production');
}
const DATABASE_URL = process.env.DATABASE_URL;

if (!DATABASE_URL) {
  throw new Error('DATABASE_URL is required. Example: postgresql://user:password@localhost:5432/rpg');
}

const pool = new Pool({
  connectionString: DATABASE_URL,
  max: Number(process.env.PGPOOL_MAX || 10),
  idleTimeoutMillis: 30000,
  connectionTimeoutMillis: 5000,
  ssl: process.env.PGSSL === 'disable' ? false : (process.env.PGSSL === 'require' ? { rejectUnauthorized: false } : undefined)
});

function pgSql(sql) {
  let i = 0;
  return sql.replace(/\?/g, () => `$${++i}`);
}

const query = (sql, params = []) => pool.query(pgSql(sql), params);
const get = async (sql, params = []) => {
  const result = await query(sql, params);
  return result.rows[0] || null;
};
const all = async (sql, params = []) => {
  const result = await query(sql, params);
  return result.rows;
};
const run = async (sql, params = []) => {
  const result = await query(sql, params);
  const row = result.rows[0] || null;
  return { id: row?.id ?? null, changes: result.rowCount, row };
};

async function initDb() {
  await pool.query(`
    CREATE TABLE IF NOT EXISTS users (
      id BIGSERIAL PRIMARY KEY,
      username VARCHAR(20) NOT NULL UNIQUE,
      password_hash TEXT NOT NULL,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );
    CREATE TABLE IF NOT EXISTS rooms (
      id BIGSERIAL PRIMARY KEY,
      code VARCHAR(6) NOT NULL UNIQUE,
      name VARCHAR(60) NOT NULL,
      owner_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      status VARCHAR(20) NOT NULL DEFAULT 'lobby',
      round INTEGER NOT NULL DEFAULT 1,
      current_actor_id BIGINT,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );
    CREATE TABLE IF NOT EXISTS room_members (
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      role VARCHAR(10) NOT NULL DEFAULT 'player',
      display_name VARCHAR(30) NOT NULL,
      hp INTEGER NOT NULL DEFAULT 100,
      max_hp INTEGER NOT NULL DEFAULT 100,
      spirit INTEGER NOT NULL DEFAULT 100,
      max_spirit INTEGER NOT NULL DEFAULT 100,
      joined_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      PRIMARY KEY(room_id, user_id)
    );
    CREATE TABLE IF NOT EXISTS events (
      id BIGSERIAL PRIMARY KEY,
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,
      type VARCHAR(30) NOT NULL,
      payload JSONB NOT NULL,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );
    CREATE INDEX IF NOT EXISTS idx_room_members_room ON room_members(room_id);
    CREATE INDEX IF NOT EXISTS idx_events_room_id_id ON events(room_id, id);
  `);
}

function signToken(user) {
  return jwt.sign({ id: user.id, username: user.username }, JWT_SECRET, { expiresIn: '7d' });
}
function auth(req, res, next) {
  const raw = req.headers.authorization || '';
  const token = raw.startsWith('Bearer ') ? raw.slice(7) : null;
  if (!token) return res.status(401).json({ error: '需要登入' });
  try { req.user = jwt.verify(token, JWT_SECRET); next(); }
  catch { return res.status(401).json({ error: '登入已失效' }); }
}
function socketAuth(socket, next) {
  try { socket.user = jwt.verify(socket.handshake.auth?.token || '', JWT_SECRET); next(); }
  catch { next(new Error('unauthorized')); }
}
function generateCode() {
  const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
  let out = '';
  for (let i = 0; i < 6; i++) out += alphabet[Math.floor(Math.random() * alphabet.length)];
  return out;
}
async function uniqueCode() {
  let c;
  do { c = generateCode(); } while (await get('SELECT id FROM rooms WHERE code = ?', [c]));
  return c;
}
async function roomSnapshot(roomId) {
  const room = await get('SELECT id, code, name, owner_id, status, round, current_actor_id, created_at FROM rooms WHERE id = ?', [roomId]);
  if (!room) return null;
  const members = await all(`SELECT rm.user_id AS id, rm.display_name, rm.role, rm.hp, rm.max_hp, rm.spirit, rm.max_spirit, u.username
                            FROM room_members rm JOIN users u ON u.id = rm.user_id WHERE rm.room_id = ? ORDER BY rm.joined_at ASC`, [roomId]);
  const events = await all(`SELECT e.id, e.type, e.payload, e.created_at, u.username FROM events e LEFT JOIN users u ON u.id=e.user_id WHERE e.room_id=? ORDER BY e.id DESC LIMIT 80`, [roomId]);
  return { room, members, events: events.reverse().map(e => ({ ...e, payload: (typeof e.payload === 'string' ? JSON.parse(e.payload) : e.payload) })) };
}
async function requireRoomMember(userId, roomId) {
  const row = await get('SELECT * FROM room_members WHERE room_id=? AND user_id=?', [roomId, userId]);
  if (!row) throw Object.assign(new Error('你不是房間成員'), { status: 403 });
  return row;
}
async function requireOwner(userId, roomId) {
  const row = await get('SELECT * FROM rooms WHERE id=? AND owner_id=?', [roomId, userId]);
  if (!row) throw Object.assign(new Error('只有GM可以執行此操作'), { status: 403 });
  return row;
}
async function addEvent(roomId, userId, type, payload) {
  await run('INSERT INTO events(room_id,user_id,type,payload) VALUES(?,?,?,?::jsonb)', [roomId, userId, type, JSON.stringify(payload)]);
}

const app = express();
app.set('trust proxy', 1);
app.disable('x-powered-by');
app.use(express.json({ limit: '1mb' }));
app.get('/api/health', async (req, res) => {
  try {
    await pool.query('SELECT 1');
    res.json({ ok: true, service: 'trpg-online', database: 'ok', time: new Date().toISOString() });
  } catch (err) {
    res.status(503).json({ ok: false, database: 'unavailable' });
  }
});
app.use(express.static(path.join(__dirname, 'public')));

app.post('/api/auth/register', async (req, res) => {
  try {
    const username = String(req.body.username || '').trim();
    const password = String(req.body.password || '');
    if (!/^[\w\u4e00-\u9fff-]{2,20}$/.test(username)) return res.status(400).json({ error: '帳號需為2-20字，可用中文、英文、數字、底線或連字號' });
    if (password.length < 6) return res.status(400).json({ error: '密碼至少6碼' });
    const hash = await bcrypt.hash(password, 10);
    const result = await run('INSERT INTO users(username,password_hash) VALUES(?,?) RETURNING id', [username, hash]);
    const user = { id: result.id, username };
    res.json({ token: signToken(user), user });
  } catch (err) {
    res.status(err.code === '23505' ? 409 : 500).json({ error: err.code === '23505' ? '帳號已存在' : '註冊失敗' });
  }
});
app.post('/api/auth/login', async (req, res) => {
  try {
    const username = String(req.body.username || '').trim();
    const password = String(req.body.password || '');
    const user = await get('SELECT id, username, password_hash FROM users WHERE username=?', [username]);
    if (!user || !(await bcrypt.compare(password, user.password_hash))) return res.status(401).json({ error: '帳號或密碼錯誤' });
    res.json({ token: signToken(user), user: { id: user.id, username: user.username } });
  } catch { res.status(500).json({ error: '登入失敗' }); }
});
app.get('/api/me', auth, async (req, res) => res.json({ user: req.user }));

app.post('/api/rooms', auth, async (req, res) => {
  try {
    const name = String(req.body.name || '未命名跑團').trim().slice(0, 60) || '未命名跑團';
    const code = await uniqueCode();
    const result = await run('INSERT INTO rooms(code,name,owner_id) VALUES(?,?,?) RETURNING id', [code, name, req.user.id]);
    await run('INSERT INTO room_members(room_id,user_id,role,display_name) VALUES(?,?,?,?)', [result.id, req.user.id, 'gm', req.user.username]);
    await addEvent(result.id, req.user.id, 'system', { text: `${req.user.username} 建立了跑團「${name}」` });
    res.json(await roomSnapshot(result.id));
  } catch (e) { res.status(500).json({ error: '建立房間失敗' }); }
});
app.post('/api/rooms/join', auth, async (req, res) => {
  try {
    const code = String(req.body.code || '').trim().toUpperCase();
    const room = await get('SELECT * FROM rooms WHERE code=?', [code]);
    if (!room) return res.status(404).json({ error: '找不到房間' });
    const existing = await get('SELECT * FROM room_members WHERE room_id=? AND user_id=?', [room.id, req.user.id]);
    if (!existing) await run('INSERT INTO room_members(room_id,user_id,role,display_name) VALUES(?,?,?,?)', [room.id, req.user.id, 'player', req.user.username]);
    await addEvent(room.id, req.user.id, 'system', { text: `${req.user.username} 加入房間` });
    res.json(await roomSnapshot(room.id));
  } catch (e) { res.status(500).json({ error: '加入房間失敗' }); }
});
app.get('/api/rooms/:id', auth, async (req, res) => {
  try { await requireRoomMember(req.user.id, req.params.id); res.json(await roomSnapshot(req.params.id)); }
  catch (e) { res.status(e.status || 500).json({ error: e.message || '讀取房間失敗' }); }
});
app.post('/api/rooms/:id/start', auth, async (req, res) => {
  try {
    await requireOwner(req.user.id, req.params.id);
    const current = await get('SELECT user_id FROM room_members WHERE room_id=? ORDER BY joined_at ASC LIMIT 1', [req.params.id]);
    await run("UPDATE rooms SET status='active', round=1, current_actor_id=? WHERE id=?", [current?.user_id || null, req.params.id]);
    await addEvent(req.params.id, req.user.id, 'system', { text: 'GM 開始跑團！' });
    res.json(await roomSnapshot(req.params.id));
  } catch (e) { res.status(e.status || 500).json({ error: e.message || '無法開始跑團' }); }
});
app.post('/api/rooms/:id/next-turn', auth, async (req, res) => {
  try {
    await requireOwner(req.user.id, req.params.id);
    const room = await get('SELECT * FROM rooms WHERE id=?', [req.params.id]);
    const members = await all('SELECT user_id FROM room_members WHERE room_id=? ORDER BY joined_at ASC', [req.params.id]);
    if (!members.length) return res.status(400).json({ error: '沒有玩家' });
    const idx = Math.max(0, members.findIndex(m => String(m.user_id) === String(room.current_actor_id)));
    const next = members[(idx + 1) % members.length];
    const nextRound = idx + 1 >= members.length ? room.round + 1 : room.round;
    await run('UPDATE rooms SET round=?, current_actor_id=? WHERE id=?', [nextRound, next.user_id, req.params.id]);
    await addEvent(req.params.id, req.user.id, 'turn', { round: nextRound, actor_id: next.user_id });
    res.json(await roomSnapshot(req.params.id));
  } catch (e) { res.status(e.status || 500).json({ error: e.message || '切換回合失敗' }); }
});
app.patch('/api/rooms/:id/member/:userId', auth, async (req, res) => {
  try {
    await requireRoomMember(req.user.id, req.params.id);
    const target = Number(req.params.userId);
    const owner = await get('SELECT owner_id FROM rooms WHERE id=?', [req.params.id]);
    const allowed = String(req.user.id) === String(target) || (owner && String(owner.owner_id) === String(req.user.id));
    if (!allowed) return res.status(403).json({ error: '沒有權限' });
    const fields = ['hp','max_hp','spirit','max_spirit','display_name'];
    const updates = [];
    const values = [];
    for (const field of fields) if (req.body[field] !== undefined) { updates.push(`${field}=?`); values.push(field === 'display_name' ? String(req.body[field]).slice(0,30) : Math.max(0, Number(req.body[field]) || 0)); }
    if (!updates.length) return res.status(400).json({ error: '沒有可更新欄位' });
    values.push(req.params.id, target);
    await run(`UPDATE room_members SET ${updates.join(',')} WHERE room_id=? AND user_id=?`, values);
    await addEvent(req.params.id, req.user.id, 'character', { target, changes: req.body });
    res.json(await roomSnapshot(req.params.id));
  } catch (e) { res.status(e.status || 500).json({ error: e.message || '更新角色失敗' }); }
});

const server = http.createServer(app);
const io = new Server(server, {
  cors: { origin: true, credentials: false },
  transports: ['websocket', 'polling'],
  pingInterval: 25000,
  pingTimeout: 20000,
  maxHttpBufferSize: 1e6
});
io.use(socketAuth);
const onlineByRoom = new Map();

io.on('connection', async (socket) => {
  socket.on('room:enter', async ({ roomId }) => {
    try {
      await requireRoomMember(socket.user.id, roomId);
      socket.join(`room:${roomId}`);
      if (!onlineByRoom.has(roomId)) onlineByRoom.set(roomId, new Set());
      onlineByRoom.get(roomId).add(socket.user.id);
      io.to(`room:${roomId}`).emit('presence', { online: [...onlineByRoom.get(roomId)] });
      socket.emit('room:snapshot', await roomSnapshot(roomId));
    } catch (e) { socket.emit('error:message', e.message || '進入房間失敗'); }
  });
  socket.on('chat:send', async ({ roomId, text }) => {
    try {
      await requireRoomMember(socket.user.id, roomId);
      const safe = String(text || '').trim().slice(0, 1000);
      if (!safe) return;
      await addEvent(roomId, socket.user.id, 'chat', { text: safe });
      io.to(`room:${roomId}`).emit('room:snapshot', await roomSnapshot(roomId));
    } catch (e) { socket.emit('error:message', e.message || '訊息失敗'); }
  });
  socket.on('dice:roll', async ({ roomId, notation = '1d20', label = '擲骰' }) => {
    try {
      await requireRoomMember(socket.user.id, roomId);
      const m = String(notation).trim().match(/^(\d{1,2})d(\d{1,4})([+-]\d{1,4})?$/i);
      if (!m) return socket.emit('error:message', '骰式格式例如 1d20 或 2d6+3');
      const count = Math.min(20, Number(m[1])); const sides = Math.min(1000, Number(m[2])); const mod = Number(m[3] || 0);
      const rolls = Array.from({ length: count }, () => 1 + Math.floor(Math.random() * sides));
      const total = rolls.reduce((a,b) => a+b, 0) + mod;
      await addEvent(roomId, socket.user.id, 'dice', { notation, label: String(label).slice(0,80), rolls, modifier: mod, total });
      io.to(`room:${roomId}`).emit('room:snapshot', await roomSnapshot(roomId));
    } catch (e) { socket.emit('error:message', e.message || '骰子失敗'); }
  });
  socket.on('disconnect', () => {
    for (const [roomId, set] of onlineByRoom.entries()) {
      if (set.delete(socket.user.id)) {
        io.to(`room:${roomId}`).emit('presence', { online: [...set] });
        if (!set.size) onlineByRoom.delete(roomId);
      }
    }
  });
});

async function shutdown(signal) {
  console.log(`${signal} received, shutting down...`);
  io.close();
  server.close(async () => {
    await pool.end();
    process.exit(0);
  });
}
process.on('SIGTERM', () => shutdown('SIGTERM'));
process.on('SIGINT', () => shutdown('SIGINT'));

initDb()
  .then(() => {
    server.listen(PORT, HOST, () => console.log(`RPG Web MVP listening on ${HOST}:${PORT}`));
  })
  .catch((err) => {
    console.error('Database initialization failed:', err);
    process.exit(1);
  });
