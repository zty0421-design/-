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
  throw new Error(
    'JWT_SECRET must be set in production'
  );
}

const DATABASE_URL =
  process.env.DATABASE_URL;

if (!DATABASE_URL) {
  throw new Error(
    'DATABASE_URL is required'
  );
}

const ADMIN_USERNAME =
  String(
    process.env.ADMIN_USERNAME || ''
  ).trim();

const ADMIN_PASSWORD =
  String(
    process.env.ADMIN_PASSWORD || ''
  );

const pool = new Pool({
  connectionString: DATABASE_URL,

  max:
    Number(
      process.env.PGPOOL_MAX || 10
    ),

  idleTimeoutMillis: 30000,

  connectionTimeoutMillis: 5000,

  ssl:
    process.env.PGSSL === 'disable'
      ? false
      : process.env.PGSSL === 'require'
        ? {
            rejectUnauthorized: false
          }
        : undefined
});

/* =========================================================
   DATABASE HELPERS
========================================================= */

function pgSql(sql) {
  let index = 0;

  return sql.replace(
    /\?/g,
    function () {
      index += 1;
      return '$' + index;
    }
  );
}

async function query(
  sql,
  params
) {
  return pool.query(
    pgSql(sql),
    params || []
  );
}

async function get(
  sql,
  params
) {
  const result =
    await query(
      sql,
      params || []
    );

  return (
    result.rows[0] || null
  );
}

async function all(
  sql,
  params
) {
  const result =
    await query(
      sql,
      params || []
    );

  return result.rows;
}

async function run(
  sql,
  params
) {
  const result =
    await query(
      sql,
      params || []
    );

  const row =
    result.rows[0] || null;

  return {
    id:
      row &&
      row.id !== undefined
        ? row.id
        : null,

    changes:
      result.rowCount,

    row
  };
}

/* =========================================================
   DATABASE INITIALIZATION
========================================================= */

async function initDb() {
  await pool.query(`
    CREATE TABLE IF NOT EXISTS users (
      id BIGSERIAL PRIMARY KEY,
      username VARCHAR(20) NOT NULL UNIQUE,
      password_hash TEXT NOT NULL,
      is_admin BOOLEAN NOT NULL DEFAULT FALSE,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

    CREATE TABLE IF NOT EXISTS rooms (
      id BIGSERIAL PRIMARY KEY,
      code VARCHAR(6) NOT NULL UNIQUE,
      name VARCHAR(60) NOT NULL,
      owner_id BIGINT NOT NULL
        REFERENCES users(id)
        ON DELETE CASCADE,
      status VARCHAR(20) NOT NULL DEFAULT 'lobby',
      round INTEGER NOT NULL DEFAULT 1,
      current_actor_id BIGINT,
      saved BOOLEAN NOT NULL DEFAULT FALSE,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      closed_at TIMESTAMPTZ
    );

    CREATE TABLE IF NOT EXISTS room_members (
      room_id BIGINT NOT NULL
        REFERENCES rooms(id)
        ON DELETE CASCADE,

      user_id BIGINT NOT NULL
        REFERENCES users(id)
        ON DELETE CASCADE,

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

      room_id BIGINT NOT NULL
        REFERENCES rooms(id)
        ON DELETE CASCADE,

      user_id BIGINT
        REFERENCES users(id)
        ON DELETE SET NULL,

      type VARCHAR(30) NOT NULL,

      payload JSONB NOT NULL,

      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

    CREATE TABLE IF NOT EXISTS character_cards (
      id BIGSERIAL PRIMARY KEY,

      user_id BIGINT NOT NULL UNIQUE
        REFERENCES users(id)
        ON DELETE CASCADE,

      name VARCHAR(50) NOT NULL DEFAULT '未命名角色',

      faction VARCHAR(20) NOT NULL DEFAULT '東國',

      tier INTEGER NOT NULL DEFAULT 1,

      level INTEGER NOT NULL DEFAULT 1,

      main_class VARCHAR(50) NOT NULL DEFAULT '未設定',

      sub_classes JSONB NOT NULL DEFAULT '[]'::jsonb,

      agility INTEGER NOT NULL DEFAULT 0,

      strength INTEGER NOT NULL DEFAULT 0,

      constitution INTEGER NOT NULL DEFAULT 0,

      spirit INTEGER NOT NULL DEFAULT 0,

      great_way INTEGER NOT NULL DEFAULT 0,

      great_way_name VARCHAR(100),

      luck INTEGER NOT NULL DEFAULT 10,

      attribute_points INTEGER NOT NULL DEFAULT 50,

      experience BIGINT NOT NULL DEFAULT 0,

      skill_points INTEGER NOT NULL DEFAULT 0,

      skills JSONB NOT NULL DEFAULT '[]'::jsonb,

      equipment JSONB NOT NULL DEFAULT '[]'::jsonb,

      weapons JSONB NOT NULL DEFAULT '[]'::jsonb,

      affinity_light INTEGER NOT NULL DEFAULT 0,

      affinity_dark INTEGER NOT NULL DEFAULT 0,

      affinity_metal INTEGER NOT NULL DEFAULT 0,

      affinity_wood INTEGER NOT NULL DEFAULT 0,

      affinity_water INTEGER NOT NULL DEFAULT 0,

      affinity_fire INTEGER NOT NULL DEFAULT 0,

      affinity_earth INTEGER NOT NULL DEFAULT 0,

      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,

      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

    CREATE TABLE IF NOT EXISTS character_room_bindings (
      room_id BIGINT NOT NULL
        REFERENCES rooms(id)
        ON DELETE CASCADE,

      character_id BIGINT NOT NULL
        REFERENCES character_cards(id)
        ON DELETE CASCADE,

      user_id BIGINT NOT NULL
        REFERENCES users(id)
        ON DELETE CASCADE,

      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,

      PRIMARY KEY(room_id, user_id),

      UNIQUE(room_id, character_id)
    );

    CREATE INDEX IF NOT EXISTS idx_room_members_room
      ON room_members(room_id);

    CREATE INDEX IF NOT EXISTS idx_room_members_user
      ON room_members(user_id);

    CREATE INDEX IF NOT EXISTS idx_events_room_id_id
      ON events(room_id, id);

    CREATE INDEX IF NOT EXISTS idx_character_room_bindings_room
      ON character_room_bindings(room_id);

    CREATE INDEX IF NOT EXISTS idx_character_room_bindings_character
      ON character_room_bindings(character_id);
  `);

  await pool.query(`
    ALTER TABLE users
    ADD COLUMN IF NOT EXISTS is_admin
    BOOLEAN NOT NULL DEFAULT FALSE;

    ALTER TABLE rooms
    ADD COLUMN IF NOT EXISTS saved
    BOOLEAN NOT NULL DEFAULT FALSE;

    ALTER TABLE rooms
    ADD COLUMN IF NOT EXISTS updated_at
    TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP;

    ALTER TABLE rooms
    ADD COLUMN IF NOT EXISTS closed_at
    TIMESTAMPTZ;
  `);

  await ensureAdmin();
}

/* =========================================================
   GLOBAL DM
========================================================= */

async function ensureAdmin() {
  if (
    !ADMIN_USERNAME ||
    !ADMIN_PASSWORD
  ) {
    throw new Error(
      'ADMIN_USERNAME and ADMIN_PASSWORD are required to configure the global DM account'
    );
  }

  if (
    ADMIN_PASSWORD.length < 6
  ) {
    throw new Error(
      'ADMIN_PASSWORD must be at least 6 characters'
    );
  }

  const existing =
    await get(
      `
        SELECT id
        FROM users
        WHERE username = ?
      `,
      [ADMIN_USERNAME]
    );

  const hash =
    await bcrypt.hash(
      ADMIN_PASSWORD,
      12
    );

  if (existing) {
    await run(
      `
        UPDATE users
        SET
          password_hash = ?,
          is_admin = TRUE
        WHERE id = ?
      `,
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
    `
      INSERT INTO users (
        username,
        password_hash,
        is_admin
      )
      VALUES (?, ?, TRUE)
      RETURNING id
    `,
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

      username:
        user.username,

      is_admin:
        Boolean(
          user.is_admin
        )
    },

    JWT_SECRET,

    {
      expiresIn: '7d'
    }
  );
}

function auth(
  req,
  res,
  next
) {
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

async function getUser(
  userId
) {
  return get(
    `
      SELECT
        id,
        username,
        is_admin,
        created_at
      FROM users
      WHERE id = ?
    `,
    [userId]
  );
}

async function requireAdminUser(
  userId
) {
  const user =
    await getUser(
      userId
    );

  if (
    !user ||
    !user.is_admin
  ) {
    throw Object.assign(
      new Error(
        '只有全站 DM 可以執行此操作'
      ),
      {
        status: 403
      }
    );
  }

  return user;
}

async function adminAuth(
  req,
  res,
  next
) {
  try {
    await requireAdminUser(
      req.user.id
    );

    next();
  } catch (error) {
    res.status(
      error.status || 403
    ).json({
      error:
        error.message ||
        '沒有 DM 權限'
    });
  }
}

function socketAuth(
  socket,
  next
) {
  try {
    socket.user =
      jwt.verify(
        socket.handshake.auth?.token || '',
        JWT_SECRET
      );

    next();
  } catch {
    next(
      new Error(
        'unauthorized'
      )
    );
  }
}

/* =========================================================
   CHARACTER CALCULATIONS
========================================================= */

function calculateCharacter(
  character
) {
  const faction =
    character.faction === '西國'
      ? '西國'
      : '東國';

  const tier =
    Math.max(
      1,
      Math.min(
        10,
        Number(character.tier) || 1
      )
    );

  const caps = {
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

  const attributeCap =
    caps[tier];

  const equipment =
    Array.isArray(character.equipment)
      ? character.equipment
      : [];

  const weapons =
    Array.isArray(character.weapons)
      ? character.weapons
      : [];

  const effects = [
    ...equipment,
    ...weapons
  ];

  const num = value => {
    const n = Number(value);
    return Number.isFinite(n) ? n : 0;
  };

  let allPercent = 0;

  const flat = {
    agility: 0,
    strength: 0,
    constitution: 0,
    spirit: 0,
    great_way: 0,
    faith: 0,
    luck: 0
  };

  const percent = {
    agility: 0,
    strength: 0,
    constitution: 0,
    spirit: 0,
    great_way: 0,
    faith: 0,
    luck: 0
  };

  for (const item of effects) {
    if (!item || typeof item !== 'object') {
      continue;
    }

    allPercent +=
      num(item.all_attributes_percent) ||
      num(item.all_attribute_percent);

    for (const field of Object.keys(flat)) {
      flat[field] += num(item[field]);
      flat[field] += num(item['flat_' + field]);

      percent[field] +=
        num(item[field + '_percent']) ||
        num(item[field + '_pct']);
    }
  }

  const calc = (field, base) => {
    return Math.floor(
      (
        num(base) +
        flat[field]
      ) *
      (
        1 +
        (
          allPercent +
          percent[field]
        ) / 100
      )
    );
  };

  const agility =
    calc('agility', character.agility);

  const strength =
    calc('strength', character.strength);

  const constitution =
    calc(
      'constitution',
      character.constitution
    );

  const spirit =
    calc('spirit', character.spirit);

  const fifthField =
    faction === '西國'
      ? 'faith'
      : 'great_way';

  const fifth =
    calc(
      fifthField,
      character[fifthField] || 0
    );

  const luck =
    Math.floor(
      (
        num(character.luck) +
        flat.luck
      ) *
      (
        1 +
        percent.luck / 100
      )
    );

  /*
   * 特殊屬性不直接吃「全屬性%」。
   * 但它們會依最終的體質／精神重新計算。
   */
  const endurance =
    Math.floor(
      (constitution + spirit) / 2
    );

  const sanity =
    Math.floor(
      spirit * 1.2
    );

  const will =
    Math.floor(
      spirit * 0.8
    );

  const life =
    Math.max(
      0,
      constitution * 2
    );

  return {
    ...character,

    attribute_cap:
      attributeCap,

    attribute_name:
      fifthField === 'faith'
        ? '信仰'
        : '大道',

    fifth_attribute_field:
      fifthField,

    final_attributes: {
      agility,
      strength,
      constitution,
      spirit,
      [fifthField]: fifth,
      luck
    },

    final_agility:
      agility,

    final_strength:
      strength,

    final_constitution:
      constitution,

    final_spirit:
      spirit,

    final_great_way:
      fifthField === 'great_way'
        ? fifth
        : num(character.great_way),

    final_faith:
      fifthField === 'faith'
        ? fifth
        : num(character.faith),

    final_luck:
      luck,

    endurance,
    sanity,
    will,
    life,

    max_hp:
      life,

    weird_resistance:
      faction === '西國'
        ? '高'
        : '低',

    weird_hatred:
      faction === '西國'
        ? 100
        : 150,

    trial_names: {
      agility: '敏捷試煉',
      strength: '力量試煉',
      constitution: '體質試煉',
      spirit: '精神試煉',
      [fifthField]:
        fifthField === 'faith'
          ? '信仰試煉'
          : '大道試煉'
    }
  };
}

/* =========================================================
   ROOM PERMISSIONS
========================================================= */

async function requireRoomMember(
  userId,
  roomId
) {
  const member =
    await get(
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

  if (!member) {
    throw Object.assign(
      new Error(
        '你不是房間成員'
      ),
      {
        status: 403
      }
    );
  }

  return member;
}

async function requireRoomOwner(
  userId,
  roomId
) {
  const room =
    await get(
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
      new Error(
        '只有房間 DM 可以執行此操作'
      ),
      {
        status: 403
      }
    );
  }

  return room;
}

/*
 * 重要：
 *
 * 房間 DM 不再由「建立房間的人」決定。
 *
 * 只有全站 is_admin = true 的帳號
 * 才可以執行 DM 管理功能。
 */

async function requireRoomAdmin(
  userId,
  roomId
) {
  const user =
    await requireAdminUser(
      userId
    );

  const room =
    await get(
      `
        SELECT *
        FROM rooms
        WHERE id = ?
      `,
      [roomId]
    );

  if (!room) {
    throw Object.assign(
      new Error(
        '找不到房間'
      ),
      {
        status: 404
      }
    );
  }

  return {
    user,
    room
  };
}

/* =========================================================
   ROOM CODE
========================================================= */

function generateCode() {
  const alphabet =
    'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';

  let code = '';

  for (
    let i = 0;
    i < 6;
    i += 1
  ) {
    code +=
      alphabet[
        Math.floor(
          Math.random() *
          alphabet.length
        )
      ];
  }

  return code;
}

async function uniqueCode() {
  let code;

  do {
    code =
      generateCode();
  } while (
    await get(
      `
        SELECT id
        FROM rooms
        WHERE code = ?
      `,
      [code]
    )
  );

  return code;
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
    `
      INSERT INTO events (
        room_id,
        user_id,
        type,
        payload
      )
      VALUES (
        ?,
        ?,
        ?,
        ?::jsonb
      )
    `,
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
    `
      UPDATE rooms
      SET updated_at =
        CURRENT_TIMESTAMP
      WHERE id = ?
    `,
    [roomId]
  );
}

/* =========================================================
   ROOM SNAPSHOT
========================================================= */

async function roomSnapshot(
  roomId
) {
  const room =
    await get(
      `
        SELECT
          id,
          code,
          name,
          owner_id,
          status,
          round,
          current_actor_id,
          saved,
          created_at,
          updated_at,
          closed_at
        FROM rooms
        WHERE id = ?
      `,
      [roomId]
    );

  if (!room) {
    return null;
  }

  const members =
    await all(
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

          u.username,
          u.is_admin,

          crb.character_id

        FROM room_members rm

        JOIN users u
          ON u.id = rm.user_id

        LEFT JOIN character_room_bindings crb
          ON crb.room_id = rm.room_id
          AND crb.user_id = rm.user_id

        WHERE rm.room_id = ?

        ORDER BY rm.joined_at ASC
      `,
      [roomId]
    );

  const events =
    await all(
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

    events:
      events
        .reverse()
        .map(
          function (event) {
            return {
              ...event,

              payload:
                parseJsonField(
                  event.payload,
                  {}
                )
            };
          }
        )
  };
}

/* =========================================================
   EXPRESS
========================================================= */

const app =
  express();

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
  async function (
    req,
    res
  ) {
    try {
      await pool.query(
        'SELECT 1'
      );

      res.json({
        ok: true,

        service:
          'trpg-online',

        database:
          'ok',

        time:
          new Date().toISOString()
      });
    } catch {
      res.status(503).json({
        ok: false,

        database:
          'unavailable'
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
  async function (
    req,
    res
  ) {
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

      if (
        password.length < 6
      ) {
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
          `
            INSERT INTO users (
              username,
              password_hash
            )
            VALUES (?, ?)
            RETURNING id
          `,
          [
            username,
            hash
          ]
        );

      /*
       * 注意：
       *
       * 新註冊的帳號永遠不是 DM。
       *
       * is_admin 不接受前端傳入。
       */

      const user = {
        id:
          result.id,

        username,

        is_admin:
          false
      };

      /*
       * 自動建立角色卡。
       *
       * 這樣玩家註冊後就有自己的角色卡，
       * 不需要進房間才能建立。
       */

      await run(
        `
          INSERT INTO character_cards (
            user_id
          )
          VALUES (?)
          ON CONFLICT (user_id)
          DO NOTHING
        `,
        [result.id]
      );

      res.json({
        token:
          signToken(
            user
          ),

        user
      });
    } catch (error) {
      res.status(
        error.code === '23505'
          ? 409
          : 500
      ).json({
        error:
          error.code === '23505'
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
  async function (
    req,
    res
  ) {
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
          `
            SELECT
              id,
              username,
              password_hash,
              is_admin
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
          error:
            '帳號或密碼錯誤'
        });
      }

      /*
       * 如果舊帳號沒有角色卡，
       * 登入時自動補建立。
       */

      await run(
        `
          INSERT INTO character_cards (
            user_id
          )
          VALUES (?)
          ON CONFLICT (user_id)
          DO NOTHING
        `,
        [user.id]
      );

      const safeUser = {
        id:
          user.id,

        username:
          user.username,

        is_admin:
          Boolean(
            user.is_admin
          )
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
   DM STATUS
========================================================= */

app.get(
  '/api/admin/status',
  auth,
  async function (
    req,
    res
  ) {
    try {
      const user =
        await getUser(
          req.user.id
        );

      if (!user) {
        return res.status(401).json({
          error: '帳號不存在'
        });
      }

      res.json({
        ok: true,
        username: user.username,
        is_admin: Boolean(user.is_admin),
        configured_admin: Boolean(
          ADMIN_USERNAME &&
          user.username === ADMIN_USERNAME
        )
      });
    } catch (error) {
      console.error(
        'ADMIN STATUS ERROR:',
        error
      );

      res.status(500).json({
        error: '讀取 DM 狀態失敗'
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
  async function (
    req,
    res
  ) {
    try {
      const user =
        await getUser(
          req.user.id
        );

      if (!user) {
        return res.status(401).json({
          error:
            '帳號不存在'
        });
      }

      res.json({
        user: {
          id:
            user.id,

          username:
            user.username,

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
   MY CHARACTER
========================================================= */

app.get(
  '/api/character',
  auth,
  async function (
    req,
    res
  ) {
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
   CREATE CHARACTER
========================================================= */

app.post(
  '/api/character',
  auth,
  async function (
    req,
    res
  ) {
    try {
      const exists =
        await get(
          `
            SELECT id
            FROM character_cards
            WHERE user_id = ?
          `,
          [req.user.id]
        );

      if (exists) {
        return res.status(409).json({
          error:
            '你已經有角色卡'
        });
      }

      await createCharacter(
        req.user.id,
        req.body
      );

      res.json({
        character:
          await getCharacterByUser(
            req.user.id
          )
      });
    } catch (error) {
      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
          '建立角色卡失敗'
      });
    }
  }
);

/* =========================================================
   CREATE CHARACTER HELPER
========================================================= */

async function createCharacter(
  userId,
  body
) {
  const name =
    String(
      body.name ||
      '未命名角色'
    )
      .trim()
      .slice(0, 50);

  const faction =
    body.faction === '西國'
      ? '西國'
      : '東國';

  const mainClass =
    String(
      body.main_class ||
      '未設定'
    )
      .trim()
      .slice(0, 50);

  const subClasses =
    Array.isArray(
      body.sub_classes
    )
      ? body.sub_classes.slice(0, 3)
      : [];

  const skills =
    Array.isArray(
      body.skills
    )
      ? body.skills
      : [];

  const equipment =
    Array.isArray(
      body.equipment
    )
      ? body.equipment
      : [];

  const weapons =
    Array.isArray(
      body.weapons
    )
      ? body.weapons
      : [];

  const luck =
    Math.max(
      0,
      Number(
        body.luck || 10
      ) || 10
    );

  await run(
    `
      INSERT INTO character_cards (
        user_id,
        name,
        faction,
        tier,
        level,
        main_class,
        faith,
        sub_classes,
        luck,
        attribute_points,
        skills,
        equipment,
        weapons,
        affinity_light,
        affinity_dark,
        affinity_metal,
        affinity_wood,
        affinity_water,
        affinity_fire,
        affinity_earth
      )
      VALUES (
        ?,
        ?,
        ?,
        1,
        1,
        ?,
        0,
        ?::jsonb,
        ?,
        50,
        ?::jsonb,
        ?::jsonb,
        ?::jsonb,
        0,
        0,
        0,
        0,
        0,
        0,
        0
      )
      RETURNING id
    `,
    [
      userId,

      name,

      faction,

      mainClass,

      JSON.stringify(
        subClasses
      ),

      luck,

      JSON.stringify(
        skills
      ),

      JSON.stringify(
        equipment
      ),

      JSON.stringify(
        weapons
      )
    ]
  );
}

/* =========================================================
   UPDATE OWN CHARACTER
========================================================= */

async function updateCharacter(
  userId,
  body
) {
  await requireCharacter(
    userId
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

  const jsonFields = [
    'sub_classes',
    'skills',
    'equipment',
    'weapons'
  ];

  const updates = [];
  const values = [];

  for (
    const field of numericFields
  ) {
    if (
      body[field] !==
      undefined
    ) {
      updates.push(
        field + ' = ?'
      );

      values.push(
        Math.max(
          0,
          Number(
            body[field]
          ) || 0
        )
      );
    }
  }

  for (
    const field of textFields
  ) {
    if (
      body[field] !==
      undefined
    ) {
      updates.push(
        field + ' = ?'
      );

      values.push(
        String(
          body[field]
        )
          .slice(0, 100)
      );
    }
  }

  for (
    const field of jsonFields
  ) {
    if (
      body[field] !==
      undefined
    ) {
      let value =
        Array.isArray(
          body[field]
        )
          ? body[field]
          : [];

      if (
        field ===
        'sub_classes'
      ) {
        value =
          value.slice(0, 3);
      }

      updates.push(
        field +
        ' = ?::jsonb'
      );

      values.push(
        JSON.stringify(
          value
        )
      );
    }
  }

  if (!updates.length) {
    throw Object.assign(
      new Error(
        '沒有可更新欄位'
      ),
      {
        status: 400
      }
    );
  }

  updates.push(
    'updated_at = CURRENT_TIMESTAMP'
  );

  values.push(
    userId
  );

  await run(
    `
      UPDATE character_cards
      SET
        ${updates.join(', ')}
      WHERE user_id = ?
    `,
    values
  );

  return getCharacterByUser(
    userId
  );
}

async function updatePlayerCharacter(
  userId,
  body
) {
  const current =
    await requireCharacter(
      userId
    );

  const faction =
    current.faction === '西國'
      ? '西國'
      : '東國';

  const fifthField =
    faction === '西國'
      ? 'faith'
      : 'great_way';

  const fields = [
    'agility',
    'strength',
    'constitution',
    'spirit',
    fifthField
  ];

  const oldValues = {};
  for (const field of fields) {
    oldValues[field] =
      Math.max(
        0,
        Math.floor(
          Number(current[field]) || 0
        )
      );
  }

  const oldPoints =
    Math.max(
      0,
      Math.floor(
        Number(current.attribute_points) || 0
      )
    );

  const oldTotal =
    fields.reduce(
      (sum, field) =>
        sum + oldValues[field],
      0
    );

  const next = {
    ...oldValues
  };

  for (const field of fields) {
    if (body[field] !== undefined) {
      next[field] =
        Math.max(
          0,
          Math.floor(
            Number(body[field]) || 0
          )
        );
    }
  }

  const capByTier = {
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

  const cap =
    capByTier[
      Math.max(
        1,
        Math.min(
          10,
          Number(current.tier) || 1
        )
      )
    ];

  for (const field of fields) {
    if (next[field] > cap) {
      throw Object.assign(
        new Error(
          `${field} 已達目前上限 ${cap}，必須完成對應屬性試煉`
        ),
        { status: 400 }
      );
    }
  }

  const newTotal =
    fields.reduce(
      (sum, field) =>
        sum + next[field],
      0
    );

  const requestedPoints =
    body.attribute_points !== undefined
      ? Math.max(
          0,
          Math.floor(
            Number(body.attribute_points) || 0
          )
        )
      : oldPoints;

  if (
    newTotal + requestedPoints !==
    oldTotal + oldPoints
  ) {
    throw Object.assign(
      new Error(
        '屬性點數不正確，玩家不能自行創造或消耗屬性點'
      ),
      { status: 400 }
    );
  }

  const updates = [];
  const values = [];

  for (const field of fields) {
    if (body[field] !== undefined) {
      updates.push(
        `${field} = ?`
      );
      values.push(next[field]);
    }
  }

  if (body.attribute_points !== undefined ||
      fields.some(field => body[field] !== undefined)) {
    updates.push(
      'attribute_points = ?'
    );
    values.push(requestedPoints);
  }

  if (body.name !== undefined) {
    updates.push('name = ?');
    values.push(
      String(body.name)
        .trim()
        .slice(0, 50)
    );
  }

  /*
   * 裝備／武器可以由既有系統處理；
   * 玩家不能藉由它們修改基礎屬性、等級、經驗或幸運。
   */
  for (const field of ['equipment', 'weapons']) {
    if (body[field] !== undefined) {
      const value =
        Array.isArray(body[field])
          ? body[field]
          : [];

      updates.push(
        `${field} = ?::jsonb`
      );

      values.push(
        JSON.stringify(value)
      );
    }
  }

  if (!updates.length) {
    throw Object.assign(
      new Error('沒有可更新欄位'),
      { status: 400 }
    );
  }

  updates.push(
    'updated_at = CURRENT_TIMESTAMP'
  );

  values.push(userId);

  await run(
    `
      UPDATE character_cards
      SET ${updates.join(', ')}
      WHERE user_id = ?
    `,
    values
  );

  return getCharacterByUser(userId);
}

app.patch(
  '/api/character',
  auth,
  async function (
    req,
    res
  ) {
    try {
      const user =
        await getUser(req.user.id);

      /*
       * DM 使用原本的 updateCharacter，
       * 保留最高權限，可無視試煉／儀式／經驗／詭晶等限制。
       */
      const character =
        user && user.is_admin
          ? await updateCharacter(
              req.user.id,
              req.body
            )
          : await updatePlayerCharacter(
              req.user.id,
              req.body
            );

      res.json({
        character
      });
    } catch (error) {
      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
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
  async function (
    req,
    res
  ) {
    try {
      const character =
        await requireCharacter(
          req.user.id
        );

      const skills =
        Array.isArray(
          character.skills
        )
          ? [
              ...character.skills
            ]
          : [];

      skills.push({
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
      });

      const updated =
        await updateCharacter(
          req.user.id,
          {
            skills
          }
        );

      res.json({
        character:
          updated
      });
    } catch (error) {
      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
          '新增技能失敗'
      });
    }
  }
);

/* =========================================================
   =========================================================
   全站 DM 區域
   =========================================================
   ========================================================= */

/*
 * 這裡非常重要：
 *
 * 以下 API 全部不需要進房間。
 *
 * DM 登入後直接可以：
 *
 * /api/admin/users
 * /api/admin/characters
 * /api/admin/players/:id
 * /api/admin/rooms
 *
 * 也就是「網站後台」。
 */

/* =========================================================
   ADMIN ME
========================================================= */

app.get(
  '/api/admin/me',
  auth,
  adminAuth,
  async function (
    req,
    res
  ) {
    const user =
      await requireAdminUser(
        req.user.id
      );

    res.json({
      admin: {
        id:
          user.id,

        username:
          user.username,

        is_admin:
          true
      }
    });
  }
);

/* =========================================================
   ADMIN USERS
========================================================= */

app.get(
  '/api/admin/users',
  auth,
  adminAuth,
  async function (
    req,
    res
  ) {
    try {
      const users =
        await all(
          `
            SELECT
              u.id,
              u.username,
              u.is_admin,
              u.created_at,

              c.id AS character_id,

              c.name AS character_name,

              c.level AS character_level,

              c.main_class AS character_class

            FROM users u

            LEFT JOIN character_cards c
              ON c.user_id = u.id

            ORDER BY u.id ASC
          `
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
   ADMIN ALL PLAYERS
========================================================= */

app.get(
  '/api/admin/players',
  auth,
  adminAuth,
  async function (
    req,
    res
  ) {
    try {
      const players =
        await all(
          `
            SELECT
              u.id,
              u.username,
              u.is_admin,
              u.created_at,

              c.id AS character_id,
              c.name AS character_name,
              c.faction,
              c.tier,
              c.level,
              c.main_class,
              c.sub_classes,
              c.agility,
              c.strength,
              c.constitution,
              c.spirit,
              c.great_way,
              c.great_way_name,
              c.luck,
              c.attribute_points,
              c.experience,
              c.skill_points,
              c.skills,
              c.equipment,
              c.weapons,
              c.affinity_light,
              c.affinity_dark,
              c.affinity_metal,
              c.affinity_wood,
              c.affinity_water,
              c.affinity_fire,
              c.affinity_earth,
              c.created_at AS character_created_at,
              c.updated_at AS character_updated_at

            FROM users u

            LEFT JOIN character_cards c
              ON c.user_id = u.id

            ORDER BY u.id ASC
          `
        );

      res.json({
        players:
          players.map(
            function (player) {
              const character =
                player.character_id
                  ? parseCharacter({
                      id:
                        player.character_id,

                      user_id:
                        player.id,

                      name:
                        player.character_name,

                      faction:
                        player.faction,

                      tier:
                        player.tier,

                      level:
                        player.level,

                      main_class:
                        player.main_class,

                      sub_classes:
                        player.sub_classes,

                      agility:
                        player.agility,

                      strength:
                        player.strength,

                      constitution:
                        player.constitution,

                      spirit:
                        player.spirit,

                      great_way:
                        player.great_way,

                      great_way_name:
                        player.great_way_name,

                      luck:
                        player.luck,

                      attribute_points:
                        player.attribute_points,

                      experience:
                        player.experience,

                      skill_points:
                        player.skill_points,

                      skills:
                        player.skills,

                      equipment:
                        player.equipment,

                      weapons:
                        player.weapons,

                      affinity_light:
                        player.affinity_light,

                      affinity_dark:
                        player.affinity_dark,

                      affinity_metal:
                        player.affinity_metal,

                      affinity_wood:
                        player.affinity_wood,

                      affinity_water:
                        player.affinity_water,

                      affinity_fire:
                        player.affinity_fire,

                      affinity_earth:
                        player.affinity_earth,

                      character_created_at:
                        player.character_created_at,

                      character_updated_at:
                        player.character_updated_at
                    })
                  : null;

              return {
                id:
                  player.id,

                username:
                  player.username,

                is_admin:
                  Boolean(
                    player.is_admin
                  ),

                created_at:
                  player.created_at,

                character
              };
            }
          )
      });
    } catch (error) {
      console.error(
        error
      );

      res.status(500).json({
        error:
          '讀取所有玩家資料失敗'
      });
    }
  }
);

/* =========================================================
   ADMIN ONE PLAYER
========================================================= */

app.get(
  '/api/admin/players/:id',
  auth,
  adminAuth,
  async function (
    req,
    res
  ) {
    try {
      const user =
        await getUser(
          req.params.id
        );

      if (!user) {
        return res.status(404).json({
          error:
            '找不到玩家'
        });
      }

      const character =
        await getCharacterByUser(
          req.params.id
        );

      res.json({
        user: {
          id:
            user.id,

          username:
            user.username,

          is_admin:
            Boolean(
              user.is_admin
            ),

          created_at:
            user.created_at
        },

        character
      });
    } catch {
      res.status(500).json({
        error:
          '讀取玩家資料失敗'
      });
    }
  }
);

/* =========================================================
   ADMIN UPDATE PLAYER CHARACTER
========================================================= */

app.patch(
  '/api/admin/players/:id/character',
  auth,
  adminAuth,
  async function (
    req,
    res
  ) {
    try {
      const targetId =
        Number(
          req.params.id
        );

      const targetUser =
        await getUser(
          targetId
        );

      if (!targetUser) {
        return res.status(404).json({
          error:
            '找不到玩家'
        });
      }

      let character =
        await getCharacterByUser(
          targetId
        );

      /*
       * 如果玩家還沒有角色卡，
       * DM 可以直接幫他建立。
       */

      if (!character) {
        await createCharacter(
          targetId,
          req.body
        );

        character =
          await getCharacterByUser(
            targetId
          );
      } else {
        character =
          await updateCharacter(
            targetId,
            req.body
          );
      }

      res.json({
        ok: true,

        character
      });
    } catch (error) {
      console.error(
        error
      );

      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
          'DM 修改角色卡失敗'
      });
    }
  }
);

/* =========================================================
   ADMIN DELETE CHARACTER
========================================================= */

app.delete(
  '/api/admin/players/:id/character',
  auth,
  adminAuth,
  async function (
    req,
    res
  ) {
    try {
      const targetId =
        Number(
          req.params.id
        );

      const user =
        await getUser(
          targetId
        );

      if (!user) {
        return res.status(404).json({
          error:
            '找不到玩家'
        });
      }

      await run(
        `
          DELETE FROM character_cards
          WHERE user_id = ?
        `,
        [targetId]
      );

      res.json({
        ok: true,

        message:
          '角色卡已刪除'
      });
    } catch {
      res.status(500).json({
        error:
          '刪除角色卡失敗'
      });
    }
  }
);

/* =========================================================
   ADMIN DELETE ACCOUNT
========================================================= */

app.delete(
  '/api/admin/users/:id',
  auth,
  adminAuth,
  async function (
    req,
    res
  ) {
    try {
      const targetId =
        Number(
          req.params.id
        );

      const target =
        await getUser(
          targetId
        );

      if (!target) {
        return res.status(404).json({
          error:
            '找不到帳號'
        });
      }

      /*
       * 防止 DM 刪除自己。
       */

      if (
        String(
          targetId
        ) ===
        String(
          req.user.id
        )
      ) {
        return res.status(400).json({
          error:
            '不能刪除目前登入中的 DM 帳號'
        });
      }

      /*
       * 不允許刪除其他 DM。
       *
       * 如果未來需要多 DM，
       * 可以再另外做權限階級。
       */

      if (
        target.is_admin
      ) {
        return res.status(403).json({
          error:
            '不能刪除其他 DM 帳號'
        });
      }

      await run(
        `
          DELETE FROM users
          WHERE id = ?
        `,
        [targetId]
      );

      res.json({
        ok: true,

        message:
          '玩家帳號、角色卡及相關資料已刪除'
      });
    } catch (error) {
      console.error(
        error
      );

      res.status(500).json({
        error:
          '刪除帳號失敗'
      });
    }
  }
);

/* =========================================================
   ADMIN ROOMS
========================================================= */

app.get(
  '/api/admin/rooms',
  auth,
  adminAuth,
  async function (
    req,
    res
  ) {
    try {
      const rooms =
        await all(
          `
            SELECT
              r.*,

              u.username
                AS owner_username,

              COUNT(
                rm.user_id
              )::INTEGER
                AS member_count

            FROM rooms r

            JOIN users u
              ON u.id =
                r.owner_id

            LEFT JOIN room_members rm
              ON rm.room_id =
                r.id

            GROUP BY
              r.id,
              u.username

            ORDER BY
              r.updated_at DESC
          `
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
   ADMIN ROOM
========================================================= */

app.get(
  '/api/admin/rooms/:id',
  auth,
  adminAuth,
  async function (
    req,
    res
  ) {
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
   ADMIN ROOM CHARACTERS
========================================================= */

app.get(
  '/api/admin/rooms/:id/characters',
  auth,
  adminAuth,
  async function (
    req,
    res
  ) {
    try {
      const characters =
        await all(
          `
            SELECT
              c.*,

              u.username

            FROM character_cards c

            JOIN character_room_bindings crb
              ON crb.character_id =
                c.id

            JOIN users u
              ON u.id =
                c.user_id

            WHERE crb.room_id = ?

            ORDER BY c.id ASC
          `,
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
   ADMIN CLOSE ROOM
========================================================= */

app.post(
  '/api/admin/rooms/:id/close',
  auth,
  adminAuth,
  async function (
    req,
    res
  ) {
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
          error:
            '找不到房間'
        });
      }

      await run(
        `
          UPDATE rooms

          SET
            saved = TRUE,
            status = 'closed',
            closed_at =
              CURRENT_TIMESTAMP,
            updated_at =
              CURRENT_TIMESTAMP

          WHERE id = ?
        `,
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

      const snapshot =
        await roomSnapshot(
          req.params.id
        );

      io.to(
        'room:' +
        req.params.id
      ).emit(
        'room:snapshot',
        snapshot
      );

      res.json({
        ok: true,

        room:
          snapshot
      });
    } catch (error) {
      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
          '關閉房間失敗'
      });
    }
  }
);

/* =========================================================
   ADMIN REOPEN ROOM
========================================================= */

app.post(
  '/api/admin/rooms/:id/reopen',
  auth,
  adminAuth,
  async function (
    req,
    res
  ) {
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
          error:
            '找不到房間'
        });
      }

      await run(
        `
          UPDATE rooms

          SET
            status = 'lobby',
            saved = FALSE,
            closed_at = NULL,
            updated_at =
              CURRENT_TIMESTAMP

          WHERE id = ?
        `,
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

      const snapshot =
        await roomSnapshot(
          req.params.id
        );

      io.to(
        'room:' +
        req.params.id
      ).emit(
        'room:snapshot',
        snapshot
      );

      res.json({
        ok: true,

        room:
          snapshot
      });
    } catch (error) {
      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
          '重新開啟房間失敗'
      });
    }
  }
);

/* =========================================================
   ADMIN DELETE ROOM
========================================================= */

app.delete(
  '/api/admin/rooms/:id',
  auth,
  adminAuth,
  async function (
    req,
    res
  ) {
    try {
      const room =
        await get(
          `
            SELECT id
            FROM rooms
            WHERE id = ?
          `,
          [req.params.id]
        );

      if (!room) {
        return res.status(404).json({
          error:
            '找不到房間'
        });
      }

      await run(
        `
          DELETE FROM rooms
          WHERE id = ?
        `,
        [req.params.id]
      );

      res.json({
        ok: true,

        message:
          '房間已永久刪除'
      });
    } catch {
      res.status(500).json({
        error:
          '刪除房間失敗'
      });
    }
  }
);

/* =========================================================
   =========================================================
   房間
   =========================================================
   ========================================================= */

/*
 * 只有全站 DM 可以建立房間。
 *
 * 這裡是修正「普通帳號也能成為 DM」的核心。
 */

app.post(
  '/api/rooms',
  auth,
  adminAuth,
  async function (
    req,
    res
  ) {
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
            INSERT INTO rooms (
              code,
              name,
              owner_id,
              status,
              saved
            )
            VALUES (
              ?,
              ?,
              ?,
              'lobby',
              FALSE
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
       * DM 可以存在於房間，
       * 但不需要角色卡。
       */

      await run(
        `
          INSERT INTO room_members (
            room_id,
            user_id,
            role,
            display_name,
            hp,
            max_hp,
            spirit,
            max_spirit
          )
          VALUES (
            ?,
            ?,
            'gm',
            ?,
            100,
            100,
            100,
            100
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
    } catch (error) {
      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
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
  async function (
    req,
    res
  ) {
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
          error:
            '找不到房間'
        });
      }

      if (
        room.status ===
        'closed'
      ) {
        return res.status(400).json({
          error:
            '這個房間目前已結束'
        });
      }

      /*
       * 玩家必須有角色卡。
       *
       * 但角色卡是帳號層級，
       * 不需要每進一個房間重新建立。
       */

      const character =
        await requireCharacter(
          req.user.id
        );

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
            INSERT INTO room_members (
              room_id,
              user_id,
              role,
              display_name,
              hp,
              max_hp,
              spirit,
              max_spirit
            )
            VALUES (
              ?,
              ?,
              'player',
              ?,
              ?,
              ?,
              ?,
              ?
            )
          `,
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

        /*
         * 只建立「房間與角色的關聯」。
         *
         * 不會建立新的角色卡。
         */

        await run(
          `
            INSERT INTO character_room_bindings (
              room_id,
              character_id,
              user_id
            )
            VALUES (
              ?,
              ?,
              ?
            )
            ON CONFLICT DO NOTHING
          `,
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
    } catch (error) {
      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
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
  async function (
    req,
    res
  ) {
    try {
      const user =
        await getUser(
          req.user.id
        );

      /*
       * DM 可以看到全部房間。
       *
       * 普通玩家只能看到自己加入的房間。
       */

      let rooms;

      if (
        user &&
        user.is_admin
      ) {
        rooms =
          await all(
            `
              SELECT
                r.id,
                r.code,
                r.name,
                r.owner_id,
                r.status,
                r.round,
                r.saved,
                r.created_at,
                r.updated_at
              FROM rooms r
              ORDER BY
                r.updated_at DESC
            `
          );
      } else {
        rooms =
          await all(
            `
              SELECT DISTINCT
                r.id,
                r.code,
                r.name,
                r.owner_id,
                r.status,
                r.round,
                r.saved,
                r.created_at,
                r.updated_at

              FROM rooms r

              JOIN room_members rm
                ON rm.room_id =
                  r.id

              WHERE rm.user_id = ?

              ORDER BY
                r.updated_at DESC
            `,
            [req.user.id]
          );
      }

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
  async function (
    req,
    res
  ) {
    try {
      const user =
        await getUser(
          req.user.id
        );

      /*
       * DM 不需要進房間。
       *
       * 玩家需要是房間成員。
       */

      if (
        !user ||
        !user.is_admin
      ) {
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
    } catch (error) {
      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
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
  async function (
    req,
    res
  ) {
    try {
      /*
       * 只有 DM 可以讀取房間全部角色卡。
       */

      await requireAdminUser(
        req.user.id
      );

      const characters =
        await all(
          `
            SELECT
              c.*,
              u.username

            FROM character_cards c

            JOIN character_room_bindings crb
              ON crb.character_id =
                c.id

            JOIN users u
              ON u.id =
                c.user_id

            WHERE crb.room_id = ?

            ORDER BY c.id ASC
          `,
          [req.params.id]
        );

      res.json({
        characters:
          characters.map(
            parseCharacter
          )
      });
    } catch (error) {
      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
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
  adminAuth,
  async function (
    req,
    res
  ) {
    try {
      await requireRoomAdmin(
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
            saved = FALSE,
            round = 1,
            current_actor_id = ?,
            updated_at =
              CURRENT_TIMESTAMP

          WHERE id = ?
        `,
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
        'room:' +
        req.params.id
      ).emit(
        'room:snapshot',
        snapshot
      );

      res.json(
        snapshot
      );
    } catch (error) {
      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
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
  adminAuth,
  async function (
    req,
    res
  ) {
    try {
      const room =
        await requireRoomAdmin(
          req.user.id,
          req.params.id
        );

      if (
        room.room.status !==
        'active'
      ) {
        return res.status(400).json({
          error:
            '跑團尚未開始'
        });
      }

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

      if (
        !members.length
      ) {
        return res.status(400).json({
          error:
            '沒有玩家'
        });
      }

      const current =
        room.room
          .current_actor_id;

      let index =
        members.findIndex(
          function (member) {
            return (
              String(
                member.user_id
              ) ===
              String(
                current
              )
            );
          }
        );

      if (
        index < 0
      ) {
        index = -1;
      }

      const nextIndex =
        (
          index + 1
        ) %
        members.length;

      const next =
        members[
          nextIndex
        ];

      const nextRound =
        nextIndex === 0
          ? room.room.round + 1
          : room.room.round;

      await run(
        `
          UPDATE rooms

          SET
            round = ?,
            current_actor_id = ?,
            updated_at =
              CURRENT_TIMESTAMP

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

      const snapshot =
        await roomSnapshot(
          req.params.id
        );

      io.to(
        'room:' +
        req.params.id
      ).emit(
        'room:snapshot',
        snapshot
      );

      res.json(
        snapshot
      );
    } catch (error) {
      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
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
  adminAuth,
  async function (
    req,
    res
  ) {
    try {
      await requireRoomAdmin(
        req.user.id,
        req.params.id
      );

      await run(
        `
          UPDATE rooms

          SET
            saved = TRUE,
            status = 'saved',
            updated_at =
              CURRENT_TIMESTAMP

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
            'DM 已將跑團存檔。'
        }
      );

      const snapshot =
        await roomSnapshot(
          req.params.id
        );

      io.to(
        'room:' +
        req.params.id
      ).emit(
        'room:snapshot',
        snapshot
      );

      res.json(
        snapshot
      );
    } catch (error) {
      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
          '存檔失敗'
      });
    }
  }
);

/* =========================================================
   CLOSE ROOM
========================================================= */

app.post(
  '/api/rooms/:id/leave',
  auth,
  adminAuth,
  async function (
    req,
    res
  ) {
    try {
      await requireRoomAdmin(
        req.user.id,
        req.params.id
      );

      await run(
        `
          UPDATE rooms

          SET
            saved = TRUE,
            status = 'closed',
            closed_at =
              CURRENT_TIMESTAMP,
            updated_at =
              CURRENT_TIMESTAMP

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
            'DM 結束本次跑團，房間已存檔。'
        }
      );

      const snapshot =
        await roomSnapshot(
          req.params.id
        );

      io.to(
        'room:' +
        req.params.id
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
    } catch (error) {
      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
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
  adminAuth,
  async function (
    req,
    res
  ) {
    try {
      await requireRoomAdmin(
        req.user.id,
        req.params.id
      );

      await run(
        `
          UPDATE rooms

          SET
            status = 'lobby',
            saved = FALSE,
            closed_at = NULL,
            updated_at =
              CURRENT_TIMESTAMP

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
            'DM 重新開啟了這場跑團。'
        }
      );

      const snapshot =
        await roomSnapshot(
          req.params.id
        );

      io.to(
        'room:' +
        req.params.id
      ).emit(
        'room:snapshot',
        snapshot
      );

      res.json(
        snapshot
      );
    } catch (error) {
      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
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
  async function (
    req,
    res
  ) {
    try {
      const currentUser =
        await getUser(
          req.user.id
        );

      const targetId =
        Number(
          req.params.userId
        );

      /*
       * DM 可以修改任何房間成員。
       *
       * 玩家只能修改自己。
       */

      const isAdmin =
        Boolean(
          currentUser &&
          currentUser.is_admin
        );

      if (
        !isAdmin &&
        String(
          req.user.id
        ) !==
        String(
          targetId
        )
      ) {
        return res.status(403).json({
          error:
            '沒有權限'
        });
      }

      if (!isAdmin) {
        await requireRoomMember(
          req.user.id,
          req.params.id
        );
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

      for (
        const field of fields
      ) {
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

      if (
        !updates.length
      ) {
        return res.status(400).json({
          error:
            '沒有可更新欄位'
        });
      }

      values.push(
        req.params.id
      );

      values.push(
        targetId
      );

      await run(
        `
          UPDATE room_members

          SET
            ${updates.join(', ')}

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
          target:
            targetId,

          changes:
            req.body
        }
      );

      const snapshot =
        await roomSnapshot(
          req.params.id
        );

      io.to(
        'room:' +
        req.params.id
      ).emit(
        'room:snapshot',
        snapshot
      );

      res.json(
        snapshot
      );
    } catch (error) {
      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
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
  async function (
    socket
  ) {
    /* =====================================================
       ENTER ROOM
    ===================================================== */

    socket.on(
      'room:enter',
      async function (
        data
      ) {
        try {
          const roomId =
            data &&
            data.roomId;

          const user =
            await getUser(
              socket.user.id
            );

          const isAdmin =
            Boolean(
              user &&
              user.is_admin
            );

          /*
           * DM 可以直接進任何房間。
           *
           * 玩家必須是房間成員。
           */

          if (
            !isAdmin
          ) {
            await requireRoomMember(
              socket.user.id,
              roomId
            );
          } else {
            const exists =
              await get(
                `
                  SELECT id
                  FROM rooms
                  WHERE id = ?
                `,
                [roomId]
              );

            if (!exists) {
              throw new Error(
                '找不到房間'
              );
            }
          }

          socket.join(
            'room:' +
            roomId
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
            'room:' +
            roomId
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
        } catch (error) {
          socket.emit(
            'error:message',
            error.message ||
              '進入房間失敗'
          );
        }
      }
    );

    /* =====================================================
       CHAT
    ===================================================== */

    socket.on(
      'chat:send',
      async function (
        data
      ) {
        try {
          const roomId =
            data &&
            data.roomId;

          const text =
            data &&
            data.text;

          const user =
            await getUser(
              socket.user.id
            );

          const isAdmin =
            Boolean(
              user &&
              user.is_admin
            );

          if (
            !isAdmin
          ) {
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
              text:
                safe
            }
          );

          io.to(
            'room:' +
            roomId
          ).emit(
            'room:snapshot',
            await roomSnapshot(
              roomId
            )
          );
        } catch (error) {
          socket.emit(
            'error:message',
            error.message ||
              '訊息失敗'
          );
        }
      }
    );

    /* =====================================================
       DICE
    ===================================================== */

    socket.on(
      'dice:roll',
      async function (
        data
      ) {
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
            await getUser(
              socket.user.id
            );

          const isAdmin =
            Boolean(
              user &&
              user.is_admin
            );

          if (
            !isAdmin
          ) {
            await requireRoomMember(
              socket.user.id,
              roomId
            );
          }

          const match =
            String(
              notation
            )
              .trim()
              .match(
                /^(\d{1,2})d(\d{1,4})([+-]\d{1,4})?$/i
              );

          if (!match) {
            return socket.emit(
              'error:message',
              '骰式格式例如 1d20 或 2d6+3'
            );
          }

          const count =
            Math.min(
              20,
              Number(
                match[1]
              )
            );

          const sides =
            Math.min(
              1000,
              Number(
                match[2]
              )
            );

          const modifier =
            Number(
              match[3] || 0
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
              function (
                sum,
                value
              ) {
                return (
                  sum +
                  value
                );
              },
              0
            ) +
            modifier;

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

              modifier,

              total
            }
          );

          io.to(
            'room:' +
            roomId
          ).emit(
            'room:snapshot',
            await roomSnapshot(
              roomId
            )
          );
        } catch (error) {
          socket.emit(
            'error:message',
            error.message ||
              '骰子失敗'
          );
        }
      }
    );

    /* =====================================================
       DISCONNECT
    ===================================================== */

    socket.on(
      'disconnect',
      function () {
        for (
          const [
            roomId,
            set
          ]
          of onlineByRoom.entries()
        ) {
          if (
            set.delete(
              socket.user.id
            )
          ) {
            io.to(
              'room:' +
              roomId
            ).emit(
              'presence',
              {
                online:
                  Array.from(
                    set
                  )
              }
            );

            if (
              !set.size
            ) {
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
    function (error) {
      console.error(
        'Database initialization failed:',
        error
      );

      process.exit(1);
    }
  )
/* =========================================================
   CHARACTER SYSTEM MIGRATION
========================================================= */

async function migrateCharacterSystem() {
  await run(`
    ALTER TABLE character_cards
      ADD COLUMN IF NOT EXISTS faith INTEGER NOT NULL DEFAULT 0
  `);

  await run(`
    ALTER TABLE character_cards
      ADD COLUMN IF NOT EXISTS agility_trial_tier INTEGER NOT NULL DEFAULT 1
  `);

  await run(`
    ALTER TABLE character_cards
      ADD COLUMN IF NOT EXISTS strength_trial_tier INTEGER NOT NULL DEFAULT 1
  `);

  await run(`
    ALTER TABLE character_cards
      ADD COLUMN IF NOT EXISTS constitution_trial_tier INTEGER NOT NULL DEFAULT 1
  `);

  await run(`
    ALTER TABLE character_cards
      ADD COLUMN IF NOT EXISTS spirit_trial_tier INTEGER NOT NULL DEFAULT 1
  `);

  await run(`
    ALTER TABLE character_cards
      ADD COLUMN IF NOT EXISTS great_way_trial_tier INTEGER NOT NULL DEFAULT 1
  `);

  await run(`
    ALTER TABLE character_cards
      ADD COLUMN IF NOT EXISTS faith_trial_tier INTEGER NOT NULL DEFAULT 1
  `);

  await run(`
    ALTER TABLE character_cards
      ADD COLUMN IF NOT EXISTS profession_rank VARCHAR(10) NOT NULL DEFAULT 'G'
  `);

  await run(`
    ALTER TABLE character_cards
      ADD COLUMN IF NOT EXISTS innate_sub_class VARCHAR(100)
  `);

  await run(`
    ALTER TABLE character_cards
      ADD COLUMN IF NOT EXISTS weird_hatred INTEGER NOT NULL DEFAULT 0
  `);
}

;