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

      class_resources JSONB NOT NULL DEFAULT '{}'::jsonb,

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

const ELEMENTS = [
  '暗',
  '光',
  '金',
  '木',
  '水',
  '火',
  '土'
];

const ELEMENT_COUNTERS = {
  '暗': '光',
  '光': '暗',
  '金': '木',
  '木': '水',
  '水': '火',
  '火': '土',
  '土': '金'
};

function getElementDamageMultiplier(
  attackerElement,
  targetElement
) {
  if (
    attackerElement &&
    targetElement &&
    ELEMENT_COUNTERS[attackerElement] === targetElement
  ) {
    return 1.5;
  }

  return 1;
}

/* =========================================================
   CHARACTER CALCULATIONS
========================================================= */

function calculateCharacter(
  character
) {
  const faction = character.faction === '西國' ? '西國' : '東國';

  const tier = Math.max(
    1,
    Math.min(10, Number(character.tier) || 1)
  );

  const caps = {
    1: 50, 2: 100, 3: 180, 4: 320, 5: 600,
    6: 1100, 7: 2000, 8: 3600, 9: 6500, 10: 10000
  };

  const equipment = [
    ...(Array.isArray(character.equipment) ? character.equipment : []),
    ...(Array.isArray(character.weapons) ? character.weapons : [])
  ];

  const num = value => {
    const n = Number(value);
    return Number.isFinite(n) ? n : 0;
  };

  const flat = {
    agility: 0,
    strength: 0,
    constitution: 0,
    spirit: 0,
    great_way: 0,
    faith: 0,
    luck: 0
  };

  const pct = {
    agility: 0,
    strength: 0,
    constitution: 0,
    spirit: 0,
    great_way: 0,
    faith: 0,
    luck: 0
  };

  let allPct = 0;

  for (const item of equipment) {
    if (!item || typeof item !== 'object') continue;

    allPct += num(item.all_attributes_percent);
    allPct += num(item.all_attribute_percent);

    for (const field of Object.keys(flat)) {
      flat[field] += num(item[field]);
      flat[field] += num(item['flat_' + field]);
      pct[field] += num(item[field + '_percent']);
      pct[field] += num(item[field + '_pct']);
    }
  }

  const finalBase = (field, value) =>
    Math.floor(
      (num(value) + flat[field]) *
      (1 + (allPct + pct[field]) / 100)
    );

  const agility = finalBase('agility', character.agility);
  const strength = finalBase('strength', character.strength);
  const constitution = finalBase('constitution', character.constitution);
  const spirit = finalBase('spirit', character.spirit);

  const fifthField = faction === '西國' ? 'faith' : 'great_way';
  const currentFifthField =
    fifthField === 'faith'
      ? 'current_faith'
      : 'current_great_way';

  const fifth = finalBase(fifthField, character[fifthField]);

  const rawCurrentFifth =
    character[currentFifthField];

  const storedCurrentFifth =
    rawCurrentFifth === null ||
    rawCurrentFifth === undefined
      ? Number(character[fifthField]) || 0
      : Math.max(
          0,
          Number(rawCurrentFifth) || 0
        );

  const currentFifth =
    Math.max(
      0,
      Math.min(
        fifth,
        storedCurrentFifth
      )
    );

  const luck = Math.floor(
    (num(character.luck) + flat.luck) *
    (1 + pct.luck / 100)
  );

  const endurance = Math.floor((constitution + spirit) / 2);
  const sanity = Math.floor(spirit * 1.2);
  const will = Math.floor(spirit * 0.8);
  const life = Math.max(0, constitution * 2);

  const roundCount =
    1 + Math.floor(agility / 50);

  const combatHatred =
    Math.max(
      0,
      Number(character.combat_hatred) || 100
    );

return {
    ...character,

    attribute_cap: caps[tier],
    attribute_name:
      fifthField === 'faith'
        ? '信仰'
        : '大道',
    attribute_label:
      fifthField === 'faith'
        ? '信仰'
        : '大道',
    fifth_attribute_field: fifthField,
    fifth_resource_field: currentFifthField,
    fifth_resource_name:
      fifthField === 'faith'
        ? '信仰'
        : '大道',
    fifth_current: currentFifth,
    fifth_max: fifth,
    round_count: roundCount,
    combat_hatred: combatHatred,
    element_affinity: {
      暗: num(character.affinity_dark),
      光: num(character.affinity_light),
      金: num(character.affinity_metal),
      木: num(character.affinity_wood),
      水: num(character.affinity_water),
      火: num(character.affinity_fire),
      土: num(character.affinity_earth)
    },

    final_attributes: {
      agility,
      strength,
      constitution,
      spirit,
      [fifthField]: fifth,
      luck
    },

    final_agility: agility,
    final_strength: strength,
    final_constitution: constitution,
    final_spirit: spirit,

    final_great_way:
      fifthField === 'great_way' ? fifth : num(character.great_way),

    final_faith:
      fifthField === 'faith' ? fifth : num(character.faith),

    current_great_way:
      fifthField === 'great_way'
        ? currentFifth
        : Math.max(
            0,
            Number(
              character.current_great_way === null ||
              character.current_great_way === undefined
                ? character.great_way
                : character.current_great_way
            ) || 0
          ),

    current_faith:
      fifthField === 'faith'
        ? currentFifth
        : Math.max(
            0,
            Number(
              character.current_faith === null ||
              character.current_faith === undefined
                ? character.faith
                : character.current_faith
            ) || 0
          ),

    final_luck: luck,

    endurance,
    sanity,
    will,
    life,
    max_hp: life,

    weird_resistance: faction === '西國' ? '高' : '低',
    weird_hatred: faction === '西國' ? 100 : 150,

    trial_names: {
      agility: '敏捷試煉',
      strength: '力量試煉',
      constitution: '體質試煉',
      spirit: '精神試煉',
      [fifthField]:
        fifthField === 'faith' ? '信仰試煉' : '大道試煉'
    }
  };
}

function parseJsonField(
  value,
  fallback
) {
  if (
    Array.isArray(value) ||
    typeof value === 'object'
  ) {
    return value;
  }

  if (
    typeof value === 'string'
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

const IDENTITY_RANKS = [
  'G','F','E','D','C','B','A','S','SS','SSS'
];

function normalizeIdentityEntries(value) {
  const list = parseJsonField(value, []);

  if (!Array.isArray(list)) {
    return [];
  }

  return list
    .map(function (item) {
      if (typeof item === 'string') {
        const name = item.trim();
        return name ? { name, rank: 'G' } : null;
      }

      if (item && typeof item === 'object') {
        const name = String(item.name || item.title || '').trim().slice(0, 100);
        let rank = String(item.rank || 'G').toUpperCase();

        if (!IDENTITY_RANKS.includes(rank)) {
          rank = 'G';
        }

        return name
          ? {
              ...item,
              name,
              rank
            }
          : null;
      }

      return null;
    })
    .filter(Boolean)
    .slice(0, 10);
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
      ),

    faiths:
      normalizeIdentityEntries(
        character.faiths
      ),

    great_ways:
      normalizeIdentityEntries(
        character.great_ways
      ),

    class_resources:
      parseJsonField(
        character.class_resources,
        {}
      )
  });
}

function clampNumber(
  value,
  min,
  max
) {
  const number =
    Number(value);

  const safe =
    Number.isFinite(number)
      ? number
      : min;

  return Math.min(
    max,
    Math.max(
      min,
      safe
    )
  );
}

function normalizeProfessionResourceDefinitions(
  value
) {
  const source =
    Array.isArray(value)
      ? value
      : [];

  const used =
    new Set();

  return source
    .slice(0, 12)
    .map(
      function (raw, index) {
        const item =
          raw &&
          typeof raw === 'object' &&
          !Array.isArray(raw)
            ? raw
            : {};

        let id =
          String(
            item.id ||
            'resource_' +
              (index + 1)
          )
            .trim()
            .replace(
              /[^a-zA-Z0-9_-]/g,
              '_'
            )
            .slice(0, 60);

        if (!id) {
          id =
            'resource_' +
            (index + 1);
        }

        let baseId = id;
        let suffix = 2;

        while (
          used.has(id)
        ) {
          id =
            baseId +
            '_' +
            suffix;
          suffix += 1;
        }

        used.add(id);

        const min =
          Math.floor(
            Number(item.min) || 0
          );

        const max =
          Math.max(
            min,
            Math.floor(
              Number(
                item.max === undefined
                  ? 100
                  : item.max
              ) || 0
            )
          );

        const initial =
          Math.floor(
            clampNumber(
              item.initial === undefined
                ? min
                : item.initial,
              min,
              max
            )
          );

        const resetValue =
          Math.floor(
            clampNumber(
              item.reset_value === undefined
                ? initial
                : item.reset_value,
              min,
              max
            )
          );

        return {
          id,
          name:
            String(
              item.name ||
              '未命名資源'
            )
              .trim()
              .slice(0, 60) ||
            '未命名資源',
          min,
          max,
          initial,
          show_bar:
            item.show_bar === undefined
              ? true
              : Boolean(
                  item.show_bar
                ),
          reset_on_battle:
            Boolean(
              item.reset_on_battle
            ),
          reset_value:
            resetValue,
          round_delta:
            Math.floor(
              Number(
                item.round_delta
              ) || 0
            ),
          player_editable:
            Boolean(
              item.player_editable
            ),
          dm_editable:
            item.dm_editable === undefined
              ? true
              : Boolean(
                  item.dm_editable
                )
        };
      }
    );
}

function professionResourceKey(
  professionId,
  resourceId
) {
  return (
    'profession:' +
    String(professionId) +
    ':' +
    String(resourceId)
  );
}

async function getCharacterProfessionResourceDefinitions(
  character
) {
  if (!character) {
    return [];
  }

  const names = [];

  const mainClass =
    String(
      character.main_class ||
      ''
    ).trim();

  if (
    mainClass &&
    mainClass !== '未設定'
  ) {
    names.push(
      mainClass
    );
  }

  const subClasses =
    parseJsonField(
      character.sub_classes,
      []
    );

  if (
    Array.isArray(subClasses)
  ) {
    for (
      const name of subClasses
    ) {
      const safe =
        String(
          name ||
          ''
        ).trim();

      if (safe) {
        names.push(safe);
      }
    }
  }

  const uniqueNames =
    Array.from(
      new Set(names)
    );

  if (!uniqueNames.length) {
    return [];
  }

  const templates =
    await all(
      `
        SELECT
          id,
          name,
          config
        FROM profession_templates
        WHERE name = ANY(?::text[])
      `,
      [uniqueNames]
    );

  const byName =
    new Map(
      templates.map(
        function (template) {
          return [
            template.name,
            template
          ];
        }
      )
    );

  const definitions = [];

  for (
    const professionName of uniqueNames
  ) {
    const template =
      byName.get(
        professionName
      );

    if (!template) {
      continue;
    }

    const config =
      parseJsonField(
        template.config,
        {}
      );

    const resources =
      normalizeProfessionResourceDefinitions(
        config.special_resources
      );

    for (
      const resource of resources
    ) {
      definitions.push({
        ...resource,
        key:
          professionResourceKey(
            template.id,
            resource.id
          ),
        profession_id:
          template.id,
        profession_name:
          template.name
      });
    }
  }

  return definitions;
}

async function syncCharacterClassResources(
  userId,
  characterRow
) {
  const character =
    characterRow ||
    await get(
      `
        SELECT *
        FROM character_cards
        WHERE user_id = ?
      `,
      [userId]
    );

  if (!character) {
    return {};
  }

  const currentMap =
    parseJsonField(
      character.class_resources,
      {}
    );

  const definitions =
    await getCharacterProfessionResourceDefinitions(
      character
    );

  const nextMap = {};

  for (
    const definition of definitions
  ) {
    const existing =
      currentMap &&
      typeof currentMap === 'object'
        ? currentMap[
            definition.key
          ]
        : null;

    const current =
      Math.floor(
        clampNumber(
          existing &&
          existing.current !== undefined
            ? existing.current
            : definition.initial,
          definition.min,
          definition.max
        )
      );

    nextMap[definition.key] = {
      ...definition,
      current
    };
  }

  if (
    JSON.stringify(
      currentMap || {}
    ) !==
    JSON.stringify(
      nextMap
    )
  ) {
    await run(
      `
        UPDATE character_cards
        SET
          class_resources =
            ?::jsonb,
          updated_at =
            CURRENT_TIMESTAMP
        WHERE user_id = ?
      `,
      [
        JSON.stringify(
          nextMap
        ),
        userId
      ]
    );
  }

  return nextMap;
}

async function syncAllCharacterClassResources() {
  const rows =
    await all(
      `
        SELECT *
        FROM character_cards
      `
    );

  for (
    const row of rows
  ) {
    await syncCharacterClassResources(
      row.user_id,
      row
    );
  }
}

function normalizeSkillResources(
  value
) {
  const source =
    value &&
    typeof value === 'object' &&
    !Array.isArray(value)
      ? {
          ...value
        }
      : {};

  const raw =
    source.class_resource;

  if (
    !raw ||
    typeof raw !== 'object' ||
    Array.isArray(raw) ||
    !String(
      raw.key ||
      ''
    ).trim()
  ) {
    delete source.class_resource;
    return source;
  }

  const operation =
    ['spend', 'gain', 'set']
      .includes(
        raw.operation
      )
      ? raw.operation
      : 'spend';

  source.class_resource = {
    key:
      String(
        raw.key
      ).slice(0, 140),
    name:
      String(
        raw.name ||
        ''
      ).slice(0, 60),
    operation,
    amount:
      Math.max(
        0,
        Math.floor(
          Number(
            raw.amount
          ) || 0
        )
      )
  };

  return source;
}

function calculateSkillClassResourceChange(
  character,
  skill
) {
  const resources =
    normalizeSkillResources(
      parseJsonField(
        skill &&
        skill.resources,
        {}
      )
    );

  const effect =
    resources.class_resource;

  if (!effect) {
    return {
      changed: false,
      next_resources:
        parseJsonField(
          character.class_resources,
          {}
        ),
      effect: null,
      before: null,
      after: null
    };
  }

  const map = {
    ...parseJsonField(
      character.class_resources,
      {}
    )
  };

  const resource =
    map[effect.key];

  if (!resource) {
    throw Object.assign(
      new Error(
        '角色目前沒有技能需要的職業資源「' +
        (
          effect.name ||
          '未知資源'
        ) +
        '」'
      ),
      {
        status: 400
      }
    );
  }

  const min =
    Number(
      resource.min
    ) || 0;

  const max =
    Math.max(
      min,
      Number(
        resource.max
      ) || 0
    );

  const before =
    Math.floor(
      clampNumber(
        resource.current,
        min,
        max
      )
    );

  const amount =
    Math.max(
      0,
      Math.floor(
        Number(
          effect.amount
        ) || 0
      )
    );

  let after = before;

  if (
    effect.operation ===
    'spend'
  ) {
    if (
      before - amount < min
    ) {
      throw Object.assign(
        new Error(
          (
            resource.name ||
            effect.name ||
            '職業資源'
          ) +
          '不足，需要 ' +
          amount +
          '，目前只有 ' +
          before
        ),
        {
          status: 400
        }
      );
    }

    after =
      before - amount;
  } else if (
    effect.operation ===
    'gain'
  ) {
    after =
      Math.min(
        max,
        before + amount
      );
  } else if (
    effect.operation ===
    'set'
  ) {
    after =
      Math.floor(
        clampNumber(
          amount,
          min,
          max
        )
      );
  }

  map[effect.key] = {
    ...resource,
    current:
      after
  };

  return {
    changed:
      after !== before,
    next_resources:
      map,
    effect:
      {
        ...effect,
        name:
          resource.name ||
          effect.name ||
          '職業資源'
      },
    before,
    after
  };
}

async function saveCharacterClassResources(
  userId,
  resourceMap
) {
  await run(
    `
      UPDATE character_cards
      SET
        class_resources =
          ?::jsonb,
        updated_at =
          CURRENT_TIMESTAMP
      WHERE user_id = ?
    `,
    [
      JSON.stringify(
        resourceMap || {}
      ),
      userId
    ]
  );
}

async function changeCharacterClassResource(
  userId,
  body,
  isAdmin
) {
  const character =
    await requireCharacter(
      userId
    );

  const key =
    String(
      body &&
      body.key ||
      ''
    );

  const map = {
    ...parseJsonField(
      character.class_resources,
      {}
    )
  };

  const resource =
    map[key];

  if (!resource) {
    throw Object.assign(
      new Error(
        '找不到職業特殊資源'
      ),
      {
        status: 404
      }
    );
  }

  if (
    isAdmin
      ? !resource.dm_editable
      : !resource.player_editable
  ) {
    throw Object.assign(
      new Error(
        isAdmin
          ? '這個資源不允許 DM 手動修改'
          : '這個資源不允許玩家手動修改'
      ),
      {
        status: 403
      }
    );
  }

  const min =
    Number(
      resource.min
    ) || 0;

  const max =
    Math.max(
      min,
      Number(
        resource.max
      ) || 0
    );

  const before =
    Number(
      resource.current
    ) || 0;

  let next = before;

  if (
    body &&
    body.value !== undefined
  ) {
    next =
      Number(
        body.value
      );
  } else {
    next =
      before +
      (
        Number(
          body &&
          body.delta
        ) || 0
      );
  }

  next =
    Math.floor(
      clampNumber(
        next,
        min,
        max
      )
    );

  map[key] = {
    ...resource,
    current:
      next
  };

  await saveCharacterClassResources(
    userId,
    map
  );

  return {
    character:
      await getCharacterByUser(
        userId
      ),
    resource:
      map[key],
    before,
    after:
      next
  };
}

async function resetCharacterClassResourcesForBattle(
  userId
) {
  const character =
    await getCharacterByUser(
      userId
    );

  if (!character) {
    return;
  }

  const map = {
    ...parseJsonField(
      character.class_resources,
      {}
    )
  };

  let changed = false;

  for (
    const [key, resource]
    of Object.entries(map)
  ) {
    if (
      !resource ||
      !resource.reset_on_battle
    ) {
      continue;
    }

    const min =
      Number(
        resource.min
      ) || 0;

    const max =
      Math.max(
        min,
        Number(
          resource.max
        ) || 0
      );

    const next =
      Math.floor(
        clampNumber(
          resource.reset_value,
          min,
          max
        )
      );

    if (
      next !==
      Number(
        resource.current
      )
    ) {
      map[key] = {
        ...resource,
        current:
          next
      };
      changed = true;
    }
  }

  if (changed) {
    await saveCharacterClassResources(
      userId,
      map
    );
  }
}

async function applyCharacterClassResourceRoundDelta(
  userId
) {
  const character =
    await getCharacterByUser(
      userId
    );

  if (!character) {
    return;
  }

  const map = {
    ...parseJsonField(
      character.class_resources,
      {}
    )
  };

  let changed = false;

  for (
    const [key, resource]
    of Object.entries(map)
  ) {
    const delta =
      Math.floor(
        Number(
          resource &&
          resource.round_delta
        ) || 0
      );

    if (!delta) {
      continue;
    }

    const min =
      Number(
        resource.min
      ) || 0;

    const max =
      Math.max(
        min,
        Number(
          resource.max
        ) || 0
      );

    const before =
      Number(
        resource.current
      ) || 0;

    const next =
      Math.floor(
        clampNumber(
          before + delta,
          min,
          max
        )
      );

    if (next !== before) {
      map[key] = {
        ...resource,
        current:
          next
      };
      changed = true;
    }
  }

  if (changed) {
    await saveCharacterClassResources(
      userId,
      map
    );
  }
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
      [userId]
    );

  if (!character) {
    return null;
  }

  const classResources =
    await syncCharacterClassResources(
      userId,
      character
    );

  return parseCharacter({
    ...character,
    class_resources:
      classResources
  });
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

          crb.character_id,
          cc.class_resources

        FROM room_members rm

        JOIN users u
          ON u.id = rm.user_id

        LEFT JOIN character_room_bindings crb
          ON crb.room_id = rm.room_id
          AND crb.user_id = rm.user_id

        LEFT JOIN character_cards cc
          ON cc.id = crb.character_id

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
          e.user_id,
          e.type,
          e.payload,
          e.created_at,
          u.username,
          c.name AS character_name

        FROM events e

        LEFT JOIN users u
          ON u.id = e.user_id

        LEFT JOIN character_cards c
          ON c.user_id = e.user_id

        WHERE e.room_id = ?

        ORDER BY e.id DESC

        LIMIT 100
      `,
      [roomId]
    );

  const summons =
    await all(
      `
        SELECT
          si.id,
          si.template_id,
          si.owner_user_id,
          si.room_id,
          si.source_skill_id,
          si.name,
          si.status,
          si.overrides,
          si.current_state,
          si.created_at,
          si.updated_at,

          u.username
            AS owner_username,

          st.description,
          st.growth_mode,
          st.control_mode,
          st.behavior_mode,
          st.self_ai_level,
          st.language_level,
          st.speech_enabled,
          st.loyalty

        FROM summon_instances si

        JOIN users u
          ON u.id =
            si.owner_user_id

        LEFT JOIN summon_templates st
          ON st.id =
            si.template_id

        WHERE si.room_id = ?
          AND si.status = 'active'

        ORDER BY si.id ASC
      `,
      [roomId]
    );

  return {
    room,

    members:
      members.map(
        function (member) {
          return {
            ...member,
            class_resources:
              parseJsonField(
                member.class_resources,
                {}
              )
          };
        }
      ),

    summons:
      summons.map(
        function (summon) {
          return {
            ...summon,
            overrides:
              parseJsonField(
                summon.overrides,
                {}
              ),
            current_state:
              parseJsonField(
                summon.current_state,
                {}
              )
          };
        }
      ),

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
  const currentCharacter =
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
    'faith',
    'current_great_way',
    'current_faith',
    'luck',
    'attribute_points',
    'experience',
    'skill_points',
    'combat_hatred',
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
    'great_way_name',
    'profession_rank',
    'innate_sub_class'
  ];

  const jsonFields = [
    'sub_classes',
    'skills',
    'equipment',
    'weapons',
    'faiths',
    'great_ways'
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

  for (const pair of [
    ['great_way', 'current_great_way'],
    ['faith', 'current_faith']
  ]) {
    const maxField =
      pair[0];

    const currentField =
      pair[1];

    if (
      body[maxField] !== undefined &&
      body[currentField] === undefined
    ) {
      const oldMax =
        Math.max(
          0,
          Number(
            currentCharacter[maxField]
          ) || 0
        );

      const oldCurrent =
        currentCharacter[currentField] === null ||
        currentCharacter[currentField] === undefined
          ? oldMax
          : Math.max(
              0,
              Number(
                currentCharacter[currentField]
              ) || 0
            );

      const newMax =
        Math.max(
          0,
          Number(
            body[maxField]
          ) || 0
        );

      updates.push(
        currentField +
        ' = ?'
      );

      values.push(
        oldCurrent >= oldMax
          ? newMax
          : Math.min(
              oldCurrent,
              newMax
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

      if (
        field === 'faiths' ||
        field === 'great_ways'
      ) {
        value = normalizeIdentityEntries(value);
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

  if (
    body.element_affinity &&
    typeof body.element_affinity === 'object' &&
    !Array.isArray(body.element_affinity)
  ) {
    const map = {
      '暗': 'affinity_dark',
      '光': 'affinity_light',
      '金': 'affinity_metal',
      '木': 'affinity_wood',
      '水': 'affinity_water',
      '火': 'affinity_fire',
      '土': 'affinity_earth'
    };

    for (const [element, field] of Object.entries(map)) {
      if (body.element_affinity[element] !== undefined) {
        updates.push(field + ' = ?');
        values.push(Math.max(0, Number(body.element_affinity[element]) || 0));
      }
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

app.patch(
  '/api/character',
  auth,
  async function (
    req,
    res
  ) {
    try {
      const user = await getUser(req.user.id);

      const character =
        user && user.is_admin
          ? await updateCharacter(req.user.id, req.body)
          : await updatePlayerCharacter(req.user.id, req.body);

      res.json({
        character
      });
    } catch (error) {
      console.error(
        'PATCH /api/character failed:',
        error
      );

      res.status(error.status || 500).json({
        error:
          error.message ||
          '更新角色卡失敗'
      });
    }
  }
);


/* =========================================================
   FIFTH RESOURCE (大道 / 信仰)
========================================================= */

async function updateFifthResource(
  userId,
  body
) {
  const character =
    await requireCharacter(
      userId
    );

  const isWest =
    character.faction === '西國';

  const maxField =
    isWest
      ? 'faith'
      : 'great_way';

  const currentField =
    isWest
      ? 'current_faith'
      : 'current_great_way';

  const maxValue =
    Math.max(
      0,
      Number(
        character[maxField]
      ) || 0
    );

  const currentValue =
    character[currentField] === null ||
    character[currentField] === undefined
      ? maxValue
      : Math.max(
          0,
          Math.min(
            maxValue,
            Number(
              character[currentField]
            ) || 0
          )
        );

  let nextValue;

  if (
    body &&
    body.value !== undefined
  ) {
    nextValue =
      Number(
        body.value
      );
  } else {
    nextValue =
      currentValue +
      Number(
        body &&
        body.delta !== undefined
          ? body.delta
          : 0
      );
  }

  if (
    !Number.isFinite(
      nextValue
    )
  ) {
    throw Object.assign(
      new Error(
        '大道／信仰數值不正確'
      ),
      {
        status: 400
      }
    );
  }

  nextValue =
    Math.max(
      0,
      Math.min(
        maxValue,
        Math.floor(
          nextValue
        )
      )
    );

  await run(
    `
      UPDATE character_cards
      SET
        ${currentField} = ?,
        updated_at =
          CURRENT_TIMESTAMP
      WHERE user_id = ?
    `,
    [
      nextValue,
      userId
    ]
  );

  return getCharacterByUser(
    userId
  );
}

app.patch(
  '/api/character/fifth-resource',
  auth,
  async function (
    req,
    res
  ) {
    try {
      const character =
        await updateFifthResource(
          req.user.id,
          req.body || {}
        );

      res.json({
        ok: true,
        character
      });
    } catch (error) {
      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
          '更新大道／信仰失敗'
      });
    }
  }
);

app.patch(
  '/api/admin/players/:id/fifth-resource',
  auth,
  adminAuth,
  async function (
    req,
    res
  ) {
    try {
      const character =
        await updateFifthResource(
          Number(
            req.params.id
          ),
          req.body || {}
        );

      res.json({
        ok: true,
        character
      });
    } catch (error) {
      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
          'DM 更新大道／信仰失敗'
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

        check:
          String(
            req.body.check ||
            ''
          ).slice(0, 50),

        damage_formula:
          String(
            req.body.damage_formula ||
            ''
          ).slice(0, 200),

        extra_tag:
          String(
            req.body.extra_tag ||
            ''
          ).slice(0, 60),

        resources:
          req.body.resources || {},

        summon_tag:
          Boolean(
            req.body.summon_tag
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



function normalizeSummonTemplateIds(
  value
) {
  const raw =
    Array.isArray(value)
      ? value
      : [];

  const unique = [];

  for (const item of raw) {
    const id =
      Number(item);

    if (
      Number.isInteger(id) &&
      id > 0 &&
      !unique.includes(id)
    ) {
      unique.push(id);
    }
  }

  return unique.slice(
    0,
    50
  );
}

async function validateSummonTemplateIds(
  ids
) {
  for (const id of ids) {
    const exists =
      await get(
        `
          SELECT id
          FROM summon_templates
          WHERE id = ?
        `,
        [id]
      );

    if (!exists) {
      throw Object.assign(
        new Error(
          '選到不存在的召喚物模板'
        ),
        {
          status: 400
        }
      );
    }
  }
}

function parsedSkillTemplate(
  row
) {
  if (!row) {
    return null;
  }

  return {
    ...row,
    resources:
      parseJsonField(
        row.resources,
        {}
      ),
    data:
      parseJsonField(
        row.data,
        {}
      )
  };
}

async function syncSkillTemplateToCharacters(
  template
) {
  const rows =
    await all(
      `
        SELECT
          id,
          skills
        FROM character_cards
      `
    );

  let changedCount = 0;

  for (const row of rows) {
    const skills =
      parseJsonField(
        row.skills,
        []
      );

    if (
      !Array.isArray(skills) ||
      !skills.length
    ) {
      continue;
    }

    let changed = false;

    const nextSkills =
      skills.map(
        function (skill) {
          if (
            String(
              skill &&
              skill.template_id
            ) !==
            String(
              template.id
            )
          ) {
            return skill;
          }

          changed = true;

          return {
            ...skill,
            template_id:
              template.id,
            name:
              template.name,
            category:
              template.category,
            type:
              template.skill_type,
            description:
              template.description ||
              '',
            check:
              template.check_name ||
              '',
            damage_formula:
              template.damage_formula ||
              '',
            extra_tag:
              template.extra_tag ||
              '',
            summon_tag:
              Boolean(
                template.summon_tag
              ),
            resources:
              parseJsonField(
                template.resources,
                {}
              ),
            data:
              parseJsonField(
                template.data,
                {}
              )
          };
        }
      );

    if (changed) {
      await run(
        `
          UPDATE character_cards
          SET
            skills = ?::jsonb,
            updated_at =
              CURRENT_TIMESTAMP
          WHERE id = ?
        `,
        [
          JSON.stringify(
            nextSkills
          ),
          row.id
        ]
      );

      changedCount += 1;
    }
  }

  return changedCount;
}

/* =========================================================
   PLAYER-FACING PROFESSION / SKILL LIBRARIES
========================================================= */
app.get('/api/professions', auth, async function(req,res){
  try{
    const professions=await all(`SELECT id,name,category,rank,description,config FROM profession_templates ORDER BY name ASC`);
    res.json({professions:professions.map(x=>({...x,config:parseJsonField(x.config,{})}))});
  }catch(error){console.error('PROFESSION LIST ERROR:',error);res.status(500).json({error:'讀取職業資料庫失敗'});}
});

app.get('/api/skill-templates', auth, async function(req,res){
  try{
    const skills=await all(`SELECT * FROM skill_templates ORDER BY category ASC,name ASC,id ASC`);
    res.json({skills:skills.map(x=>({...x,resources:parseJsonField(x.resources,{}),data:parseJsonField(x.data,{})}))});
  }catch(error){console.error('SKILL TEMPLATE LIST ERROR:',error);res.status(500).json({error:'讀取技能資料庫失敗'});}
});

app.get('/api/summon-templates', auth, async function(req,res){
  try{
    const rows=await all(`SELECT * FROM summon_templates ORDER BY id ASC`);
    res.json({summon_templates:rows.map(x=>({...x,attributes:parseJsonField(x.attributes,{}),element_affinity:parseJsonField(x.element_affinity,{}),traits:parseJsonField(x.traits,[]),skills:parseJsonField(x.skills,[]),config:parseJsonField(x.config,{})}))});
  }catch(error){console.error('SUMMON TEMPLATE LIST ERROR:',error);res.status(500).json({error:'讀取召喚物資料庫失敗'});}
});

app.post('/api/character/skills/from-template', auth, async function(req,res){
  try{
    const templateId=Number(req.body.template_id);
    if(!templateId) return res.status(400).json({error:'請選擇技能'});
    const t=await get(`SELECT * FROM skill_templates WHERE id = ?`,[templateId]);
    if(!t) return res.status(404).json({error:'找不到技能資料'});
    const c=await requireCharacter(req.user.id);
    const skills=Array.isArray(c.skills)?[...c.skills]:[];
    if(skills.some(skill=>String(skill.template_id||'')===String(t.id))) return res.status(409).json({error:'角色已經擁有這個技能'});
    skills.push({id:'template_'+t.id+'_'+Date.now(),template_id:t.id,name:t.name,category:t.category,type:t.skill_type,level:Math.max(1,Number(t.level)||1),description:t.description||'',check:t.check_name||'',damage_formula:t.damage_formula||'',extra_tag:t.extra_tag||'',summon_tag:Boolean(t.summon_tag),resources:parseJsonField(t.resources,{}),data:parseJsonField(t.data,{})});
    const character=await updateCharacter(req.user.id,{skills});
    res.json({ok:true,character});
  }catch(error){console.error('ADD TEMPLATE SKILL ERROR:',error);res.status(error.status||500).json({error:error.message||'新增技能失敗'});}
});

app.delete('/api/character/skills/:skillId', auth, async function(req,res){
  try{
    const c=await requireCharacter(req.user.id);
    const skills=Array.isArray(c.skills)?c.skills:[];
    const next=skills.filter(skill=>String(skill.id)!==String(req.params.skillId));
    if(next.length===skills.length) return res.status(404).json({error:'找不到角色技能'});
    const character=await updateCharacter(req.user.id,{skills:next});
    res.json({ok:true,character});
  }catch(error){console.error('REMOVE CHARACTER SKILL ERROR:',error);res.status(error.status||500).json({error:error.message||'移除技能失敗'});}
});

app.patch(
  '/api/character/class-resource',
  auth,
  async function (req, res) {
    try {
      const result =
        await changeCharacterClassResource(
          req.user.id,
          req.body || {},
          false
        );

      const roomId =
        Number(
          req.body &&
          req.body.room_id
        ) || null;

      let snapshot = null;

      if (roomId) {
        await requireRoomMember(
          req.user.id,
          roomId
        );

        snapshot =
          await roomSnapshot(
            roomId
          );

        io.to(
          'room:' +
          roomId
        ).emit(
          'room:snapshot',
          snapshot
        );
      }

      res.json({
        ok: true,
        ...result,
        room:
          snapshot
      });
    } catch (error) {
      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
          '修改職業資源失敗'
      });
    }
  }
);

app.post(
  '/api/character/skills/:skillId/use',
  auth,
  async function (req, res) {
    try {
      const character =
        await requireCharacter(
          req.user.id
        );

      const skill =
        (
          Array.isArray(
            character.skills
          )
            ? character.skills
            : []
        ).find(
          function (item) {
            return (
              String(
                item &&
                item.id
              ) ===
              String(
                req.params.skillId
              )
            );
          }
        );

      if (!skill) {
        return res.status(404).json({
          error:
            '找不到角色技能'
        });
      }

      if (
        skill.summon_tag
      ) {
        return res.status(400).json({
          error:
            '召喚技能請使用「召喚」按鈕'
        });
      }

      const change =
        calculateSkillClassResourceChange(
          character,
          skill
        );

      if (
        change.changed
      ) {
        await saveCharacterClassResources(
          req.user.id,
          change.next_resources
        );
      }

      const roomId =
        Number(
          req.body &&
          req.body.room_id
        ) || null;

      if (roomId) {
        const currentUser =
          await getUser(
            req.user.id
          );

        if (
          !currentUser ||
          !currentUser.is_admin
        ) {
          await requireRoomMember(
            req.user.id,
            roomId
          );
        }

        const resourceText =
          change.effect
            ? (
                '（' +
                change.effect.name +
                ' ' +
                change.before +
                ' → ' +
                change.after +
                '）'
              )
            : '';

        await addEvent(
          roomId,
          req.user.id,
          'skill',
          {
            text:
              character.name +
              ' 使用技能「' +
              (
                skill.name ||
                '未命名技能'
              ) +
              '」' +
              resourceText,
            skill_id:
              skill.id,
            resource_change:
              change.effect
                ? {
                    name:
                      change.effect.name,
                    before:
                      change.before,
                    after:
                      change.after
                  }
                : null
          }
        );

        const snapshot =
          await roomSnapshot(
            roomId
          );

        io.to(
          'room:' +
          roomId
        ).emit(
          'room:snapshot',
          snapshot
        );
      }

      res.json({
        ok: true,
        character:
          await getCharacterByUser(
            req.user.id
          ),
        resource_change:
          change.effect
            ? {
                name:
                  change.effect.name,
                before:
                  change.before,
                after:
                  change.after
              }
            : null
      });
    } catch (error) {
      console.error(
        'USE CHARACTER SKILL ERROR:',
        error
      );

      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
          '使用技能失敗'
      });
    }
  }
);


/* =========================================================
   SUMMON INSTANCES
========================================================= */

function parseSummonInstance(
  row
) {
  if (!row) {
    return null;
  }

  return {
    ...row,
    overrides:
      parseJsonField(
        row.overrides,
        {}
      ),
    current_state:
      parseJsonField(
        row.current_state,
        {}
      )
  };
}

async function getMyActiveSummons(
  userId
) {
  const rows =
    await all(
      `
        SELECT
          si.*,

          st.description,
          st.growth_mode,
          st.control_mode,
          st.behavior_mode,
          st.self_ai_level,
          st.language_level,
          st.speech_enabled,
          st.loyalty,

          r.name AS room_name,
          r.code AS room_code

        FROM summon_instances si

        LEFT JOIN summon_templates st
          ON st.id =
            si.template_id

        LEFT JOIN rooms r
          ON r.id =
            si.room_id

        WHERE si.owner_user_id = ?
          AND si.status = 'active'

        ORDER BY si.id ASC
      `,
      [userId]
    );

  return rows.map(
    parseSummonInstance
  );
}

app.get(
  '/api/character/summons',
  auth,
  async function (
    req,
    res
  ) {
    try {
      res.json({
        summons:
          await getMyActiveSummons(
            req.user.id
          )
      });
    } catch (error) {
      console.error(
        'LIST SUMMON INSTANCES ERROR:',
        error
      );

      res.status(500).json({
        error:
          '讀取目前召喚物失敗'
      });
    }
  }
);

app.post(
  '/api/character/skills/:skillId/summon',
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
          ? character.skills
          : [];

      const skill =
        skills.find(
          function (item) {
            return (
              String(
                item &&
                item.id
              ) ===
              String(
                req.params.skillId
              )
            );
          }
        );

      if (!skill) {
        return res.status(404).json({
          error:
            '找不到角色技能'
        });
      }

      if (
        !skill.summon_tag
      ) {
        return res.status(400).json({
          error:
            '這不是召喚技能'
        });
      }

      const skillData =
        parseJsonField(
          skill.data,
          {}
        );

      const allowedIds =
        normalizeSummonTemplateIds(
          skillData.summon_template_ids
        );

      if (
        !allowedIds.length
      ) {
        return res.status(400).json({
          error:
            '這個召喚技能尚未設定可召喚的召喚物，請 DM 修改技能'
        });
      }

      const requestedId =
        Number(
          req.body &&
          req.body.template_id
        );

      const templateId =
        requestedId ||
        (
          allowedIds.length === 1
            ? allowedIds[0]
            : 0
        );

      if (
        !templateId
      ) {
        return res.status(400).json({
          error:
            '請選擇要召喚的召喚物'
        });
      }

      if (
        !allowedIds.includes(
          templateId
        )
      ) {
        return res.status(403).json({
          error:
            '這個技能不能召喚該召喚物'
        });
      }

      const template =
        await get(
          `
            SELECT *
            FROM summon_templates
            WHERE id = ?
          `,
          [templateId]
        );

      if (!template) {
        return res.status(404).json({
          error:
            '召喚物模板已不存在'
        });
      }

      let roomId =
        Number(
          req.body &&
          req.body.room_id
        ) || null;

      if (roomId) {
        const currentUser =
          await getUser(
            req.user.id
          );

        if (
          !currentUser ||
          !currentUser.is_admin
        ) {
          await requireRoomMember(
            req.user.id,
            roomId
          );
        } else {
          const roomExists =
            await get(
              `
                SELECT id
                FROM rooms
                WHERE id = ?
              `,
              [roomId]
            );

          if (!roomExists) {
            return res.status(404).json({
              error:
                '找不到房間'
            });
          }
        }
      }

      const skillResourceChange =
        calculateSkillClassResourceChange(
          character,
          skill
        );

      const currentState = {
        attributes:
          parseJsonField(
            template.attributes,
            {}
          ),
        element_affinity:
          parseJsonField(
            template.element_affinity,
            {}
          ),
        traits:
          parseJsonField(
            template.traits,
            []
          ),
        skills:
          parseJsonField(
            template.skills,
            []
          ),
        control_mode:
          template.control_mode ||
          'manual',
        behavior_mode:
          template.behavior_mode ||
          'balanced',
        self_ai_level:
          Math.max(
            0,
            Number(
              template.self_ai_level
            ) || 0
          ),
        language_level:
          Math.max(
            0,
            Number(
              template.language_level
            ) || 0
          ),
        speech_enabled:
          Boolean(
            template.speech_enabled
          ),
        loyalty:
          Math.max(
            0,
            Number(
              template.loyalty
            ) || 0
          ),
        source_skill_name:
          skill.name ||
          '召喚技能'
      };

      const row =
        await get(
          `
            INSERT INTO summon_instances (
              template_id,
              owner_user_id,
              room_id,
              source_skill_id,
              name,
              status,
              overrides,
              current_state
            )
            VALUES (
              ?,
              ?,
              ?,
              ?,
              ?,
              'active',
              '{}'::jsonb,
              ?::jsonb
            )
            RETURNING *
          `,
          [
            template.id,
            req.user.id,
            roomId,
            String(
              skill.id ||
              ''
            ).slice(0,100),
            template.name,
            JSON.stringify(
              currentState
            )
          ]
        );

      if (
        skillResourceChange.changed
      ) {
        await saveCharacterClassResources(
          req.user.id,
          skillResourceChange.next_resources
        );
      }

      if (roomId) {
        await addEvent(
          roomId,
          req.user.id,
          'summon',
          {
            text:
              character.name +
              ' 使用「' +
              skill.name +
              '」召喚了「' +
              template.name +
              '」' +
              (
                skillResourceChange.effect
                  ? '（' +
                    skillResourceChange.effect.name +
                    ' ' +
                    skillResourceChange.before +
                    ' → ' +
                    skillResourceChange.after +
                    '）'
                  : ''
              ),
            summon_id:
              row.id,
            template_id:
              template.id
          }
        );
      }

      const summons =
        await getMyActiveSummons(
          req.user.id
        );

      const snapshot =
        roomId
          ? await roomSnapshot(
              roomId
            )
          : null;

      if (snapshot) {
        io.to(
          'room:' +
          roomId
        ).emit(
          'room:snapshot',
          snapshot
        );
      }

      res.json({
        ok: true,
        summon:
          parseSummonInstance(
            row
          ),
        summons,
        room:
          snapshot,
        character:
          await getCharacterByUser(
            req.user.id
          ),
        resource_change:
          skillResourceChange.effect
            ? {
                name:
                  skillResourceChange.effect.name,
                before:
                  skillResourceChange.before,
                after:
                  skillResourceChange.after
              }
            : null
      });
    } catch (error) {
      console.error(
        'SUMMON SKILL ERROR:',
        error
      );

      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
          '召喚失敗'
      });
    }
  }
);

app.post(
  '/api/character/summons/:id/dismiss',
  auth,
  async function (
    req,
    res
  ) {
    try {
      const summon =
        await get(
          `
            SELECT *
            FROM summon_instances
            WHERE id = ?
              AND owner_user_id = ?
          `,
          [
            req.params.id,
            req.user.id
          ]
        );

      if (!summon) {
        return res.status(404).json({
          error:
            '找不到召喚物'
        });
      }

      await run(
        `
          UPDATE summon_instances
          SET
            status = 'dismissed',
            updated_at =
              CURRENT_TIMESTAMP
          WHERE id = ?
        `,
        [summon.id]
      );

      if (summon.room_id) {
        await addEvent(
          summon.room_id,
          req.user.id,
          'summon',
          {
            text:
              '召喚物「' +
              summon.name +
              '」已解除召喚',
            summon_id:
              summon.id
          }
        );
      }

      const summons =
        await getMyActiveSummons(
          req.user.id
        );

      const snapshot =
        summon.room_id
          ? await roomSnapshot(
              summon.room_id
            )
          : null;

      if (snapshot) {
        io.to(
          'room:' +
          summon.room_id
        ).emit(
          'room:snapshot',
          snapshot
        );
      }

      res.json({
        ok: true,
        summons,
        room:
          snapshot
      });
    } catch (error) {
      console.error(
        'DISMISS SUMMON ERROR:',
        error
      );

      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
          '解除召喚失敗'
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
   EXTRA SKILLS / SUMMON / RECIPES
========================================================= */

app.get(
  '/api/admin/extra-skills',
  auth,
  adminAuth,
  async function (req, res) {
    try {
      const rows = await all(
        `SELECT * FROM extra_skills ORDER BY id ASC`
      );
      res.json({ extra_skills: rows });
    } catch (error) {
      console.error(error);
      res.status(500).json({ error: '讀取額外技能失敗' });
    }
  }
);

app.post(
  '/api/admin/extra-skills',
  auth,
  adminAuth,
  async function (req, res) {
    try {
      const row = await get(
        `
          INSERT INTO extra_skills
            (name, tag, description, target_type, config)
          VALUES (?, ?, ?, ?, ?::jsonb)
          RETURNING *
        `,
        [
          String(req.body.name || '未命名額外技能').slice(0, 120),
          String(req.body.tag || '其他特殊').slice(0, 60),
          String(req.body.description || ''),
          String(req.body.target_type || '').slice(0, 60),
          JSON.stringify(req.body.config || {})
        ]
      );
      res.json({ extra_skill: row });
    } catch (error) {
      console.error(error);
      res.status(500).json({ error: '建立額外技能失敗' });
    }
  }
);

app.get(
  '/api/admin/summon-templates',
  auth,
  adminAuth,
  async function (req, res) {
    try {
      const rows = await all(
        `SELECT * FROM summon_templates ORDER BY id ASC`
      );
      res.json({ summon_templates: rows });
    } catch (error) {
      console.error(error);
      res.status(500).json({ error: '讀取召喚物模板失敗' });
    }
  }
);

app.post(
  '/api/admin/summon-templates',
  auth,
  adminAuth,
  async function (req, res) {
    try {
      const row = await get(
        `
          INSERT INTO summon_templates
            (
              name,
              description,
              growth_mode,
              control_mode,
              behavior_mode,
              self_ai_level,
              language_level,
              speech_enabled,
              loyalty,
              attributes,
              element_affinity,
              traits,
              skills,
              config
            )
          VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?::jsonb, ?::jsonb, ?::jsonb, ?::jsonb, ?::jsonb)
          RETURNING *
        `,
        [
          String(req.body.name || '未命名召喚物').slice(0, 120),
          String(req.body.description || ''),
          String(req.body.growth_mode || 'fixed').slice(0, 30),
          ['manual','assist','auto'].includes(req.body.control_mode) ? req.body.control_mode : 'manual',
          String(req.body.behavior_mode || 'balanced').slice(0, 30),
          Math.max(0, Number(req.body.self_ai_level) || 0),
          Math.max(0, Number(req.body.language_level) || 0),
          Boolean(req.body.speech_enabled),
          Math.max(0, Number(req.body.loyalty) || 100),
          JSON.stringify(req.body.attributes || {}),
          JSON.stringify(req.body.element_affinity || {}),
          JSON.stringify(Array.isArray(req.body.traits) ? req.body.traits : []),
          JSON.stringify(Array.isArray(req.body.skills) ? req.body.skills : []),
          JSON.stringify(req.body.config || {})
        ]
      );
      res.json({ summon_template: row });
    } catch (error) {
      console.error(error);
      res.status(500).json({ error: '建立召喚物模板失敗' });
    }
  }
);


app.patch('/api/admin/summon-templates/:id',auth,adminAuth,async function(req,res){
  try{
    const c=await get(`SELECT * FROM summon_templates WHERE id = ?`,[req.params.id]);if(!c)return res.status(404).json({error:'找不到召喚物模板'});
    const row=await get(`UPDATE summon_templates SET name=?,description=?,growth_mode=?,control_mode=?,behavior_mode=?,self_ai_level=?,language_level=?,speech_enabled=?,loyalty=?,updated_at=CURRENT_TIMESTAMP WHERE id=? RETURNING *`,[
      String(req.body.name===undefined?c.name:req.body.name).slice(0,120),String(req.body.description===undefined?c.description:req.body.description),String(req.body.growth_mode===undefined?c.growth_mode:req.body.growth_mode).slice(0,30),['manual','assist','auto'].includes(req.body.control_mode)?req.body.control_mode:c.control_mode,String(req.body.behavior_mode===undefined?c.behavior_mode:req.body.behavior_mode).slice(0,30),Math.max(0,Number(req.body.self_ai_level===undefined?c.self_ai_level:req.body.self_ai_level)||0),Math.max(0,Number(req.body.language_level===undefined?c.language_level:req.body.language_level)||0),req.body.speech_enabled===undefined?Boolean(c.speech_enabled):Boolean(req.body.speech_enabled),Math.max(0,Number(req.body.loyalty===undefined?c.loyalty:req.body.loyalty)||0),req.params.id]);
    res.json({summon_template:row});
  }catch(error){console.error(error);res.status(500).json({error:'修改召喚物模板失敗'});}
});
app.delete('/api/admin/summon-templates/:id',auth,adminAuth,async function(req,res){
  try{const r=await run(`DELETE FROM summon_templates WHERE id = ?`,[req.params.id]);if(!r.changes)return res.status(404).json({error:'找不到召喚物模板'});res.json({ok:true});}
  catch(error){console.error(error);res.status(500).json({error:'刪除召喚物模板失敗'});}
});

app.get(
  '/api/admin/recipes',
  auth,
  adminAuth,
  async function (req, res) {
    try {
      const rows = await all(
        `SELECT * FROM recipes ORDER BY id ASC`
      );
      res.json({ recipes: rows });
    } catch (error) {
      console.error(error);
      res.status(500).json({ error: '讀取配方失敗' });
    }
  }
);

app.post(
  '/api/admin/recipes',
  auth,
  adminAuth,
  async function (req, res) {
    try {
      const row = await get(
        `
          INSERT INTO recipes
            (name, recipe_type, description, materials, output, config)
          VALUES (?, ?, ?, ?::jsonb, ?::jsonb, ?::jsonb)
          RETURNING *
        `,
        [
          String(req.body.name || '未命名配方').slice(0, 120),
          String(req.body.recipe_type || 'item').slice(0, 60),
          String(req.body.description || ''),
          JSON.stringify(Array.isArray(req.body.materials) ? req.body.materials : []),
          JSON.stringify(req.body.output || {}),
          JSON.stringify(req.body.config || {})
        ]
      );
      res.json({ recipe: row });
    } catch (error) {
      console.error(error);
      res.status(500).json({ error: '建立配方失敗' });
    }
  }
);


/* =========================================================
   ADMIN PROFESSION / SKILL DATABASES
========================================================= */
app.post('/api/admin/professions',auth,adminAuth,async function(req,res){
  try{
    const name=String(req.body.name||'').trim().slice(0,100);
    if(!name)return res.status(400).json({error:'職業名稱不能為空'});

    const category=['main','sub','both'].includes(req.body.category)?req.body.category:'both';
    const baseConfig=req.body.config&&typeof req.body.config==='object'&&!Array.isArray(req.body.config)?{...req.body.config}:{};
    baseConfig.special_resources=normalizeProfessionResourceDefinitions(
      req.body.special_resources===undefined
        ? baseConfig.special_resources
        : req.body.special_resources
    );

    const row=await get(
      `INSERT INTO profession_templates (name,category,rank,description,config) VALUES (?,?,?,?,?::jsonb) RETURNING *`,
      [name,category,String(req.body.rank||'G').slice(0,10),String(req.body.description||''),JSON.stringify(baseConfig)]
    );

    res.json({
      profession:{
        ...row,
        config:parseJsonField(row.config,{})
      }
    });
  }catch(error){
    console.error('CREATE PROFESSION ERROR:',error);
    res.status(error.code==='23505'?409:500).json({error:error.code==='23505'?'這個職業名稱已存在':'建立職業失敗'});
  }
});

app.patch('/api/admin/professions/:id',auth,adminAuth,async function(req,res){
  try{
    const current=await get(`SELECT * FROM profession_templates WHERE id = ?`,[req.params.id]);
    if(!current)return res.status(404).json({error:'找不到職業'});

    const oldName=current.name;
    const name=String(req.body.name===undefined?current.name:req.body.name).trim().slice(0,100);
    if(!name)return res.status(400).json({error:'職業名稱不能為空'});

    const category=['main','sub','both'].includes(req.body.category)
      ? req.body.category
      : current.category;

    const currentConfig=parseJsonField(current.config,{});
    const configInput=req.body.config&&typeof req.body.config==='object'&&!Array.isArray(req.body.config)
      ? {...currentConfig,...req.body.config}
      : {...currentConfig};

    configInput.special_resources=normalizeProfessionResourceDefinitions(
      req.body.special_resources===undefined
        ? configInput.special_resources
        : req.body.special_resources
    );

    const row=await get(
      `UPDATE profession_templates SET name=?,category=?,rank=?,description=?,config=?::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=? RETURNING *`,
      [
        name,
        category,
        String(req.body.rank===undefined?current.rank:req.body.rank).slice(0,10),
        String(req.body.description===undefined?current.description:req.body.description),
        JSON.stringify(configInput),
        req.params.id
      ]
    );

    if(oldName!==name){
      const characters=await all(`SELECT id,main_class,sub_classes FROM character_cards`);
      for(const character of characters){
        let changed=false;
        let mainClass=character.main_class;
        if(mainClass===oldName){
          mainClass=name;
          changed=true;
        }
        const subs=parseJsonField(character.sub_classes,[]);
        const nextSubs=Array.isArray(subs)?subs.map(value=>value===oldName?name:value):[];
        if(JSON.stringify(nextSubs)!==JSON.stringify(subs)){
          changed=true;
        }
        if(changed){
          await run(`UPDATE character_cards SET main_class=?,sub_classes=?::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[mainClass,JSON.stringify(nextSubs),character.id]);
        }
      }
    }

    await syncAllCharacterClassResources();

    res.json({
      profession:{
        ...row,
        config:parseJsonField(row.config,{})
      }
    });
  }catch(error){
    console.error('UPDATE PROFESSION ERROR:',error);
    res.status(error.code==='23505'?409:(error.status||500)).json({error:error.code==='23505'?'這個職業名稱已存在':(error.message||'修改職業失敗')});
  }
});

app.delete('/api/admin/professions/:id',auth,adminAuth,async function(req,res){
  try{
    const r=await run(`DELETE FROM profession_templates WHERE id = ?`,[req.params.id]);
    if(!r.changes)return res.status(404).json({error:'找不到職業'});
    await syncAllCharacterClassResources();
    res.json({ok:true});
  }
  catch(error){console.error(error);res.status(500).json({error:'刪除職業失敗'});}
});

app.post('/api/admin/skill-templates',auth,adminAuth,async function(req,res){
  try{
    const name=
      String(
        req.body.name || ''
      )
        .trim()
        .slice(0,120);

    if(!name){
      return res.status(400).json({
        error:'技能名稱不能為空'
      });
    }

    const summonTag=
      Boolean(
        req.body.summon_tag
      );

    const summonTemplateIds=
      normalizeSummonTemplateIds(
        req.body.summon_template_ids
      );

    if(
      summonTag &&
      !summonTemplateIds.length
    ){
      return res.status(400).json({
        error:'召喚技能至少要選擇一個可召喚的召喚物'
      });
    }

    await validateSummonTemplateIds(
      summonTemplateIds
    );

    const skillData=
      req.body.data &&
      typeof req.body.data === 'object' &&
      !Array.isArray(req.body.data)
        ? {
            ...req.body.data
          }
        : {};

    skillData.summon_template_ids=
      summonTag
        ? summonTemplateIds
        : [];

    const row=
      await get(
        `
          INSERT INTO skill_templates (
            name,
            category,
            skill_type,
            level,
            description,
            check_name,
            damage_formula,
            extra_tag,
            summon_tag,
            resources,
            data
          )
          VALUES (
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
            ?::jsonb
          )
          RETURNING *
        `,
        [
          name,
          String(req.body.category||'一般').slice(0,60),
          String(req.body.skill_type||'active').slice(0,30),
          Math.max(1,Number(req.body.level)||1),
          String(req.body.description||''),
          String(req.body.check_name||'').slice(0,60),
          String(req.body.damage_formula||'').slice(0,200),
          String(req.body.extra_tag||'').slice(0,60),
          summonTag,
          JSON.stringify(normalizeSkillResources(req.body.resources||{})),
          JSON.stringify(skillData)
        ]
      );

    res.json({
      skill:
        parsedSkillTemplate(
          row
        )
    });
  }catch(error){
    console.error(
      'CREATE SKILL TEMPLATE ERROR:',
      error
    );

    res.status(
      error.status || 500
    ).json({
      error:
        error.message ||
        '建立技能失敗'
    });
  }
});

app.patch('/api/admin/skill-templates/:id',auth,adminAuth,async function(req,res){
  try{
    const current=
      await get(
        `
          SELECT *
          FROM skill_templates
          WHERE id = ?
        `,
        [req.params.id]
      );

    if(!current){
      return res.status(404).json({
        error:'找不到技能'
      });
    }

    const summonTag=
      req.body.summon_tag === undefined
        ? Boolean(current.summon_tag)
        : Boolean(req.body.summon_tag);

    const currentData=
      parseJsonField(
        current.data,
        {}
      );

    const summonTemplateIds=
      req.body.summon_template_ids === undefined
        ? normalizeSummonTemplateIds(
            currentData.summon_template_ids
          )
        : normalizeSummonTemplateIds(
            req.body.summon_template_ids
          );

    if(
      summonTag &&
      !summonTemplateIds.length
    ){
      return res.status(400).json({
        error:'召喚技能至少要選擇一個可召喚的召喚物'
      });
    }

    await validateSummonTemplateIds(
      summonTemplateIds
    );

    const nextData=
      req.body.data &&
      typeof req.body.data === 'object' &&
      !Array.isArray(req.body.data)
        ? {
            ...currentData,
            ...req.body.data
          }
        : {
            ...currentData
          };

    nextData.summon_template_ids=
      summonTag
        ? summonTemplateIds
        : [];

    const nextResources=
      normalizeSkillResources(
        req.body.resources &&
        typeof req.body.resources === 'object' &&
        !Array.isArray(req.body.resources)
          ? req.body.resources
          : parseJsonField(
              current.resources,
              {}
            )
      );

    const row=
      await get(
        `
          UPDATE skill_templates
          SET
            name = ?,
            category = ?,
            skill_type = ?,
            level = ?,
            description = ?,
            check_name = ?,
            damage_formula = ?,
            extra_tag = ?,
            summon_tag = ?,
            resources = ?::jsonb,
            data = ?::jsonb,
            updated_at =
              CURRENT_TIMESTAMP
          WHERE id = ?
          RETURNING *
        `,
        [
          String(
            req.body.name === undefined
              ? current.name
              : req.body.name
          ).trim().slice(0,120),
          String(
            req.body.category === undefined
              ? current.category
              : req.body.category
          ).slice(0,60),
          String(
            req.body.skill_type === undefined
              ? current.skill_type
              : req.body.skill_type
          ).slice(0,30),
          Math.max(
            1,
            Number(
              req.body.level === undefined
                ? current.level
                : req.body.level
            ) || 1
          ),
          String(
            req.body.description === undefined
              ? current.description
              : req.body.description
          ),
          String(
            req.body.check_name === undefined
              ? current.check_name
              : req.body.check_name
          ).slice(0,60),
          String(
            req.body.damage_formula === undefined
              ? current.damage_formula
              : req.body.damage_formula
          ).slice(0,200),
          String(
            req.body.extra_tag === undefined
              ? current.extra_tag
              : req.body.extra_tag
          ).slice(0,60),
          summonTag,
          JSON.stringify(
            nextResources
          ),
          JSON.stringify(
            nextData
          ),
          req.params.id
        ]
      );

    const updatedCharacterCount=
      await syncSkillTemplateToCharacters(
        row
      );

    res.json({
      ok:true,
      skill:
        parsedSkillTemplate(
          row
        ),
      updated_character_count:
        updatedCharacterCount
    });
  }catch(error){
    console.error(
      'UPDATE SKILL TEMPLATE ERROR:',
      error
    );

    res.status(
      error.status || 500
    ).json({
      error:
        error.message ||
        '修改技能失敗'
    });
  }
});

app.delete('/api/admin/skill-templates/:id',auth,adminAuth,async function(req,res){
  try{const r=await run(`DELETE FROM skill_templates WHERE id = ?`,[req.params.id]);if(!r.changes)return res.status(404).json({error:'找不到技能'});res.json({ok:true});}
  catch(error){console.error(error);res.status(500).json({error:'刪除技能失敗'});}
});

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

/* =========================================================
   ADMIN ALL CHARACTERS
========================================================= */

app.get(
  '/api/admin/characters',
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
            JOIN users u
              ON u.id = c.user_id
            ORDER BY c.id ASC
          `
        );

      const parsedCharacters = [];

      for (
        const character of characters
      ) {
        const classResources =
          await syncCharacterClassResources(
            character.user_id,
            character
          );

        parsedCharacters.push(
          parseCharacter({
            ...character,
            class_resources:
              classResources
          })
        );
      }

      res.json({
        characters:
          parsedCharacters
      });
    } catch (error) {
      console.error(
        'ADMIN CHARACTERS ERROR:',
        error
      );

      res.status(500).json({
        error:
          '讀取全站角色卡失敗'
      });
    }
  }
);

/* =========================================================
   ADMIN CHARACTER BY CHARACTER ID
   相容前端使用 /api/admin/characters/:id
========================================================= */

app.get(
  '/api/admin/characters/:id',
  auth,
  adminAuth,
  async function (
    req,
    res
  ) {
    try {
      const character =
        await get(
          `
            SELECT
              c.*,
              u.username
            FROM character_cards c
            JOIN users u
              ON u.id = c.user_id
            WHERE c.id = ?
          `,
          [req.params.id]
        );

      if (!character) {
        return res.status(404).json({
          error: '找不到角色卡'
        });
      }

      res.json({
        character:
          parseCharacter(character)
      });
    } catch (error) {
      console.error(
        'ADMIN CHARACTER GET ERROR:',
        error
      );

      res.status(500).json({
        error:
          '讀取角色卡失敗'
      });
    }
  }
);

app.patch(
  '/api/admin/characters/:id',
  auth,
  adminAuth,
  async function (
    req,
    res
  ) {
    try {
      const character =
        await get(
          `
            SELECT *
            FROM character_cards
            WHERE id = ?
          `,
          [req.params.id]
        );

      if (!character) {
        return res.status(404).json({
          error: '找不到角色卡'
        });
      }

      const updated =
        await updateCharacter(
          character.user_id,
          req.body
        );

      res.json({
        ok: true,
        character: updated
      });
    } catch (error) {
      console.error(
        'ADMIN CHARACTER UPDATE ERROR:',
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

app.delete(
  '/api/admin/characters/:id',
  auth,
  adminAuth,
  async function (
    req,
    res
  ) {
    try {
      const character =
        await get(
          `
            SELECT id
            FROM character_cards
            WHERE id = ?
          `,
          [req.params.id]
        );

      if (!character) {
        return res.status(404).json({
          error: '找不到角色卡'
        });
      }

      await run(
        `
          DELETE FROM character_cards
          WHERE id = ?
        `,
        [req.params.id]
      );

      res.json({
        ok: true,
        message: '角色卡已刪除'
      });
    } catch (error) {
      console.error(
        'ADMIN CHARACTER DELETE ERROR:',
        error
      );

      res.status(500).json({
        error:
          '刪除角色卡失敗'
      });
    }
  }
);

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

app.patch(
  '/api/admin/players/:id/class-resource',
  auth,
  adminAuth,
  async function (req, res) {
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

      const result =
        await changeCharacterClassResource(
          targetId,
          req.body || {},
          true
        );

      res.json({
        ok: true,
        ...result
      });
    } catch (error) {
      res.status(
        error.status || 500
      ).json({
        error:
          error.message ||
          'DM 修改職業資源失敗'
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

      const battleMembers =
        await all(
          `
            SELECT user_id
            FROM room_members
            WHERE room_id = ?
            AND role = 'player'
          `,
          [req.params.id]
        );

      for (
        const member of battleMembers
      ) {
        await resetCharacterClassResourcesForBattle(
          member.user_id
        );
      }

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

      if (
        nextIndex === 0
      ) {
        for (
          const member of members
        ) {
          await applyCharacterClassResourceRoundDelta(
            member.user_id
          );
        }
      }

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
   CHARACTER SYSTEM MIGRATION
========================================================= */

async function migrateCharacterSystem() {
  const statements = [
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS combat_hatred INTEGER NOT NULL DEFAULT 100`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS faiths JSONB NOT NULL DEFAULT '[]'::jsonb`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS great_ways JSONB NOT NULL DEFAULT '[]'::jsonb`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS faith INTEGER NOT NULL DEFAULT 0`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS agility_trial_tier INTEGER NOT NULL DEFAULT 1`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS strength_trial_tier INTEGER NOT NULL DEFAULT 1`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS constitution_trial_tier INTEGER NOT NULL DEFAULT 1`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS spirit_trial_tier INTEGER NOT NULL DEFAULT 1`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS great_way_trial_tier INTEGER NOT NULL DEFAULT 1`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS faith_trial_tier INTEGER NOT NULL DEFAULT 1`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS current_great_way INTEGER`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS current_faith INTEGER`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS profession_rank VARCHAR(10) NOT NULL DEFAULT 'G'`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS innate_sub_class VARCHAR(100)`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS weird_hatred INTEGER NOT NULL DEFAULT 0`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS class_resources JSONB NOT NULL DEFAULT '{}'::jsonb`
  ];

  statements.push(
    `CREATE TABLE IF NOT EXISTS extra_skills (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(120) NOT NULL,
      tag VARCHAR(60) NOT NULL DEFAULT '其他特殊',
      description TEXT NOT NULL DEFAULT '',
      target_type VARCHAR(60) NOT NULL DEFAULT '',
      config JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );

  statements.push(
    `CREATE TABLE IF NOT EXISTS summon_templates (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(120) NOT NULL,
      description TEXT NOT NULL DEFAULT '',
      growth_mode VARCHAR(30) NOT NULL DEFAULT 'fixed',
      self_ai_level INTEGER NOT NULL DEFAULT 0,
      language_level INTEGER NOT NULL DEFAULT 0,
      loyalty INTEGER NOT NULL DEFAULT 100,
      attributes JSONB NOT NULL DEFAULT '{}'::jsonb,
      element_affinity JSONB NOT NULL DEFAULT '{}'::jsonb,
      traits JSONB NOT NULL DEFAULT '[]'::jsonb,
      skills JSONB NOT NULL DEFAULT '[]'::jsonb,
      config JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );

  statements.push(
    `CREATE TABLE IF NOT EXISTS summon_instances (
      id BIGSERIAL PRIMARY KEY,
      template_id BIGINT REFERENCES summon_templates(id) ON DELETE SET NULL,
      owner_user_id BIGINT REFERENCES users(id) ON DELETE CASCADE,
      name VARCHAR(120) NOT NULL,
      status VARCHAR(30) NOT NULL DEFAULT 'active',
      overrides JSONB NOT NULL DEFAULT '{}'::jsonb,
      current_state JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );

  statements.push(
    `CREATE TABLE IF NOT EXISTS recipes (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(120) NOT NULL,
      recipe_type VARCHAR(60) NOT NULL DEFAULT 'item',
      description TEXT NOT NULL DEFAULT '',
      materials JSONB NOT NULL DEFAULT '[]'::jsonb,
      output JSONB NOT NULL DEFAULT '{}'::jsonb,
      config JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );

  statements.push(
    `CREATE TABLE IF NOT EXISTS profession_templates (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(100) NOT NULL UNIQUE,
      category VARCHAR(20) NOT NULL DEFAULT 'both',
      rank VARCHAR(10) NOT NULL DEFAULT 'G',
      description TEXT NOT NULL DEFAULT '',
      config JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );

  statements.push(
    `CREATE TABLE IF NOT EXISTS skill_templates (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(120) NOT NULL,
      category VARCHAR(60) NOT NULL DEFAULT '一般',
      skill_type VARCHAR(30) NOT NULL DEFAULT 'active',
      level INTEGER NOT NULL DEFAULT 1,
      description TEXT NOT NULL DEFAULT '',
      check_name VARCHAR(60) NOT NULL DEFAULT '',
      damage_formula VARCHAR(200) NOT NULL DEFAULT '',
      extra_tag VARCHAR(60) NOT NULL DEFAULT '',
      summon_tag BOOLEAN NOT NULL DEFAULT FALSE,
      resources JSONB NOT NULL DEFAULT '{}'::jsonb,
      data JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );

  statements.push(`ALTER TABLE summon_templates ADD COLUMN IF NOT EXISTS control_mode VARCHAR(20) NOT NULL DEFAULT 'manual'`);
  statements.push(`ALTER TABLE summon_templates ADD COLUMN IF NOT EXISTS behavior_mode VARCHAR(30) NOT NULL DEFAULT 'balanced'`);
  statements.push(`ALTER TABLE summon_templates ADD COLUMN IF NOT EXISTS speech_enabled BOOLEAN NOT NULL DEFAULT FALSE`);
  statements.push(`ALTER TABLE summon_instances ADD COLUMN IF NOT EXISTS room_id BIGINT REFERENCES rooms(id) ON DELETE SET NULL`);
  statements.push(`ALTER TABLE summon_instances ADD COLUMN IF NOT EXISTS source_skill_id VARCHAR(100)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_summon_instances_owner_status ON summon_instances(owner_user_id, status)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_summon_instances_room_status ON summon_instances(room_id, status)`);

  for (const sql of statements) {
    await run(sql);
  }
}

async function updatePlayerCharacter(userId, body) {
  const current = await requireCharacter(userId);
  const faction = current.faction === '西國' ? '西國' : '東國';
  const fifth = faction === '西國' ? 'faith' : 'great_way';
  const currentFifthField =
    faction === '西國'
      ? 'current_faith'
      : 'current_great_way';
  const fields = ['agility', 'strength', 'constitution', 'spirit', fifth];

  const oldTotal = fields.reduce((sum, field) => sum + Math.max(0, Number(current[field]) || 0), 0);
  const oldPoints = Math.max(0, Number(current.attribute_points) || 0);
  const next = {};

  for (const field of fields) {
    next[field] = body[field] === undefined
      ? Math.max(0, Number(current[field]) || 0)
      : Math.max(0, Math.floor(Number(body[field]) || 0));
  }

  const caps = {1:50,2:100,3:180,4:320,5:600,6:1100,7:2000,8:3600,9:6500,10:10000};
  const tier = Math.max(1, Math.min(10, Number(current.tier) || 1));
  const cap = caps[tier];

  for (const field of fields) {
    if (next[field] > cap) {
      throw Object.assign(new Error(`目前 ${tier} 階單項屬性上限為 ${cap}`), {status:400});
    }
  }

  const newTotal = fields.reduce((sum, field) => sum + next[field], 0);
  const newPoints = body.attribute_points === undefined
    ? oldPoints
    : Math.max(0, Math.floor(Number(body.attribute_points) || 0));

  if (newTotal + newPoints !== oldTotal + oldPoints) {
    throw Object.assign(new Error('屬性點數不正確，總點數必須保持不變'), {status:400});
  }

  const updates=[];
  const values=[];

  for (const field of fields) {
    if (body[field] !== undefined) {
      updates.push(`${field} = ?`);
      values.push(next[field]);
    }
  }

  if (body.attribute_points !== undefined || fields.some(field => body[field] !== undefined)) {
    updates.push('attribute_points = ?');
    values.push(newPoints);
  }

  /*
   * 大道／信仰是第五維的「最大值」，
   * current_great_way / current_faith 則是目前剩餘值。
   *
   * 若原本是滿值，調高第五維時會一起補到新的滿值；
   * 若原本已經被扣除，則保留目前剩餘值並限制不能超過新上限。
   */
  if (body[fifth] !== undefined) {
    const oldMax =
      Math.max(
        0,
        Number(current[fifth]) || 0
      );

    const oldCurrent =
      current[currentFifthField] === null ||
      current[currentFifthField] === undefined
        ? oldMax
        : Math.max(
            0,
            Number(
              current[currentFifthField]
            ) || 0
          );

    const newMax =
      Math.max(
        0,
        Number(next[fifth]) || 0
      );

    const nextCurrent =
      oldCurrent >= oldMax
        ? newMax
        : Math.min(
            oldCurrent,
            newMax
          );

    updates.push(
      currentFifthField +
      ' = ?'
    );

    values.push(
      nextCurrent
    );
  }

  if (body.name !== undefined) {
    const name=String(body.name||'').trim().slice(0,50);
    if(!name) throw Object.assign(new Error('角色名稱不能為空'),{status:400});
    updates.push('name = ?'); values.push(name);
  }

  if (body.main_class !== undefined) {
    const mainClass=String(body.main_class||'').trim().slice(0,50);
    if(mainClass && mainClass!=='未設定' && mainClass!==current.main_class) {
      const exists=await get(`SELECT id FROM profession_templates WHERE name = ? AND category IN ('main','both')`,[mainClass]);
      if(!exists) throw Object.assign(new Error('主職業必須從職業資料庫選擇'),{status:400});
    }
    updates.push('main_class = ?'); values.push(mainClass||'未設定');
  }

  if (body.sub_classes !== undefined) {
    const subClasses=Array.isArray(body.sub_classes)
      ? body.sub_classes.map(v=>String(v||'').trim().slice(0,50)).filter(Boolean).slice(0,3)
      : [];
    const currentSubs=Array.isArray(current.sub_classes)?current.sub_classes:[];
    for(const subClass of subClasses){
      if(currentSubs.includes(subClass)) continue;
      const exists=await get(`SELECT id FROM profession_templates WHERE name = ? AND category IN ('sub','both')`,[subClass]);
      if(!exists) throw Object.assign(new Error(`副職業「${subClass}」不在職業資料庫中`),{status:400});
    }
    updates.push('sub_classes = ?::jsonb'); values.push(JSON.stringify(subClasses));
  }

  const identityField=faction==='西國'?'faiths':'great_ways';
  if(body[identityField]!==undefined){
    const existing=normalizeIdentityEntries(current[identityField]);
    const existingRankByName=new Map(existing.map(item=>[item.name,item.rank]));
    const incoming=Array.isArray(body[identityField])?body[identityField]:[];
    const list=incoming.map(item=>{
      const name=typeof item==='string'
        ? item.trim().slice(0,100)
        : String(item?.name||'').trim().slice(0,100);
      if(!name)return null;
      return {
        name,
        rank:existingRankByName.get(name)||'G'
      };
    }).filter(Boolean).slice(0,10);
    updates.push(`${identityField} = ?::jsonb`); values.push(JSON.stringify(list));
  }

  if(!updates.length) throw Object.assign(new Error('沒有可更新欄位'),{status:400});
  updates.push('updated_at = CURRENT_TIMESTAMP'); values.push(userId);
  await run(`UPDATE character_cards SET ${updates.join(', ')} WHERE user_id = ?`, values);
  return getCharacterByUser(userId);
}


async function migrateTaskSystem() {
  await run(`
    CREATE TABLE IF NOT EXISTS tasks (
      id BIGSERIAL PRIMARY KEY,
      title VARCHAR(120) NOT NULL,
      description TEXT NOT NULL DEFAULT '',
      task_type VARCHAR(20) NOT NULL DEFAULT 'public',
      status VARCHAR(20) NOT NULL DEFAULT 'open',
      creator_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      target_user_id BIGINT REFERENCES users(id) ON DELETE CASCADE,
      min_tier INTEGER NOT NULL DEFAULT 1,
      requirements TEXT NOT NULL DEFAULT '',
      rewards JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      closed_at TIMESTAMPTZ
    )
  `);

  await run(`
    CREATE TABLE IF NOT EXISTS task_participants (
      task_id BIGINT NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
      user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      status VARCHAR(20) NOT NULL DEFAULT 'accepted',
      accepted_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      completed_at TIMESTAMPTZ,
      PRIMARY KEY(task_id, user_id)
    )
  `);

  await run(`CREATE INDEX IF NOT EXISTS idx_tasks_status_type ON tasks(status, task_type)`);
  await run(`CREATE INDEX IF NOT EXISTS idx_tasks_target_user ON tasks(target_user_id)`);
  await run(`CREATE INDEX IF NOT EXISTS idx_task_participants_user ON task_participants(user_id)`);
}


/* =========================================================
   TASK SYSTEM
========================================================= */

function normalizeTaskRewards(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    return {};
  }

  const rewards = {};

  for (const field of [
    'experience',
    'attribute_points',
    'skill_points'
  ]) {
    if (value[field] !== undefined) {
      rewards[field] = Math.max(
        0,
        Math.floor(Number(value[field]) || 0)
      );
    }
  }

  for (const field of ['equipment', 'weapons']) {
    if (value[field] !== undefined) {
      rewards[field] = Array.isArray(value[field])
        ? value[field]
        : [];
    }
  }

  return rewards;
}

function parseTask(row, myStatus) {
  if (!row) return null;

  return {
    ...row,
    rewards: parseJsonField(row.rewards, {}),
    my_status: myStatus || row.my_status || null
  };
}

async function getTask(taskId) {
  return get(
    `
      SELECT
        t.*,
        creator.username AS creator_username,
        target.username AS target_username
      FROM tasks t
      JOIN users creator
        ON creator.id = t.creator_id
      LEFT JOIN users target
        ON target.id = t.target_user_id
      WHERE t.id = ?
    `,
    [taskId]
  );
}

async function grantTaskRewards(userId, rewards) {
  const normalized = normalizeTaskRewards(rewards);
  const updates = [];
  const values = [];

  for (const field of [
    'experience',
    'attribute_points',
    'skill_points'
  ]) {
    if (normalized[field]) {
      updates.push(`${field} = ${field} + ?`);
      values.push(normalized[field]);
    }
  }

  if (normalized.equipment?.length) {
    updates.push(`equipment = equipment || ?::jsonb`);
    values.push(JSON.stringify(normalized.equipment));
  }

  if (normalized.weapons?.length) {
    updates.push(`weapons = weapons || ?::jsonb`);
    values.push(JSON.stringify(normalized.weapons));
  }

  if (!updates.length) return;

  updates.push('updated_at = CURRENT_TIMESTAMP');
  values.push(userId);

  await run(
    `
      UPDATE character_cards
      SET ${updates.join(', ')}
      WHERE user_id = ?
    `,
    values
  );
}

/* PUBLIC TASK HALL */
app.get(
  '/api/tasks/public',
  auth,
  async function (req, res) {
    try {
      const rows = await all(
        `
          SELECT
            t.*,
            creator.username AS creator_username,
            tp.status AS my_status
          FROM tasks t
          JOIN users creator
            ON creator.id = t.creator_id
          LEFT JOIN task_participants tp
            ON tp.task_id = t.id
           AND tp.user_id = ?
          WHERE t.task_type = 'public'
            AND t.status = 'open'
          ORDER BY t.created_at DESC
        `,
        [req.user.id]
      );

      res.json({
        tasks: rows.map(row => parseTask(row))
      });
    } catch (error) {
      console.error('PUBLIC TASKS ERROR:', error);
      res.status(500).json({
        error: '讀取任務大廳失敗'
      });
    }
  }
);

/* MY PRIVATE / ACCEPTED TASKS */
app.get(
  '/api/tasks/my',
  auth,
  async function (req, res) {
    try {
      const rows = await all(
        `
          SELECT
            t.*,
            creator.username AS creator_username,
            tp.status AS my_status,
            tp.accepted_at,
            tp.completed_at
          FROM task_participants tp
          JOIN tasks t
            ON t.id = tp.task_id
          JOIN users creator
            ON creator.id = t.creator_id
          WHERE tp.user_id = ?
          ORDER BY
            CASE WHEN tp.status = 'completed' THEN 1 ELSE 0 END,
            tp.accepted_at DESC
        `,
        [req.user.id]
      );

      res.json({
        tasks: rows.map(row => parseTask(row))
      });
    } catch (error) {
      console.error('MY TASKS ERROR:', error);
      res.status(500).json({
        error: '讀取我的任務失敗'
      });
    }
  }
);

/* ACCEPT PUBLIC TASK */
app.post(
  '/api/tasks/:id/accept',
  auth,
  async function (req, res) {
    try {
      const task = await getTask(req.params.id);

      if (!task) {
        return res.status(404).json({
          error: '找不到任務'
        });
      }

      if (task.task_type !== 'public') {
        return res.status(403).json({
          error: '這不是公共任務'
        });
      }

      if (task.status !== 'open') {
        return res.status(400).json({
          error: '任務目前不是開放狀態'
        });
      }

      const character = await getCharacterByUser(req.user.id);
      if (!character) {
        return res.status(400).json({
          error: '請先建立角色卡'
        });
      }

      if ((Number(character.tier) || 1) < (Number(task.min_tier) || 1)) {
        return res.status(403).json({
          error: `此任務最低需要 ${task.min_tier} 階`
        });
      }

      await run(
        `
          INSERT INTO task_participants (
            task_id,
            user_id,
            status
          )
          VALUES (?, ?, 'accepted')
          ON CONFLICT (task_id, user_id)
          DO NOTHING
        `,
        [task.id, req.user.id]
      );

      res.json({
        ok: true,
        task: parseTask(
          await getTask(task.id),
          'accepted'
        )
      });
    } catch (error) {
      console.error('ACCEPT TASK ERROR:', error);
      res.status(error.status || 500).json({
        error: error.message || '接受任務失敗'
      });
    }
  }
);

/* ADMIN TASK LIST */
app.get(
  '/api/admin/tasks',
  auth,
  adminAuth,
  async function (req, res) {
    try {
      const rows = await all(
        `
          SELECT
            t.*,
            creator.username AS creator_username,
            target.username AS target_username,
            COUNT(tp.user_id)::INTEGER AS participant_count
          FROM tasks t
          JOIN users creator
            ON creator.id = t.creator_id
          LEFT JOIN users target
            ON target.id = t.target_user_id
          LEFT JOIN task_participants tp
            ON tp.task_id = t.id
          GROUP BY t.id, creator.username, target.username
          ORDER BY t.created_at DESC
        `
      );

      res.json({
        tasks: rows.map(row => parseTask(row))
      });
    } catch (error) {
      console.error('ADMIN TASKS ERROR:', error);
      res.status(500).json({
        error: '讀取任務管理失敗'
      });
    }
  }
);

/* ADMIN CREATE TASK */
app.post(
  '/api/admin/tasks',
  auth,
  adminAuth,
  async function (req, res) {
    try {
      const title = String(req.body.title || '').trim().slice(0, 120);
      if (!title) {
        return res.status(400).json({
          error: '任務名稱不能為空'
        });
      }

      const taskType = req.body.task_type === 'private'
        ? 'private'
        : 'public';

      const targetUserId = taskType === 'private'
        ? Number(req.body.target_user_id || 0)
        : null;

      if (taskType === 'private') {
        if (!targetUserId) {
          return res.status(400).json({
            error: '私人任務必須指定玩家'
          });
        }

        const target = await getUser(targetUserId);
        if (!target || target.is_admin) {
          return res.status(400).json({
            error: '私人任務只能指定玩家帳號'
          });
        }
      }

      const minTier = Math.max(
        1,
        Math.min(10, Math.floor(Number(req.body.min_tier) || 1))
      );

      const rewards = normalizeTaskRewards(req.body.rewards);

      const result = await run(
        `
          INSERT INTO tasks (
            title,
            description,
            task_type,
            status,
            creator_id,
            target_user_id,
            min_tier,
            requirements,
            rewards
          )
          VALUES (?, ?, ?, 'open', ?, ?, ?, ?, ?::jsonb)
          RETURNING id
        `,
        [
          title,
          String(req.body.description || ''),
          taskType,
          req.user.id,
          targetUserId,
          minTier,
          String(req.body.requirements || ''),
          JSON.stringify(rewards)
        ]
      );

      if (taskType === 'private') {
        await run(
          `
            INSERT INTO task_participants (
              task_id,
              user_id,
              status
            )
            VALUES (?, ?, 'accepted')
            ON CONFLICT (task_id, user_id)
            DO NOTHING
          `,
          [result.id, targetUserId]
        );
      }

      res.json({
        ok: true,
        task: parseTask(await getTask(result.id))
      });
    } catch (error) {
      console.error('CREATE TASK ERROR:', error);
      res.status(error.status || 500).json({
        error: error.message || '建立任務失敗'
      });
    }
  }
);

/* ADMIN UPDATE TASK */
app.patch(
  '/api/admin/tasks/:id',
  auth,
  adminAuth,
  async function (req, res) {
    try {
      const task = await getTask(req.params.id);
      if (!task) {
        return res.status(404).json({ error: '找不到任務' });
      }

      const updates = [];
      const values = [];

      for (const field of ['title', 'description', 'requirements', 'status']) {
        if (req.body[field] !== undefined) {
          updates.push(`${field} = ?`);
          values.push(String(req.body[field]));
        }
      }

      if (req.body.min_tier !== undefined) {
        updates.push('min_tier = ?');
        values.push(Math.max(1, Math.min(10, Math.floor(Number(req.body.min_tier) || 1))));
      }

      if (req.body.rewards !== undefined) {
        updates.push('rewards = ?::jsonb');
        values.push(JSON.stringify(normalizeTaskRewards(req.body.rewards)));
      }

      if (!updates.length) {
        return res.status(400).json({ error: '沒有可更新欄位' });
      }

      updates.push('updated_at = CURRENT_TIMESTAMP');
      values.push(req.params.id);

      await run(
        `UPDATE tasks SET ${updates.join(', ')} WHERE id = ?`,
        values
      );

      res.json({
        ok: true,
        task: parseTask(await getTask(req.params.id))
      });
    } catch (error) {
      console.error('UPDATE TASK ERROR:', error);
      res.status(error.status || 500).json({
        error: error.message || '修改任務失敗'
      });
    }
  }
);

/* ADMIN COMPLETE TASK FOR A PLAYER */
app.post(
  '/api/admin/tasks/:id/complete',
  auth,
  adminAuth,
  async function (req, res) {
    const client = await pool.connect();

    try {
      await client.query('BEGIN');

      const taskResult = await client.query(
        pgSql(`
          SELECT *
          FROM tasks
          WHERE id = ?
          FOR UPDATE
        `),
        [req.params.id]
      );

      const task = taskResult.rows[0];
      if (!task) {
        await client.query('ROLLBACK');
        return res.status(404).json({ error: '找不到任務' });
      }

      const targetUserId = Number(
        req.body.user_id || task.target_user_id || 0
      );

      if (!targetUserId) {
        await client.query('ROLLBACK');
        return res.status(400).json({
          error: '公共任務完成時必須指定玩家'
        });
      }

      const participantResult = await client.query(
        pgSql(`
          SELECT *
          FROM task_participants
          WHERE task_id = ?
            AND user_id = ?
          FOR UPDATE
        `),
        [task.id, targetUserId]
      );

      const participant = participantResult.rows[0];
      if (!participant) {
        await client.query('ROLLBACK');
        return res.status(400).json({
          error: '該玩家尚未接受這個任務'
        });
      }

      if (participant.status === 'completed') {
        await client.query('ROLLBACK');
        return res.status(400).json({
          error: '這個玩家已經完成過此任務'
        });
      }

      const rewards = normalizeTaskRewards(
        parseJsonField(task.rewards, {})
      );

      const updates = [];
      const values = [];

      for (const field of ['experience', 'attribute_points', 'skill_points']) {
        if (rewards[field]) {
          updates.push(`${field} = ${field} + ?`);
          values.push(rewards[field]);
        }
      }

      if (rewards.equipment?.length) {
        updates.push(`equipment = equipment || ?::jsonb`);
        values.push(JSON.stringify(rewards.equipment));
      }

      if (rewards.weapons?.length) {
        updates.push(`weapons = weapons || ?::jsonb`);
        values.push(JSON.stringify(rewards.weapons));
      }

      if (updates.length) {
        updates.push('updated_at = CURRENT_TIMESTAMP');
        values.push(targetUserId);

        await client.query(
          pgSql(`
            UPDATE character_cards
            SET ${updates.join(', ')}
            WHERE user_id = ?
          `),
          values
        );
      }

      await client.query(
        pgSql(`
          UPDATE task_participants
          SET
            status = 'completed',
            completed_at = CURRENT_TIMESTAMP
          WHERE task_id = ?
            AND user_id = ?
        `),
        [task.id, targetUserId]
      );

      await client.query('COMMIT');

      res.json({
        ok: true,
        message: '任務已完成並發放獎勵',
        rewards
      });
    } catch (error) {
      await client.query('ROLLBACK').catch(() => {});
      console.error('COMPLETE TASK ERROR:', error);
      res.status(error.status || 500).json({
        error: error.message || '完成任務失敗'
      });
    } finally {
      client.release();
    }
  }
);

/* ADMIN DELETE TASK */
app.delete(
  '/api/admin/tasks/:id',
  auth,
  adminAuth,
  async function (req, res) {
    try {
      const task = await getTask(req.params.id);
      if (!task) {
        return res.status(404).json({ error: '找不到任務' });
      }

      await run(
        `DELETE FROM tasks WHERE id = ?`,
        [req.params.id]
      );

      res.json({
        ok: true,
        message: '任務已刪除'
      });
    } catch (error) {
      console.error('DELETE TASK ERROR:', error);
      res.status(500).json({
        error: '刪除任務失敗'
      });
    }
  }
);

/* =========================================================
   START SERVER
========================================================= */

initDb()
  .then(function () {
    return migrateCharacterSystem();
  })
  .then(function () {
    return migrateTaskSystem();
  })
  .then(function () {
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
  })
  .catch(function (error) {
    console.error(
      'Database initialization failed:',
      error
    );

    process.exit(1);
  });
