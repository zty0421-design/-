const path = require('path');
const http = require('http');
const express = require('express');
const { Server } = require('socket.io');
const { Pool } = require('pg');
const bcrypt = require('bcryptjs');
const jwt = require('jsonwebtoken');

const PORT = Number(process.env.PORT || 3000);
const HOST = process.env.HOST || '0.0.0.0';

const JWT_SECRET =
  process.env.JWT_SECRET || 'dev-only-change-me';

if (
  process.env.NODE_ENV === 'production' &&
  JWT_SECRET === 'dev-only-change-me'
) {
  throw new Error('JWT_SECRET must be set in production');
}

const DATABASE_URL = process.env.DATABASE_URL;

if (!DATABASE_URL) {
  throw new Error(
    'DATABASE_URL is required'
  );
}

const ADMIN_USERNAME =
  String(process.env.ADMIN_USERNAME || '').trim();

const ADMIN_PASSWORD =
  String(process.env.ADMIN_PASSWORD || '');

const pool = new Pool({
  connectionString: DATABASE_URL,
  max: Number(process.env.PGPOOL_MAX || 10),
  idleTimeoutMillis: 30000,
  connectionTimeoutMillis: 5000,

  ssl:
    process.env.PGSSL === 'disable'
      ? false
      : process.env.PGSSL === 'require'
        ? { rejectUnauthorized: false }
        : undefined
});

/* =========================================================
   DATABASE HELPERS
========================================================= */

function pgSql(sql) {
  let i = 0;

  return sql.replace(/\?/g, function () {
    i += 1;
    return '$' + i;
  });
}

async function query(sql, params) {
  return pool.query(
    pgSql(sql),
    params || []
  );
}

async function get(sql, params) {
  const result = await query(
    sql,
    params || []
  );

  return result.rows[0] || null;
}

async function all(sql, params) {
  const result = await query(
    sql,
    params || []
  );

  return result.rows;
}

async function run(sql, params) {
  const result = await query(
    sql,
    params || []
  );

  const row =
    result.rows[0] || null;

  return {
    id: row && row.id !== undefined
      ? row.id
      : null,

    changes: result.rowCount,

    row
  };
}

/* =========================================================
   DATABASE
========================================================= */

async function initDb() {
  const sql = [
    'CREATE TABLE IF NOT EXISTS users (',
    '  id BIGSERIAL PRIMARY KEY,',
    '  username VARCHAR(20) NOT NULL UNIQUE,',
    '  password_hash TEXT NOT NULL,',
    '  is_admin BOOLEAN NOT NULL DEFAULT FALSE,',
    '  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP',
    ');',

    'CREATE TABLE IF NOT EXISTS rooms (',
    '  id BIGSERIAL PRIMARY KEY,',
    '  code VARCHAR(6) NOT NULL UNIQUE,',
    '  name VARCHAR(60) NOT NULL,',
    '  owner_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,',
    '  status VARCHAR(20) NOT NULL DEFAULT \'lobby\',',
    '  round INTEGER NOT NULL DEFAULT 1,',
    '  current_actor_id BIGINT,',
    '  saved BOOLEAN NOT NULL DEFAULT FALSE,',
    '  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,',
    '  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,',
    '  closed_at TIMESTAMPTZ',
    ');',

    'CREATE TABLE IF NOT EXISTS room_members (',
    '  room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,',
    '  user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,',
    '  role VARCHAR(10) NOT NULL DEFAULT \'player\',',
    '  display_name VARCHAR(30) NOT NULL,',
    '  hp INTEGER NOT NULL DEFAULT 100,',
    '  max_hp INTEGER NOT NULL DEFAULT 100,',
    '  spirit INTEGER NOT NULL DEFAULT 100,',
    '  max_spirit INTEGER NOT NULL DEFAULT 100,',
    '  joined_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,',
    '  PRIMARY KEY(room_id, user_id)',
    ');',

    'CREATE TABLE IF NOT EXISTS events (',
    '  id BIGSERIAL PRIMARY KEY,',
    '  room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,',
    '  user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,',
    '  type VARCHAR(30) NOT NULL,',
    '  payload JSONB NOT NULL,',
    '  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP',
    ');',

    'CREATE TABLE IF NOT EXISTS character_cards (',
    '  id BIGSERIAL PRIMARY KEY,',
    '  user_id BIGINT NOT NULL UNIQUE REFERENCES users(id) ON DELETE CASCADE,',
    '  name VARCHAR(50) NOT NULL DEFAULT \'未命名角色\',',
    '  faction VARCHAR(20) NOT NULL DEFAULT \'東國\',',
    '  tier INTEGER NOT NULL DEFAULT 1,',
    '  level INTEGER NOT NULL DEFAULT 1,',
    '  main_class VARCHAR(50) NOT NULL DEFAULT \'未設定\',',
    '  sub_classes JSONB NOT NULL DEFAULT \'[]\'::jsonb,',
    '  agility INTEGER NOT NULL DEFAULT 0,',
    '  strength INTEGER NOT NULL DEFAULT 0,',
    '  constitution INTEGER NOT NULL DEFAULT 0,',
    '  spirit INTEGER NOT NULL DEFAULT 0,',
    '  great_way INTEGER NOT NULL DEFAULT 0,',
    '  great_way_name VARCHAR(100),',
    '  luck INTEGER NOT NULL DEFAULT 10,',
    '  attribute_points INTEGER NOT NULL DEFAULT 50,',
    '  experience BIGINT NOT NULL DEFAULT 0,',
    '  skill_points INTEGER NOT NULL DEFAULT 0,',
    '  skills JSONB NOT NULL DEFAULT \'[]\'::jsonb,',
    '  equipment JSONB NOT NULL DEFAULT \'[]\'::jsonb,',
    '  weapons JSONB NOT NULL DEFAULT \'[]\'::jsonb,',
    '  affinity_light INTEGER NOT NULL DEFAULT 0,',
    '  affinity_dark INTEGER NOT NULL DEFAULT 0,',
    '  affinity_metal INTEGER NOT NULL DEFAULT 0,',
    '  affinity_wood INTEGER NOT NULL DEFAULT 0,',
    '  affinity_water INTEGER NOT NULL DEFAULT 0,',
    '  affinity_fire INTEGER NOT NULL DEFAULT 0,',
    '  affinity_earth INTEGER NOT NULL DEFAULT 0,',
    '  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,',
    '  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP',
    ');',

    'CREATE TABLE IF NOT EXISTS character_room_bindings (',
    '  room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,',
    '  character_id BIGINT NOT NULL REFERENCES character_cards(id) ON DELETE CASCADE,',
    '  user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,',
    '  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,',
    '  PRIMARY KEY(room_id, user_id),',
    '  UNIQUE(room_id, character_id)',
    ');',

    'CREATE INDEX IF NOT EXISTS idx_room_members_room',
    'ON room_members(room_id);',

    'CREATE INDEX IF NOT EXISTS idx_events_room_id_id',
    'ON events(room_id, id);',

    'CREATE INDEX IF NOT EXISTS idx_character_room_bindings_room',
    'ON character_room_bindings(room_id);',

    'CREATE INDEX IF NOT EXISTS idx_character_room_bindings_character',
    'ON character_room_bindings(character_id);'
  ].join('\n');

  await pool.query(sql);

  /* 舊資料庫升級 */

  await pool.query([
    'ALTER TABLE users',
    'ADD COLUMN IF NOT EXISTS is_admin BOOLEAN NOT NULL DEFAULT FALSE;',

    'ALTER TABLE rooms',
    'ADD COLUMN IF NOT EXISTS saved BOOLEAN NOT NULL DEFAULT FALSE;',

    'ALTER TABLE rooms',
    'ADD COLUMN IF NOT EXISTS updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP;',

    'ALTER TABLE rooms',
    'ADD COLUMN IF NOT EXISTS closed_at TIMESTAMPTZ;'
  ].join('\n'));

  await ensureAdmin();
}

/* =========================================================
   GLOBAL ADMIN
========================================================= */

async function ensureAdmin() {
  if (!ADMIN_USERNAME || !ADMIN_PASSWORD) {
    console.log(
      'ADMIN_USERNAME / ADMIN_PASSWORD not set. Global DM account will not be created automatically.'
    );

    return;
  }

  if (ADMIN_PASSWORD.length < 6) {
    throw new Error(
      'ADMIN_PASSWORD must be at least 6 characters'
    );
  }

  const existing = await get(
    'SELECT id FROM users WHERE username = ?',
    [ADMIN_USERNAME]
  );

  const hash =
    await bcrypt.hash(
      ADMIN_PASSWORD,
      12
    );

  if (existing) {
    await run(
      [
        'UPDATE users',
        'SET password_hash = ?,',
        '    is_admin = TRUE',
        'WHERE id = ?'
      ].join('\n'),
      [
        hash,
        existing.id
      ]
    );

    console.log(
      'Global DM administrator updated: ' +
      ADMIN_USERNAME
    );

    return;
  }

  await run(
    [
      'INSERT INTO users(',
      '  username,',
      '  password_hash,',
      '  is_admin',
      ')',
      'VALUES(?, ?, TRUE)',
      'RETURNING id'
    ].join('\n'),
    [
      ADMIN_USERNAME,
      hash
    ]
  );

  console.log(
    'Global DM administrator created: ' +
    ADMIN_USERNAME
  );
}

/* =========================================================
   AUTH
========================================================= */

function signToken(user) {
  return jwt.sign(
    {
      id: user.id,
      username: user.username,
      is_admin: Boolean(user.is_admin)
    },
    JWT_SECRET,
    {
      expiresIn: '7d'
    }
  );
}

function auth(req, res, next) {
  const raw =
    req.headers.authorization || '';

  const token =
    raw.startsWith('Bearer ')
      ? raw.slice(7)
      : null;

  if (!token) {
    return res.status(401).json({
      error: '需要登入'
    });
  }

  try {
    req.user =
      jwt.verify(
        token,
        JWT_SECRET
      );

    next();
  } catch {
    return res.status(401).json({
      error: '登入已失效'
    });
  }
}

async function requireAdminUser(userId) {
  const user = await get(
    [
      'SELECT id, username, is_admin',
      'FROM users',
      'WHERE id = ?'
    ].join('\n'),
    [userId]
  );

  if (!user || !user.is_admin) {
    throw Object.assign(
      new Error('只有全站管理員 DM 可以執行此操作'),
      {
        status: 403
      }
    );
  }

  return user;
}

async function adminAuth(req, res, next) {
  try {
    await requireAdminUser(
      req.user.id
    );

    next();
  } catch (e) {
    res.status(
      e.status || 403
    ).json({
      error:
        e.message ||
        '沒有管理員權限'
    });
  }
}

function socketAuth(socket, next) {
  try {
    socket.user =
      jwt.verify(
        socket.handshake.auth?.token || '',
        JWT_SECRET
      );

    next();
  } catch {
    next(
      new Error('unauthorized')
    );
  }
}

/* =========================================================
   ROOM CODE
========================================================= */

function generateCode() {
  const alphabet =
    'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';

  let out = '';

  for (let i = 0; i < 6; i += 1) {
    out += alphabet[
      Math.floor(
        Math.random() *
        alphabet.length
      )
    ];
  }

  return out;
}

async function uniqueCode() {
  let code;

  do {
    code = generateCode();
  } while (
    await get(
      'SELECT id FROM rooms WHERE code = ?',
      [code]
    )
  );

  return code;
}

/* =========================================================
   CHARACTER CALCULATIONS
========================================================= */

function calculateCharacter(character) {
  const constitution =
    Number(
      character.constitution
    ) || 0;

  const spirit =
    Number(
      character.spirit
    ) || 0;

  return {
    ...character,

    endurance:
      Math.floor(
        (constitution + spirit) / 2
      ),

    sanity:
      Math.floor(
        spirit * 1.2
      ),

    will:
      Math.floor(
        spirit * 0.8
      ),

    max_hp:
      Math.max(
        0,
        constitution * 2
      )
  };
}

function parseCharacter(character) {
  if (!character) {
    return null;
  }

  return calculateCharacter({
    ...character,

    sub_classes:
      typeof character.sub_classes === 'string'
        ? JSON.parse(character.sub_classes)
        : character.sub_classes,

    skills:
      typeof character.skills === 'string'
        ? JSON.parse(character.skills)
        : character.skills,

    equipment:
      typeof character.equipment === 'string'
        ? JSON.parse(character.equipment)
        : character.equipment,

    weapons:
      typeof character.weapons === 'string'
        ? JSON.parse(character.weapons)
        : character.weapons
  });
}

async function getCharacterByUser(userId) {
  const character =
    await get(
      [
        'SELECT *',
        'FROM character_cards',
        'WHERE user_id = ?'
      ].join('\n'),
      [userId]
    );

  return parseCharacter(
    character
  );
}

async function requireCharacter(userId) {
  const character =
    await getCharacterByUser(
      userId
    );

  if (!character) {
    throw Object.assign(
      new Error(
        '你還沒有建立角色卡'
      ),
      {
        status: 404
      }
    );
  }

  return character;
}

/* =========================================================
   ROOM PERMISSION
========================================================= */

async function requireRoomMember(
  userId,
  roomId
) {
  const row =
    await get(
      [
        'SELECT *',
        'FROM room_members',
        'WHERE room_id = ?',
        'AND user_id = ?'
      ].join('\n'),
      [
        roomId,
        userId
      ]
    );

  if (!row) {
    throw Object.assign(
      new Error(
        '你不是房間成員'
      ),
      {
        status: 403
      }
    );
  }

  return row;
}

async function requireOwner(
  userId,
  roomId
) {
  const row =
    await get(
      [
        'SELECT *',
        'FROM rooms',
        'WHERE id = ?',
        'AND owner_id = ?'
      ].join('\n'),
      [
        roomId,
        userId
      ]
    );

  if (!row) {
    throw Object.assign(
      new Error(
        '只有房間 DM 可以執行此操作'
      ),
      {
        status: 403
      }
    );
  }

  return row;
}

/*
 * 全站 DM 可以查看／管理任何房間。
 */

async function requireRoomOwnerOrAdmin(
  userId,
  roomId
) {
  const user =
    await get(
      [
        'SELECT id, is_admin',
        'FROM users',
        'WHERE id = ?'
      ].join('\n'),
      [userId]
    );

  if (user && user.is_admin) {
    const room =
      await get(
        'SELECT * FROM rooms WHERE id = ?',
        [roomId]
      );

    if (!room) {
      throw Object.assign(
        new Error('找不到房間'),
        {
          status: 404
        }
      );
    }

    return room;
  }

  return requireOwner(
    userId,
    roomId
  );
}

/* =========================================================
   EVENTS
========================================================= */

async function addEvent(
  roomId,
  userId,
  type,
  payload
) {
  await run(
    [
      'INSERT INTO events(',
      '  room_id,',
      '  user_id,',
      '  type,',
      '  payload',
      ')',
      'VALUES(?, ?, ?, ?::jsonb)'
    ].join('\n'),
    [
      roomId,
      userId,
      type,
      JSON.stringify(
        payload || {}
      )
    ]
  );

  await run(
    [
      'UPDATE rooms',
      'SET updated_at = CURRENT_TIMESTAMP',
      'WHERE id = ?'
    ].join('\n'),
    [roomId]
  );
}

/* =========================================================
   ROOM SNAPSHOT
========================================================= */

async function roomSnapshot(roomId) {
  const room =
    await get(
      [
        'SELECT',
        '  id,',
        '  code,',
        '  name,',
        '  owner_id,',
        '  status,',
        '  round,',
        '  current_actor_id,',
        '  saved,',
        '  created_at,',
        '  updated_at,',
        '  closed_at',
        'FROM rooms',
        'WHERE id = ?'
      ].join('\n'),
      [roomId]
    );

  if (!room) {
    return null;
  }

  const members =
    await all(
      [
        'SELECT',
        '  rm.user_id AS id,',
        '  rm.display_name,',
        '  rm.role,',
        '  rm.hp,',
        '  rm.max_hp,',
        '  rm.spirit,',
        '  rm.max_spirit,',
        '  rm.joined_at,',
        '  u.username,',
        '  u.is_admin,',
        '  crb.character_id',
        'FROM room_members rm',
        'JOIN users u',
        '  ON u.id = rm.user_id',
        'LEFT JOIN character_room_bindings crb',
        '  ON crb.room_id = rm.room_id',
        '  AND crb.user_id = rm.user_id',
        'WHERE rm.room_id = ?',
        'ORDER BY rm.joined_at ASC'
      ].join('\n'),
      [roomId]
    );

  const events =
    await all(
      [
        'SELECT',
        '  e.id,',
        '  e.type,',
        '  e.payload,',
        '  e.created_at,',
        '  u.username',
        'FROM events e',
        'LEFT JOIN users u',
        '  ON u.id = e.user_id',
        'WHERE e.room_id = ?',
        'ORDER BY e.id DESC',
        'LIMIT 100'
      ].join('\n'),
      [roomId]
    );

  return {
    room,

    members,

    events:
      events
        .reverse()
        .map(function (e) {
          return {
            ...e,

            payload:
              typeof e.payload === 'string'
                ? JSON.parse(e.payload)
                : e.payload
          };
        })
  };
}

/* =========================================================
   EXPRESS
========================================================= */

const app = express();

app.set(
  'trust proxy',
  1
);

app.disable(
  'x-powered-by'
);

app.use(
  express.json({
    limit: '2mb'
  })
);

/* =========================================================
   HEALTH
========================================================= */

app.get(
  '/api/health',
  async function (req, res) {
    try {
      await pool.query(
        'SELECT 1'
      );

      res.json({
        ok: true,
        service: 'trpg-online',
        database: 'ok',
        time:
          new Date().toISOString()
      });
    } catch {
      res.status(503).json({
        ok: false,
        database: 'unavailable'
      });
    }
  }
);

/* =========================================================
   STATIC
========================================================= */

app.use(
  express.static(
    path.join(
      __dirname,
      'public'
    )
  )
);

/* =========================================================
   REGISTER
========================================================= */

app.post(
  '/api/auth/register',
  async function (req, res) {
    try {
      const username =
        String(
          req.body.username || ''
        )
          .trim();

      const password =
        String(
          req.body.password || ''
        );

      if (
        !/^[\w\u4e00-\u9fff-]{2,20}$/.test(
          username
        )
      ) {
        return res.status(400).json({
          error:
            '帳號需為2-20字，可用中文、英文、數字、底線或連字號'
        });
      }

      if (password.length < 6) {
        return res.status(400).json({
          error:
            '密碼至少6碼'
        });
      }

      const hash =
        await bcrypt.hash(
          password,
          10
        );

      const result =
        await run(
          [
            'INSERT INTO users(',
            '  username,',
            '  password_hash',
            ')',
            'VALUES(?, ?)',
            'RETURNING id'
          ].join('\n'),
          [
            username,
            hash
          ]
        );

      const user = {
        id: result.id,
        username,
        is_admin: false
      };

      res.json({
        token:
          signToken(user),

        user
      });
    } catch (err) {
      res.status(
        err.code === '23505'
          ? 409
          : 500
      ).json({
        error:
          err.code === '23505'
            ? '帳號已存在'
            : '註冊失敗'
      });
    }
  }
);

/* =========================================================
   LOGIN
========================================================= */

app.post(
  '/api/auth/login',
  async function (req, res) {
    try {
      const username =
        String(
          req.body.username || ''
        ).trim();

      const password =
        String(
          req.body.password || ''
        );

      const user =
        await get(
          [
            'SELECT',
            '  id,',
            '  username,',
            '  password_hash,',
            '  is_admin',
            'FROM users',
            'WHERE username = ?'
          ].join('\n'),
          [username]
        );

      if (
        !user ||
        !(await bcrypt.compare(
          password,
          user.password_hash
        ))
      ) {
        return res.status(401).json({
          error:
            '帳號或密碼錯誤'
        });
      }

      const safeUser = {
        id: user.id,
        username: user.username,
        is_admin:
          Boolean(user.is_admin)
      };

      res.json({
        token:
          signToken(
            safeUser
          ),

        user:
          safeUser
      });
    } catch {
      res.status(500).json({
        error:
          '登入失敗'
      });
    }
  }
);

/* =========================================================
   CURRENT USER
========================================================= */

app.get(
  '/api/me',
  auth,
  async function (req, res) {
    try {
      const user =
        await get(
          [
            'SELECT',
            '  id,',
            '  username,',
            '  is_admin',
            'FROM users',
            'WHERE id = ?'
          ].join('\n'),
          [req.user.id]
        );

      if (!user) {
        return res.status(401).json({
          error:
            '帳號不存在'
        });
      }

      res.json({
        user: {
          id: user.id,
          username: user.username,
          is_admin:
            Boolean(
              user.is_admin
            )
        }
      });
    } catch {
      res.status(500).json({
        error:
          '讀取使用者資料失敗'
      });
    }
  }
);

/* =========================================================
   GLOBAL ADMIN INFO
========================================================= */

app.get(
  '/api/admin/me',
  auth,
  adminAuth,
  async function (req, res) {
    const user =
      await requireAdminUser(
        req.user.id
      );

    res.json({
      admin: {
        id: user.id,
        username: user.username,
        is_admin: true
      }
    });
  }
);

/* =========================================================
   ADMIN - USERS
========================================================= */

app.get(
  '/api/admin/users',
  auth,
  adminAuth,
  async function (req, res) {
    try {
      const users =
        await all(
          [
            'SELECT',
            '  u.id,',
            '  u.username,',
            '  u.is_admin,',
            '  u.created_at,',
            '  c.id AS character_id,',
            '  c.name AS character_name',
            'FROM users u',
            'LEFT JOIN character_cards c',
            '  ON c.user_id = u.id',
            'ORDER BY u.id ASC'
          ].join('\n')
        );

      res.json({
        users
      });
    } catch {
      res.status(500).json({
        error:
          '讀取全站使用者失敗'
      });
    }
  }
);

/* =========================================================
   ADMIN - ROOMS
========================================================= */

app.get(
  '/api/admin/rooms',
  auth,
  adminAuth,
  async function (req, res) {
    try {
      const rooms =
        await all(
          [
            'SELECT',
            '  r.*,',
            '  u.username AS owner_username,',
            '  COUNT(rm.user_id)::INTEGER AS member_count',
            'FROM rooms r',
            'JOIN users u',
            '  ON u.id = r.owner_id',
            'LEFT JOIN room_members rm',
            '  ON rm.room_id = r.id',
            'GROUP BY r.id, u.username',
            'ORDER BY r.updated_at DESC'
          ].join('\n')
        );

      res.json({
        rooms
      });
    } catch {
      res.status(500).json({
        error:
          '讀取全站房間失敗'
      });
    }
  }
);

/* =========================================================
   ADMIN - ALL CHARACTERS
========================================================= */

app.get(
  '/api/admin/characters',
  auth,
  adminAuth,
  async function (req, res) {
    try {
      const rows =
        await all(
          [
            'SELECT',
            '  c.*,',
            '  u.username',
            'FROM character_cards c',
            'JOIN users u',
            '  ON u.id = c.user_id',
            'ORDER BY c.id ASC'
          ].join('\n')
        );

      res.json({
        characters:
          rows.map(
            parseCharacter
          )
      });
    } catch {
      res.status(500).json({
        error:
          '讀取全站角色卡失敗'
      });
    }
  }
);

/* =========================================================
   ADMIN - VIEW ROOM
========================================================= */

app.get(
  '/api/admin/rooms/:id',
  auth,
  adminAuth,
  async function (req, res) {
    try {
      const snapshot =
        await roomSnapshot(
          req.params.id
        );

      if (!snapshot) {
        return res.status(404).json({
          error:
            '找不到房間'
        });
      }

      res.json(
        snapshot
      );
    } catch {
      res.status(500).json({
        error:
          '讀取房間失敗'
      });
    }
  }
);

/* =========================================================
   ADMIN - VIEW ROOM CHARACTERS
========================================================= */

app.get(
  '/api/admin/rooms/:id/characters',
  auth,
  adminAuth,
  async function (req, res) {
    try {
      const characters =
        await all(
          [
            'SELECT',
            '  c.*,',
            '  u.username',
            'FROM character_cards c',
            'JOIN character_room_bindings crb',
            '  ON crb.character_id = c.id',
            'JOIN users u',
            '  ON u.id = c.user_id',
            'WHERE crb.room_id = ?',
            'ORDER BY c.id ASC'
          ].join('\n'),
          [req.params.id]
        );

      res.json({
        characters:
          characters.map(
            parseCharacter
          )
      });
    } catch {
      res.status(500).json({
        error:
          '讀取房間角色卡失敗'
      });
    }
  }
);

/* =========================================================
   ADMIN - CLOSE ROOM
========================================================= */

app.post(
  '/api/admin/rooms/:id/close',
  auth,
  adminAuth,
  async function (req, res) {
    try {
      const room =
        await get(
          'SELECT * FROM rooms WHERE id = ?',
          [req.params.id]
        );

      if (!room) {
        return res.status(404).json({
          error:
            '找不到房間'
        });
      }

      await run(
        [
          'UPDATE rooms',
          'SET',
          '  saved = TRUE,',
          '  status = \'closed\',',
          '  closed_at = CURRENT_TIMESTAMP,',
          '  updated_at = CURRENT_TIMESTAMP',
          'WHERE id = ?'
        ].join('\n'),
        [req.params.id]
      );

      await addEvent(
        req.params.id,
        req.user.id,
        'admin',
        {
          text:
            '全站 DM 已結束本次跑團並保存房間。'
        }
      );

      io.to(
        'room:' + req.params.id
      ).emit(
        'room:snapshot',
        await roomSnapshot(
          req.params.id
        )
      );

      res.json({
        ok: true,
        room:
          await roomSnapshot(
            req.params.id
          )
      });
    } catch (e) {
      res.status(
        e.status || 500
      ).json({
        error:
          e.message ||
          '關閉房間失敗'
      });
    }
  }
);

/* =========================================================
   ADMIN - REOPEN ROOM
========================================================= */

app.post(
  '/api/admin/rooms/:id/reopen',
  auth,
  adminAuth,
  async function (req, res) {
    try {
      const room =
        await get(
          'SELECT * FROM rooms WHERE id = ?',
          [req.params.id]
        );

      if (!room) {
        return res.status(404).json({
          error:
            '找不到房間'
        });
      }

      await run(
        [
          'UPDATE rooms',
          'SET',
          '  status = \'lobby\',',
          '  saved = FALSE,',
          '  closed_at = NULL,',
          '  updated_at = CURRENT_TIMESTAMP',
          'WHERE id = ?'
        ].join('\n'),
        [req.params.id]
      );

      await addEvent(
        req.params.id,
        req.user.id,
        'admin',
        {
          text:
            '全站 DM 重新開啟了這場跑團。'
        }
      );

      io.to(
        'room:' + req.params.id
      ).emit(
        'room:snapshot',
        await roomSnapshot(
          req.params.id
        )
      );

      res.json({
        ok: true,
        room:
          await roomSnapshot(
            req.params.id
          )
      });
    } catch (e) {
      res.status(
        e.status || 500
      ).json({
        error:
          e.message ||
          '重新開啟房間失敗'
      });
    }
  }
);

/* =========================================================
   CHARACTER GET
========================================================= */

app.get(
  '/api/character',
  auth,
  async function (req, res) {
    try {
      res.json({
        character:
          await getCharacterByUser(
            req.user.id
          )
      });
    } catch {
      res.status(500).json({
        error:
          '讀取角色卡失敗'
      });
    }
  }
);

/* =========================================================
   CHARACTER CREATE
========================================================= */

app.post(
  '/api/character',
  auth,
  async function (req, res) {
    try {
      const exists =
        await get(
          [
            'SELECT id',
            'FROM character_cards',
            'WHERE user_id = ?'
          ].join('\n'),
          [req.user.id]
        );

      if (exists) {
        return res.status(409).json({
          error:
            '你已經有角色卡，每個玩家只能擁有一張角色卡'
        });
      }

      const name =
        String(
          req.body.name ||
          '未命名角色'
        )
          .trim()
          .slice(0, 50);

      const faction =
        req.body.faction === '西國'
          ? '西國'
          : '東國';

      const mainClass =
        String(
          req.body.main_class ||
          '未設定'
        )
          .trim()
          .slice(0, 50);

      const subClasses =
        Array.isArray(
          req.body.sub_classes
        )
          ? req.body.sub_classes.slice(0, 3)
          : [];

      const skills =
        Array.isArray(
          req.body.skills
        )
          ? req.body.skills
          : [];

      const equipment =
        Array.isArray(
          req.body.equipment
        )
          ? req.body.equipment
          : [];

      const weapons =
        Array.isArray(
          req.body.weapons
        )
          ? req.body.weapons
          : [];

      const luck =
        Math.max(
          0,
          Number(
            req.body.luck || 10
          ) || 10
        );

      const result =
        await run(
          [
            'INSERT INTO character_cards(',
            '  user_id,',
            '  name,',
            '  faction,',
            '  tier,',
            '  level,',
            '  main_class,',
            '  sub_classes,',
            '  luck,',
            '  attribute_points,',
            '  skills,',
            '  equipment,',
            '  weapons,',
            '  affinity_light,',
            '  affinity_dark,',
            '  affinity_metal,',
            '  affinity_wood,',
            '  affinity_water,',
            '  affinity_fire,',
            '  affinity_earth',
            ')',
            'VALUES(',
            '  ?, ?, ?, ?, ?, ?, ?, ?::jsonb, ?, ?,',
            '  ?::jsonb, ?::jsonb, ?::jsonb,',
            '  ?, ?, ?, ?, ?, ?, ?',
            ')',
            'RETURNING id'
          ].join('\n'),
          [
            req.user.id,
            name,
            faction,
            1,
            1,
            mainClass,
            JSON.stringify(
              subClasses
            ),
            luck,
            50,
            JSON.stringify(
              skills
            ),
            JSON.stringify(
              equipment
            ),
            JSON.stringify(
              weapons
            ),
            0,
            0,
            0,
            0,
            0,
            0,
            0
          ]
        );

      res.json({
        character:
          await getCharacterByUser(
            req.user.id
          )
      });
    } catch (e) {
      res.status(
        e.code === '23505'
          ? 409
          : 500
      ).json({
        error:
          e.code === '23505'
            ? '你已經有角色卡'
            : '建立角色卡失敗'
      });
    }
  }
);

/* =========================================================
   CHARACTER UPDATE
========================================================= */

app.patch(
  '/api/character',
  auth,
  async function (req, res) {
    try {
      await requireCharacter(
        req.user.id
      );

      const numericFields = [
        'tier',
        'level',
        'agility',
        'strength',
        'constitution',
        'spirit',
        'great_way',
        'luck',
        'attribute_points',
        'experience',
        'skill_points',
        'affinity_light',
        'affinity_dark',
        'affinity_metal',
        'affinity_wood',
        'affinity_water',
        'affinity_fire',
        'affinity_earth'
      ];

      const textFields = [
        'name',
        'faction',
        'main_class',
        'great_way_name'
      ];

      const updates = [];
      const values = [];

      for (const field of numericFields) {
        if (
          req.body[field] !==
          undefined
        ) {
          updates.push(
            field + ' = ?'
          );

          values.push(
            Math.max(
              0,
              Number(
                req.body[field]
              ) || 0
            )
          );
        }
      }

      for (const field of textFields) {
        if (
          req.body[field] !==
          undefined
        ) {
          updates.push(
            field + ' = ?'
          );

          values.push(
            String(
              req.body[field]
            ).slice(0, 100)
          );
        }
      }

      const jsonFields = [
        'sub_classes',
        'skills',
        'equipment',
        'weapons'
      ];

      for (const field of jsonFields) {
        if (
          req.body[field] !==
          undefined
        ) {
          let value =
            Array.isArray(
              req.body[field]
            )
              ? req.body[field]
              : [];

          if (
            field ===
            'sub_classes'
          ) {
            value =
              value.slice(0, 3);
          }

          updates.push(
            field + ' = ?::jsonb'
          );

          values.push(
            JSON.stringify(
              value
            )
          );
        }
      }

      if (!updates.length) {
        return res.status(400).json({
          error:
            '沒有可更新欄位'
        });
      }

      updates.push(
        'updated_at = CURRENT_TIMESTAMP'
      );

      values.push(
        req.user.id
      );

      await run(
        [
          'UPDATE character_cards',
          'SET ' +
            updates.join(', '),
          'WHERE user_id = ?'
        ].join('\n'),
        values
      );

      res.json({
        character:
          await getCharacterByUser(
            req.user.id
          )
      });
    } catch (e) {
      res.status(
        e.status || 500
      ).json({
        error:
          e.message ||
          '更新角色卡失敗'
      });
    }
  }
);

/* =========================================================
   ADD SKILL
========================================================= */

app.post(
  '/api/character/skill',
  auth,
  async function (req, res) {
    try {
      const character =
        await requireCharacter(
          req.user.id
        );

      const skill = {
        id:
          String(
            req.body.id ||
            'skill_' +
              Date.now()
          ).slice(0, 50),

        name:
          String(
            req.body.name ||
            '未命名技能'
          ).slice(0, 100),

        type:
          String(
            req.body.type ||
            'active'
          ).slice(0, 30),

        level:
          Math.max(
            1,
            Number(
              req.body.level ||
              1
            )
          ),

        description:
          String(
            req.body.description ||
            ''
          ).slice(0, 2000),

        cost:
          Math.max(
            0,
            Number(
              req.body.cost ||
              0
            )
          ),

        data:
          req.body.data || {}
      };

      const skills =
        Array.isArray(
          character.skills
        )
          ? character.skills
          : [];

      skills.push(
        skill
      );

      await run(
        [
          'UPDATE character_cards',
          'SET',
          '  skills = ?::jsonb,',
          '  updated_at = CURRENT_TIMESTAMP',
          'WHERE user_id = ?'
        ].join('\n'),
        [
          JSON.stringify(
            skills
          ),
          req.user.id
        ]
      );

      res.json({
        character:
          await getCharacterByUser(
            req.user.id
          )
      });
    } catch (e) {
      res.status(
        e.status || 500
      ).json({
        error:
          e.message ||
          '新增技能失敗'
      });
    }
  }
);

/* =========================================================
   CREATE ROOM
========================================================= */

app.post(
  '/api/rooms',
  auth,
  async function (req, res) {
    try {
      const name =
        String(
          req.body.name ||
          '未命名跑團'
        )
          .trim()
          .slice(0, 60) ||
          '未命名跑團';

      const character =
        await requireCharacter(
          req.user.id
        );

      const code =
        await uniqueCode();

      const result =
        await run(
          [
            'INSERT INTO rooms(',
            '  code,',
            '  name,',
            '  owner_id,',
            '  status,',
            '  saved',
            ')',
            'VALUES(?, ?, ?, \'lobby\', FALSE)',
            'RETURNING id'
          ].join('\n'),
          [
            code,
            name,
            req.user.id
          ]
        );

      await run(
        [
          'INSERT INTO room_members(',
          '  room_id,',
          '  user_id,',
          '  role,',
          '  display_name,',
          '  hp,',
          '  max_hp,',
          '  spirit,',
          '  max_spirit',
          ')',
          'VALUES(?, ?, \'gm\', ?, ?, ?, ?, ?)'
        ].join('\n'),
        [
          result.id,
          req.user.id,
          req.user.username,
          character.max_hp,
          character.max_hp,
          character.spirit,
          character.spirit
        ]
      );

      await run(
        [
          'INSERT INTO character_room_bindings(',
          '  room_id,',
          '  character_id,',
          '  user_id',
          ')',
          'VALUES(?, ?, ?)'
        ].join('\n'),
        [
          result.id,
          character.id,
          req.user.id
        ]
      );

      await addEvent(
        result.id,
        req.user.id,
        'system',
        {
          text:
            req.user.username +
            ' 建立了跑團「' +
            name +
            '」'
        }
      );

      res.json(
        await roomSnapshot(
          result.id
        )
      );
    } catch (e) {
      res.status(
        e.status || 500
      ).json({
        error:
          e.message ||
          '建立房間失敗'
      });
    }
  }
);

/* =========================================================
   JOIN ROOM
========================================================= */

app.post(
  '/api/rooms/join',
  auth,
  async function (req, res) {
    try {
      const code =
        String(
          req.body.code || ''
        )
          .trim()
          .toUpperCase();

      const room =
        await get(
          'SELECT * FROM rooms WHERE code = ?',
          [code]
        );

      if (!room) {
        return res.status(404).json({
          error:
            '找不到房間'
        });
      }

      if (
        room.status === 'closed'
      ) {
        return res.status(400).json({
          error:
            '這個房間目前已結束'
        });
      }

      const character =
        await requireCharacter(
          req.user.id
        );

      const existing =
        await get(
          [
            'SELECT *',
            'FROM room_members',
            'WHERE room_id = ?',
            'AND user_id = ?'
          ].join('\n'),
          [
            room.id,
            req.user.id
          ]
        );

      if (!existing) {
        await run(
          [
            'INSERT INTO room_members(',
            '  room_id,',
            '  user_id,',
            '  role,',
            '  display_name,',
            '  hp,',
            '  max_hp,',
            '  spirit,',
            '  max_spirit',
            ')',
            'VALUES(?, ?, \'player\', ?, ?, ?, ?, ?)'
          ].join('\n'),
          [
            room.id,
            req.user.id,
            character.name,
            character.max_hp,
            character.max_hp,
            character.spirit,
            character.spirit
          ]
        );

        await run(
          [
            'INSERT INTO character_room_bindings(',
            '  room_id,',
            '  character_id,',
            '  user_id',
            ')',
            'VALUES(?, ?, ?)',
            'ON CONFLICT DO NOTHING'
          ].join('\n'),
          [
            room.id,
            character.id,
            req.user.id
          ]
        );

        await addEvent(
          room.id,
          req.user.id,
          'system',
          {
            text:
              character.name +
              ' 加入房間'
          }
        );
      }

      res.json(
        await roomSnapshot(
          room.id
        )
      );
    } catch (e) {
      res.status(
        e.status || 500
      ).json({
        error:
          e.message ||
          '加入房間失敗'
      });
    }
  }
);

/* =========================================================
   ROOM LIST
========================================================= */

app.get(
  '/api/rooms',
  auth,
  async function (req, res) {
    try {
      const rooms =
        await all(
          [
            'SELECT DISTINCT',
            '  r.id,',
            '  r.code,',
            '  r.name,',
            '  r.owner_id,',
            '  r.status,',
            '  r.round,',
            '  r.saved,',
            '  r.created_at,',
            '  r.updated_at',
            'FROM rooms r',
            'JOIN room_members rm',
            '  ON rm.room_id = r.id',
            'WHERE rm.user_id = ?',
            'ORDER BY r.updated_at DESC'
          ].join('\n'),
          [req.user.id]
        );

      res.json({
        rooms
      });
    } catch {
      res.status(500).json({
        error:
          '讀取房間列表失敗'
      });
    }
  }
);

/* =========================================================
   ROOM SNAPSHOT
========================================================= */

app.get(
  '/api/rooms/:id',
  auth,
  async function (req, res) {
    try {
      const user =
        await get(
          [
            'SELECT id, is_admin',
            'FROM users',
            'WHERE id = ?'
          ].join('\n'),
          [req.user.id]
        );

      if (!user?.is_admin) {
        await requireRoomMember(
          req.user.id,
          req.params.id
        );
      }

      const snapshot =
        await roomSnapshot(
          req.params.id
        );

      if (!snapshot) {
        return res.status(404).json({
          error:
            '找不到房間'
        });
      }

      res.json(
        snapshot
      );
    } catch (e) {
      res.status(
        e.status || 500
      ).json({
        error:
          e.message ||
          '讀取房間失敗'
      });
    }
  }
);

/* =========================================================
   ROOM CHARACTERS
========================================================= */

app.get(
  '/api/rooms/:id/characters',
  auth,
  async function (req, res) {
    try {
      await requireRoomOwnerOrAdmin(
        req.user.id,
        req.params.id
      );

      const characters =
        await all(
          [
            'SELECT',
            '  c.*,',
            '  u.username',
            'FROM character_cards c',
            'JOIN character_room_bindings crb',
            '  ON crb.character_id = c.id',
            'JOIN users u',
            '  ON u.id = c.user_id',
            'WHERE crb.room_id = ?',
            'ORDER BY c.id ASC'
          ].join('\n'),
          [req.params.id]
        );

      res.json({
        characters:
          characters.map(
            parseCharacter
          )
      });
    } catch (e) {
      res.status(
        e.status || 500
      ).json({
        error:
          e.message ||
          '讀取角色卡失敗'
      });
    }
  }
);

/* =========================================================
   START ROOM
========================================================= */

app.post(
  '/api/rooms/:id/start',
  auth,
  async function (req, res) {
    try {
      await requireRoomOwnerOrAdmin(
        req.user.id,
        req.params.id
      );

      const current =
        await get(
          [
            'SELECT user_id',
            'FROM room_members',
            'WHERE room_id = ?',
            'AND role = \'player\'',
            'ORDER BY joined_at ASC',
            'LIMIT 1'
          ].join('\n'),
          [req.params.id]
        );

      await run(
        [
          'UPDATE rooms',
          'SET',
          '  status = \'active\',',
          '  saved = FALSE,',
          '  round = 1,',
          '  current_actor_id = ?,',
          '  updated_at = CURRENT_TIMESTAMP',
          'WHERE id = ?'
        ].join('\n'),
        [
          current
            ? current.user_id
            : null,
          req.params.id
        ]
      );

      await addEvent(
        req.params.id,
        req.user.id,
        'system',
        {
          text:
            'DM 開始跑團！'
        }
      );

      const snapshot =
        await roomSnapshot(
          req.params.id
        );

      io.to(
        'room:' + req.params.id
      ).emit(
        'room:snapshot',
        snapshot
      );

      res.json(
        snapshot
      );
    } catch (e) {
      res.status(
        e.status || 500
      ).json({
        error:
          e.message ||
          '無法開始跑團'
      });
    }
  }
);

/* =========================================================
   NEXT TURN
========================================================= */

app.post(
  '/api/rooms/:id/next-turn',
  auth,
  async function (req, res) {
    try {
      await requireRoomOwnerOrAdmin(
        req.user.id,
        req.params.id
      );

      const room =
        await get(
          'SELECT * FROM rooms WHERE id = ?',
          [req.params.id]
        );

      if (
        room.status !== 'active'
      ) {
        return res.status(400).json({
          error:
            '跑團尚未開始'
        });
      }

      const members =
        await all(
          [
            'SELECT user_id',
            'FROM room_members',
            'WHERE room_id = ?',
            'AND role = \'player\'',
            'ORDER BY joined_at ASC'
          ].join('\n'),
          [req.params.id]
        );

      if (!members.length) {
        return res.status(400).json({
          error:
            '沒有玩家'
        });
      }

      const idx =
        Math.max(
          0,
          members.findIndex(
            function (m) {
              return (
                String(
                  m.user_id
                ) ===
                String(
                  room.current_actor_id
                )
              );
            }
          )
        );

      const next =
        members[
          (idx + 1) %
          members.length
        ];

      const nextRound =
        idx + 1 >= members.length
          ? room.round + 1
          : room.round;

      await run(
        [
          'UPDATE rooms',
          'SET',
          '  round = ?,',
          '  current_actor_id = ?,',
          '  updated_at = CURRENT_TIMESTAMP',
          'WHERE id = ?'
        ].join('\n'),
        [
          nextRound,
          next.user_id,
          req.params.id
        ]
      );

      await addEvent(
        req.params.id,
        req.user.id,
        'turn',
        {
          round:
            nextRound,

          actor_id:
            next.user_id
        }
      );

      const snapshot =
        await roomSnapshot(
          req.params.id
        );

      io.to(
        'room:' + req.params.id
      ).emit(
        'room:snapshot',
        snapshot
      );

      res.json(
        snapshot
      );
    } catch (e) {
      res.status(
        e.status || 500
      ).json({
        error:
          e.message ||
          '切換回合失敗'
      });
    }
  }
);

/* =========================================================
   SAVE ROOM
========================================================= */

app.post(
  '/api/rooms/:id/save',
  auth,
  async function (req, res) {
    try {
      await requireRoomOwnerOrAdmin(
        req.user.id,
        req.params.id
      );

      await run(
        [
          'UPDATE rooms',
          'SET',
          '  saved = TRUE,',
          '  status = \'saved\',',
          '  updated_at = CURRENT_TIMESTAMP',
          'WHERE id = ?'
        ].join('\n'),
        [req.params.id]
      );

      await addEvent(
        req.params.id,
        req.user.id,
        'system',
        {
          text:
            'DM 已將跑團存檔。'
        }
      );

      const snapshot =
        await roomSnapshot(
          req.params.id
        );

      io.to(
        'room:' + req.params.id
      ).emit(
        'room:snapshot',
        snapshot
      );

      res.json(
        snapshot
      );
    } catch (e) {
      res.status(
        e.status || 500
      ).json({
        error:
          e.message ||
          '存檔失敗'
      });
    }
  }
);

/* =========================================================
   DM EXIT / CLOSE ROOM
========================================================= */

app.post(
  '/api/rooms/:id/leave',
  auth,
  async function (req, res) {
    try {
      await requireRoomOwnerOrAdmin(
        req.user.id,
        req.params.id
      );

      await run(
        [
          'UPDATE rooms',
          'SET',
          '  saved = TRUE,',
          '  status = \'closed\',',
          '  closed_at = CURRENT_TIMESTAMP,',
          '  updated_at = CURRENT_TIMESTAMP',
          'WHERE id = ?'
        ].join('\n'),
        [req.params.id]
      );

      await addEvent(
        req.params.id,
        req.user.id,
        'system',
        {
          text:
            'DM 結束本次跑團，房間已存檔。'
        }
      );

      const snapshot =
        await roomSnapshot(
          req.params.id
        );

      io.to(
        'room:' + req.params.id
      ).emit(
        'room:snapshot',
        snapshot
      );

      res.json({
        ok: true,
        saved: true,
        message:
          '房間已存檔並結束'
      });
    } catch (e) {
      res.status(
        e.status || 500
      ).json({
        error:
          e.message ||
          '結束房間失敗'
      });
    }
  }
);

/* =========================================================
   REOPEN ROOM
========================================================= */

app.post(
  '/api/rooms/:id/reopen',
  auth,
  async function (req, res) {
    try {
      await requireRoomOwnerOrAdmin(
        req.user.id,
        req.params.id
      );

      await run(
        [
          'UPDATE rooms',
          'SET',
          '  status = \'lobby\',',
          '  saved = FALSE,',
          '  closed_at = NULL,',
          '  updated_at = CURRENT_TIMESTAMP',
          'WHERE id = ?'
        ].join('\n'),
        [req.params.id]
      );

      await addEvent(
        req.params.id,
        req.user.id,
        'system',
        {
          text:
            'DM 重新開啟了這場跑團。'
        }
      );

      const snapshot =
        await roomSnapshot(
          req.params.id
        );

      io.to(
        'room:' + req.params.id
      ).emit(
        'room:snapshot',
        snapshot
      );

      res.json(
        snapshot
      );
    } catch (e) {
      res.status(
        e.status || 500
      ).json({
        error:
          e.message ||
          '重新開啟房間失敗'
      });
    }
  }
);

/* =========================================================
   UPDATE ROOM MEMBER
========================================================= */

app.patch(
  '/api/rooms/:id/member/:userId',
  auth,
  async function (req, res) {
    try {
      await requireRoomMember(
        req.user.id,
        req.params.id
      );

      const target =
        Number(
          req.params.userId
        );

      const room =
        await get(
          [
            'SELECT owner_id',
            'FROM rooms',
            'WHERE id = ?'
          ].join('\n'),
          [req.params.id]
        );

      const currentUser =
        await get(
          [
            'SELECT is_admin',
            'FROM users',
            'WHERE id = ?'
          ].join('\n'),
          [req.user.id]
        );

      const isOwner =
        room &&
        String(
          room.owner_id
        ) ===
        String(
          req.user.id
        );

      const isAdmin =
        Boolean(
          currentUser &&
          currentUser.is_admin
        );

      const isSelf =
        String(
          req.user.id
        ) ===
        String(target);

      if (
        !isSelf &&
        !isOwner &&
        !isAdmin
      ) {
        return res.status(403).json({
          error:
            '沒有權限'
        });
      }

      const fields = [
        'hp',
        'max_hp',
        'spirit',
        'max_spirit',
        'display_name'
      ];

      const updates = [];
      const values = [];

      for (const field of fields) {
        if (
          req.body[field] !==
          undefined
        ) {
          updates.push(
            field + ' = ?'
          );

          values.push(
            field ===
            'display_name'
              ? String(
                  req.body[field]
                ).slice(0, 30)
              : Math.max(
                  0,
                  Number(
                    req.body[field]
                  ) || 0
                )
          );
        }
      }

      if (!updates.length) {
        return res.status(400).json({
          error:
            '沒有可更新欄位'
        });
      }

      values.push(
        req.params.id,
        target
      );

      await run(
        [
          'UPDATE room_members',
          'SET ' +
            updates.join(', '),
          'WHERE room_id = ?',
          'AND user_id = ?'
        ].join('\n'),
        values
      );

      await addEvent(
        req.params.id,
        req.user.id,
        'character',
        {
          target,
          changes:
            req.body
        }
      );

      const snapshot =
        await roomSnapshot(
          req.params.id
        );

      io.to(
        'room:' + req.params.id
      ).emit(
        'room:snapshot',
        snapshot
      );

      res.json(
        snapshot
      );
    } catch (e) {
      res.status(
        e.status || 500
      ).json({
        error:
          e.message ||
          '更新角色失敗'
      });
    }
  }
);

/* =========================================================
   HTTP SERVER
========================================================= */

const server =
  http.createServer(
    app
  );

/* =========================================================
   SOCKET.IO
========================================================= */

const io =
  new Server(
    server,
    {
      cors: {
        origin: true,
        credentials: false
      },

      transports: [
        'websocket',
        'polling'
      ],

      pingInterval:
        25000,

      pingTimeout:
        20000,

      maxHttpBufferSize:
        1e6
    }
  );

io.use(
  socketAuth
);

const onlineByRoom =
  new Map();

/* =========================================================
   SOCKET CONNECTION
========================================================= */

io.on(
  'connection',
  async function (socket) {

    /* ================================================
       ENTER ROOM
    ================================================= */

    socket.on(
      'room:enter',
      async function (data) {
        try {
          const roomId =
            data &&
            data.roomId;

          const user =
            await get(
              [
                'SELECT id, is_admin',
                'FROM users',
                'WHERE id = ?'
              ].join('\n'),
              [socket.user.id]
            );

          const isAdmin =
            Boolean(
              user &&
              user.is_admin
            );

          if (!isAdmin) {
            await requireRoomMember(
              socket.user.id,
              roomId
            );
          } else {
            const exists =
              await get(
                'SELECT id FROM rooms WHERE id = ?',
                [roomId]
              );

            if (!exists) {
              throw new Error(
                '找不到房間'
              );
            }
          }

          socket.join(
            'room:' + roomId
          );

          if (
            !onlineByRoom.has(
              roomId
            )
          ) {
            onlineByRoom.set(
              roomId,
              new Set()
            );
          }

          onlineByRoom
            .get(roomId)
            .add(
              socket.user.id
            );

          io.to(
            'room:' + roomId
          ).emit(
            'presence',
            {
              online:
                Array.from(
                  onlineByRoom.get(
                    roomId
                  )
                )
            }
          );

          socket.emit(
            'room:snapshot',
            await roomSnapshot(
              roomId
            )
          );
        } catch (e) {
          socket.emit(
            'error:message',
            e.message ||
              '進入房間失敗'
          );
        }
      }
    );

    /* ================================================
       CHAT
    ================================================= */

    socket.on(
      'chat:send',
      async function (data) {
        try {
          const roomId =
            data &&
            data.roomId;

          const text =
            data &&
            data.text;

          const user =
            await get(
              [
                'SELECT id, is_admin',
                'FROM users',
                'WHERE id = ?'
              ].join('\n'),
              [socket.user.id]
            );

          const isAdmin =
            Boolean(
              user &&
              user.is_admin
            );

          if (!isAdmin) {
            await requireRoomMember(
              socket.user.id,
              roomId
            );
          }

          const safe =
            String(
              text || ''
            )
              .trim()
              .slice(0, 1000);

          if (!safe) {
            return;
          }

          await addEvent(
            roomId,
            socket.user.id,
            'chat',
            {
              text: safe
            }
          );

          io.to(
            'room:' + roomId
          ).emit(
            'room:snapshot',
            await roomSnapshot(
              roomId
            )
          );
        } catch (e) {
          socket.emit(
            'error:message',
            e.message ||
              '訊息失敗'
          );
        }
      }
    );

    /* ================================================
       DICE
    ================================================= */

    socket.on(
      'dice:roll',
      async function (data) {
        try {
          const roomId =
            data &&
            data.roomId;

          const notation =
            data &&
            data.notation
              ? data.notation
              : '1d20';

          const label =
            data &&
            data.label
              ? data.label
              : '擲骰';

          const user =
            await get(
              [
                'SELECT id, is_admin',
                'FROM users',
                'WHERE id = ?'
              ].join('\n'),
              [socket.user.id]
            );

          const isAdmin =
            Boolean(
              user &&
              user.is_admin
            );

          if (!isAdmin) {
            await requireRoomMember(
              socket.user.id,
              roomId
            );
          }

          const m =
            String(
              notation
            )
              .trim()
              .match(
                /^(\d{1,2})d(\d{1,4})([+-]\d{1,4})?$/i
              );

          if (!m) {
            return socket.emit(
              'error:message',
              '骰式格式例如 1d20 或 2d6+3'
            );
          }

          const count =
            Math.min(
              20,
              Number(m[1])
            );

          const sides =
            Math.min(
              1000,
              Number(m[2])
            );

          const mod =
            Number(
              m[3] || 0
            );

          const rolls =
            Array.from(
              {
                length:
                  count
              },
              function () {
                return (
                  1 +
                  Math.floor(
                    Math.random() *
                    sides
                  )
                );
              }
            );

          const total =
            rolls.reduce(
              function (a, b) {
                return a + b;
              },
              0
            ) + mod;

          await addEvent(
            roomId,
            socket.user.id,
            'dice',
            {
              notation,
              label:
                String(
                  label
                ).slice(0, 80),

              rolls,

              modifier:
                mod,

              total
            }
          );

          io.to(
            'room:' + roomId
          ).emit(
            'room:snapshot',
            await roomSnapshot(
              roomId
            )
          );
        } catch (e) {
          socket.emit(
            'error:message',
            e.message ||
              '骰子失敗'
          );
        }
      }
    );

    /* ================================================
       DISCONNECT
    ================================================= */

    socket.on(
      'disconnect',
      function () {
        for (
          const [
            roomId,
            set
          ] of onlineByRoom.entries()
        ) {
          if (
            set.delete(
              socket.user.id
            )
          ) {
            io.to(
              'room:' + roomId
            ).emit(
              'presence',
              {
                online:
                  Array.from(
                    set
                  )
              }
            );

            if (!set.size) {
              onlineByRoom.delete(
                roomId
              );
            }
          }
        }
      }
    );
  }
);

/* =========================================================
   SHUTDOWN
========================================================= */

async function shutdown(
  signal
) {
  console.log(
    signal +
    ' received, shutting down...'
  );

  io.close();

  server.close(
    async function () {
      await pool.end();

      process.exit(0);
    }
  );
}

process.on(
  'SIGTERM',
  function () {
    shutdown(
      'SIGTERM'
    );
  }
);

process.on(
  'SIGINT',
  function () {
    shutdown(
      'SIGINT'
    );
  }
);

/* =========================================================
   START SERVER
========================================================= */

initDb()
  .then(
    function () {
      server.listen(
        PORT,
        HOST,
        function () {
          console.log(
            'TRPG Online listening on ' +
            HOST +
            ':' +
            PORT
          );
        }
      );
    }
  )
  .catch(
    function (err) {
      console.error(
        'Database initialization failed:',
        err
      );

      process.exit(1);
    }
  );
```
