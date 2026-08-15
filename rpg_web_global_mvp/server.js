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

const DM_USERNAME =
  String(process.env.DM_USERNAME || '').trim();

if (
  process.env.NODE_ENV === 'production' &&
  JWT_SECRET === 'dev-only-change-me'
) {
  throw new Error('JWT_SECRET must be set in production');
}

if (!DM_USERNAME) {
  console.warn(
    '[WARNING] DM_USERNAME is not set. No account will automatically become DM.'
  );
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

      role VARCHAR(10) NOT NULL DEFAULT 'player',

      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

    CREATE TABLE IF NOT EXISTS rooms (
      id BIGSERIAL PRIMARY KEY,
      code VARCHAR(6) NOT NULL UNIQUE,
      name VARCHAR(60) NOT NULL,

      /*
       * owner_id 現在只代表：
       * 「這個房間最初由誰建立」
       *
       * 不代表 DM。
       */
      owner_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,

      status VARCHAR(20) NOT NULL DEFAULT 'lobby',
      round INTEGER NOT NULL DEFAULT 1,
      current_actor_id BIGINT,

      saved BOOLEAN NOT NULL DEFAULT FALSE,

      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      closed_at TIMESTAMPTZ
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
  `);

  /*
   * =======================================================
   * 舊資料庫升級
   * =======================================================
   */

  await pool.query(`
    ALTER TABLE users
      ADD COLUMN IF NOT EXISTS
      role VARCHAR(10) NOT NULL DEFAULT 'player';

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
   * 修正舊資料
   * =======================================================
   */

  await pool.query(`
    UPDATE users
    SET role = 'player'
    WHERE role IS NULL
       OR role NOT IN ('dm', 'player');
  `);

  /*
   * =======================================================
   * 指定 DM 帳號
   * =======================================================
   *
   * DM_USERNAME 例如：
   *
   * DM_USERNAME=admin
   *
   * 如果 admin 已存在：
   *   自動變成 DM
   *
   * 如果 admin 尚不存在：
   *   使用者之後註冊 admin 時會成為 DM
   */

  if (DM_USERNAME) {
    await query(
      `
        UPDATE users
        SET role = 'dm'
        WHERE username = ?
      `,
      [DM_USERNAME]
    );

    console.log(
      `[AUTH] DM account configured: ${DM_USERNAME}`
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
      username: user.username
    },
    JWT_SECRET,
    {
      expiresIn: '7d'
    }
  );
}

/*
 * HTTP 身分驗證
 *
 * JWT 只負責確認「是哪個帳號」。
 *
 * 真正的 DM / PLAYER 身分
 * 每次從資料庫確認。
 */
async function auth(req, res, next) {
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
    const decoded =
      jwt.verify(
        token,
        JWT_SECRET
      );

    const user =
      await get(
        `
          SELECT
            id,
            username,
            role
          FROM users
          WHERE id = ?
        `,
        [decoded.id]
      );

    if (!user) {
      return res.status(401).json({
        error: '帳號不存在'
      });
    }

    req.user = {
      id: user.id,
      username: user.username,
      role: user.role
    };

    next();

  } catch {
    return res.status(401).json({
      error: '登入已失效'
    });
  }
}

/*
 * Socket.IO 身分驗證
 */
async function socketAuth(socket, next) {
  try {
    const decoded =
      jwt.verify(
        socket.handshake.auth?.token || '',
        JWT_SECRET
      );

    const user =
      await get(
        `
          SELECT
            id,
            username,
            role
          FROM users
          WHERE id = ?
        `,
        [decoded.id]
      );

    if (!user) {
      return next(
        new Error('unauthorized')
      );
    }

    socket.user = {
      id: user.id,
      username: user.username,
      role: user.role
    };

    next();

  } catch {
    next(
      new Error('unauthorized')
    );
  }
}

/* =========================================================
   ROLE PERMISSION
========================================================= */

function isDM(user) {
  return (
    user &&
    String(user.role).toLowerCase() === 'dm'
  );
}

function requireDM(req, res, next) {
  if (!isDM(req.user)) {
    return res.status(403).json({
      error: '需要 DM 管理員權限'
    });
  }

  next();
}

/*
 * Promise 版本
 *
 * 用於 async route。
 */
function assertDM(user) {
  if (!isDM(user)) {
    throw Object.assign(
      new Error('需要 DM 管理員權限'),
      {
        status: 403
      }
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

  for (let i = 0; i < 6; i++) {
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
    Number(character.constitution) || 0;

  const spirit =
    Number(character.spirit) || 0;

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

/* =========================================================
   CHARACTER CARD
========================================================= */

async function getCharacterByUser(userId) {
  const character =
    await get(
      `
        SELECT *
        FROM character_cards
        WHERE user_id = ?
      `,
      [userId]
    );

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

async function requireCharacter(userId) {
  const character =
    await getCharacterByUser(
      userId
    );

  if (!character) {
    throw Object.assign(
      new Error('你還沒有建立角色卡'),
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
      new Error('你不是房間成員'),
      {
        status: 403
      }
    );
  }

  return row;
}

/*
 * 舊名稱保留，避免其他地方出錯。
 *
 * 現在它的意義已經完全改變：
 *
 * 只有「系統 DM」才能執行管理操作。
 *
 * 不再檢查 rooms.owner_id。
 */
async function requireOwner(
  userId,
  roomId
) {
  const user =
    await get(
      `
        SELECT
          id,
          username,
          role
        FROM users
        WHERE id = ?
      `,
      [userId]
    );

  if (!user) {
    throw Object.assign(
      new Error('帳號不存在'),
      {
        status: 401
      }
    );
  }

  if (
    String(user.role).toLowerCase() !==
    'dm'
  ) {
    throw Object.assign(
      new Error(
        '只有系統 DM 可以執行此操作'
      ),
      {
        status: 403
      }
    );
  }

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
      new Error('找不到房間'),
      {
        status: 404
      }
    );
  }

  return room;
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
      JSON.stringify(payload)
    ]
  );

  await run(
    `
      UPDATE rooms
      SET updated_at = CURRENT_TIMESTAMP
      WHERE id = ?
    `,
    [roomId]
  );
}

/* =========================================================
   ROOM SNAPSHOT
========================================================= */

async function roomSnapshot(roomId) {
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
          u.role AS account_role,

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
        .map(e => ({
          ...e,

          payload:
            typeof e.payload === 'string'
              ? JSON.parse(e.payload)
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
  async (req, res) => {
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
   AUTH REGISTER
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

      /*
       * 只有與環境變數 DM_USERNAME
       * 完全相同的帳號才能在註冊時成為 DM。
       *
       * 玩家無法自行指定 role。
       */
      const role =
        DM_USERNAME &&
        username === DM_USERNAME
          ? 'dm'
          : 'player';

      const result =
        await run(
          `
            INSERT INTO users(
              username,
              password_hash,
              role
            )
            VALUES(
              ?,
              ?,
              ?
            )
            RETURNING id
          `,
          [
            username,
            hash,
            role
          ]
        );

      const user = {
        id: result.id,
        username,
        role
      };

      console.log(
        `[AUTH] Registered user: ${username} (${role})`
      );

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
   AUTH LOGIN
========================================================= */

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

      const user =
        await get(
          `
            SELECT
              id,
              username,
              password_hash,
              role
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
       * 如果環境變數指定了 DM_USERNAME，
       * 登入時再次確認。
       */
      if (
        DM_USERNAME &&
        user.username === DM_USERNAME &&
        user.role !== 'dm'
      ) {
        await run(
          `
            UPDATE users
            SET role = 'dm'
            WHERE id = ?
          `,
          [user.id]
        );

        user.role = 'dm';
      }

      res.json({
        token:
          signToken(user),

        user: {
          id: user.id,
          username: user.username,
          role: user.role
        }
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
  async (req, res) => {
    res.json({
      user: req.user
    });
  }
);

/* =========================================================
   DM ADMIN
   系統管理員 API
========================================================= */

/*
 * DM 身分確認
 */

app.get(
  '/api/admin/me',
  auth,
  requireDM,
  async (req, res) => {
    res.json({
      ok: true,
      role: 'dm',
      user: req.user
    });
  }
);

/*
 * DM 查看所有玩家帳號
 */

app.get(
  '/api/admin/players',
  auth,
  requireDM,
  async (req, res) => {
    try {
      const players =
        await all(
          `
            SELECT
              u.id,
              u.username,
              u.role,
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
              u.created_at ASC
          `
        );

      res.json({
        players
      });

    } catch (err) {
      console.error(
        'DM players error:',
        err
      );

      res.status(500).json({
        error:
          '讀取玩家列表失敗'
      });
    }
  }
);

/*
 * DM 查看所有角色卡
 */

app.get(
  '/api/admin/characters',
  auth,
  requireDM,
  async (req, res) => {
    try {
      const characters =
        await all(
          `
            SELECT
              c.*,
              u.username,
              u.role AS account_role

            FROM character_cards c

            JOIN users u
              ON u.id = c.user_id

            ORDER BY
              c.id ASC
          `
        );

      const parsed =
        characters.map(
          character =>
            calculateCharacter({
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
            })
        );

      res.json({
        characters:
          parsed
      });

    } catch (err) {
      console.error(
        'DM characters error:',
        err
      );

      res.status(500).json({
        error:
          '讀取全部角色卡失敗'
      });
    }
  }
);

/*
 * DM 查看所有房間
 *
 * 不需要是房間成員。
 */

app.get(
  '/api/admin/rooms',
  auth,
  requireDM,
  async (req, res) => {
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

              u.username AS creator_username,

              (
                SELECT COUNT(*)
                FROM room_members rm
                WHERE rm.room_id = r.id
              ) AS member_count

            FROM rooms r

            JOIN users u
              ON u.id = r.owner_id

            ORDER BY
              r.updated_at DESC
          `
        );

      res.json({
        rooms
      });

    } catch (err) {
      console.error(
        'DM rooms error:',
        err
      );

      res.status(500).json({
        error:
          '讀取所有房間失敗'
      });
    }
  }
);

/*
 * DM 直接查看任何房間
 *
 * 不需要加入房間。
 */

app.get(
  '/api/admin/rooms/:id',
  auth,
  requireDM,
  async (req, res) => {
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

    } catch (err) {
      console.error(
        'DM room view error:',
        err
      );

      res.status(500).json({
        error:
          '讀取房間失敗'
      });
    }
  }
);

/*
 * DM 查看指定玩家的角色卡
 */

app.get(
  '/api/admin/players/:userId/character',
  auth,
  requireDM,
  async (req, res) => {
    try {
      const character =
        await getCharacterByUser(
          req.params.userId
        );

      if (!character) {
        return res.status(404).json({
          error:
            '這個玩家沒有角色卡'
        });
      }

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

/* =========================================================
   CHARACTER CARD
========================================================= */

/*
 * 取得自己的角色卡
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
        error:
          '讀取角色卡失敗'
      });
    }
  }
);

/*
 * 建立角色卡
 *
 * 每個帳號只能建立一次。
 */

app.post(
  '/api/character',
  auth,
  async (req, res) => {
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
          ? req.body.sub_classes
          : [];

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
            subClasses.slice(0, 3)
          ),

          0,
          0,
          0,
          0,
          0,

          Number(
            req.body.luck || 10
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
        err.code === '23505'
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
 * PLAYER：
 *   只能修改自己的角色卡。
 *
 * DM：
 *   後續管理 API 可以修改所有角色。
 *
 * 這個 endpoint 目前仍然只修改自己的角色，
 * 保留原本功能。
 */

app.patch(
  '/api/character',
  auth,
  async (req, res) => {
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
            [
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
            ].includes(field)
          ) {
            value =
              Math.max(
                0,
                Number(value) || 0
              );
          } else {
            value =
              String(value)
                .slice(0, 100);
          }

          updates.push(
            `${field} = ?`
          );

          values.push(value);
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
          SET ${updates.join(', ')}
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
  async (req, res) => {
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
              req.body.level || 1
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
              req.body.cost || 0
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
        `
          UPDATE character_cards
          SET
            skills = ?::jsonb,
            updated_at = CURRENT_TIMESTAMP
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

      /*
       * 建立房間的人不會因此成為 DM。
       *
       * 只有真正的系統 DM
       * 才有 DM 權限。
       *
       * 建立者會成為普通 PLAYER。
       */

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

      /*
       * 建立者 = PLAYER
       *
       * 不再使用 gm。
       */

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
          result.id,
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
  async (req, res) => {
    try {
      /*
       * DM：
       *   可以看到整個系統所有房間。
       *
       * PLAYER：
       *   只能看到自己加入的房間。
       */

      if (isDM(req.user)) {
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
                r.saved,
                r.created_at,
                r.updated_at,

                u.username AS creator_username,

                (
                  SELECT COUNT(*)
                  FROM room_members rm
                  WHERE rm.room_id = r.id
                ) AS member_count

              FROM rooms r

              JOIN users u
                ON u.id = r.owner_id

              ORDER BY
                r.updated_at DESC
            `
          );

        return res.json({
          rooms
        });
      }

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
              ON rm.room_id = r.id

            WHERE rm.user_id = ?

            ORDER BY
              r.updated_at DESC
          `,
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
  async (req, res) => {
    try {
      /*
       * DM 可以查看所有房間。
       *
       * PLAYER 必須是房間成員。
       */

      if (!isDM(req.user)) {
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
   DM VIEW ALL CHARACTER CARDS IN ROOM
========================================================= */

app.get(
  '/api/rooms/:id/characters',
  auth,
  async (req, res) => {
    try {
      /*
       * 只有系統 DM。
       */
      assertDM(
        req.user
      );

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

      const characters =
        await all(
          `
            SELECT
              c.*,
              u.username,
              u.role AS account_role,
              crb.room_id

            FROM character_cards c

            JOIN character_room_bindings crb
              ON crb.character_id = c.id

            JOIN users u
              ON u.id = c.user_id

            WHERE crb.room_id = ?

            ORDER BY
              c.id ASC
          `,
          [req.params.id]
        );

      res.json({
        characters:
          characters.map(
            character =>
              calculateCharacter({
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
              })
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
  async (req, res) => {
    try {
      /*
       * 只有系統 DM。
       */
      await requireOwner(
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
            updated_at = CURRENT_TIMESTAMP
          WHERE id = ?
        `,
        [
          current?.user_id || null,
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
  async (req, res) => {
    try {
      await requireOwner(
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
          error:
            '沒有玩家'
        });
      }

      const idx =
        Math.max(
          0,
          members.findIndex(
            m =>
              String(m.user_id) ===
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
        idx + 1 >= members.length
          ? room.round + 1
          : room.round;

      await run(
        `
          UPDATE rooms
          SET
            round = ?,
            current_actor_id = ?,
            updated_at = CURRENT_TIMESTAMP
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
  async (req, res) => {
    try {
      await requireOwner(
        req.user.id,
        req.params.id
      );

      await run(
        `
          UPDATE rooms
          SET
            saved = TRUE,
            status = 'saved',
            updated_at = CURRENT_TIMESTAMP
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
  async (req, res) => {
    try {
      await requireOwner(
        req.user.id,
        req.params.id
      );

      await run(
        `
          UPDATE rooms
          SET
            saved = TRUE,
            status = 'closed',
            closed_at = CURRENT_TIMESTAMP,
            updated_at = CURRENT_TIMESTAMP
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
  async (req, res) => {
    try {
      await requireOwner(
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
            updated_at = CURRENT_TIMESTAMP
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
  async (req, res) => {
    try {
      await requireRoomMember(
        req.user.id,
        req.params.id
      );

      const target =
        Number(
          req.params.userId
        );

      /*
       * 系統 DM 可以修改任何人。
       *
       * PLAYER 只能修改自己。
       */

      const isDMUser =
        isDM(
          req.user
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
        !isDMUser
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
           * DM 可以進入任何房間。
           *
           * PLAYER 必須是成員。
           */
          if (
            !isDM(
              socket.user
            )
          ) {
            await requireRoomMember(
              socket.user.id,
              roomId
            );
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
          if (
            !isDM(
              socket.user
            )
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
          if (
            !isDM(
              socket.user
            )
          ) {
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
              (a, b) =>
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
  .then(() => {
    server.listen(
      PORT,
      HOST,
      () =>
        console.log(
          `TRPG Online listening on ${HOST}:${PORT}`
        )
    );
  })
  .catch(err => {
    console.error(
      'Database initialization failed:',
      err
    );

    process.exit(
      1
    );
  });

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
  .then(() => {
    server.listen(
      PORT,
      HOST,
      () =>
        console.log(
          `TRPG Online listening on ${HOST}:${PORT}`
        )
    );
  })
  .catch(err => {
    console.error(
      'Database initialization failed:',
      err
    );

    process.exit(1);
  });
