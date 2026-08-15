```js
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
    'DATABASE_URL is required. Example: postgresql://user:password@localhost:5432/rpg'
  );
}

/*
 * =========================================================
 * 全站管理員設定
 *
 * 建議 Render / Railway / Railway 等平台設定：
 *
 * ADMIN_USERNAME=DM
 * ADMIN_PASSWORD=你的管理員密碼
 *
 * 第一次啟動：
 * 如果帳號不存在 -> 自動建立
 * 如果帳號已存在 -> 自動升級為管理員
 * =========================================================
 */

const ADMIN_USERNAME =
  String(
    process.env.ADMIN_USERNAME || 'DM'
  )
    .trim()
    .slice(0, 20);

const ADMIN_PASSWORD =
  String(
    process.env.ADMIN_PASSWORD || ''
  );

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

function pgSql(sql) {
  let i = 0;

  return sql.replace(/\?/g, () => `$${++i}`);
}

const query = (sql, params = []) =>
  pool.query(pgSql(sql), params);

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
   DATABASE
========================================================= */

async function initDb() {
  await pool.query(`
    CREATE TABLE IF NOT EXISTS users (
      id BIGSERIAL PRIMARY KEY,
      username VARCHAR(20) NOT NULL UNIQUE,
      password_hash TEXT NOT NULL,

      /*
       * 全站管理員
       *
       * false = 一般玩家
       * true  = 全站 DM
       */
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

      created_at TIMESTAMPTZ NOT NULL
        DEFAULT CURRENT_TIMESTAMP,

      updated_at TIMESTAMPTZ NOT NULL
        DEFAULT CURRENT_TIMESTAMP,

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

      joined_at TIMESTAMPTZ NOT NULL
        DEFAULT CURRENT_TIMESTAMP,

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

      created_at TIMESTAMPTZ NOT NULL
        DEFAULT CURRENT_TIMESTAMP
    );

    /* =====================================================
       CHARACTER CARDS
    ===================================================== */

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

      sub_classes JSONB NOT NULL
        DEFAULT '[]'::jsonb,

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

      /* 技能 */
      skills JSONB NOT NULL
        DEFAULT '[]'::jsonb,

      /* 裝備 */
      equipment JSONB NOT NULL
        DEFAULT '[]'::jsonb,

      /* 武器 */
      weapons JSONB NOT NULL
        DEFAULT '[]'::jsonb,

      /* 光暗金木水火土親和 */
      affinity_light INTEGER NOT NULL DEFAULT 0,
      affinity_dark INTEGER NOT NULL DEFAULT 0,
      affinity_metal INTEGER NOT NULL DEFAULT 0,
      affinity_wood INTEGER NOT NULL DEFAULT 0,
      affinity_water INTEGER NOT NULL DEFAULT 0,
      affinity_fire INTEGER NOT NULL DEFAULT 0,
      affinity_earth INTEGER NOT NULL DEFAULT 0,

      created_at TIMESTAMPTZ NOT NULL
        DEFAULT CURRENT_TIMESTAMP,

      updated_at TIMESTAMPTZ NOT NULL
        DEFAULT CURRENT_TIMESTAMP
    );

    /* =====================================================
       CHARACTER ROOM BINDINGS
    ===================================================== */

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

      created_at TIMESTAMPTZ NOT NULL
        DEFAULT CURRENT_TIMESTAMP,

      PRIMARY KEY(room_id, user_id),

      UNIQUE(room_id, character_id)
    );

    /* =====================================================
       INDEX
    ===================================================== */

    CREATE INDEX IF NOT EXISTS
      idx_room_members_room
      ON room_members(room_id);

    CREATE INDEX IF NOT EXISTS
      idx_events_room_id_id
      ON events(room_id, id);

    CREATE INDEX IF NOT EXISTS
      idx_character_room_bindings_room
      ON character_room_bindings(room_id);

    CREATE INDEX IF NOT EXISTS
      idx_character_room_bindings_character
      ON character_room_bindings(character_id);

    CREATE INDEX IF NOT EXISTS
      idx_users_admin
      ON users(is_admin);
  `);

  /*
   * =======================================================
   * 舊資料庫升級
   * =======================================================
   */

  await pool.query(`
    ALTER TABLE users
      ADD COLUMN IF NOT EXISTS
      is_admin BOOLEAN NOT NULL DEFAULT FALSE;

    ALTER TABLE rooms
      ADD COLUMN IF NOT EXISTS
      saved BOOLEAN NOT NULL DEFAULT FALSE;

    ALTER TABLE rooms
      ADD COLUMN IF NOT EXISTS
      updated_at TIMESTAMPTZ
      NOT NULL DEFAULT CURRENT_TIMESTAMP;

    ALTER TABLE rooms
      ADD COLUMN IF NOT EXISTS
      closed_at TIMESTAMPTZ;
  `);

  /*
   * =======================================================
   * 建立／升級全站管理員
   * =======================================================
   */

  if (ADMIN_PASSWORD) {
    const existingAdmin =
      await get(
        `
          SELECT
            id,
            username
          FROM users
          WHERE username = ?
        `,
        [ADMIN_USERNAME]
      );

    if (!existingAdmin) {
      const passwordHash =
        await bcrypt.hash(
          ADMIN_PASSWORD,
          12
        );

      await run(
        `
          INSERT INTO users(
            username,
            password_hash,
            is_admin
          )
          VALUES(
            ?,
            ?,
            TRUE
          )
        `,
        [
          ADMIN_USERNAME,
          passwordHash
        ]
      );

      console.log(
        `[ADMIN] Created global DM account: ${ADMIN_USERNAME}`
      );
    } else {
      await run(
        `
          UPDATE users
          SET
            is_admin = TRUE
          WHERE id = ?
        `,
        [existingAdmin.id]
      );

      console.log(
        `[ADMIN] Global DM enabled: ${ADMIN_USERNAME}`
      );
    }
  } else {
    console.warn(
      '[ADMIN] ADMIN_PASSWORD is not set. No automatic admin account will be created.'
    );
  }
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

/*
 * =========================================================
 * 全站管理員權限
 *
 * 注意：
 * 不只相信 JWT 裡的 is_admin。
 *
 * 每次管理員操作都會重新查資料庫，
 * 因此如果帳號被取消管理員權限，
 * 舊 Token 也不能繼續操作。
 * =========================================================
 */

async function requireAdmin(
  req,
  res,
  next
) {
  try {
    if (!req.user?.id) {
      return res.status(401).json({
        error: '需要登入'
      });
    }

    const user =
      await get(
        `
          SELECT
            id,
            username,
            is_admin
          FROM users
          WHERE id = ?
        `,
        [req.user.id]
      );

    if (
      !user ||
      !user.is_admin
    ) {
      return res.status(403).json({
        error:
          '只有全站 DM 管理員可以執行此操作'
      });
    }

    req.admin = user;

    next();
  } catch (err) {
    console.error(
      'requireAdmin error:',
      err
    );

    return res.status(500).json({
      error:
        '管理員權限檢查失敗'
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
   ROOM CODE
========================================================= */

function generateCode() {
  const alphabet =
    'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';

  let out = '';

  for (
    let i = 0;
    i < 6;
    i++
  ) {
    out +=
      alphabet[
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
   CHARACTER CALCULATIONS
========================================================= */

function calculateCharacter(
  character
) {
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
        (constitution +
          spirit) / 2
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

/* =========================================================
   JSON NORMALIZATION
========================================================= */

function normalizeCharacter(
  character
) {
  if (!character) {
    return null;
  }

  return calculateCharacter({
    ...character,

    sub_classes:
      typeof character.sub_classes === 'string'
        ? JSON.parse(
            character.sub_classes
          )
        : character.sub_classes,

    skills:
      typeof character.skills === 'string'
        ? JSON.parse(
            character.skills
          )
        : character.skills,

    equipment:
      typeof character.equipment === 'string'
        ? JSON.parse(
            character.equipment
          )
        : character.equipment,

    weapons:
      typeof character.weapons === 'string'
        ? JSON.parse(
            character.weapons
          )
        : character.weapons
  });
}

/* =========================================================
   CHARACTER CARD
========================================================= */

async function getCharacterByUser(
  userId
) {
  const character =
    await get(
      `
        SELECT *
        FROM character_cards
        WHERE user_id = ?
      `,
      [userId]
    );

  return normalizeCharacter(
    character
  );
}

async function getCharacterById(
  characterId
) {
  const character =
    await get(
      `
        SELECT *
        FROM character_cards
        WHERE id = ?
      `,
      [characterId]
    );

  return normalizeCharacter(
    character
  );
}

async function requireCharacter(
  userId
) {
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

  if (!row) {
    throw Object.assign(
      new Error(
        '只有該房間 DM 可以執行此操作'
      ),
      {
        status: 403
      }
    );
  }

  return row;
}

/*
 * =========================================================
 * 房間 DM OR 全站 DM
 *
 * 這個權限給需要「管理某一場房間」的 API 使用。
 * =========================================================
 */

async function requireRoomManager(
  userId,
  roomId
) {
  const admin =
    await get(
      `
        SELECT
          id,
          is_admin
        FROM users
        WHERE id = ?
      `,
      [userId]
    );

  if (
    admin?.is_admin
  ) {
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
    `
      INSERT INTO events(
        room_id,
        user_id,
        type,
        payload
      )
      VALUES(
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
        payload
      )
    ]
  );

  await run(
    `
      UPDATE rooms
      SET
        updated_at =
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

          crb.character_id

        FROM room_members rm

        JOIN users u
          ON u.id = rm.user_id

        LEFT JOIN character_room_bindings crb
          ON crb.room_id =
            rm.room_id
          AND crb.user_id =
            rm.user_id

        WHERE rm.room_id = ?

        ORDER BY
          rm.joined_at ASC
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
        .map(e => ({
          ...e,

          payload:
            typeof e.payload === 'string'
              ? JSON.parse(
                  e.payload
                )
              : e.payload
        }))
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
  async (
    req,
    res
  ) => {
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
   AUTH REGISTER
========================================================= */

app.post(
  '/api/auth/register',
  async (
    req,
    res
  ) => {
    try {
      const username =
        String(
          req.body.username ||
            ''
        )
          .trim();

      const password =
        String(
          req.body.password ||
            ''
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
            INSERT INTO users(
              username,
              password_hash,
              is_admin
            )
            VALUES(
              ?,
              ?,
              FALSE
            )
            RETURNING id
          `,
          [
            username,
            hash
          ]
        );

      const user = {
        id:
          result.id,
        username,
        is_admin:
          false
      };

      res.json({
        token:
          signToken(
            user
          ),
        user
      });
    } catch (err) {
      res.status(
        err.code ===
          '23505'
          ? 409
          : 500
      ).json({
        error:
          err.code ===
          '23505'
            ? '帳號已存在'
            : '註冊失敗'
      });
    }
  }
);

/* =========================================================
   AUTH LOGIN
========================================================= */

app.post(
  '/api/auth/login',
  async (
    req,
    res
  ) => {
    try {
      const username =
        String(
          req.body.username ||
            ''
        )
          .trim();

      const password =
        String(
          req.body.password ||
            ''
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
   CURRENT USER
========================================================= */

app.get(
  '/api/me',
  auth,
  async (
    req,
    res
  ) => {
    try {
      const user =
        await get(
          `
            SELECT
              id,
              username,
              is_admin,
              created_at
            FROM users
            WHERE id = ?
          `,
          [req.user.id]
        );

      if (!user) {
        return res.status(404).json({
          error:
            '使用者不存在'
        });
      }

      res.json({
        user: {
          ...user,
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
   GLOBAL ADMIN
   全站管理員
========================================================= */

/*
 * 管理員自己的資訊
 */

app.get(
  '/api/admin/me',
  auth,
  requireAdmin,
  async (
    req,
    res
  ) => {
    res.json({
      admin: {
        id:
          req.admin.id,
        username:
          req.admin.username,
        is_admin:
          true
      }
    });
  }
);

/*
 * =========================================================
 * 全站玩家列表
 * =========================================================
 */

app.get(
  '/api/admin/users',
  auth,
  requireAdmin,
  async (
    req,
    res
  ) => {
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
              c.faction,
              c.tier,
              c.level,
              c.main_class

            FROM users u

            LEFT JOIN character_cards c
              ON c.user_id = u.id

            ORDER BY
              u.id ASC
          `
        );

      res.json({
        users:
          users.map(
            user => ({
              ...user,
              is_admin:
                Boolean(
                  user.is_admin
                )
            })
          )
      });
    } catch (e) {
      console.error(e);

      res.status(500).json({
        error:
          '讀取全站玩家失敗'
      });
    }
  }
);

/*
 * =========================================================
 * 全站角色卡
 * =========================================================
 */

app.get(
  '/api/admin/characters',
  auth,
  requireAdmin,
  async (
    req,
    res
  ) => {
    try {
      const characters =
        await all(
          `
            SELECT
              c.*,
              u.username

            FROM character_cards c

            JOIN users u
              ON u.id =
                c.user_id

            ORDER BY
              c.id ASC
          `
        );

      res.json({
        characters:
          characters.map(
            normalizeCharacter
          )
      });
    } catch (e) {
      console.error(e);

      res.status(500).json({
        error:
          '讀取全站角色卡失敗'
      });
    }
  }
);

/*
 * =========================================================
 * 查看指定角色卡
 * =========================================================
 */

app.get(
  '/api/admin/characters/:id',
  auth,
  requireAdmin,
  async (
    req,
    res
  ) => {
    try {
      const character =
        await get(
          `
            SELECT
              c.*,
              u.username

            FROM character_cards c

            JOIN users u
              ON u.id =
                c.user_id

            WHERE c.id = ?
          `,
          [
            req.params.id
          ]
        );

      if (!character) {
        return res.status(404).json({
          error:
            '找不到角色卡'
        });
      }

      res.json({
        character:
          normalizeCharacter(
            character
          )
      });
    } catch (e) {
      console.error(e);

      res.status(500).json({
        error:
          '讀取角色卡失敗'
      });
    }
  }
);

/*
 * =========================================================
 * 全站 DM 修改任意角色卡
 * =========================================================
 */

app.patch(
  '/api/admin/characters/:id',
  auth,
  requireAdmin,
  async (
    req,
    res
  ) => {
    try {
      const character =
        await getCharacterById(
          req.params.id
        );

      if (!character) {
        return res.status(404).json({
          error:
            '找不到角色卡'
        });
      }

      const fields = [
        'name',
        'faction',
        'tier',
        'level',
        'main_class',
        'agility',
        'strength',
        'constitution',
        'spirit',
        'great_way',
        'great_way_name',
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

      const updates = [];
      const values = [];

      for (
        const field of fields
      ) {
        if (
          req.body[field] !==
          undefined
        ) {
          let value =
            req.body[field];

          if (
            numericFields.includes(
              field
            )
          ) {
            value =
              Math.max(
                0,
                Number(value) || 0
              );
          } else {
            value =
              String(
                value
              ).slice(
                0,
                field ===
                  'name'
                  ? 50
                  : field ===
                    'main_class'
                    ? 50
                    : 100
              );
          }

          updates.push(
            `${field} = ?`
          );

          values.push(
            value
          );
        }
      }

      if (
        req.body.sub_classes !==
        undefined
      ) {
        updates.push(
          'sub_classes = ?::jsonb'
        );

        values.push(
          JSON.stringify(
            Array.isArray(
              req.body.sub_classes
            )
              ? req.body.sub_classes.slice(
                  0,
                  3
                )
              : []
          )
        );
      }

      if (
        req.body.skills !==
        undefined
      ) {
        updates.push(
          'skills = ?::jsonb'
        );

        values.push(
          JSON.stringify(
            Array.isArray(
              req.body.skills
            )
              ? req.body.skills
              : []
          )
        );
      }

      if (
        req.body.equipment !==
        undefined
      ) {
        updates.push(
          'equipment = ?::jsonb'
        );

        values.push(
          JSON.stringify(
            Array.isArray(
              req.body.equipment
            )
              ? req.body.equipment
              : []
          )
        );
      }

      if (
        req.body.weapons !==
        undefined
      ) {
        updates.push(
          'weapons = ?::jsonb'
        );

        values.push(
          JSON.stringify(
            Array.isArray(
              req.body.weapons
            )
              ? req.body.weapons
              : []
          )
        );
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
        req.params.id
      );

      await run(
        `
          UPDATE character_cards
          SET ${updates.join(
            ', '
          )}
          WHERE id = ?
        `,
        values
      );

      res.json({
        character:
          await getCharacterById(
            req.params.id
          )
      });
    } catch (e) {
      console.error(e);

      res.status(500).json({
        error:
          '管理員修改角色卡失敗'
      });
    }
  }
);

/*
 * =========================================================
 * 全站 DM 刪除角色卡
 *
 * 刪除後玩家可以重新建立一張。
 * =========================================================
 */

app.delete(
  '/api/admin/characters/:id',
  auth,
  requireAdmin,
  async (
    req,
    res
  ) => {
    try {
      const character =
        await get(
          `
            SELECT
              id,
              user_id,
              name
            FROM character_cards
            WHERE id = ?
          `,
          [
            req.params.id
          ]
        );

      if (!character) {
        return res.status(404).json({
          error:
            '找不到角色卡'
        });
      }

      /*
       * 先解除房間綁定
       */

      await run(
        `
          DELETE FROM
            character_room_bindings
          WHERE character_id = ?
        `,
        [
          req.params.id
        ]
      );

      /*
       * 再刪除角色卡
       */

      await run(
        `
          DELETE FROM
            character_cards
          WHERE id = ?
        `,
        [
          req.params.id
        ]
      );

      res.json({
        ok: true,

        message:
          `角色卡「${character.name}」已刪除`
      });
    } catch (e) {
      console.error(e);

      res.status(500).json({
        error:
          '刪除角色卡失敗'
      });
    }
  }
);

/*
 * =========================================================
 * 全站所有房間
 * =========================================================
 */

app.get(
  '/api/admin/rooms',
  auth,
  requireAdmin,
  async (
    req,
    res
  ) => {
    try {
      const rooms =
        await all(
          `
            SELECT
              r.id,
              r.code,
              r.name,
              r.owner_id,
              r.status,
              r.round,
              r.current_actor_id,
              r.saved,
              r.created_at,
              r.updated_at,
              r.closed_at,

              u.username AS owner_username,

              (
                SELECT COUNT(*)
                FROM room_members rm
                WHERE rm.room_id = r.id
                AND rm.role = 'player'
              ) AS player_count

            FROM rooms r

            LEFT JOIN users u
              ON u.id =
                r.owner_id

            ORDER BY
              r.updated_at DESC
          `
        );

      res.json({
        rooms
      });
    } catch (e) {
      console.error(e);

      res.status(500).json({
        error:
          '讀取全站房間失敗'
      });
    }
  }
);

/*
 * =========================================================
 * 全站 DM 查看任意房間
 * =========================================================
 */

app.get(
  '/api/admin/rooms/:id',
  auth,
  requireAdmin,
  async (
    req,
    res
  ) => {
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
    } catch (e) {
      console.error(e);

      res.status(500).json({
        error:
          '讀取房間失敗'
      });
    }
  }
);

/*
 * =========================================================
 * 全站 DM 查看任意房間所有角色卡
 * =========================================================
 */

app.get(
  '/api/admin/rooms/:id/characters',
  auth,
  requireAdmin,
  async (
    req,
    res
  ) => {
    try {
      const room =
        await get(
          `
            SELECT id
            FROM rooms
            WHERE id = ?
          `,
          [
            req.params.id
          ]
        );

      if (!room) {
        return res.status(404).json({
          error:
            '找不到房間'
        });
      }

      const characters =
        await all(
          `
            SELECT
              c.*,
              u.username,
              crb.room_id

            FROM character_cards c

            JOIN character_room_bindings crb
              ON crb.character_id =
                c.id

            JOIN users u
              ON u.id =
                c.user_id

            WHERE crb.room_id = ?

            ORDER BY
              c.id ASC
          `,
          [
            req.params.id
          ]
        );

      res.json({
        characters:
          characters.map(
            normalizeCharacter
          )
      });
    } catch (e) {
      console.error(e);

      res.status(500).json({
        error:
          '讀取房間角色卡失敗'
      });
    }
  }
);

/*
 * =========================================================
 * 全站 DM 關閉房間
 * =========================================================
 */

app.post(
  '/api/admin/rooms/:id/close',
  auth,
  requireAdmin,
  async (
    req,
    res
  ) => {
    try {
      const room =
        await get(
          `
            SELECT *
            FROM rooms
            WHERE id = ?
          `,
          [
            req.params.id
          ]
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
        [
          req.params.id
        ]
      );

      await addEvent(
        req.params.id,
        req.user.id,
        'system',
        {
          text:
            '全站 DM 已結束這場跑團，房間已存檔。'
        }
      );

      res.json(
        await roomSnapshot(
          req.params.id
        )
      );
    } catch (e) {
      console.error(e);

      res.status(500).json({
        error:
          '關閉房間失敗'
      });
    }
  }
);

/*
 * =========================================================
 * 全站 DM 重新開啟房間
 * =========================================================
 */

app.post(
  '/api/admin/rooms/:id/reopen',
  auth,
  requireAdmin,
  async (
    req,
    res
  ) => {
    try {
      const room =
        await get(
          `
            SELECT *
            FROM rooms
            WHERE id = ?
          `,
          [
            req.params.id
          ]
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
        [
          req.params.id
        ]
      );

      await addEvent(
        req.params.id,
        req.user.id,
        'system',
        {
          text:
            '全站 DM 重新開啟了這場跑團。'
        }
      );

      res.json(
        await roomSnapshot(
          req.params.id
        )
      );
    } catch (e) {
      console.error(e);

      res.status(500).json({
        error:
          '重新開啟房間失敗'
      });
    }
  }
);

/*
 * =========================================================
 * 全站 DM 強制移除玩家
 * =========================================================
 */

app.delete(
  '/api/admin/rooms/:id/members/:userId',
  auth,
  requireAdmin,
  async (
    req,
    res
  ) => {
    try {
      const room =
        await get(
          `
            SELECT *
            FROM rooms
            WHERE id = ?
          `,
          [
            req.params.id
          ]
        );

      if (!room) {
        return res.status(404).json({
          error:
            '找不到房間'
        });
      }

      const target =
        await get(
          `
            SELECT *
            FROM room_members
            WHERE room_id = ?
            AND user_id = ?
          `,
          [
            req.params.id,
            req.params.userId
          ]
        );

      if (!target) {
        return res.status(404).json({
          error:
            '該玩家不在此房間'
        });
      }

      /*
       * DM 不會被移除。
       * 房間擁有者也不會被移除。
       */

      if (
        target.role ===
        'gm'
      ) {
        return res.status(400).json({
          error:
            '不能移除房間 DM'
        });
      }

      await run(
        `
          DELETE FROM
            character_room_bindings
          WHERE room_id = ?
          AND user_id = ?
        `,
        [
          req.params.id,
          req.params.userId
        ]
      );

      await run(
        `
          DELETE FROM
            room_members
          WHERE room_id = ?
          AND user_id = ?
        `,
        [
          req.params.id,
          req.params.userId
        ]
      );

      await addEvent(
        req.params.id,
        req.user.id,
        'system',
        {
          text:
            `全站 DM 移除了玩家 ${target.display_name}`
        }
      );

      res.json(
        await roomSnapshot(
          req.params.id
        )
      );
    } catch (e) {
      console.error(e);

      res.status(500).json({
        error:
          '移除玩家失敗'
      });
    }
  }
);

/*
 * =========================================================
 * 全站 DM 修改使用者管理員權限
 *
 * 保留這個接口，但防止最後一個管理員
 * 把自己取消。
 * =========================================================
 */

app.patch(
  '/api/admin/users/:id',
  auth,
  requireAdmin,
  async (
    req,
    res
  ) => {
    try {
      const targetId =
        Number(
          req.params.id
        );

      const target =
        await get(
          `
            SELECT
              id,
              username,
              is_admin
            FROM users
            WHERE id = ?
          `,
          [
            targetId
          ]
        );

      if (!target) {
        return res.status(404).json({
          error:
            '找不到使用者'
        });
      }

      if (
        req.body.is_admin !==
        undefined
      ) {
        const nextAdmin =
          Boolean(
            req.body.is_admin
          );

        /*
         * 不允許自己取消自己的管理員權限。
         */

        if (
          String(
            target.id
          ) ===
          String(
            req.user.id
          ) &&
          !nextAdmin
        ) {
          return res.status(400).json({
            error:
              '不能取消自己的全站 DM 權限'
          });
        }

        /*
         * 防止系統變成完全沒有管理員。
         */

        if (
          !nextAdmin &&
          target.is_admin
        ) {
          const count =
            await get(
              `
                SELECT COUNT(*)::INTEGER
                  AS count
                FROM users
                WHERE is_admin = TRUE
              `
            );

          if (
            Number(
              count.count
            ) <= 1
          ) {
            return res.status(400).json({
              error:
                '系統至少需要保留一名全站 DM'
            });
          }
        }

        await run(
          `
            UPDATE users
            SET
              is_admin = ?
            WHERE id = ?
          `,
          [
            nextAdmin,
            targetId
          ]
        );
      }

      const updated =
        await get(
          `
            SELECT
              id,
              username,
              is_admin,
              created_at
            FROM users
            WHERE id = ?
          `,
          [
            targetId
          ]
        );

      res.json({
        user: {
          ...updated,
          is_admin:
            Boolean(
              updated.is_admin
            )
        }
      });
    } catch (e) {
      console.error(e);

      res.status(500).json({
        error:
          '更新管理員權限失敗'
      });
    }
  }
);

/* =========================================================
   CHARACTER CARD
========================================================= */

/*
 * 取得自己的角色卡
 */

app.get(
  '/api/character',
  auth,
  async (
    req,
    res
  ) => {
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
        error:
          '讀取角色卡失敗'
      });
    }
  }
);

/*
 * 建立角色卡
 *
 * 每個帳號只能建立一次
 */

app.post(
  '/api/character',
  auth,
  async (
    req,
    res
  ) => {
    try {
      const exists =
        await get(
          `
            SELECT id
            FROM character_cards
            WHERE user_id = ?
          `,
          [
            req.user.id
          ]
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
          .slice(
            0,
            50
          );

      const faction =
        req.body.faction ===
        '西國'
          ? '西國'
          : '東國';

      const mainClass =
        String(
          req.body.main_class ||
            '未設定'
        )
          .trim()
          .slice(
            0,
            50
          );

      const subClasses =
        Array.isArray(
          req.body.sub_classes
        )
          ? req.body.sub_classes
          : [];

      const result =
        await run(
          `
            INSERT INTO character_cards(
              user_id,
              name,
              faction,
              tier,
              level,
              main_class,
              sub_classes,

              agility,
              strength,
              constitution,
              spirit,
              great_way,

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
            VALUES(
              ?,
              ?,
              ?,
              ?,
              ?,
              ?,
              ?,

              ?,
              ?,
              ?,
              ?,
              ?,

              ?,
              ?,

              ?::jsonb,
              ?::jsonb,
              ?::jsonb,

              ?,
              ?,
              ?,
              ?,
              ?,
              ?,
              ?
            )
            RETURNING id
          `,
          [
            req.user.id,

            name,
            faction,

            1,
            1,

            mainClass,

            JSON.stringify(
              subClasses.slice(
                0,
                3
              )
            ),

            0,
            0,
            0,
            0,
            0,

            Number(
              req.body.luck ||
                10
            ),

            50,

            JSON.stringify(
              Array.isArray(
                req.body.skills
              )
                ? req.body.skills
                : []
            ),

            JSON.stringify(
              Array.isArray(
                req.body.equipment
              )
                ? req.body.equipment
                : []
            ),

            JSON.stringify(
              Array.isArray(
                req.body.weapons
              )
                ? req.body.weapons
                : []
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
    } catch (err) {
      if (
        err.code ===
        '23505'
      ) {
        return res.status(409).json({
          error:
            '你已經有角色卡'
        });
      }

      console.error(err);

      res.status(500).json({
        error:
          '建立角色卡失敗'
      });
    }
  }
);

/*
 * 修改自己的角色卡
 *
 * DM 可以透過全站管理員 API 修改任何角色卡。
 */

app.patch(
  '/api/character',
  auth,
  async (
    req,
    res
  ) => {
    try {
      const character =
        await requireCharacter(
          req.user.id
        );

      const fields = [
        'name',
        'faction',
        'tier',
        'level',
        'main_class',
        'agility',
        'strength',
        'constitution',
        'spirit',
        'great_way',
        'great_way_name',
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

      const updates = [];
      const values = [];

      for (
        const field of fields
      ) {
        if (
          req.body[field] !==
          undefined
        ) {
          let value =
            req.body[field];

          if (
            numericFields.includes(
              field
            )
          ) {
            value =
              Math.max(
                0,
                Number(value) || 0
              );
          } else {
            value =
              String(
                value
              ).slice(
                0,
                100
              );
          }

          updates.push(
            `${field} = ?`
          );

          values.push(
            value
          );
        }
      }

      if (
        req.body.sub_classes !==
        undefined
      ) {
        updates.push(
          'sub_classes = ?::jsonb'
        );

        values.push(
          JSON.stringify(
            Array.isArray(
              req.body.sub_classes
            )
              ? req.body.sub_classes.slice(
                  0,
                  3
                )
              : []
          )
        );
      }

      if (
        req.body.skills !==
        undefined
      ) {
        updates.push(
          'skills = ?::jsonb'
        );

        values.push(
          JSON.stringify(
            Array.isArray(
              req.body.skills
            )
              ? req.body.skills
              : []
          )
        );
      }

      if (
        req.body.equipment !==
        undefined
      ) {
        updates.push(
          'equipment = ?::jsonb'
        );

        values.push(
          JSON.stringify(
            Array.isArray(
              req.body.equipment
            )
              ? req.body.equipment
              : []
          )
        );
      }

      if (
        req.body.weapons !==
        undefined
      ) {
        updates.push(
          'weapons = ?::jsonb'
        );

        values.push(
          JSON.stringify(
            Array.isArray(
              req.body.weapons
            )
              ? req.body.weapons
              : []
          )
        );
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
        `
          UPDATE character_cards
          SET ${updates.join(
            ', '
          )}
          WHERE user_id = ?
        `,
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
   CHARACTER SKILLS
========================================================= */

app.post(
  '/api/character/skill',
  auth,
  async (
    req,
    res
  ) => {
    try {
      const character =
        await requireCharacter(
          req.user.id
        );

      const skill = {
        id:
          String(
            req.body.id ||
              `skill_${Date.now()}`
          ).slice(
            0,
            50
          ),

        name:
          String(
            req.body.name ||
              '未命名技能'
          ).slice(
            0,
            100
          ),

        type:
          String(
            req.body.type ||
              'active'
          ).slice(
            0,
            30
          ),

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
          ).slice(
            0,
            2000
          ),

        cost:
          Math.max(
            0,
            Number(
              req.body.cost ||
                0
            )
          ),

        data:
          req.body.data ||
          {}
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
        `
          UPDATE character_cards
          SET
            skills = ?::jsonb,
            updated_at =
              CURRENT_TIMESTAMP
          WHERE user_id = ?
        `,
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
   ROOM CREATE
========================================================= */

app.post(
  '/api/rooms',
  auth,
  async (
    req,
    res
  ) => {
    try {
      const name =
        String(
          req.body.name ||
            '未命名跑團'
        )
          .trim()
          .slice(
            0,
            60
          ) ||
        '未命名跑團';

      const character =
        await requireCharacter(
          req.user.id
        );

      const code =
        await uniqueCode();

      const result =
        await run(
          `
            INSERT INTO rooms(
              code,
              name,
              owner_id,
              status,
              saved
            )
            VALUES(
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

      await run(
        `
          INSERT INTO room_members(
            room_id,
            user_id,
            role,
            display_name,
            hp,
            max_hp,
            spirit,
            max_spirit
          )
          VALUES(
            ?,
            ?,
            'gm',
            ?,
            ?,
            ?,
            ?,
            ?
          )
        `,
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
        `
          INSERT INTO character_room_bindings(
            room_id,
            character_id,
            user_id
          )
          VALUES(
            ?,
            ?,
            ?
          )
        `,
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
            `${req.user.username} 建立了跑團「${name}」`
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
  async (
    req,
    res
  ) => {
    try {
      const code =
        String(
          req.body.code ||
            ''
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
            INSERT INTO room_members(
              room_id,
              user_id,
              role,
              display_name,
              hp,
              max_hp,
              spirit,
              max_spirit
            )
            VALUES(
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

        await run(
          `
            INSERT INTO character_room_bindings(
              room_id,
              character_id,
              user_id
            )
            VALUES(
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
              `${character.name} 加入房間`
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
  async (
    req,
    res
  ) => {
    try {
      const rooms =
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
          [
            req.user.id
          ]
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
  async (
    req,
    res
  ) => {
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
   ROOM DM VIEW ALL CHARACTER CARDS
========================================================= */

app.get(
  '/api/rooms/:id/characters',
  auth,
  async (
    req,
    res
  ) => {
    try {
      await requireRoomManager(
        req.user.id,
        req.params.id
      );

      const characters =
        await all(
          `
            SELECT
              c.*,
              u.username,
              crb.room_id

            FROM character_cards c

            JOIN character_room_bindings crb
              ON crb.character_id =
                c.id

            JOIN users u
              ON u.id =
                c.user_id

            WHERE crb.room_id = ?

            ORDER BY
              c.id ASC
          `,
          [
            req.params.id
          ]
        );

      res.json({
        characters:
          characters.map(
            normalizeCharacter
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
   START CAMPAIGN
========================================================= */

app.post(
  '/api/rooms/:id/start',
  auth,
  async (
    req,
    res
  ) => {
    try {
      await requireRoomManager(
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
            ORDER BY
              joined_at ASC
            LIMIT 1
          `,
          [
            req.params.id
          ]
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
  async (
    req,
    res
  ) => {
    try {
      await requireRoomManager(
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
          [
            req.params.id
          ]
        );

      if (
        room.status !==
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
            ORDER BY
              joined_at ASC
          `,
          [
            req.params.id
          ]
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

      res.json(
        await roomSnapshot(
          req.params.id
        )
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
  async (
    req,
    res
  ) => {
    try {
      await requireRoomManager(
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
        [
          req.params.id
        ]
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

      res.json(
        await roomSnapshot(
          req.params.id
        )
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
   DM EXIT
   DM 退出 = 存檔並結束目前房間
========================================================= */

app.post(
  '/api/rooms/:id/leave',
  auth,
  async (
    req,
    res
  ) => {
    try {
      await requireRoomManager(
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
        [
          req.params.id
        ]
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
   REOPEN SAVED ROOM
========================================================= */

app.post(
  '/api/rooms/:id/reopen',
  auth,
  async (
    req,
    res
  ) => {
    try {
      await requireRoomManager(
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
        [
          req.params.id
        ]
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

      res.json(
        await roomSnapshot(
          req.params.id
        )
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
  async (
    req,
    res
  ) => {
    try {
      await requireRoomMember(
        req.user.id,
        req.params.id
      );

      const target =
        Number(
          req.params.userId
        );

      const owner =
        await get(
          `
            SELECT owner_id
            FROM rooms
            WHERE id = ?
          `,
          [
            req.params.id
          ]
        );

      const admin =
        await get(
          `
            SELECT is_admin
            FROM users
            WHERE id = ?
          `,
          [
            req.user.id
          ]
        );

      const isOwner =
        owner &&
        String(
          owner.owner_id
        ) ===
        String(
          req.user.id
        );

      const isAdmin =
        Boolean(
          admin?.is_admin
        );

      const isSelf =
        String(
          req.user.id
        ) ===
        String(
          target
        );

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

      for (
        const field of fields
      ) {
        if (
          req.body[field] !==
          undefined
        ) {
          updates.push(
            `${field} = ?`
          );

          values.push(
            field ===
              'display_name'
              ? String(
                  req.body[field]
                ).slice(
                  0,
                  30
                )
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
        `
          UPDATE room_members
          SET ${updates.join(
            ', '
          )}
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
   SOCKET.IO
========================================================= */

const server =
  http.createServer(
    app
  );

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
  async socket => {

    /* ================================================
       ENTER ROOM
    ================================================= */

    socket.on(
      'room:enter',
      async ({
        roomId
      }) => {
        try {
          /*
           * 全站 DM 可以進入任何房間。
           * 一般玩家仍然必須是房間成員。
           */

          const admin =
            await get(
              `
                SELECT is_admin
                FROM users
                WHERE id = ?
              `,
              [
                socket.user.id
              ]
            );

          if (
            !admin?.is_admin
          ) {
            await requireRoomMember(
              socket.user.id,
              roomId
            );
          } else {
            const room =
              await get(
                `
                  SELECT id
                  FROM rooms
                  WHERE id = ?
                `,
                [roomId]
              );

            if (!room) {
              throw new Error(
                '找不到房間'
              );
            }
          }

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

          io.to(
            `room:${roomId}`
          ).emit(
            'presence',
            {
              online:
                [
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

    /* ================================================
       CHAT
    ================================================= */

    socket.on(
      'chat:send',
      async ({
        roomId,
        text
      }) => {
        try {
          const admin =
            await get(
              `
                SELECT is_admin
                FROM users
                WHERE id = ?
              `,
              [
                socket.user.id
              ]
            );

          if (
            !admin?.is_admin
          ) {
            await requireRoomMember(
              socket.user.id,
              roomId
            );
          } else {
            const room =
              await get(
                `
                  SELECT id
                  FROM rooms
                  WHERE id = ?
                `,
                [roomId]
              );

            if (!room) {
              throw new Error(
                '找不到房間'
              );
            }
          }

          const safe =
            String(
              text || ''
            )
              .trim()
              .slice(
                0,
                1000
              );

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
            `room:${roomId}`
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
      async ({
        roomId,
        notation = '1d20',
        label = '擲骰'
      }) => {
        try {
          const admin =
            await get(
              `
                SELECT is_admin
                FROM users
                WHERE id = ?
              `,
              [
                socket.user.id
              ]
            );

          if (
            !admin?.is_admin
          ) {
            await requireRoomMember(
              socket.user.id,
              roomId
            );
          } else {
            const room =
              await get(
                `
                  SELECT id
                  FROM rooms
                  WHERE id = ?
                `,
                [roomId]
              );

            if (!room) {
              throw new Error(
                '找不到房間'
              );
            }
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
              Number(
                m[1]
              )
            );

          const sides =
            Math.min(
              1000,
              Number(
                m[2]
              )
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
              () =>
                1 +
                Math.floor(
                  Math.random() *
                    sides
                )
            );

          const total =
            rolls.reduce(
              (
                a,
                b
              ) =>
                a + b,
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
                ).slice(
                  0,
                  80
                ),

              rolls,

              modifier:
                mod,

              total
            }
          );

          io.to(
            `room:${roomId}`
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
            io.to(
              `room:${roomId}`
            ).emit(
              'presence',
              {
                online:
                  [
                    ...set
                  ]
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
   ERROR HANDLER
========================================================= */

app.use(
  (
    err,
    req,
    res,
    next
  ) => {
    console.error(
      'Unhandled error:',
      err
    );

    if (
      res.headersSent
    ) {
      return next(err);
    }

    res.status(500).json({
      error:
        '伺服器內部錯誤'
    });
  }
);

/* =========================================================
   SHUTDOWN
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

      process.exit(
        0
      );
    }
  );
}

process.on(
  'SIGTERM',
  () =>
    shutdown(
      'SIGTERM'
    )
);

process.on(
  'SIGINT',
  () =>
    shutdown(
      'SIGINT'
    )
);

/* =========================================================
   START SERVER
========================================================= */

initDb()
  .then(
    () => {
      server.listen(
        PORT,
        HOST,
        () =>
          console.log(
            `TRPG Online listening on ${HOST}:${PORT}`
          )
      );
    }
  )
  .catch(
    err => {
      console.error(
        'Database initialization failed:',
        err
      );

      process.exit(
        1
      );
    }
  );
```
     
