const path = require('path');
const http = require('http');
const express = require('express');
const { Server } = require('socket.io');
const { Pool } = require('pg');
const bcrypt = require('bcryptjs');
const jwt = require('jsonwebtoken');

const PORT =
  Number(
    process.env.PORT || 3000
  );

const HOST =
  process.env.HOST ||
  '0.0.0.0';

const JWT_SECRET =
  process.env.JWT_SECRET ||
  'dev-only-change-me';

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
  ).trim();

const pool =
  new Pool({
    connectionString:
      DATABASE_URL,

    max:
      Number(
        process.env.PGPOOL_MAX || 10
      ),

    idleTimeoutMillis:
      30000,

    connectionTimeoutMillis:
      5000,

    ssl:
      process.env.PGSSL ===
      'disable'
        ? false
        : process.env.PGSSL ===
          'require'
          ? {
              rejectUnauthorized:
                false
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
    result.rows[0] ||
    null
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
    result.rows[0] ||
    null;

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

      username VARCHAR(20)
        NOT NULL UNIQUE,

      password_hash TEXT
        NOT NULL,

      is_admin BOOLEAN
        NOT NULL DEFAULT FALSE,

      created_at TIMESTAMPTZ
        NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

    CREATE TABLE IF NOT EXISTS rooms (
      id BIGSERIAL PRIMARY KEY,

      code VARCHAR(6)
        NOT NULL UNIQUE,

      name VARCHAR(60)
        NOT NULL,

      owner_id BIGINT
        NOT NULL
        REFERENCES users(id)
        ON DELETE CASCADE,

      status VARCHAR(20)
        NOT NULL DEFAULT 'lobby',

      round INTEGER
        NOT NULL DEFAULT 1,

      current_actor_id BIGINT,

      saved BOOLEAN
        NOT NULL DEFAULT FALSE,

      created_at TIMESTAMPTZ
        NOT NULL DEFAULT CURRENT_TIMESTAMP,

      updated_at TIMESTAMPTZ
        NOT NULL DEFAULT CURRENT_TIMESTAMP,

      closed_at TIMESTAMPTZ
    );

    CREATE TABLE IF NOT EXISTS room_members (
      room_id BIGINT
        NOT NULL
        REFERENCES rooms(id)
        ON DELETE CASCADE,

      user_id BIGINT
        NOT NULL
        REFERENCES users(id)
        ON DELETE CASCADE,

      role VARCHAR(20)
        NOT NULL DEFAULT 'player',

      display_name VARCHAR(30),

      hp INTEGER
        NOT NULL DEFAULT 0,

      max_hp INTEGER
        NOT NULL DEFAULT 0,

      spirit INTEGER
        NOT NULL DEFAULT 0,

      max_spirit INTEGER
        NOT NULL DEFAULT 0,

      joined_at TIMESTAMPTZ
        NOT NULL DEFAULT CURRENT_TIMESTAMP,

      PRIMARY KEY (
        room_id,
        user_id
      )
    );

    CREATE TABLE IF NOT EXISTS events (
      id BIGSERIAL PRIMARY KEY,

      room_id BIGINT
        NOT NULL
        REFERENCES rooms(id)
        ON DELETE CASCADE,

      user_id BIGINT
        REFERENCES users(id)
        ON DELETE SET NULL,

      type VARCHAR(30)
        NOT NULL,

      payload JSONB
        NOT NULL DEFAULT '{}'::jsonb,

      created_at TIMESTAMPTZ
        NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

    CREATE TABLE IF NOT EXISTS character_cards (
      id BIGSERIAL PRIMARY KEY,

      user_id BIGINT
        NOT NULL UNIQUE
        REFERENCES users(id)
        ON DELETE CASCADE,

      name VARCHAR(50)
        NOT NULL DEFAULT '未命名角色',

      faction VARCHAR(20)
        NOT NULL DEFAULT '東國',

      tier VARCHAR(50)
        NOT NULL DEFAULT '凡人',

      level INTEGER
        NOT NULL DEFAULT 1,

      main_class VARCHAR(50)
        NOT NULL DEFAULT '未設定',

      sub_classes JSONB
        NOT NULL DEFAULT '[]'::jsonb,

      agility INTEGER
        NOT NULL DEFAULT 0,

      strength INTEGER
        NOT NULL DEFAULT 0,

      constitution INTEGER
        NOT NULL DEFAULT 0,

      spirit INTEGER
        NOT NULL DEFAULT 0,

      great_way INTEGER
        NOT NULL DEFAULT 0,

      great_way_name VARCHAR(100),

      luck INTEGER
        NOT NULL DEFAULT 10,

      attribute_points INTEGER
        NOT NULL DEFAULT 50,

      experience BIGINT
        NOT NULL DEFAULT 0,

      skill_points INTEGER
        NOT NULL DEFAULT 0,

      skills JSONB
        NOT NULL DEFAULT '[]'::jsonb,

      equipment JSONB
        NOT NULL DEFAULT '[]'::jsonb,

      weapons JSONB
        NOT NULL DEFAULT '[]'::jsonb,

      affinity_light INTEGER
        NOT NULL DEFAULT 0,

      affinity_dark INTEGER
        NOT NULL DEFAULT 0,

      affinity_metal INTEGER
        NOT NULL DEFAULT 0,

      affinity_wood INTEGER
        NOT NULL DEFAULT 0,

      affinity_water INTEGER
        NOT NULL DEFAULT 0,

      affinity_fire INTEGER
        NOT NULL DEFAULT 0,

      affinity_earth INTEGER
        NOT NULL DEFAULT 0,

      created_at TIMESTAMPTZ
        NOT NULL DEFAULT CURRENT_TIMESTAMP,

      updated_at TIMESTAMPTZ
        NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

    CREATE TABLE IF NOT EXISTS character_room_bindings (
      room_id BIGINT
        NOT NULL
        REFERENCES rooms(id)
        ON DELETE CASCADE,

      character_id BIGINT
        NOT NULL
        REFERENCES character_cards(id)
        ON DELETE CASCADE,

      user_id BIGINT
        NOT NULL
        REFERENCES users(id)
        ON DELETE CASCADE,

      created_at TIMESTAMPTZ
        NOT NULL DEFAULT CURRENT_TIMESTAMP,

      PRIMARY KEY (
        room_id,
        user_id
      ),

      UNIQUE (
        room_id,
        character_id
      )
    );

    CREATE INDEX IF NOT EXISTS
      idx_room_members_room
      ON room_members(room_id);

    CREATE INDEX IF NOT EXISTS
      idx_room_members_user
      ON room_members(user_id);

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

  await pool.query(`
    ALTER TABLE users
    ADD COLUMN IF NOT EXISTS is_admin
    BOOLEAN NOT NULL DEFAULT FALSE;

    ALTER TABLE rooms
    ADD COLUMN IF NOT EXISTS saved
    BOOLEAN NOT NULL DEFAULT FALSE;

    ALTER TABLE rooms
    ADD COLUMN IF NOT EXISTS updated_at
    TIMESTAMPTZ
    NOT NULL
    DEFAULT CURRENT_TIMESTAMP;

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
  /*
   * DM 帳號完全由 Render 的環境變數控制。
   *
   * ADMIN_USERNAME
   * ADMIN_PASSWORD
   *
   * 啟動時：
   *
   * 1. 帳號不存在 -> 建立
   * 2. 帳號存在 -> 更新密碼
   * 3. 強制 is_admin = TRUE
   *
   * 一般註冊不能取得 DM 權限。
   */

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
        SELECT
          id,
          username,
          is_admin
        FROM users
        WHERE username = ?
      `,
      [
        ADMIN_USERNAME
      ]
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

      VALUES (
        ?,
        ?,
        TRUE
      )

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
      id:
        user.id,

      username:
        user.username,

      is_admin:
        user.is_admin === true
    },

    JWT_SECRET,

    {
      expiresIn:
        '7d'
    }
  );
}

function auth(
  req,
  res,
  next
) {
  const raw =
    req.headers.authorization ||
    '';

  const token =
    raw.startsWith(
      'Bearer '
    )
      ? raw.slice(7)
      : null;

  if (!token) {
    return res.status(401).json({
      error:
        '需要登入'
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
      error:
        '登入已失效'
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
    [
      userId
    ]
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
    user.is_admin !== true
  ) {
    throw Object.assign(
      new Error(
        '只有全站 DM 可以執行此操作'
      ),
      {
        status:
          403
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
      error.status ||
      403
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
        socket.handshake.auth?.token ||
          '',
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
        (
          constitution +
          spirit
        ) / 2
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

function parseJsonField(
  value,
  fallback
) {
  if (
    Array.isArray(value) ||
    typeof value ===
      'object'
  ) {
    return value;
  }

  if (
    typeof value ===
    'string'
  ) {
    try {
      return JSON.parse(
        value
      );
    } catch {
      return fallback;
    }
  }

  return fallback;
}

function parseCharacter(
  character
) {
  if (!character) {
    return null;
  }

  return calculateCharacter({
    ...character,

    sub_classes:
      parseJsonField(
        character.sub_classes,
        []
      ),

    skills:
      parseJsonField(
        character.skills,
        []
      ),

    equipment:
      parseJsonField(
        character.equipment,
        []
      ),

    weapons:
      parseJsonField(
        character.weapons,
        []
      )
  });
}

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
      [
        userId
      ]
    );

  return parseCharacter(
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
        status:
          404
      }
    );
  }

  return character;
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
        status:
          403
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
        status:
          403
      }
    );
  }

  return room;
}

/*
 * 重要：
 *
 * 房間 DM 不再由建立房間的人決定。
 *
 * 只有全站 is_admin = true
 * 的帳號才能執行 DM 管理功能。
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
      [
        roomId
      ]
    );

  if (!room) {
    throw Object.assign(
      new Error(
        '找不到房間'
      ),
      {
        status:
          404
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
      [
        code
      ]
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

      SET
        updated_at =
          CURRENT_TIMESTAMP

      WHERE id = ?
    `,
    [
      roomId
    ]
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
      [
        roomId
      ]
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
          ON u.id =
            rm.user_id

        LEFT JOIN character_room_bindings crb
          ON crb.room_id =
            rm.room_id

          AND crb.user_id =
            rm.user_id

        WHERE rm.room_id = ?

        ORDER BY
          rm.joined_at ASC
      `,
      [
        roomId
      ]
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
          ON u.id =
            e.user_id

        WHERE e.room_id = ?

        ORDER BY
          e.id DESC

        LIMIT 100
      `,
      [
        roomId
      ]
    );

  return {
    room,

    members,

    events:
      events
        .reverse()
        .map(
          function (
            event
          ) {
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
    limit:
      '2mb'
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

      const admin =
        await get(
          `
            SELECT
              username,
              is_admin

            FROM users

            WHERE username = ?
          `,
          [
            ADMIN_USERNAME
          ]
        );

      res.json({
        ok:
          true,

        service:
          'trpg-online',

        database:
          'ok',

        dm_configured:
          Boolean(
            ADMIN_USERNAME &&
            ADMIN_PASSWORD
          ),

        dm_account:
          Boolean(
            admin &&
            admin.is_admin ===
              true
          ),

        time:
          new Date()
            .toISOString()
      });
    } catch {
      res.status(
        503
      ).json({
        ok:
          false,

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
          req.body.username ||
            ''
        ).trim();

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
        return res.status(
          400
        ).json({
          error:
            '帳號需為2-20字，可用中文、英文、數字、底線或連字號'
        });
      }

      if (
        password.length < 6
      ) {
        return res.status(
          400
        ).json({
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
              password_hash,
              is_admin
            )

            VALUES (
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

      await run(
        `
          INSERT INTO character_cards (
            user_id
          )

          VALUES (?)

          ON CONFLICT (
            user_id
          )

          DO NOTHING
        `,
        [
          result.id
        ]
      );

      res.json({
        token:
          signToken(
            user
          ),

        user
      });
    } catch (
      error
    ) {
      res.status(
        error.code ===
          '23505'
          ? 409
          : 500
      ).json({
        error:
          error.code ===
          '23505'
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
          req.body.username ||
            ''
        ).trim();

      /*
       * 注意：
       *
       * 密碼不能 trim。
       *
       * 如果使用者設定的密碼
       * 包含空白，空白也是密碼的一部分。
       */

      const password =
        String(
          req.body.password ||
            ''
        );

      if (
        !username ||
        !password
      ) {
        return res.status(
          400
        ).json({
          error:
            '請輸入帳號與密碼'
        });
      }

      const user =
        await get(
          `
            SELECT
              id,
              username,
              password_hash,
              is_admin,
              created_at

            FROM users

            WHERE username = ?
          `,
          [
            username
          ]
        );

      if (!user) {
        return res.status(
          401
        ).json({
          error:
            '帳號或密碼錯誤'
        });
      }

      const passwordOk =
        await bcrypt.compare(
          password,
          user.password_hash
        );

      if (!passwordOk) {
        return res.status(
          401
        ).json({
          error:
            '帳號或密碼錯誤'
        });
      }

      /*
       * 如果是環境變數指定的 DM，
       * 登入時再次確認資料庫權限。
       *
       * 正常情況 ensureAdmin()
       * 已經在啟動時設定好。
       */

      const isConfiguredAdmin =
        ADMIN_USERNAME &&
        user.username ===
          ADMIN_USERNAME;

      if (
        isConfiguredAdmin &&
        user.is_admin !== true
      ) {
        await run(
          `
            UPDATE users

            SET
              is_admin = TRUE

            WHERE id = ?
          `,
          [
            user.id
          ]
        );

        user.is_admin =
          true;
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

          ON CONFLICT (
            user_id
          )

          DO NOTHING
        `,
        [
          user.id
        ]
      );

      const safeUser = {
        id:
          user.id,

        username:
          user.username,

        is_admin:
          user.is_admin ===
          true
      };

      const token =
        signToken(
          safeUser
        );

      res.json({
        token,

        user:
          safeUser
      });
    } catch (
      error
    ) {
      console.error(
        'LOGIN ERROR:',
        error
      );

      res.status(
        500
      ).json({
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
        return res.status(
          401
        ).json({
          error:
            '帳號不存在'
        });
      }

      res.json({
        ok:
          true,

        username:
          user.username,

        is_admin:
          user.is_admin ===
          true
      });
    } catch (
      error
    ) {
      console.error(
        'ADMIN STATUS ERROR:',
        error
      );

      res.status(
        500
      ).json({
        error:
          '讀取 DM 狀態失敗'
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
        return res.status(
          401
        ).json({
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
            user.is_admin ===
            true
        }
      });
    } catch (
      error
    ) {
      console.error(
        'ME ERROR:',
        error
      );

      res.status(
        500
      ).json({
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
      res.status(
        500
      ).json({
        error:
          '讀取角色卡失敗'
      });
    }
  }
);
