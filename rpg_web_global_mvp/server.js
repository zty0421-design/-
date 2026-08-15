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
  throw new Error(
    'DATABASE_URL is required. Example: postgresql://user:password@localhost:5432/rpg'
  );
}

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
   PostgreSQL helper
========================================================= */

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

  return {
    id: row?.id ?? null,
    changes: result.rowCount,
    row
  };
};

/* =========================================================
   Database
========================================================= */

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
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      archived_at TIMESTAMPTZ
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

    /* =====================================================
       玩家角色卡
       一個帳號只能有一張角色卡
    ===================================================== */

    CREATE TABLE IF NOT EXISTS characters (
      id BIGSERIAL PRIMARY KEY,

      user_id BIGINT NOT NULL UNIQUE
        REFERENCES users(id) ON DELETE CASCADE,

      name VARCHAR(50) NOT NULL DEFAULT '未命名角色',

      faction VARCHAR(20) NOT NULL DEFAULT '東國',

      tier INTEGER NOT NULL DEFAULT 1,
      level INTEGER NOT NULL DEFAULT 1,

      /*
        每階基礎自由屬性點。
        free_points 是目前尚未分配的點數。
      */
      free_points INTEGER NOT NULL DEFAULT 50,

      agility INTEGER NOT NULL DEFAULT 0,
      strength INTEGER NOT NULL DEFAULT 0,
      constitution INTEGER NOT NULL DEFAULT 0,
      spirit INTEGER NOT NULL DEFAULT 0,
      dao INTEGER NOT NULL DEFAULT 0,

      /*
        特殊屬性
      */
      luck INTEGER NOT NULL DEFAULT 10,

      /*
        職業
      */
      main_class VARCHAR(60),

      sub_class_1 VARCHAR(60),
      sub_class_2 VARCHAR(60),
      sub_class_3 VARCHAR(60),

      /*
        技能與裝備使用 JSONB。
        後續可以逐步獨立成正式資料表。
      */
      skills JSONB NOT NULL DEFAULT '[]'::jsonb,

      equipment JSONB NOT NULL DEFAULT '[]'::jsonb,

      /*
        光暗金木水火土親和
      */
      affinities JSONB NOT NULL DEFAULT '{
        "light": 0,
        "dark": 0,
        "metal": 0,
        "wood": 0,
        "water": 0,
        "fire": 0,
        "earth": 0
      }'::jsonb,

      /*
        其他角色資料
      */
      data JSONB NOT NULL DEFAULT '{}'::jsonb,

      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

    /*
      角色成長紀錄
    */
    CREATE TABLE IF NOT EXISTS character_growth (
      id BIGSERIAL PRIMARY KEY,

      character_id BIGINT NOT NULL
        REFERENCES characters(id) ON DELETE CASCADE,

      type VARCHAR(30) NOT NULL,

      payload JSONB NOT NULL DEFAULT '{}'::jsonb,

      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

    /*
      房間存檔紀錄
    */
    CREATE TABLE IF NOT EXISTS room_saves (
      id BIGSERIAL PRIMARY KEY,

      room_id BIGINT NOT NULL
        REFERENCES rooms(id) ON DELETE CASCADE,

      saved_by BIGINT REFERENCES users(id) ON DELETE SET NULL,

      snapshot JSONB NOT NULL,

      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

    CREATE INDEX IF NOT EXISTS idx_room_members_room
      ON room_members(room_id);

    CREATE INDEX IF NOT EXISTS idx_events_room_id_id
      ON events(room_id, id);

    CREATE INDEX IF NOT EXISTS idx_character_growth_character
      ON character_growth(character_id);

    CREATE INDEX IF NOT EXISTS idx_room_saves_room
      ON room_saves(room_id);
  `);

  /*
    相容舊資料庫：
    如果 rooms 是以前建立的，補 archived_at。
  */
  await pool.query(`
    ALTER TABLE rooms
    ADD COLUMN IF NOT EXISTS archived_at TIMESTAMPTZ;
  `);
}

/* =========================================================
   JWT
========================================================= */

function signToken(user) {
  return jwt.sign(
    {
      id: user.id,
      username: user.username
    },
    JWT_SECRET,
    {
      expiresIn: '7d'
    }
  );
}

function auth(req, res, next) {
  const raw = req.headers.authorization || '';

  const token = raw.startsWith('Bearer ')
    ? raw.slice(7)
    : null;

  if (!token) {
    return res.status(401).json({
      error: '需要登入'
    });
  }

  try {
    req.user = jwt.verify(token, JWT_SECRET);
    next();
  } catch {
    return res.status(401).json({
      error: '登入已失效'
    });
  }
}

function socketAuth(socket, next) {
  try {
    socket.user = jwt.verify(
      socket.handshake.auth?.token || '',
      JWT_SECRET
    );

    next();
  } catch {
    next(new Error('unauthorized'));
  }
}

/* =========================================================
   Utilities
========================================================= */

function generateCode() {
  const alphabet =
    'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';

  let out = '';

  for (let i = 0; i < 6; i++) {
    out += alphabet[
      Math.floor(Math.random() * alphabet.length)
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

function safeJson(value, fallback) {
  if (value === null || value === undefined) {
    return fallback;
  }

  if (typeof value === 'string') {
    try {
      return JSON.parse(value);
    } catch {
      return fallback;
    }
  }

  return value;
}

function clampInt(value, min = 0, max = 2147483647) {
  const n = Number(value);

  if (!Number.isFinite(n)) {
    return min;
  }

  return Math.min(
    max,
    Math.max(min, Math.floor(n))
  );
}

/* =========================================================
   Events
========================================================= */

async function addEvent(
  roomId,
  userId,
  type,
  payload
) {
  await run(
    `
      INSERT INTO events(
        room_id,
        user_id,
        type,
        payload
      )
      VALUES(?,?,?,?::jsonb)
    `,
    [
      roomId,
      userId,
      type,
      JSON.stringify(payload)
    ]
  );
}

/* =========================================================
   Room permission
========================================================= */

async function requireRoomMember(
  userId,
  roomId
) {
  const row = await get(
    `
      SELECT *
      FROM room_members
      WHERE room_id = ?
        AND user_id = ?
    `,
    [
      roomId,
      userId
    ]
  );

  if (!row) {
    throw Object.assign(
      new Error('你不是房間成員'),
      {
        status: 403
      }
    );
  }

  return row;
}

async function requireDM(
  userId,
  roomId
) {
  const room = await get(
    `
      SELECT *
      FROM rooms
      WHERE id = ?
        AND owner_id = ?
    `,
    [
      roomId,
      userId
    ]
  );

  if (!room) {
    throw Object.assign(
      new Error('只有DM可以執行此操作'),
      {
        status: 403
      }
    );
  }

  return room;
}

/* =========================================================
   Character
========================================================= */

const DEFAULT_AFFINITIES = {
  light: 0,
  dark: 0,
  metal: 0,
  wood: 0,
  water: 0,
  fire: 0,
  earth: 0
};

function normalizeCharacter(character) {
  if (!character) return null;

  return {
    id: character.id,
    user_id: character.user_id,

    name: character.name,

    faction: character.faction,

    tier: character.tier,
    level: character.level,

    free_points: character.free_points,

    attributes: {
      agility: character.agility,
      strength: character.strength,
      constitution: character.constitution,
      spirit: character.spirit,
      dao: character.dao
    },

    special: {
      luck: character.luck,

      endurance:
        Math.floor(
          (character.constitution +
            character.spirit) /
          2
        ),

      sanity:
        Math.floor(
          character.spirit * 1.2
        ),

      will:
        Math.floor(
          character.spirit * 0.8
        ),

      hp:
        character.constitution * 2,

      max_hp:
        character.constitution * 2
    },

    classes: {
      main: character.main_class,

      sub: [
        character.sub_class_1,
        character.sub_class_2,
        character.sub_class_3
      ].filter(Boolean)
    },

    affinities: {
      ...DEFAULT_AFFINITIES,
      ...safeJson(
        character.affinities,
        DEFAULT_AFFINITIES
      )
    },

    skills: safeJson(
      character.skills,
      []
    ),

    equipment: safeJson(
      character.equipment,
      []
    ),

    data: safeJson(
      character.data,
      {}
    ),

    created_at: character.created_at,
    updated_at: character.updated_at
  };
}

async function getCharacterByUser(
  userId
) {
  const character = await get(
    `
      SELECT *
      FROM characters
      WHERE user_id = ?
    `,
    [userId]
  );

  return normalizeCharacter(character);
}

async function getCharacterRaw(
  characterId
) {
  return get(
    `
      SELECT *
      FROM characters
      WHERE id = ?
    `,
    [characterId]
  );
}

/* =========================================================
   Room snapshot
========================================================= */

async function roomSnapshot(roomId) {
  const room = await get(
    `
      SELECT
        id,
        code,
        name,
        owner_id,
        status,
        round,
        current_actor_id,
        created_at,
        archived_at
      FROM rooms
      WHERE id = ?
    `,
    [roomId]
  );

  if (!room) {
    return null;
  }

  const members = await all(
    `
      SELECT
        rm.user_id AS id,
        rm.display_name,
        rm.role,
        rm.hp,
        rm.max_hp,
        rm.spirit,
        rm.max_spirit,
        rm.joined_at,
        u.username
      FROM room_members rm
      JOIN users u
        ON u.id = rm.user_id
      WHERE rm.room_id = ?
      ORDER BY rm.joined_at ASC
    `,
    [roomId]
  );

  const events = await all(
    `
      SELECT
        e.id,
        e.type,
        e.payload,
        e.created_at,
        u.username
      FROM events e
      LEFT JOIN users u
        ON u.id = e.user_id
      WHERE e.room_id = ?
      ORDER BY e.id DESC
      LIMIT 100
    `,
    [roomId]
  );

  return {
    room,

    members,

    events: events
      .reverse()
      .map(e => ({
        ...e,
        payload: safeJson(
          e.payload,
          {}
        )
      }))
  };
}

/* =========================================================
   Save room snapshot
========================================================= */

async function createRoomSave(
  roomId,
  userId
) {
  const snapshot =
    await roomSnapshot(roomId);

  if (!snapshot) {
    throw new Error('房間不存在');
  }

  const result = await run(
    `
      INSERT INTO room_saves(
        room_id,
        saved_by,
        snapshot
      )
      VALUES(?,?,?::jsonb)
      RETURNING id
    `,
    [
      roomId,
      userId,
      JSON.stringify(snapshot)
    ]
  );

  return result.id;
}

/* =========================================================
   Express
========================================================= */

const app = express();

app.set('trust proxy', 1);

app.disable('x-powered-by');

app.use(
  express.json({
    limit: '1mb'
  })
);

/* =========================================================
   Health
========================================================= */

app.get(
  '/api/health',
  async (req, res) => {
    try {
      await pool.query('SELECT 1');

      res.json({
        ok: true,
        service: 'trpg-online',
        database: 'ok',
        time: new Date().toISOString()
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
   Auth
========================================================= */

app.post(
  '/api/auth/register',
  async (req, res) => {
    try {
      const username =
        String(
          req.body.username || ''
        ).trim();

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
          error: '密碼至少6碼'
        });
      }

      const hash =
        await bcrypt.hash(
          password,
          10
        );

      const result =
        await run(
          `
            INSERT INTO users(
              username,
              password_hash
            )
            VALUES(?,?)
            RETURNING id
          `,
          [
            username,
            hash
          ]
        );

      const user = {
        id: result.id,
        username
      };

      res.json({
        token: signToken(user),
        user
      });
    } catch (err) {
      res
        .status(
          err.code === '23505'
            ? 409
            : 500
        )
        .json({
          error:
            err.code === '23505'
              ? '帳號已存在'
              : '註冊失敗'
        });
    }
  }
);

app.post(
  '/api/auth/login',
  async (req, res) => {
    try {
      const username =
        String(
          req.body.username || ''
        ).trim();

      const password =
        String(
          req.body.password || ''
        );

      const user = await get(
        `
          SELECT
            id,
            username,
            password_hash
          FROM users
          WHERE username = ?
        `,
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
          error: '帳號或密碼錯誤'
        });
      }

      res.json({
        token: signToken(user),

        user: {
          id: user.id,
          username: user.username
        }
      });
    } catch {
      res.status(500).json({
        error: '登入失敗'
      });
    }
  }
);

app.get(
  '/api/me',
  auth,
  async (req, res) => {
    const character =
      await getCharacterByUser(
        req.user.id
      );

    res.json({
      user: req.user,
      character
    });
  }
);

/* =========================================================
   Character API
========================================================= */

/*
  取得自己的角色卡
*/
app.get(
  '/api/character',
  auth,
  async (req, res) => {
    try {
      const character =
        await getCharacterByUser(
          req.user.id
        );

      res.json({
        character
      });
    } catch {
      res.status(500).json({
        error: '讀取角色卡失敗'
      });
    }
  }
);

/*
  建立角色卡
  每個帳號只能建立一次
*/
app.post(
  '/api/character',
  auth,
  async (req, res) => {
    try {
      const existing =
        await get(
          `
            SELECT id
            FROM characters
            WHERE user_id = ?
          `,
          [req.user.id]
        );

      if (existing) {
        return res.status(409).json({
          error:
            '你已經有角色卡，每個帳號只能擁有一張角色卡'
        });
      }

      const name =
        String(
          req.body.name ||
            req.user.username
        )
          .trim()
          .slice(0, 50);

      const faction =
        req.body.faction === '西國'
          ? '西國'
          : '東國';

      const mainClass =
        req.body.main_class
          ? String(
              req.body.main_class
            ).slice(0, 60)
          : null;

      /*
        初始角色：
        0階 → 1階
        1階擁有50自由屬性點
      */

      const result =
        await run(
          `
            INSERT INTO characters(
              user_id,
              name,
              faction,
              tier,
              level,
              free_points,
              main_class,
              affinities
            )
            VALUES(
              ?,?,
              ?,
              1,
              1,
              50,
              ?,
              ?::jsonb
            )
            RETURNING id
          `,
          [
            req.user.id,
            name || '未命名角色',
            faction,
            mainClass,
            JSON.stringify(
              DEFAULT_AFFINITIES
            )
          ]
        );

      await run(
        `
          INSERT INTO character_growth(
            character_id,
            type,
            payload
          )
          VALUES(
            ?,
            ?,
            ?::jsonb
          )
        `,
        [
          result.id,
          'create',
          JSON.stringify({
            tier: 1,
            level: 1,
            free_points: 50
          })
        ]
      );

      const character =
        await getCharacterByUser(
          req.user.id
        );

      res.json({
        character
      });
    } catch (err) {
      if (err.code === '23505') {
        return res.status(409).json({
          error:
            '你已經有角色卡'
        });
      }

      res.status(500).json({
        error: '建立角色卡失敗'
      });
    }
  }
);

/*
  修改角色基本資料
*/
app.patch(
  '/api/character',
  auth,
  async (req, res) => {
    try {
      const character =
        await get(
          `
            SELECT *
            FROM characters
            WHERE user_id = ?
          `,
          [req.user.id]
        );

      if (!character) {
        return res.status(404).json({
          error: '尚未建立角色卡'
        });
      }

      const updates = [];
      const values = [];

      if (req.body.name !== undefined) {
        updates.push('name = ?');
        values.push(
          String(
            req.body.name
          )
            .trim()
            .slice(0, 50)
        );
      }

      if (
        req.body.faction === '東國' ||
        req.body.faction === '西國'
      ) {
        updates.push(
          'faction = ?'
        );

        values.push(
          req.body.faction
        );
      }

      if (
        req.body.main_class !==
        undefined
      ) {
        updates.push(
          'main_class = ?'
        );

        values.push(
          req.body.main_class
            ? String(
                req.body.main_class
              ).slice(0, 60)
            : null
        );
      }

      if (
        req.body.sub_class_1 !==
        undefined
      ) {
        updates.push(
          'sub_class_1 = ?'
        );

        values.push(
          req.body.sub_class_1
            ? String(
                req.body.sub_class_1
              ).slice(0, 60)
            : null
        );
      }

      if (
        req.body.sub_class_2 !==
        undefined
      ) {
        updates.push(
          'sub_class_2 = ?'
        );

        values.push(
          req.body.sub_class_2
            ? String(
                req.body.sub_class_2
              ).slice(0, 60)
            : null
        );
      }

      if (
        req.body.sub_class_3 !==
        undefined
      ) {
        updates.push(
          'sub_class_3 = ?'
        );

        values.push(
          req.body.sub_class_3
            ? String(
                req.body.sub_class_3
              ).slice(0, 60)
            : null
        );
      }

      if (!updates.length) {
        return res.status(400).json({
          error:
            '沒有可更新的資料'
        });
      }

      updates.push(
        'updated_at = CURRENT_TIMESTAMP'
      );

      values.push(
        character.id
      );

      await run(
        `
          UPDATE characters
          SET ${updates.join(', ')}
          WHERE id = ?
        `,
        values
      );

      const updated =
        await getCharacterByUser(
          req.user.id
        );

      res.json({
        character: updated
      });
    } catch {
      res.status(500).json({
        error: '更新角色卡失敗'
      });
    }
  }
);

/* =========================================================
   自由屬性點
========================================================= */

app.post(
  '/api/character/attributes',
  auth,
  async (req, res) => {
    try {
      const character =
        await get(
          `
            SELECT *
            FROM characters
            WHERE user_id = ?
          `,
          [req.user.id]
        );

      if (!character) {
        return res.status(404).json({
          error: '尚未建立角色卡'
        });
      }

      const requested = {
        agility: clampInt(
          req.body.agility
        ),

        strength: clampInt(
          req.body.strength
        ),

        constitution: clampInt(
          req.body.constitution
        ),

        spirit: clampInt(
          req.body.spirit
        ),

        dao: clampInt(
          req.body.dao
        )
      };

      const total =
        Object.values(
          requested
        ).reduce(
          (a, b) => a + b,
          0
        );

      if (
        total >
        character.free_points
      ) {
        return res.status(400).json({
          error:
            `自由屬性點不足，目前剩餘 ${character.free_points} 點`
        });
      }

      const newFreePoints =
        character.free_points -
        total;

      await run(
        `
          UPDATE characters
          SET
            agility = agility + ?,
            strength = strength + ?,
            constitution = constitution + ?,
            spirit = spirit + ?,
            dao = dao + ?,
            free_points = ?,
            updated_at = CURRENT_TIMESTAMP
          WHERE id = ?
        `,
        [
          requested.agility,
          requested.strength,
          requested.constitution,
          requested.spirit,
          requested.dao,
          newFreePoints,
          character.id
        ]
      );

      await run(
        `
          INSERT INTO character_growth(
            character_id,
            type,
            payload
          )
          VALUES(
            ?,
            'attribute',
            ?::jsonb
          )
        `,
        [
          character.id,
          JSON.stringify({
            spent: requested,
            remaining:
              newFreePoints
          })
        ]
      );

      const updated =
        await getCharacterByUser(
          req.user.id
        );

      res.json({
        character: updated
      });
    } catch {
      res.status(500).json({
        error:
          '分配屬性點失敗'
      });
    }
  }
);

/* =========================================================
   角色升階
========================================================= */

app.post(
  '/api/character/growth',
  auth,
  async (req, res) => {
    try {
      const character =
        await get(
          `
            SELECT *
            FROM characters
            WHERE user_id = ?
          `,
          [req.user.id]
        );

      if (!character) {
        return res.status(404).json({
          error: '尚未建立角色卡'
        });
      }

      const tier =
        clampInt(
          req.body.tier,
          1,
          10
        );

      const level =
        clampInt(
          req.body.level,
          1,
          4
        );

      /*
        這裡暫時只提供資料接口。
        真正的登階儀式由DM之後控制。
      */

      const tierPoints = {
        1: 50,
        2: 100,
        3: 180,
        4: 320,
        5: 600,
        6: 1100,
        7: 2000,
        8: 3600,
        9: 6500,
        10: 10000
      };

      const points =
        tierPoints[tier] || 0;

      await run(
        `
          UPDATE characters
          SET
            tier = ?,
            level = ?,
            free_points = ?,
            updated_at = CURRENT_TIMESTAMP
          WHERE id = ?
        `,
        [
          tier,
          level,
          points,
          character.id
        ]
      );

      await run(
        `
          INSERT INTO character_growth(
            character_id,
            type,
            payload
          )
          VALUES(
            ?,
            'tier',
            ?::jsonb
          )
        `,
        [
          character.id,
          JSON.stringify({
            tier,
            level,
            granted_points:
              points
          })
        ]
      );

      const updated =
        await getCharacterByUser(
          req.user.id
        );

      res.json({
        character: updated
      });
    } catch {
      res.status(500).json({
        error:
          '角色成長更新失敗'
      });
    }
  }
);

/* =========================================================
   技能系統接口
========================================================= */

/*
  取得技能
*/
app.get(
  '/api/character/skills',
  auth,
  async (req, res) => {
    try {
      const character =
        await get(
          `
            SELECT skills
            FROM characters
            WHERE user_id = ?
          `,
          [req.user.id]
        );

      if (!character) {
        return res.status(404).json({
          error: '尚未建立角色卡'
        });
      }

      res.json({
        skills: safeJson(
          character.skills,
          []
        )
      });
    } catch {
      res.status(500).json({
        error: '讀取技能失敗'
      });
    }
  }
);

/*
  新增技能
  技能格式目前先使用 JSON
*/
app.post(
  '/api/character/skills',
  auth,
  async (req, res) => {
    try {
      const character =
        await get(
          `
            SELECT *
            FROM characters
            WHERE user_id = ?
          `,
          [req.user.id]
        );

      if (!character) {
        return res.status(404).json({
          error: '尚未建立角色卡'
        });
      }

      const skills =
        safeJson(
          character.skills,
          []
        );

      const skill = {
        id:
          req.body.id ||
          `skill_${Date.now()}`,

        name:
          String(
            req.body.name ||
              '未命名技能'
          ).slice(0, 80),

        level:
          clampInt(
            req.body.level || 1,
            1,
            10
          ),

        description:
          String(
            req.body.description ||
              ''
          ).slice(0, 2000),

        type:
          String(
            req.body.type ||
              'active'
          ).slice(0, 30),

        data:
          req.body.data || {}
      };

      skills.push(skill);

      await run(
        `
          UPDATE characters
          SET
            skills = ?::jsonb,
            updated_at = CURRENT_TIMESTAMP
          WHERE id = ?
        `,
        [
          JSON.stringify(skills),
          character.id
        ]
      );

      await run(
        `
          INSERT INTO character_growth(
            character_id,
            type,
            payload
          )
          VALUES(
            ?,
            'skill',
            ?::jsonb
          )
        `,
        [
          character.id,
          JSON.stringify({
            action: 'add',
            skill
          })
        ]
      );

      res.json({
        skills
      });
    } catch {
      res.status(500).json({
        error: '新增技能失敗'
      });
    }
  }
);

/*
  更新技能
*/
app.patch(
  '/api/character/skills/:skillId',
  auth,
  async (req, res) => {
    try {
      const character =
        await get(
          `
            SELECT *
            FROM characters
            WHERE user_id = ?
          `,
          [req.user.id]
        );

      if (!character) {
        return res.status(404).json({
          error: '尚未建立角色卡'
        });
      }

      const skills =
        safeJson(
          character.skills,
          []
        );

      const index =
        skills.findIndex(
          s =>
            String(s.id) ===
            String(req.params.skillId)
        );

      if (index === -1) {
        return res.status(404).json({
          error: '找不到技能'
        });
      }

      const oldSkill =
        skills[index];

      skills[index] = {
        ...oldSkill,

        ...(req.body.name !== undefined
          ? {
              name: String(
                req.body.name
              ).slice(0, 80)
            }
          : {}),

        ...(req.body.level !== undefined
          ? {
              level: clampInt(
                req.body.level,
                1,
                10
              )
            }
          : {}),

        ...(req.body.description !==
        undefined
          ? {
              description:
                String(
                  req.body.description
                ).slice(0, 2000)
            }
          : {}),

        ...(req.body.type !== undefined
          ? {
              type: String(
                req.body.type
              ).slice(0, 30)
            }
          : {}),

        ...(req.body.data !== undefined
          ? {
              data:
                req.body.data
            }
          : {})
      };

      await run(
        `
          UPDATE characters
          SET
            skills = ?::jsonb,
            updated_at = CURRENT_TIMESTAMP
          WHERE id = ?
        `,
        [
          JSON.stringify(skills),
          character.id
        ]
      );

      res.json({
        skills
      });
    } catch {
      res.status(500).json({
        error: '更新技能失敗'
      });
    }
  }
);

/* =========================================================
   裝備系統接口
========================================================= */

app.get(
  '/api/character/equipment',
  auth,
  async (req, res) => {
    try {
      const character =
        await get(
          `
            SELECT equipment
            FROM characters
            WHERE user_id = ?
          `,
          [req.user.id]
        );

      if (!character) {
        return res.status(404).json({
          error: '尚未建立角色卡'
        });
      }

      res.json({
        equipment:
          safeJson(
            character.equipment,
            []
          )
      });
    } catch {
      res.status(500).json({
        error: '讀取裝備失敗'
      });
    }
  }
);

app.post(
  '/api/character/equipment',
  auth,
  async (req, res) => {
    try {
      const character =
        await get(
          `
            SELECT *
            FROM characters
            WHERE user_id = ?
          `,
          [req.user.id]
        );

      if (!character) {
        return res.status(404).json({
          error: '尚未建立角色卡'
        });
      }

      const equipment =
        safeJson(
          character.equipment,
          []
        );

      const item = {
        id:
          req.body.id ||
          `item_${Date.now()}`,

        name:
          String(
            req.body.name ||
              '未命名裝備'
          ).slice(0, 80),

        category:
          String(
            req.body.category ||
              'other'
          ).slice(0, 40),

        rarity:
          String(
            req.body.rarity ||
              'G'
          ).slice(0, 10),

        description:
          String(
            req.body.description ||
              ''
          ).slice(0, 2000),

        data:
          req.body.data || {}
      };

      equipment.push(item);

      await run(
        `
          UPDATE characters
          SET
            equipment = ?::jsonb,
            updated_at = CURRENT_TIMESTAMP
          WHERE id = ?
        `,
        [
          JSON.stringify(equipment),
          character.id
        ]
      );

      res.json({
        equipment
      });
    } catch {
      res.status(500).json({
        error: '新增裝備失敗'
      });
    }
  }
);

/* =========================================================
   屬性親和
========================================================= */

app.patch(
  '/api/character/affinities',
  auth,
  async (req, res) => {
    try {
      const character =
        await get(
          `
            SELECT *
            FROM characters
            WHERE user_id = ?
          `,
          [req.user.id]
        );

      if (!character) {
        return res.status(404).json({
          error: '尚未建立角色卡'
        });
      }

      const old =
        safeJson(
          character.affinities,
          DEFAULT_AFFINITIES
        );

      const allowed = [
        'light',
        'dark',
        'metal',
        'wood',
        'water',
        'fire',
        'earth'
      ];

      const updated = {
        ...DEFAULT_AFFINITIES,
        ...old
      };

      for (const key of allowed) {
        if (
          req.body[key] !==
          undefined
        ) {
          updated[key] =
            clampInt(
              req.body[key],
              0,
              100
            );
        }
      }

      await run(
        `
          UPDATE characters
          SET
            affinities = ?::jsonb,
            updated_at = CURRENT_TIMESTAMP
          WHERE id = ?
        `,
        [
          JSON.stringify(updated),
          character.id
        ]
      );

      res.json({
        affinities: updated
      });
    } catch {
      res.status(500).json({
        error:
          '更新屬性親和失敗'
      });
    }
  }
);

/* =========================================================
   Room
========================================================= */

/*
  建立房間的人就是這個房間的DM。

  注意：
  「加入別人的房間」
  不會讓加入者變成DM。
*/
app.post(
  '/api/rooms',
  auth,
  async (req, res) => {
    try {
      const name =
        String(
          req.body.name ||
            '未命名跑團'
        )
          .trim()
          .slice(0, 60) ||
        '未命名跑團';

      const code =
        await uniqueCode();

      const result =
        await run(
          `
            INSERT INTO rooms(
              code,
              name,
              owner_id,
              status
            )
            VALUES(
              ?,
              ?,
              ?,
              'lobby'
            )
            RETURNING id
          `,
          [
            code,
            name,
            req.user.id
          ]
        );

      /*
        建立者才加入成為DM
      */
      await run(
        `
          INSERT INTO room_members(
            room_id,
            user_id,
            role,
            display_name
          )
          VALUES(
            ?,
            ?,
            'gm',
            ?
          )
        `,
        [
          result.id,
          req.user.id,
          req.user.username
        ]
      );

      await addEvent(
        result.id,
        req.user.id,
        'system',
        {
          text:
            `${req.user.username} 建立了跑團「${name}」`
        }
      );

      res.json(
        await roomSnapshot(
          result.id
        )
      );
    } catch {
      res.status(500).json({
        error: '建立房間失敗'
      });
    }
  }
);

/*
  加入房間
*/
app.post(
  '/api/rooms/join',
  auth,
  async (req, res) => {
    try {
      const code =
        String(
          req.body.code || ''
        )
          .trim()
          .toUpperCase();

      const room =
        await get(
          `
            SELECT *
            FROM rooms
            WHERE code = ?
          `,
          [code]
        );

      if (!room) {
        return res.status(404).json({
          error: '找不到房間'
        });
      }

      if (
        room.status ===
        'archived'
      ) {
        return res.status(400).json({
          error:
            '這個房間已經存檔'
        });
      }

      /*
        加入者永遠是 player
      */
      const existing =
        await get(
          `
            SELECT *
            FROM room_members
            WHERE room_id = ?
              AND user_id = ?
          `,
          [
            room.id,
            req.user.id
          ]
        );

      if (!existing) {
        await run(
          `
            INSERT INTO room_members(
              room_id,
              user_id,
              role,
              display_name
            )
            VALUES(
              ?,
              ?,
              'player',
              ?
            )
          `,
          [
            room.id,
            req.user.id,
            req.user.username
          ]
        );

        await addEvent(
          room.id,
          req.user.id,
          'system',
          {
            text:
              `${req.user.username} 加入房間`
          }
        );
      }

      res.json(
        await roomSnapshot(
          room.id
        )
      );
    } catch {
      res.status(500).json({
        error: '加入房間失敗'
      });
    }
  }
);

/*
  讀取房間
*/
app.get(
  '/api/rooms/:id',
  auth,
  async (req, res) => {
    try {
      await requireRoomMember(
        req.user.id,
        req.params.id
      );

      res.json(
        await roomSnapshot(
          req.params.id
        )
      );
    } catch (e) {
      res
        .status(
          e.status || 500
        )
        .json({
          error:
            e.message ||
            '讀取房間失敗'
        });
    }
  }
);

/* =========================================================
   DM 查看所有角色卡
========================================================= */

app.get(
  '/api/rooms/:id/characters',
  auth,
  async (req, res) => {
    try {
      await requireDM(
        req.user.id,
        req.params.id
      );

      const members =
        await all(
          `
            SELECT
              rm.user_id,
              rm.role,
              rm.display_name,
              u.username,
              c.id AS character_id
            FROM room_members rm
            JOIN users u
              ON u.id = rm.user_id
            LEFT JOIN characters c
              ON c.user_id = rm.user_id
            WHERE rm.room_id = ?
            ORDER BY rm.joined_at ASC
          `,
          [req.params.id]
        );

      const result = [];

      for (const member of members) {
        const character =
          await getCharacterByUser(
            member.user_id
          );

        result.push({
          user: {
            id: member.user_id,
            username:
              member.username
          },

          room_role:
            member.role,

          display_name:
            member.display_name,

          character
        });
      }

      res.json({
        characters: result
      });
    } catch (e) {
      res
        .status(
          e.status || 500
        )
        .json({
          error:
            e.message ||
            '讀取角色卡失敗'
        });
    }
  }
);

/* =========================================================
   DM 修改玩家角色
========================================================= */

app.patch(
  '/api/rooms/:id/member/:userId',
  auth,
  async (req, res) => {
    try {
      const member =
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
          `
            SELECT owner_id
            FROM rooms
            WHERE id = ?
          `,
          [req.params.id]
        );

      const isDM =
        room &&
        String(
          room.owner_id
        ) ===
          String(
            req.user.id
          );

      /*
        玩家只能修改自己的HP/精神/名稱
        DM可以修改房間內任何玩家
      */
      const allowed =
        String(
          req.user.id
        ) ===
          String(target) ||
        isDM;

      if (!allowed) {
        return res.status(403).json({
          error: '沒有權限'
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
            `${field} = ?`
          );

          if (
            field ===
            'display_name'
          ) {
            values.push(
              String(
                req.body[field]
              ).slice(0, 30)
            );
          } else {
            values.push(
              clampInt(
                req.body[field]
              )
            );
          }
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
        `
          UPDATE room_members
          SET ${updates.join(', ')}
          WHERE room_id = ?
            AND user_id = ?
        `,
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

      res.json(
        await roomSnapshot(
          req.params.id
        )
      );
    } catch (e) {
      res
        .status(
          e.status || 500
        )
        .json({
          error:
            e.message ||
            '更新角色失敗'
        });
    }
  }
);

/* =========================================================
   DM 開團
========================================================= */

app.post(
  '/api/rooms/:id/start',
  auth,
  async (req, res) => {
    try {
      await requireDM(
        req.user.id,
        req.params.id
      );

      const current =
        await get(
          `
            SELECT user_id
            FROM room_members
            WHERE room_id = ?
              AND role = 'player'
            ORDER BY joined_at ASC
            LIMIT 1
          `,
          [req.params.id]
        );

      await run(
        `
          UPDATE rooms
          SET
            status = 'active',
            round = 1,
            current_actor_id = ?
          WHERE id = ?
        `,
        [
          current?.user_id ||
            null,
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

      res.json(
        await roomSnapshot(
          req.params.id
        )
      );
    } catch (e) {
      res
        .status(
          e.status || 500
        )
        .json({
          error:
            e.message ||
            '無法開始跑團'
        });
    }
  }
);

/* =========================================================
   下一回合
========================================================= */

app.post(
  '/api/rooms/:id/next-turn',
  auth,
  async (req, res) => {
    try {
      await requireDM(
        req.user.id,
        req.params.id
      );

      const room =
        await get(
          `
            SELECT *
            FROM rooms
            WHERE id = ?
          `,
          [req.params.id]
        );

      const members =
        await all(
          `
            SELECT user_id
            FROM room_members
            WHERE room_id = ?
              AND role = 'player'
            ORDER BY joined_at ASC
          `,
          [req.params.id]
        );

      if (!members.length) {
        return res.status(400).json({
          error: '沒有玩家'
        });
      }

      const idx =
        Math.max(
          0,
          members.findIndex(
            m =>
              String(
                m.user_id
              ) ===
              String(
                room.current_actor_id
              )
          )
        );

      const next =
        members[
          (idx + 1) %
            members.length
        ];

      const nextRound =
        idx + 1 >=
        members.length
          ? room.round + 1
          : room.round;

      await run(
        `
          UPDATE rooms
          SET
            round = ?,
            current_actor_id = ?
          WHERE id = ?
        `,
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

      res.json(
        await roomSnapshot(
          req.params.id
        )
      );
    } catch (e) {
      res
        .status(
          e.status || 500
        )
        .json({
          error:
            e.message ||
            '切換回合失敗'
        });
    }
  }
);

/* =========================================================
   玩家退出房間
========================================================= */

app.post(
  '/api/rooms/:id/leave',
  auth,
  async (req, res) => {
    try {
      const room =
        await get(
          `
            SELECT *
            FROM rooms
            WHERE id = ?
          `,
          [req.params.id]
        );

      if (!room) {
        return res.status(404).json({
          error: '房間不存在'
        });
      }

      /*
        DM不能使用普通離開。
        DM應該使用「存檔／結束房間」。
      */
      if (
        String(
          room.owner_id
        ) ===
        String(
          req.user.id
        )
      ) {
        return res.status(400).json({
          error:
            'DM不能直接退出房間，請使用「存檔並結束房間」'
        });
      }

      await requireRoomMember(
        req.user.id,
        req.params.id
      );

      await run(
        `
          DELETE FROM room_members
          WHERE room_id = ?
            AND user_id = ?
        `,
        [
          req.params.id,
          req.user.id
        ]
      );

      await addEvent(
        req.params.id,
        req.user.id,
        'system',
        {
          text:
            `${req.user.username} 離開了房間`
        }
      );

      res.json({
        ok: true
      });
    } catch (e) {
      res
        .status(
          e.status || 500
        )
        .json({
          error:
            e.message ||
            '退出房間失敗'
        });
    }
  }
);

/* =========================================================
   DM 存檔房間
========================================================= */

app.post(
  '/api/rooms/:id/archive',
  auth,
  async (req, res) => {
    try {
      await requireDM(
        req.user.id,
        req.params.id
      );

      /*
        先保存完整房間快照
      */
      const saveId =
        await createRoomSave(
          req.params.id,
          req.user.id
        );

      await addEvent(
        req.params.id,
        req.user.id,
        'system',
        {
          text:
            'DM 已將房間存檔'
        }
      );

      await run(
        `
          UPDATE rooms
          SET
            status = 'archived',
            archived_at = CURRENT_TIMESTAMP
          WHERE id = ?
        `,
        [req.params.id]
      );

      res.json({
        ok: true,
        save_id: saveId
      });
    } catch (e) {
      res
        .status(
          e.status || 500
        )
        .json({
          error:
            e.message ||
            '房間存檔失敗'
        });
    }
  }
);

/* =========================================================
   DM 取得存檔
========================================================= */

app.get(
  '/api/rooms/:id/saves',
  auth,
  async (req, res) => {
    try {
      await requireDM(
        req.user.id,
        req.params.id
      );

      const saves =
        await all(
          `
            SELECT
              id,
              room_id,
              saved_by,
              created_at
            FROM room_saves
            WHERE room_id = ?
            ORDER BY id DESC
          `,
          [req.params.id]
        );

      res.json({
        saves
      });
    } catch (e) {
      res
        .status(
          e.status || 500
        )
        .json({
          error:
            e.message ||
            '讀取存檔失敗'
        });
    }
  }
);

/* =========================================================
   重新開啟存檔房間
========================================================= */

app.post(
  '/api/rooms/:id/resume',
  auth,
  async (req, res) => {
    try {
      await requireDM(
        req.user.id,
        req.params.id
      );

      const room =
        await get(
          `
            SELECT *
            FROM rooms
            WHERE id = ?
          `,
          [req.params.id]
        );

      if (!room) {
        return res.status(404).json({
          error: '房間不存在'
        });
      }

      await run(
        `
          UPDATE rooms
          SET
            status = 'lobby',
            archived_at = NULL
          WHERE id = ?
        `,
        [req.params.id]
      );

      await addEvent(
        req.params.id,
        req.user.id,
        'system',
        {
          text:
            'DM 重新開啟了跑團存檔'
        }
      );

      res.json(
        await roomSnapshot(
          req.params.id
        )
      );
    } catch (e) {
      res
        .status(
          e.status || 500
        )
        .json({
          error:
            e.message ||
            '重新開啟房間失敗'
        });
    }
  }
);

/* =========================================================
   Dice
========================================================= */

const appDice = async (
  roomId,
  userId,
  notation,
  label
) => {
  const m =
    String(notation)
      .trim()
      .match(
        /^(\d{1,2})d(\d{1,4})([+-]\d{1,4})?$/i
      );

  if (!m) {
    throw new Error(
      '骰式格式例如 1d20 或 2d6+3'
    );
  }

  const count = Math.min(
    20,
    Number(m[1])
  );

  const sides = Math.min(
    1000,
    Number(m[2])
  );

  const modifier =
    Number(m[3] || 0);

  const rolls =
    Array.from(
      {
        length: count
      },
      () =>
        1 +
        Math.floor(
          Math.random() *
            sides
        )
    );

  const total =
    rolls.reduce(
      (a, b) =>
        a + b,
      0
    ) + modifier;

  await addEvent(
    roomId,
    userId,
    'dice',
    {
      notation,
      label: String(
        label
      ).slice(0, 80),
      rolls,
      modifier,
      total
    }
  );

  return {
    notation,
    label,
    rolls,
    modifier,
    total
  };
};

/* =========================================================
   Static
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
   HTTP + Socket.IO
========================================================= */

const server =
  http.createServer(app);

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

      pingInterval: 25000,

      pingTimeout: 20000,

      maxHttpBufferSize:
        1e6
    }
  );

io.use(socketAuth);

/*
  roomId -> Set(userId)
*/
const onlineByRoom =
  new Map();

/* =========================================================
   Socket
========================================================= */

io.on(
  'connection',
  async socket => {

    /* -----------------------------------------------------
       Enter room
    ----------------------------------------------------- */

    socket.on(
      'room:enter',
      async ({
        roomId
      }) => {
        try {
          await requireRoomMember(
            socket.user.id,
            roomId
          );

          socket.join(
            `room:${roomId}`
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

          io
            .to(
              `room:${roomId}`
            )
            .emit(
              'presence',
              {
                online: [
                  ...onlineByRoom.get(
                    roomId
                  )
                ]
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

    /* -----------------------------------------------------
       Chat
    ----------------------------------------------------- */

    socket.on(
      'chat:send',
      async ({
        roomId,
        text
      }) => {
        try {
          await requireRoomMember(
            socket.user.id,
            roomId
          );

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

          io
            .to(
              `room:${roomId}`
            )
            .emit(
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

    /* -----------------------------------------------------
       Dice
    ----------------------------------------------------- */

    socket.on(
      'dice:roll',
      async ({
        roomId,
        notation = '1d20',
        label = '擲骰'
      }) => {
        try {
          await requireRoomMember(
            socket.user.id,
            roomId
          );

          await appDice(
            roomId,
            socket.user.id,
            notation,
            label
          );

          io
            .to(
              `room:${roomId}`
            )
            .emit(
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

    /* -----------------------------------------------------
       使用技能
       現階段只建立事件。
       真正技能計算之後接技能系統。
    ----------------------------------------------------- */

    socket.on(
      'skill:use',
      async ({
        roomId,
        skillId,
        targetId,
        data = {}
      }) => {
        try {
          await requireRoomMember(
            socket.user.id,
            roomId
          );

          const character =
            await get(
              `
                SELECT skills
                FROM characters
                WHERE user_id = ?
              `,
              [socket.user.id]
            );

          if (!character) {
            throw new Error(
              '你還沒有角色卡'
            );
          }

          const skills =
            safeJson(
              character.skills,
              []
            );

          const skill =
            skills.find(
              s =>
                String(
                  s.id
                ) ===
                String(
                  skillId
                )
            );

          if (!skill) {
            throw new Error(
              '找不到這個技能'
            );
          }

          await addEvent(
            roomId,
            socket.user.id,
            'skill',
            {
              skill_id:
                skill.id,

              skill_name:
                skill.name,

              target_id:
                targetId ||
                null,

              data
            }
          );

          io
            .to(
              `room:${roomId}`
            )
            .emit(
              'room:snapshot',
              await roomSnapshot(
                roomId
              )
            );
        } catch (e) {
          socket.emit(
            'error:message',
            e.message ||
              '技能使用失敗'
          );
        }
      }
    );

    /* -----------------------------------------------------
       Disconnect
    ----------------------------------------------------- */

    socket.on(
      'disconnect',
      () => {
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
            io
              .to(
                `room:${roomId}`
              )
              .emit(
                'presence',
                {
                  online: [
                    ...set
                  ]
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
   Shutdown
========================================================= */

async function shutdown(
  signal
) {
  console.log(
    `${signal} received, shutting down...`
  );

  io.close();

  server.close(
    async () => {
      await pool.end();

      process.exit(0);
    }
  );
}

process.on(
  'SIGTERM',
  () =>
    shutdown('SIGTERM')
);

process.on(
  'SIGINT',
  () =>
    shutdown('SIGINT')
);

/* =========================================================
   Start
========================================================= */

initDb()
  .then(() => {
    server.listen(
      PORT,
      HOST,
      () => {
        console.log(
          `RPG Web MVP listening on ${HOST}:${PORT}`
        );
      }
    );
  })
  .catch(err => {
    console.error(
      'Database initialization failed:',
      err
    );

    process.exit(1);
  });
