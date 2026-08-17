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

const ELEMENT_AFFINITY_DAMAGE_PER_POINT = (() => {
  const value = Number(process.env.ELEMENT_AFFINITY_DAMAGE_PER_POINT);
  return Number.isFinite(value) ? value : 0.5;
})();

function normalizeElementAffinityModifier(value) {
  const source = value && typeof value === 'object' && !Array.isArray(value) ? value : {};
  const out = {};
  for (const element of ELEMENTS) {
    const n = Number(source[element]);
    out[element] = Number.isFinite(n) ? n : 0;
  }
  return out;
}


/* =========================================================
   V16 UNIFIED EFFECT SYSTEM
========================================================= */

const UNIFIED_EFFECT_TYPES = new Set([
  'stat_flat',
  'stat_percent',
  'all_attributes_percent',
  'element_affinity',
  'heal_hp',
  'damage_hp',
  'heal_spirit',
  'damage_spirit',
  'heal_sanity',
  'damage_sanity',
  'fifth_change',
  'weird_coin',
  'game_coin',
  'inventory',
  'status_apply',
  'revive',
  'inventory_slots',
  'class_resource'
]);

const UNIFIED_STAT_KEYS = new Set([
  'strength',
  'agility',
  'constitution',
  'spirit',
  'luck',
  'great_way',
  'faith',
  'fifth'
]);

function normalizeUnifiedEffect(effect) {
  if (!effect || typeof effect !== 'object' || Array.isArray(effect)) return null;
  const type = String(effect.type || '').trim();
  if (!UNIFIED_EFFECT_TYPES.has(type)) return null;

  const operation = ['gain','spend','set'].includes(String(effect.operation || ''))
    ? String(effect.operation)
    : 'gain';

  const timing = ['immediate','passive','round_end'].includes(String(effect.timing || ''))
    ? String(effect.timing)
    : (
        ['stat_flat','stat_percent','all_attributes_percent','element_affinity'].includes(type)
          ? 'passive'
          : 'immediate'
      );

  const rawValue = Number(effect.value ?? effect.amount ?? 0);
  const value = Number.isFinite(rawValue) ? rawValue : 0;
  const chanceRaw = Number(effect.chance ?? 100);
  const chance = Math.max(0, Math.min(100, Number.isFinite(chanceRaw) ? chanceRaw : 100));
  const duration = Math.max(0, Math.floor(Number(effect.duration_rounds ?? effect.duration ?? 0) || 0));

  let key = String(effect.key || effect.target_key || '').trim();
  if (type === 'stat_flat' || type === 'stat_percent') {
    if (!UNIFIED_STAT_KEYS.has(key)) key = 'strength';
  }
  if (type === 'element_affinity' && !ELEMENTS.includes(key)) key = '火';

  return {
    type,
    key: key.slice(0,120),
    value,
    operation,
    timing,
    duration_rounds: duration,
    chance,
    extra: String(effect.extra || effect.item_name || effect.status_name || effect.resource_name || '').trim().slice(0,160),
    category: String(effect.category || '').trim().slice(0,80),
    note: String(effect.note || '').trim().slice(0,300)
  };
}

function legacyEffectsToUnified(source) {
  const list = [];
  if (!source || typeof source !== 'object' || Array.isArray(source)) return list;

  const statFlat = source.stat_flat && typeof source.stat_flat === 'object' ? source.stat_flat : {};
  for (const [key,value] of Object.entries(statFlat)) {
    if (Number(value)) list.push({type:'stat_flat',key,value:Number(value),timing:'passive'});
  }
  const statPct = source.stat_percent && typeof source.stat_percent === 'object' ? source.stat_percent : {};
  for (const [key,value] of Object.entries(statPct)) {
    if (Number(value)) list.push({type:'stat_percent',key,value:Number(value),timing:'passive'});
  }
  if (Number(source.all_attributes_percent)) {
    list.push({type:'all_attributes_percent',value:Number(source.all_attributes_percent),timing:'passive'});
  }
  const affinity = source.element_affinity && typeof source.element_affinity === 'object' ? source.element_affinity : {};
  for (const [key,value] of Object.entries(affinity)) {
    if (Number(value)) list.push({type:'element_affinity',key,value:Number(value),timing:'passive'});
  }
  if (Number(source.heal_hp)) list.push({type:'heal_hp',value:Number(source.heal_hp),timing:'immediate'});
  if (Number(source.restore_spirit)) list.push({type:'heal_spirit',value:Number(source.restore_spirit),timing:'immediate'});
  if (Number(source.revive_hp_percent)) list.push({type:'revive',value:Number(source.revive_hp_percent),timing:'immediate'});
  return list;
}

function normalizeUnifiedEffects(value) {
  const source = parseJsonField(value, value);
  let rawList = [];
  if (Array.isArray(source)) rawList = source;
  else if (source && typeof source === 'object') {
    if (Array.isArray(source.list)) rawList = source.list;
    else if (Array.isArray(source.effects)) rawList = source.effects;
    rawList = [...rawList, ...legacyEffectsToUnified(source)];
  }

  const out = [];
  const seen = new Set();
  for (const raw of rawList) {
    const effect = normalizeUnifiedEffect(raw);
    if (!effect) continue;
    const sig = JSON.stringify(effect);
    if (seen.has(sig)) continue;
    seen.add(sig);
    out.push(effect);
  }
  return out.slice(0,80);
}

function effectContainerWithList(container, list) {
  const source = container && typeof container === 'object' && !Array.isArray(container)
    ? {...container}
    : {};
  source.list = normalizeUnifiedEffects(Array.isArray(list) ? list : []);
  return source;
}

function passiveModifierSummaryFromEffects(value, stacks = 1) {
  const factor = Math.max(1, Math.floor(Number(stacks) || 1));
  const flat = {strength:0,agility:0,constitution:0,spirit:0,luck:0,great_way:0,faith:0};
  const pct = {strength:0,agility:0,constitution:0,spirit:0,luck:0,great_way:0,faith:0};
  let allPct = 0;
  const affinity = Object.fromEntries(ELEMENTS.map(element => [element,0]));

  for (const effect of normalizeUnifiedEffects(value)) {
    if (effect.timing !== 'passive') continue;
    if (effect.type === 'stat_flat') {
      const keys = effect.key === 'fifth' ? ['great_way','faith'] : [effect.key];
      for (const key of keys) if (Object.prototype.hasOwnProperty.call(flat,key)) flat[key] += effect.value * factor;
    } else if (effect.type === 'stat_percent') {
      const keys = effect.key === 'fifth' ? ['great_way','faith'] : [effect.key];
      for (const key of keys) if (Object.prototype.hasOwnProperty.call(pct,key)) pct[key] += effect.value * factor;
    } else if (effect.type === 'all_attributes_percent') {
      allPct += effect.value * factor;
    } else if (effect.type === 'element_affinity' && ELEMENTS.includes(effect.key)) {
      affinity[effect.key] += effect.value * factor;
    }
  }
  return {flat,pct,all_attributes_percent:allPct,element_affinity:affinity};
}

function mergePassiveSummaryInto(baseFlat, basePct, affinityMap, summary) {
  if (!summary) return 0;
  for (const key of Object.keys(baseFlat)) {
    baseFlat[key] += Number(summary.flat && summary.flat[key]) || 0;
    basePct[key] += Number(summary.pct && summary.pct[key]) || 0;
  }
  if (affinityMap) {
    for (const element of ELEMENTS) affinityMap[element] += Number(summary.element_affinity && summary.element_affinity[element]) || 0;
  }
  return Number(summary.all_attributes_percent) || 0;
}

function calculateEntityEffectStats(attributesValue, affinityValue, effectsValue) {
  const attributes = attributesValue && typeof attributesValue === 'object' && !Array.isArray(attributesValue)
    ? {...attributesValue}
    : {};
  const baseAffinity = normalizeElementAffinityModifier(
    affinityValue && typeof affinityValue === 'object' && !Array.isArray(affinityValue)
      ? affinityValue
      : {}
  );
  const flat = {strength:0,agility:0,constitution:0,spirit:0,luck:0,great_way:0,faith:0};
  const pct = {strength:0,agility:0,constitution:0,spirit:0,luck:0,great_way:0,faith:0};
  const affinity = Object.fromEntries(ELEMENTS.map(element => [element,0]));
  const summary = passiveModifierSummaryFromEffects(effectsValue || []);
  const allPct = mergePassiveSummaryInto(flat,pct,affinity,summary);
  const finalAttributes = {};
  for (const key of ['strength','agility','constitution','spirit','luck','great_way','faith']) {
    const base = Number(attributes[key]) || 0;
    finalAttributes[key] = Math.floor((base + flat[key]) * (1 + (allPct + pct[key]) / 100));
  }
  const finalAffinity = {};
  const elementDamageBonusPct = {};
  for (const element of ELEMENTS) {
    finalAffinity[element] = Math.round((baseAffinity[element] + (Number(affinity[element]) || 0)) * 100) / 100;
    elementDamageBonusPct[element] = Math.round(finalAffinity[element] * ELEMENT_AFFINITY_DAMAGE_PER_POINT * 100) / 100;
  }
  return {
    attributes,
    base_element_affinity: baseAffinity,
    final_attributes: finalAttributes,
    element_affinity: finalAffinity,
    element_damage_bonus_pct: elementDamageBonusPct,
    effects: effectContainerWithList(effectsValue, normalizeUnifiedEffects(effectsValue))
  };
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

  const ownedEquipment = normalizeGearList(character.equipment);
  const ownedWeapons = normalizeGearList(character.weapons);
  const rawLoadout = parseJsonField(character.gear_loadout, {equipment:[], weapons:[]});
  const loadoutEquipment = Array.isArray(rawLoadout && rawLoadout.equipment) ? rawLoadout.equipment.map(String) : [];
  const loadoutWeapons = Array.isArray(rawLoadout && rawLoadout.weapons) ? rawLoadout.weapons.map(String) : [];
  const pickOwned = (owned, names, fallbackAll) => {
    if (!names.length && fallbackAll) return [...owned];
    const remaining = [...names];
    const selected = [];
    for (const item of owned) {
      const itemName = String(item && typeof item === 'object' ? (item.name || item.title || '') : item || '');
      const idx = remaining.findIndex(name => name === itemName);
      if (idx >= 0) {
        selected.push(item);
        remaining.splice(idx, 1);
      }
    }
    return selected;
  };
  const hasExplicitLoadout = Boolean(rawLoadout && rawLoadout.configured);
  const selectedEquipment = pickOwned(ownedEquipment, loadoutEquipment, !hasExplicitLoadout);
  const selectedWeapons = pickOwned(ownedWeapons, loadoutWeapons, !hasExplicitLoadout);
  const blockedEquipment = [];
  const blockedWeapons = [];
  const equippedEquipment = selectedEquipment.filter(gear => {
    const result = gearRequirementResult(character, gear);
    if (!result.ok) blockedEquipment.push({name:gear.name, reason:result.reason});
    return result.ok;
  });
  const equippedWeapons = selectedWeapons.filter(gear => {
    const result = gearRequirementResult(character, gear);
    if (!result.ok) blockedWeapons.push({name:gear.name, reason:result.reason});
    return result.ok;
  });
  const equipment = [...equippedEquipment, ...equippedWeapons];
  const activeSetBonuses = activeGearSetBonuses(character, equipment);

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

    const itemEffectSummary = passiveModifierSummaryFromEffects(
      item.effects || item.effect_list || (item.data && item.data.effects) || []
    );
    allPct += mergePassiveSummaryInto(flat,pct,null,itemEffectSummary);
  }

  /* v18：達成套裝件數後，套裝效果也使用統一效果引擎。 */
  for (const bonus of activeSetBonuses) {
    const summary = passiveModifierSummaryFromEffects(bonus.effects || []);
    allPct += mergePassiveSummaryInto(flat,pct,null,summary);
  }

  /* 被動技能也可直接提供統一效果，不需要再另外寫一套屬性公式。 */
  for (const skill of Array.isArray(character.skills) ? character.skills : []) {
    const data = parseJsonField(skill && skill.data, {});
    const skillEffectSummary = passiveModifierSummaryFromEffects(data.effects || []);
    allPct += mergePassiveSummaryInto(flat,pct,null,skillEffectSummary);
  }

  /* v17：職業與血脈共用同一套統一效果。 */
  const professionElementAffinityModifiers = Object.fromEntries(ELEMENTS.map(element => [element, 0]));
  const bloodlineElementAffinityModifiers = Object.fromEntries(ELEMENTS.map(element => [element, 0]));
  const professionEffectSummary = passiveModifierSummaryFromEffects(character.profession_effects || []);
  const bloodlineEffectSummary = passiveModifierSummaryFromEffects(character.bloodline_effects || []);
  const identityElementAffinityModifiers = Object.fromEntries(ELEMENTS.map(element => [element, 0]));
  const identityEffectSummary = passiveModifierSummaryFromEffects(
    faction === '西國' ? (character.faith_effects || []) : (character.great_way_effects || [])
  );
  allPct += mergePassiveSummaryInto(flat,pct,professionElementAffinityModifiers,professionEffectSummary);
  allPct += mergePassiveSummaryInto(flat,pct,bloodlineElementAffinityModifiers,bloodlineEffectSummary);
  allPct += mergePassiveSummaryInto(flat,pct,identityElementAffinityModifiers,identityEffectSummary);

  const activeStatuses = Array.isArray(character.active_statuses) ? character.active_statuses : [];
  const statusElementAffinityModifiers = Object.fromEntries(ELEMENTS.map(element => [element, 0]));

  for (const status of activeStatuses) {
    if (!status || status.active === false) continue;
    const stacks = Math.max(1, Number(status.stacks) || 1);
    const summary = passiveModifierSummaryFromEffects(parseJsonField(status.effects, {}), stacks);
    allPct += mergePassiveSummaryInto(flat,pct,statusElementAffinityModifiers,summary);
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

  const baseElementAffinity = {
    暗: num(character.affinity_dark),
    光: num(character.affinity_light),
    金: num(character.affinity_metal),
    木: num(character.affinity_wood),
    水: num(character.affinity_water),
    火: num(character.affinity_fire),
    土: num(character.affinity_earth)
  };

  const skillElementAffinityModifiers = Object.fromEntries(
    ELEMENTS.map(element => [element, 0])
  );

  for (const skill of Array.isArray(character.skills) ? character.skills : []) {
    const data = parseJsonField(skill && skill.data, {});
    const modifiers = normalizeElementAffinityModifier(
      data && data.element_affinity_modifiers
    );
    for (const element of ELEMENTS) {
      skillElementAffinityModifiers[element] += modifiers[element];
    }
    const unified = passiveModifierSummaryFromEffects(data.effects || []);
    for (const element of ELEMENTS) {
      skillElementAffinityModifiers[element] += Number(unified.element_affinity[element]) || 0;
    }
  }

  const equipmentElementAffinityModifiers = Object.fromEntries(
    ELEMENTS.map(element => [element, 0])
  );

  for (const item of equipment) {
    if (!item || typeof item !== 'object') continue;
    const modifiers = normalizeElementAffinityModifier(
      item.element_affinity || item.affinity || {}
    );
    for (const element of ELEMENTS) {
      equipmentElementAffinityModifiers[element] += modifiers[element];
    }
    const unified = passiveModifierSummaryFromEffects(
      item.effects || item.effect_list || (item.data && item.data.effects) || []
    );
    for (const element of ELEMENTS) {
      equipmentElementAffinityModifiers[element] += Number(unified.element_affinity[element]) || 0;
    }
  }

  for (const bonus of activeSetBonuses) {
    const unified = passiveModifierSummaryFromEffects(bonus.effects || []);
    for (const element of ELEMENTS) {
      equipmentElementAffinityModifiers[element] += Number(unified.element_affinity[element]) || 0;
    }
  }

  const elementAffinity = {};
  const elementDamageBonusPct = {};
  const elementDamageMultiplier = {};

  for (const element of ELEMENTS) {
    const total =
      baseElementAffinity[element] +
      skillElementAffinityModifiers[element] +
      equipmentElementAffinityModifiers[element] +
      professionElementAffinityModifiers[element] +
      bloodlineElementAffinityModifiers[element] +
      identityElementAffinityModifiers[element] +
      statusElementAffinityModifiers[element];

    elementAffinity[element] = Math.round(total * 100) / 100;
    elementDamageBonusPct[element] =
      Math.round(elementAffinity[element] * ELEMENT_AFFINITY_DAMAGE_PER_POINT * 100) / 100;
    elementDamageMultiplier[element] =
      Math.max(0, Math.round((1 + elementDamageBonusPct[element] / 100) * 10000) / 10000);
  }

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
    base_element_affinity: baseElementAffinity,
    skill_element_affinity_modifiers: skillElementAffinityModifiers,
    equipment_element_affinity_modifiers: equipmentElementAffinityModifiers,
    profession_element_affinity_modifiers: professionElementAffinityModifiers,
    bloodline_element_affinity_modifiers: bloodlineElementAffinityModifiers,
    identity_element_affinity_modifiers: identityElementAffinityModifiers,
    status_element_affinity_modifiers: statusElementAffinityModifiers,
    equipped_equipment: equippedEquipment,
    equipped_weapons: equippedWeapons,
    blocked_equipment: blockedEquipment,
    blocked_weapons: blockedWeapons,
    active_set_bonuses: activeSetBonuses,
    equipment_granted_skill_template_ids: gearGrantedSkillTemplateIds({equipped_equipment:equippedEquipment,equipped_weapons:equippedWeapons}),
    element_affinity: elementAffinity,
    element_affinity_damage_per_point: ELEMENT_AFFINITY_DAMAGE_PER_POINT,
    element_damage_bonus_pct: elementDamageBonusPct,
    element_damage_multiplier: elementDamageMultiplier,

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

    inventory:
      parseJsonField(
        character.inventory,
        []
      ),

    game_coin_unlocked:
      Boolean(
        character.game_coin_unlocked
      ),

    game_room_unlocked:
      Boolean(
        character.game_room_unlocked
      ),

    bloodline:
      String(
        character.bloodline || ''
      ),

    bloodline_effects:
      effectContainerWithList(
        parseJsonField(character.bloodline_effects, {}),
        normalizeUnifiedEffects(parseJsonField(character.bloodline_effects, {}))
      ),

    profession_effects:
      effectContainerWithList(
        parseJsonField(character.profession_effects, {}),
        normalizeUnifiedEffects(parseJsonField(character.profession_effects, {}))
      ),

    great_way_effects:
      effectContainerWithList(
        parseJsonField(character.great_way_effects, {}),
        normalizeUnifiedEffects(parseJsonField(character.great_way_effects, {}))
      ),

    faith_effects:
      effectContainerWithList(
        parseJsonField(character.faith_effects, {}),
        normalizeUnifiedEffects(parseJsonField(character.faith_effects, {}))
      ),

    self_intro:
      String(
        character.self_intro || ''
      ),

    name_locked:
      Boolean(character.name_locked),

    backpack_bonus_applied:
      Boolean(character.backpack_bonus_applied),

    gear_loadout:
      parseJsonField(character.gear_loadout, {equipment:[], weapons:[]}),

    active_statuses:
      Array.isArray(character.active_statuses) ? character.active_statuses : [],

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
      ),

    profession_progress:
      normalizeProfessionProgress(
        character.profession_progress
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


const PROFESSION_RANKS = [
  'G','F','E','D','C','B','A','S','SS','SSS'
];

function normalizeProfessionRank(value, fallback = 'G') {
  const rank = String(value || fallback).toUpperCase();
  return PROFESSION_RANKS.includes(rank) ? rank : fallback;
}

function nextProfessionRank(rank) {
  const index = PROFESSION_RANKS.indexOf(normalizeProfessionRank(rank));
  if (index < 0 || index >= PROFESSION_RANKS.length - 1) return null;
  return PROFESSION_RANKS[index + 1];
}

function normalizeProfessionProgress(value) {
  const source = parseJsonField(value, {});
  if (!source || typeof source !== 'object' || Array.isArray(source)) return {};

  const result = {};
  for (const [key, raw] of Object.entries(source)) {
    if (!raw || typeof raw !== 'object' || Array.isArray(raw)) continue;
    const professionId = Math.max(0, Number(raw.profession_id || key) || 0);
    const safeKey = professionId ? String(professionId) : String(key || '').trim();
    if (!safeKey) continue;
    const rank = normalizeProfessionRank(raw.rank, 'G');
    const level = Math.max(1, Math.min(2, Math.floor(Number(raw.level) || 1)));
    result[safeKey] = {
      ...raw,
      profession_id: professionId || raw.profession_id || null,
      profession_name: String(raw.profession_name || raw.name || '').slice(0, 100),
      role: raw.role === 'main' ? 'main' : raw.role === 'sub' ? 'sub' : String(raw.role || ''),
      rank,
      level,
      active: Boolean(raw.active),
      promotion_ready: Boolean(raw.promotion_ready || (level >= 2 && nextProfessionRank(rank)))
    };
  }
  return result;
}

function normalizeProfessionAutoSkills(value) {
  const list = Array.isArray(value) ? value : [];
  return list
    .map(raw => {
      const item = raw && typeof raw === 'object' && !Array.isArray(raw) ? raw : {};
      const skillTemplateId = Math.max(0, Number(item.skill_template_id || item.skill_id) || 0);
      if (!skillTemplateId) return null;
      return {
        rank: normalizeProfessionRank(item.rank, 'G'),
        level: Math.max(1, Math.min(2, Math.floor(Number(item.level) || 1))),
        skill_template_id: skillTemplateId
      };
    })
    .filter(Boolean)
    .slice(0, 100);
}

function normalizeSkillLearnCost(value) {
  const source = value && typeof value === 'object' && !Array.isArray(value) ? value : {};
  const mode = source.mode === 'cost' ? 'cost' : 'reward_only';
  return {
    mode,
    item_name: String(source.item_name || '').trim().slice(0, 120),
    item_quantity: Math.max(1, Math.floor(Number(source.item_quantity) || 1)),
    weird_coins: Math.max(0, Math.floor(Number(source.weird_coins) || 0)),
    game_coins: Math.max(0, Math.floor(Number(source.game_coins) || 0)),
    skill_points: Math.max(0, Math.floor(Number(source.skill_points) || 0)),
    required_profession_id: Math.max(0, Number(source.required_profession_id) || 0),
    required_profession_rank: normalizeProfessionRank(source.required_profession_rank, 'G'),
    required_profession_level: Math.max(1, Math.min(2, Math.floor(Number(source.required_profession_level) || 1)))
  };
}

function skillLearnCostHasPayment(cost) {
  const c = normalizeSkillLearnCost(cost);
  return Boolean(c.item_name || c.weird_coins > 0 || c.game_coins > 0 || c.skill_points > 0);
}

function buildCharacterSkillFromTemplate(template, acquisition) {
  const data = parseJsonField(template.data, {});
  return {
    id: 'template_' + template.id + '_' + Date.now() + '_' + Math.random().toString(36).slice(2, 8),
    template_id: template.id,
    name: template.name,
    category: template.category,
    type: template.skill_type,
    level: Math.max(1, Number(template.level) || 1),
    description: template.description || '',
    check: template.check_name || '',
    damage_formula: template.damage_formula || '',
    extra_tag: template.extra_tag || '',
    summon_tag: Boolean(template.summon_tag),
    resources: parseJsonField(template.resources, {}),
    data,
    acquisition: acquisition && typeof acquisition === 'object' ? acquisition : {type:'system'}
  };
}

function professionMilestoneReached(progress, rule) {
  const currentRankIndex = PROFESSION_RANKS.indexOf(normalizeProfessionRank(progress.rank, 'G'));
  const ruleRankIndex = PROFESSION_RANKS.indexOf(normalizeProfessionRank(rule.rank, 'G'));
  if (ruleRankIndex < currentRankIndex) return true;
  if (ruleRankIndex > currentRankIndex) return false;
  return Math.max(1, Number(rule.level) || 1) <= Math.max(1, Number(progress.level) || 1);
}

async function grantSkillTemplateIdsToCharacterWithClient(client, character, templateIds, acquisition) {
  const ids = [...new Set((Array.isArray(templateIds) ? templateIds : []).map(Number).filter(Boolean))];
  const skills = Array.isArray(character.skills) ? [...character.skills] : [];
  const existingIds = new Set(skills.map(skill => String(skill && skill.template_id || '')));
  const granted = [];
  let gameCoinUnlocked = Boolean(character.game_coin_unlocked);

  for (const id of ids) {
    if (existingIds.has(String(id))) continue;
    const result = await client.query('SELECT * FROM skill_templates WHERE id = $1', [id]);
    const template = result.rows[0];
    if (!template) continue;
    const skill = buildCharacterSkillFromTemplate(template, acquisition);
    skills.push(skill);
    existingIds.add(String(id));
    granted.push({id: template.id, name: template.name});
    const data = parseJsonField(template.data, {});
    if (data.unlock_game_coin) gameCoinUnlocked = true;
    if (data.unlock_inventory_expansion || String(template.name || '').trim() === '玩家背包') {
      if (!character.backpack_bonus_applied) {
        character.inventory_slots = Math.max(5, Math.floor(Number(character.inventory_slots) || 5)) + 20;
        character.backpack_bonus_applied = true;
      }
    }
  }

  character.skills = skills;
  character.game_coin_unlocked = gameCoinUnlocked;
  return granted;
}

async function resolveProfessionTemplateWithClient(client, professionId) {
  const id = Math.max(0, Number(professionId) || 0);
  if (!id) return null;
  const result = await client.query('SELECT * FROM profession_templates WHERE id = $1', [id]);
  const row = result.rows[0];
  if (!row) return null;
  return {...row, config: parseJsonField(row.config, {})};
}

async function ensureCharacterProfessionEntryWithClient(client, character, professionId) {
  const profession = await resolveProfessionTemplateWithClient(client, professionId);
  if (!profession) throw Object.assign(new Error('找不到職業資料'), {status:404});

  const progress = normalizeProfessionProgress(character.profession_progress);
  const key = String(profession.id);
  const subs = Array.isArray(character.sub_classes) ? character.sub_classes : [];
  const activeRole = character.main_class === profession.name
    ? 'main'
    : subs.includes(profession.name)
      ? 'sub'
      : null;

  let entry = progress[key];
  if (!entry && activeRole) {
    entry = {
      profession_id: profession.id,
      profession_name: profession.name,
      role: activeRole,
      rank: normalizeProfessionRank(profession.rank, 'G'),
      level: 1,
      active: true,
      promotion_ready: false
    };
    progress[key] = entry;
  }

  if (!entry) throw Object.assign(new Error('角色目前沒有這個職業'), {status:400});
  entry.profession_name = profession.name;
  entry.active = Boolean(activeRole);
  if (activeRole) entry.role = activeRole;
  entry.rank = normalizeProfessionRank(entry.rank, profession.rank || 'G');
  entry.level = Math.max(1, Math.min(2, Number(entry.level) || 1));
  entry.promotion_ready = Boolean(entry.level >= 2 && nextProfessionRank(entry.rank));
  progress[key] = entry;
  character.profession_progress = progress;

  return {profession, progress, entry};
}

async function applyProfessionAutoSkillsWithClient(client, character, profession, entry) {
  const rules = normalizeProfessionAutoSkills(profession.config && profession.config.auto_skills);
  const ids = rules
    .filter(rule => professionMilestoneReached(entry, rule))
    .map(rule => rule.skill_template_id);

  return grantSkillTemplateIdsToCharacterWithClient(
    client,
    character,
    ids,
    {
      type: 'profession',
      profession_id: profession.id,
      profession_name: profession.name,
      rank: entry.rank,
      level: entry.level
    }
  );
}

async function levelUpProfessionOnCharacterWithClient(client, character, professionId) {
  const {profession, progress, entry} = await ensureCharacterProfessionEntryWithClient(client, character, professionId);
  if (!entry.active) throw Object.assign(new Error('只能升級目前裝備中的主／副職業'), {status:400});
  if (entry.level >= 2) {
    throw Object.assign(new Error('此職業已達本階 Lv.2，必須完成職業升階任務才能繼續'), {status:400});
  }

  entry.level += 1;
  entry.promotion_ready = Boolean(entry.level >= 2 && nextProfessionRank(entry.rank));
  progress[String(profession.id)] = entry;
  character.profession_progress = progress;
  const granted = await applyProfessionAutoSkillsWithClient(client, character, profession, entry);
  return {profession, entry, granted};
}

async function promoteProfessionOnCharacterWithClient(client, character, professionId, expectedRank) {
  const {profession, progress, entry} = await ensureCharacterProfessionEntryWithClient(client, character, professionId);
  if (!entry.active) throw Object.assign(new Error('只能升階目前裝備中的主／副職業'), {status:400});
  if (entry.level < 2) throw Object.assign(new Error('職業必須先升到本階 Lv.2 才能完成升階任務'), {status:400});
  if (expectedRank && normalizeProfessionRank(expectedRank) !== entry.rank) {
    throw Object.assign(new Error(`這張升階任務是 ${expectedRank} 階任務，但角色目前是 ${entry.rank} 階`), {status:400});
  }
  const nextRank = nextProfessionRank(entry.rank);
  if (!nextRank) throw Object.assign(new Error('此職業已經是最高 SSS 階'), {status:400});

  const fromRank = entry.rank;
  entry.rank = nextRank;
  entry.level = 1;
  entry.promotion_ready = false;
  progress[String(profession.id)] = entry;
  character.profession_progress = progress;
  const granted = await applyProfessionAutoSkillsWithClient(client, character, profession, entry);
  return {profession, entry, from_rank: fromRank, to_rank: nextRank, granted};
}

async function grantProfessionToCharacterWithClient(client, character, professionId, role) {
  const profession = await resolveProfessionTemplateWithClient(client, professionId);
  if (!profession) throw Object.assign(new Error('找不到任務獎勵指定的職業'), {status:404});
  const targetRole = role === 'main' ? 'main' : 'sub';

  if (targetRole === 'main') {
    if (!['main','both'].includes(profession.category)) {
      throw Object.assign(new Error(`職業「${profession.name}」不能作為主職業`), {status:400});
    }
    character.main_class = profession.name;
  } else {
    if (!['sub','both'].includes(profession.category)) {
      throw Object.assign(new Error(`職業「${profession.name}」不能作為副職業`), {status:400});
    }
    const subs = Array.isArray(character.sub_classes) ? [...character.sub_classes] : [];
    if (!subs.includes(profession.name)) {
      if (subs.length >= 3) throw Object.assign(new Error('副職業已滿 3 個，無法發放新的副職業'), {status:400});
      subs.push(profession.name);
    }
    character.sub_classes = subs;
  }

  const progress = normalizeProfessionProgress(character.profession_progress);
  if (targetRole === 'main') {
    for (const existing of Object.values(progress)) {
      if (existing && existing.role === 'main') existing.active = false;
    }
  }
  const key = String(profession.id);
  const current = progress[key] || {};
  const rank = normalizeProfessionRank(current.rank, profession.rank || 'G');
  const level = Math.max(1, Math.min(2, Number(current.level) || 1));
  progress[key] = {
    ...current,
    profession_id: profession.id,
    profession_name: profession.name,
    role: targetRole,
    rank,
    level,
    active: true,
    promotion_ready: Boolean(level >= 2 && nextProfessionRank(rank))
  };
  character.profession_progress = progress;
  const granted = await applyProfessionAutoSkillsWithClient(client, character, profession, progress[key]);
  return {profession, entry: progress[key], granted};
}

async function persistCharacterProgressWithClient(client, character, userId) {
  const progress = normalizeProfessionProgress(character.profession_progress);
  let mainProfessionRank = character.profession_rank || 'G';
  for (const entry of Object.values(progress)) {
    if (entry.active && entry.role === 'main') {
      mainProfessionRank = entry.rank;
      break;
    }
  }

  await client.query(
    `UPDATE character_cards
     SET main_class=$1,
         sub_classes=$2::jsonb,
         profession_progress=$3::jsonb,
         profession_rank=$4,
         skills=$5::jsonb,
         game_coin_unlocked=$6,
         inventory_slots=$7,
         backpack_bonus_applied=$8,
         updated_at=CURRENT_TIMESTAMP
     WHERE user_id=$9`,
    [
      character.main_class || '未設定',
      JSON.stringify(Array.isArray(character.sub_classes) ? character.sub_classes : []),
      JSON.stringify(progress),
      mainProfessionRank,
      JSON.stringify(Array.isArray(character.skills) ? character.skills : []),
      Boolean(character.game_coin_unlocked),
      Math.max(5, Math.floor(Number(character.inventory_slots) || 5)),
      Boolean(character.backpack_bonus_applied),
      userId
    ]
  );
}

async function syncCharacterProfessionProgress(userId, rawCharacter) {
  const character = parseCharacter(rawCharacter || await get(`SELECT * FROM character_cards WHERE user_id = ?`, [userId]));
  if (!character) return {progress:{}, skills:[], game_coin_unlocked:false};

  const professions = await all(`SELECT * FROM profession_templates ORDER BY id ASC`);
  const byName = new Map(professions.map(row => [row.name, {...row, config:parseJsonField(row.config,{})}]));
  const progress = normalizeProfessionProgress(character.profession_progress);

  for (const entry of Object.values(progress)) entry.active = false;

  const activeNames = [];
  const mainName = String(character.main_class || '').trim();
  if (mainName && mainName !== '未設定') activeNames.push({name:mainName, role:'main'});
  for (const name of (Array.isArray(character.sub_classes) ? character.sub_classes : [])) {
    if (name) activeNames.push({name:String(name), role:'sub'});
  }

  const client = await pool.connect();
  try {
    await client.query('BEGIN');
    for (const selected of activeNames) {
      const profession = byName.get(selected.name);
      if (!profession) continue;
      const key = String(profession.id);
      const current = progress[key] || {};
      const rank = normalizeProfessionRank(current.rank, profession.rank || 'G');
      const level = Math.max(1, Math.min(2, Number(current.level) || 1));
      progress[key] = {
        ...current,
        profession_id: profession.id,
        profession_name: profession.name,
        role: selected.role,
        rank,
        level,
        active: true,
        promotion_ready: Boolean(level >= 2 && nextProfessionRank(rank))
      };
      character.profession_progress = progress;
      await applyProfessionAutoSkillsWithClient(client, character, profession, progress[key]);
    }

    character.profession_progress = progress;
    await persistCharacterProgressWithClient(client, character, userId);
    await client.query('COMMIT');
  } catch (error) {
    await client.query('ROLLBACK').catch(()=>{});
    throw error;
  } finally {
    client.release();
  }

  return {
    progress: normalizeProfessionProgress(character.profession_progress),
    skills: Array.isArray(character.skills) ? character.skills : [],
    game_coin_unlocked: Boolean(character.game_coin_unlocked)
  };
}

async function syncAllCharacterProfessionProgress() {
  const rows = await all(`SELECT * FROM character_cards ORDER BY id ASC`);
  for (const row of rows) {
    await syncCharacterProfessionProgress(row.user_id, row);
  }
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

async function syncCharacterSourceEffects(userId, rawCharacter) {
  const character = rawCharacter || await get(`SELECT * FROM character_cards WHERE user_id = ?`, [userId]);
  if (!character) return {profession_effects:{list:[]},bloodline_effects:{list:[]},great_way_effects:{list:[]},faith_effects:{list:[]}};

  const names = [];
  const main = String(character.main_class || '').trim();
  if (main && main !== '未設定') names.push(main);
  for (const value of parseJsonField(character.sub_classes, [])) {
    const name = String(value || '').trim();
    if (name) names.push(name);
  }

  const professionList = [];
  if (names.length) {
    const rows = await all(`SELECT name,config FROM profession_templates WHERE name = ANY(?::text[])`, [[...new Set(names)]]);
    const byName = new Map(rows.map(row => [row.name, row]));
    for (const name of names) {
      const row = byName.get(name);
      if (!row) continue;
      const config = parseJsonField(row.config, {});
      professionList.push(...normalizeUnifiedEffects(config.effects || []));
    }
  }

  const bloodlineName = String(character.bloodline || '').trim();
  let bloodlineList = [];
  if (bloodlineName) {
    try {
      const row = await get(`SELECT effects FROM bloodline_templates WHERE name = ?`, [bloodlineName]);
      if (row) bloodlineList = normalizeUnifiedEffects(parseJsonField(row.effects, {}));
    } catch (_) { bloodlineList = []; }
  }

  let greatWayRows=[];
  let faithRows=[];
  try{
    greatWayRows=await all(`SELECT cgw.rank,cgw.progress,cgw.active,gwt.id,gwt.name,gwt.effects FROM character_great_ways cgw JOIN great_way_templates gwt ON gwt.id=cgw.great_way_id WHERE cgw.user_id=? AND cgw.active=TRUE AND gwt.active=TRUE ORDER BY cgw.created_at,gwt.id`,[userId]);
    faithRows=await all(`SELECT cf.rank,cf.devotion,cf.favor,cf.active,fd.id,fd.name,fd.effects FROM character_faiths cf JOIN faith_deities fd ON fd.id=cf.deity_id WHERE cf.user_id=? AND cf.active=TRUE AND fd.active=TRUE ORDER BY cf.created_at,fd.id`,[userId]);
  }catch(_){ greatWayRows=[];faithRows=[]; }
  const greatWayList=[];for(const row of greatWayRows)greatWayList.push(...normalizeUnifiedEffects(parseJsonField(row.effects,{})));
  const faithList=[];for(const row of faithRows)faithList.push(...normalizeUnifiedEffects(parseJsonField(row.effects,{})));

  const mergeIdentity=(legacy,rows,kind)=>{
    const out=normalizeIdentityEntries(parseJsonField(legacy,[])).filter(item=>kind==='faith'?!item.deity_id:!item.great_way_id);
    const names=new Set(out.map(x=>String(x.name||'').trim()));
    for(const row of rows){if(names.has(row.name))continue;out.push(kind==='faith'?{name:row.name,rank:normalizeProfessionRank(row.rank,'G'),deity_id:row.id}:{name:row.name,rank:normalizeProfessionRank(row.rank,'G'),great_way_id:row.id});names.add(row.name);}
    return out.slice(0,10);
  };
  const greatWays=mergeIdentity(character.great_ways,greatWayRows,'great_way');
  const faiths=mergeIdentity(character.faiths,faithRows,'faith');

  const professionEffects = effectContainerWithList({}, professionList);
  const bloodlineEffects = effectContainerWithList({}, bloodlineList);
  const greatWayEffects = effectContainerWithList({}, greatWayList);
  const faithEffects = effectContainerWithList({}, faithList);
  await run(
    `UPDATE character_cards SET profession_effects=?::jsonb,bloodline_effects=?::jsonb,great_way_effects=?::jsonb,faith_effects=?::jsonb,great_ways=?::jsonb,faiths=?::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=?`,
    [JSON.stringify(professionEffects),JSON.stringify(bloodlineEffects),JSON.stringify(greatWayEffects),JSON.stringify(faithEffects),JSON.stringify(greatWays),JSON.stringify(faiths),userId]
  );
  return {profession_effects:professionEffects,bloodline_effects:bloodlineEffects,great_way_effects:greatWayEffects,faith_effects:faithEffects,great_ways:greatWays,faiths};
}

async function syncAllCharacterSourceEffects() {
  const rows = await all(`SELECT * FROM character_cards ORDER BY id ASC`);
  for (const row of rows) await syncCharacterSourceEffects(row.user_id, row);
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

  const professionSync =
    await syncCharacterProfessionProgress(
      userId,
      character
    );

  const sourceEffects = await syncCharacterSourceEffects(userId, {
    ...character,
    skills: professionSync.skills,
    profession_progress: professionSync.progress
  });

  let statuses = [];
  try {
    statuses = await all(
      `SELECT id,status_template_id,name,description,remaining_rounds,effects,source,active,stacks,stack_mode,max_stacks,dispellable,expires_by_round,created_at,updated_at
       FROM character_statuses WHERE user_id=? AND active=TRUE ORDER BY id ASC`,
      [userId]
    );
  } catch (_) {
    statuses = [];
  }

  return parseCharacter({
    ...character,
    class_resources:
      classResources,
    profession_progress:
      professionSync.progress,
    skills:
      professionSync.skills,
    game_coin_unlocked:
      professionSync.game_coin_unlocked,
    profession_effects: sourceEffects.profession_effects,
    bloodline_effects: sourceEffects.bloodline_effects,
    great_way_effects: sourceEffects.great_way_effects,
    faith_effects: sourceEffects.faith_effects,
    great_ways: sourceEffects.great_ways || parseJsonField(character.great_ways,[]),
    faiths: sourceEffects.faiths || parseJsonField(character.faiths,[]),
    active_statuses: statuses.map(row => ({...row,effects:parseJsonField(row.effects,{})}))
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
          closed_at,
          map_name,
          map_image_url,
          map_description,
          game_day,
          game_hour,
          game_minute,
          time_hidden,
          battle_active,
          current_actor_type,
          current_actor_ref_id,
          combat_order,
          combat_index
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
          rm.current_sanity,
          rm.max_sanity,
          rm.sanity_initialized,
          rm.sanity_stage,
          rm.seven_sin,
          rm.joined_at,
          rm.turn_actions_remaining,
          rm.skill_charge_state,
          rm.mounted_summon_id,
          rm.combat_reaction,
          rm.combat_reaction_skill_id,
          rm.combat_reaction_round,
          rm.combat_reaction_uses,
          rm.combat_state,

          u.username,
          u.is_admin,

          crb.character_id,
          cc.class_resources,
          cc.pollution

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

  for (const member of members) {
    if (member.role === 'player') {
      const sanity = await ensureRoomMemberSanity(roomId, member.id);
      if (sanity) Object.assign(member, sanity);
    }
  }

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
          st.loyalty,
          st.attributes,
          st.element_affinity,
          st.traits,
          st.skills,
          st.effects,
          st.config

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

  const publicRoom = {
    ...room,
    // V26：真正遊戲時間不再透過一般房間快照廣播。
    // 玩家／DM 改由 /api/rooms/:id/time-view 取得各自可見的時間。
    game_day: null,
    game_hour: null,
    game_minute: null
  };

  return {
    room: publicRoom,

    members:
      members.map(
        function (member) {
          return {
            ...member,
            class_resources:
              parseJsonField(
                member.class_resources,
                {}
              ),
            skill_charge_state:
              parseJsonField(
                member.skill_charge_state,
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
              ),
            ...calculateEntityEffectStats(
              parseJsonField(summon.attributes, {}),
              parseJsonField(summon.element_affinity, {}),
              parseJsonField(summon.effects, {})
            ),
            traits: parseJsonField(summon.traits, []),
            skills: parseJsonField(summon.skills, []),
            config: parseJsonField(summon.config, {})
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
                (function () {
                  const payload = parseJsonField(event.payload, {});
                  if (event.type === 'time') {
                    return {
                      ...payload,
                      text: '遊戲時間發生了變化。',
                      game_day: undefined,
                      game_hour: undefined,
                      game_minute: undefined
                    };
                  }
                  if (event.type === 'scene') {
                    return {
                      ...payload,
                      text: payload.map_name
                        ? `場景更新：${payload.map_name}`
                        : '場景已更新。',
                      game_day: undefined,
                      game_hour: undefined,
                      game_minute: undefined
                    };
                  }
                  return payload;
                })()
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
      const baseCharacter=await getCharacterByUser(req.user.id);
      res.json({character:await decorateCharacterInventoryKnowledge(req.user.id,baseCharacter)});
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

      const currentUser=await getUser(req.user.id);
      const createBody=currentUser&&currentUser.is_admin
        ? req.body
        : {name:req.body&&req.body.name,faction:req.body&&req.body.faction};

      await createCharacter(
        req.user.id,
        createBody
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
    'weird_coins',
    'game_coins',
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
    'innate_sub_class',
    'bloodline'
  ];

  const jsonFields = [
    'sub_classes',
    'skills',
    'equipment',
    'weapons',
    'inventory',
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

  if (body.self_intro !== undefined) {
    updates.push('self_intro = ?');
    values.push(String(body.self_intro || '').trim().slice(0,4000));
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

  if (body.skills !== undefined && Array.isArray(body.skills)) {
    const unlockGameCoin =
      body.skills.some(
        skill =>
          Boolean(
            skill &&
            skill.data &&
            typeof skill.data === 'object' &&
            !Array.isArray(skill.data) &&
            skill.data.unlock_game_coin
          )
      );

    if (unlockGameCoin) {
      updates.push('game_coin_unlocked = TRUE');
    } else if (body.game_coin_unlocked !== undefined) {
      updates.push('game_coin_unlocked = ?');
      values.push(Boolean(body.game_coin_unlocked));
    }
  } else if (body.game_coin_unlocked !== undefined) {
    updates.push('game_coin_unlocked = ?');
    values.push(Boolean(body.game_coin_unlocked));
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
  async function (req, res) {
    return res.status(403).json({error:'玩家不能直接修改大道／信仰目前值；由 DM、技能或事件處理'});
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
  async function (req, res) {
    return res.status(403).json({
      error: '不能直接免費建立技能；請透過技能資料庫支付學習代價、職業自動取得、任務獎勵或由 DM 授予'
    });
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
            game_coin_unlocked =
              CASE
                WHEN ? THEN TRUE
                ELSE game_coin_unlocked
              END,
            updated_at =
              CURRENT_TIMESTAMP
          WHERE id = ?
        `,
        [
          JSON.stringify(
            nextSkills
          ),
          Boolean(
            parseJsonField(
              template.data,
              {}
            ).unlock_game_coin
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

app.post('/api/character/professions/acquire', auth, async function(req,res){
  const client=await pool.connect();
  try{
    await client.query('BEGIN');
    const professionId=Math.max(0,Number(req.body.profession_id)||0);
    const role=req.body.role==='sub'?'sub':'main';
    const profession=await resolveProfessionTemplateWithClient(client,professionId);
    if(!profession){await client.query('ROLLBACK');return res.status(404).json({error:'找不到職業'});}
    const result=await client.query('SELECT * FROM character_cards WHERE user_id=$1 FOR UPDATE',[req.user.id]);
    const character=parseCharacter(result.rows[0]);
    if(!character){await client.query('ROLLBACK');return res.status(404).json({error:'你還沒有角色卡'});}
    const progress=normalizeProfessionProgress(character.profession_progress);
    if(progress[String(profession.id)]){await client.query('ROLLBACK');return res.status(409).json({error:'你已經擁有這個職業'});}
    if(role==='main'){
      if(!['main','both'].includes(profession.category)){await client.query('ROLLBACK');return res.status(400).json({error:'這個職業不能作為主職業'});}
      if(character.main_class && character.main_class!=='未設定'){await client.query('ROLLBACK');return res.status(400).json({error:'你已經有主職業，玩家不能自行替換職業'});}
    }else{
      if(!['sub','both'].includes(profession.category)){await client.query('ROLLBACK');return res.status(400).json({error:'這個職業不能作為副職業'});}
      if((Array.isArray(character.sub_classes)?character.sub_classes:[]).length>=3){await client.query('ROLLBACK');return res.status(400).json({error:'副職業已滿 3 個'});}
    }
    const cfg=profession.config&&typeof profession.config==='object'?profession.config:{};
    const bookName=String(cfg.book_name || `${profession.name}職業書`).trim();
    const bookQty=Math.max(1,Math.floor(Number(cfg.book_quantity)||1));
    let inventory=normalizeInventory(character.inventory);
    if(inventoryQuantity(inventory,bookName)<bookQty){await client.query('ROLLBACK');return res.status(400).json({error:`需要「${bookName}」×${bookQty} 才能取得職業「${profession.name}」`});}
    inventory=changeInventoryQuantity(inventory,bookName,-bookQty);
    character.inventory=inventory;
    const granted=await grantProfessionToCharacterWithClient(client,character,profession.id,role);
    await persistCharacterProgressWithClient(client,character,req.user.id);
    await client.query(`UPDATE character_cards SET inventory=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2`,[JSON.stringify(inventory),req.user.id]);
    await client.query('COMMIT');
    const updated=await ensureBackpackBonusForUser(req.user.id);
    res.json({ok:true,profession:{id:profession.id,name:profession.name,role,book_name:bookName,book_quantity:bookQty},granted,character:updated});
  }catch(error){await client.query('ROLLBACK').catch(()=>{});console.error('ACQUIRE PROFESSION ERROR:',error);res.status(error.status||500).json({error:error.message||'取得職業失敗'});}finally{client.release();}
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
    res.json({summon_templates:rows.map(x=>({
      ...x,
      ...calculateEntityEffectStats(parseJsonField(x.attributes,{}),parseJsonField(x.element_affinity,{}),parseJsonField(x.effects,{})),
      traits:parseJsonField(x.traits,[]),
      skills:parseJsonField(x.skills,[]),
      config:parseJsonField(x.config,{})
    }))});
  }catch(error){console.error('SUMMON TEMPLATE LIST ERROR:',error);res.status(500).json({error:'讀取召喚物資料庫失敗'});}
});


/* =========================================================
   CRAFTING / CURRENCY
========================================================= */

function normalizeInventory(value) {
  const source = Array.isArray(value) ? value : [];
  const merged = new Map();

  for (const raw of source) {
    const name = String(
      typeof raw === 'string'
        ? raw
        : raw && typeof raw === 'object'
          ? raw.name || raw.title || raw.item_name || ''
          : ''
    ).trim().slice(0, 120);

    if (!name) continue;

    const quantity = Math.max(
      1,
      Math.floor(
        Number(
          raw && typeof raw === 'object'
            ? raw.quantity ?? raw.qty ?? raw.count ?? 1
            : 1
        ) || 1
      )
    );

    const category = String(
      raw && typeof raw === 'object'
        ? raw.category || raw.item_category || raw.type_name || ''
        : ''
    ).trim().slice(0, 80);

    const key = `${name.toLocaleLowerCase()}\u0000${category.toLocaleLowerCase()}`;
    const current = merged.get(key);

    if (current) {
      current.quantity += quantity;
    } else {
      merged.set(key, {name, quantity, category});
    }
  }

  return Array.from(merged.values()).slice(0, 500);
}

function inventoryQuantity(inventory, name, category = '') {
  const key = String(name || '').trim().toLocaleLowerCase();
  const categoryKey = String(category || '').trim().toLocaleLowerCase();
  return normalizeInventory(inventory)
    .filter(entry => entry.name.toLocaleLowerCase() === key)
    .filter(entry => !categoryKey || String(entry.category || '').toLocaleLowerCase() === categoryKey)
    .reduce((sum, entry) => sum + entry.quantity, 0);
}

function changeInventoryQuantity(inventory, name, delta, category = '') {
  const safeName = String(name || '').trim().slice(0, 120);
  const safeCategory = String(category || '').trim().slice(0, 80);
  if (!safeName) return normalizeInventory(inventory);

  const list = normalizeInventory(inventory);
  const key = safeName.toLocaleLowerCase();
  const categoryKey = safeCategory.toLocaleLowerCase();
  let index = list.findIndex(item =>
    item.name.toLocaleLowerCase() === key &&
    String(item.category || '').toLocaleLowerCase() === categoryKey
  );

  // 舊資料沒有類別時，未指定類別的扣除仍可正常找到。
  if (index < 0 && !safeCategory) {
    index = list.findIndex(item => item.name.toLocaleLowerCase() === key);
  }

  const current = index >= 0 ? list[index].quantity : 0;
  const next = current + Math.floor(Number(delta) || 0);

  if (next <= 0) {
    if (index >= 0) list.splice(index, 1);
  } else if (index >= 0) {
    list[index] = {...list[index], quantity: next};
  } else {
    list.push({name: safeName, quantity: next, category: safeCategory});
  }

  return list;
}

function inventoryCategoryLabel(item) {
  return String(item && item.category || '').trim().slice(0, 80);
}

function normalizeGearEntry(raw) {
  if (typeof raw === 'string') {
    return {
      name: String(raw).trim().slice(0,120),
      consume_item_name: '',
      consume_item_category: '',
      consume_quantity: 0,
      ammo_options: [],
      grant_mode: 'none',
      granted_skill_template_ids: [],
      requirements: {},
      set_name: '',
      set_bonus_count: 0,
      set_bonus_effects: effectContainerWithList({}, []),
      set_bonus_skill_template_ids: [],
      charges_max: 0,
      charges_current: 0,
      charge_cost: 0,
      durability_max: 0,
      durability_current: 0,
      durability_cost: 0,
      skill_modifiers: [],
      effects: effectContainerWithList({}, [])
    };
  }

  const item = raw && typeof raw === 'object' && !Array.isArray(raw) ? raw : {};
  const useCost = item.use_cost && typeof item.use_cost === 'object' && !Array.isArray(item.use_cost)
    ? item.use_cost
    : {};
  const grantMode = ['none','equipped','permanent_first_equip'].includes(String(item.grant_mode || ''))
    ? String(item.grant_mode)
    : 'none';
  const requirementSource = item.requirements && typeof item.requirements === 'object' && !Array.isArray(item.requirements)
    ? item.requirements
    : {};
  const requiredStat = ['strength','agility','constitution','spirit','luck'].includes(String(requirementSource.stat || ''))
    ? String(requirementSource.stat)
    : '';
  const requirements = {
    profession_id: Math.max(0, Number(requirementSource.profession_id) || 0),
    profession_rank: normalizeProfessionRank(requirementSource.profession_rank, 'G'),
    profession_level: Math.max(1, Math.min(2, Math.floor(Number(requirementSource.profession_level) || 1))),
    bloodline: String(requirementSource.bloodline || '').trim().slice(0,120),
    stat: requiredStat,
    stat_min: Math.max(0, Math.floor(Number(requirementSource.stat_min) || 0))
  };
  const chargesMax = Math.max(0, Math.floor(Number(item.charges_max) || 0));
  const chargesCurrent = chargesMax > 0
    ? Math.max(0, Math.min(chargesMax, Math.floor(Number(item.charges_current ?? chargesMax) || 0)))
    : 0;
  const durabilityMax = Math.max(0, Math.floor(Number(item.durability_max) || 0));
  const durabilityCurrent = durabilityMax > 0
    ? Math.max(0, Math.min(durabilityMax, Math.floor(Number(item.durability_current ?? durabilityMax) || 0)))
    : 0;
  const skillModifiers = (Array.isArray(item.skill_modifiers) ? item.skill_modifiers : [])
    .map(rawModifier => {
      const modifier = rawModifier && typeof rawModifier === 'object' && !Array.isArray(rawModifier) ? rawModifier : {};
      const skillTemplateId = Math.max(0, Number(modifier.skill_template_id) || 0);
      if (!skillTemplateId) return null;
      return {
        skill_template_id: skillTemplateId,
        damage_multiplier: Math.max(0, Math.min(20, Number(modifier.damage_multiplier) || 1)),
        turn_cost_delta: Math.max(-1, Math.min(1, Math.floor(Number(modifier.turn_cost_delta) || 0))),
        charge_turns_delta: Math.max(-20, Math.min(20, Math.floor(Number(modifier.charge_turns_delta) || 0)))
      };
    })
    .filter(Boolean)
    .slice(0,100);
  const ammoOptions = (Array.isArray(item.ammo_options) ? item.ammo_options : [])
    .map(rawAmmo => {
      const ammo = rawAmmo && typeof rawAmmo === 'object' && !Array.isArray(rawAmmo) ? rawAmmo : {};
      const name = String(ammo.name || ammo.item_name || '').trim().slice(0,120);
      if (!name) return null;
      return {
        name,
        category: String(ammo.category || ammo.item_category || '').trim().slice(0,80),
        quantity: Math.max(1, Math.min(99, Math.floor(Number(ammo.quantity) || 1))),
        damage_multiplier: Math.max(0, Math.min(20, Number(ammo.damage_multiplier) || 1)),
        note: String(ammo.note || '').trim().slice(0,500)
      };
    })
    .filter(Boolean)
    .slice(0,50);
  const rawEffects = item.effects || item.effect_list || (item.data && item.data.effects) || {};
  const rawSetBonusEffects = item.set_bonus_effects || {};
  return {
    ...item,
    name: String(item.name || item.title || '未命名裝備').trim().slice(0,120),
    consume_item_name: String(item.consume_item_name || useCost.item_name || '').trim().slice(0,120),
    consume_item_category: String(item.consume_item_category || useCost.category || '').trim().slice(0,80),
    consume_quantity: Math.max(0, Math.floor(Number(item.consume_quantity ?? useCost.quantity ?? 0) || 0)),
    ammo_options: ammoOptions,
    grant_mode: grantMode,
    granted_skill_template_ids: [...new Set((Array.isArray(item.granted_skill_template_ids) ? item.granted_skill_template_ids : []).map(Number).filter(Boolean))].slice(0,100),
    requirements,
    set_name: String(item.set_name || '').trim().slice(0,120),
    set_bonus_count: Math.max(0, Math.min(20, Math.floor(Number(item.set_bonus_count) || 0))),
    set_bonus_effects: effectContainerWithList(rawSetBonusEffects, normalizeUnifiedEffects(rawSetBonusEffects)),
    set_bonus_skill_template_ids: [...new Set((Array.isArray(item.set_bonus_skill_template_ids) ? item.set_bonus_skill_template_ids : []).map(Number).filter(Boolean))].slice(0,100),
    charges_max: chargesMax,
    charges_current: chargesCurrent,
    charge_cost: chargesMax > 0 ? Math.max(0, Math.min(chargesMax, Math.floor(Number(item.charge_cost) || 1))) : 0,
    durability_max: durabilityMax,
    durability_current: durabilityCurrent,
    durability_cost: durabilityMax > 0 ? Math.max(0, Math.min(durabilityMax, Math.floor(Number(item.durability_cost) || 1))) : 0,
    skill_modifiers: skillModifiers,
    effects: effectContainerWithList(rawEffects, normalizeUnifiedEffects(rawEffects))
  };
}

function normalizeGearList(value) {
  return (Array.isArray(value) ? value : [])
    .map(normalizeGearEntry)
    .filter(item => item.name)
    .slice(0,200);
}

function gearIsBroken(gearValue) {
  const gear = normalizeGearEntry(gearValue);
  return gear.durability_max > 0 && gear.durability_current <= 0;
}

function characterProfessionEntryForRequirement(character, professionId) {
  const id = Math.max(0, Number(professionId) || 0);
  if (!id) return null;
  const progress = normalizeProfessionProgress(character && character.profession_progress);
  return progress[String(id)] || Object.values(progress).find(entry => Number(entry && entry.profession_id) === id) || null;
}

function gearRequirementResult(character, gearValue) {
  const gear = normalizeGearEntry(gearValue);
  const req = gear.requirements || {};
  if (gearIsBroken(gear)) return {ok:false, reason:'裝備耐久已歸零'};

  if (req.profession_id) {
    const entry = characterProfessionEntryForRequirement(character, req.profession_id);
    if (!entry || !entry.active) return {ok:false, reason:'未持有所需職業'};
    const haveRank = PROFESSION_RANKS.indexOf(normalizeProfessionRank(entry.rank, 'G'));
    const needRank = PROFESSION_RANKS.indexOf(normalizeProfessionRank(req.profession_rank, 'G'));
    if (haveRank < needRank) return {ok:false, reason:`職業等階不足，需要 ${req.profession_rank}`};
    if (haveRank === needRank && Math.max(1, Number(entry.level) || 1) < Math.max(1, Number(req.profession_level) || 1)) {
      return {ok:false, reason:`職業等級不足，需要 ${req.profession_rank} Lv.${req.profession_level}`};
    }
  }

  if (req.bloodline && String(character && character.bloodline || '') !== String(req.bloodline)) {
    return {ok:false, reason:`需要血脈「${req.bloodline}」`};
  }

  if (req.stat && req.stat_min > 0) {
    const have = Math.max(0, Number(character && character[req.stat]) || 0);
    if (have < req.stat_min) return {ok:false, reason:`${req.stat} 需求 ${req.stat_min}，目前 ${have}`};
  }

  return {ok:true, reason:''};
}

function activeGearSetBonuses(character, equipmentList) {
  const list = Array.isArray(equipmentList) ? equipmentList.map(normalizeGearEntry) : [];
  const counts = new Map();
  for (const gear of list) {
    if (!gear.set_name || gearIsBroken(gear)) continue;
    counts.set(gear.set_name, (counts.get(gear.set_name) || 0) + 1);
  }
  const result = [];
  for (const gear of list) {
    if (!gear.set_name || !gear.set_bonus_count || gearIsBroken(gear)) continue;
    const count = counts.get(gear.set_name) || 0;
    if (count < gear.set_bonus_count) continue;
    result.push({
      source_gear: gear.name,
      set_name: gear.set_name,
      count_required: gear.set_bonus_count,
      count_equipped: count,
      effects: gear.set_bonus_effects,
      skill_template_ids: gear.set_bonus_skill_template_ids || []
    });
  }
  return result;
}

function gearGrantedSkillTemplateIds(character) {
  const equipment = [
    ...(Array.isArray(character && character.equipped_equipment) ? character.equipped_equipment : []),
    ...(Array.isArray(character && character.equipped_weapons) ? character.equipped_weapons : [])
  ].map(normalizeGearEntry).filter(gear => !gearIsBroken(gear));
  const ids = [];
  for (const gear of equipment) {
    if (gear.grant_mode === 'equipped') ids.push(...gear.granted_skill_template_ids);
  }
  for (const bonus of activeGearSetBonuses(character, equipment)) ids.push(...(bonus.skill_template_ids || []));
  return [...new Set(ids.map(Number).filter(Boolean))];
}

function equipmentSkillModifierForCharacter(character, skill) {
  const templateId = Math.max(0, Number(skill && skill.template_id) || 0);
  const result = {damage_multiplier:1, turn_cost_delta:0, charge_turns_delta:0, sources:[]};
  if (!templateId) return result;
  const equipment = [
    ...(Array.isArray(character && character.equipped_equipment) ? character.equipped_equipment : []),
    ...(Array.isArray(character && character.equipped_weapons) ? character.equipped_weapons : [])
  ].map(normalizeGearEntry).filter(gear => !gearIsBroken(gear));
  for (const gear of equipment) {
    for (const modifier of gear.skill_modifiers || []) {
      if (Number(modifier.skill_template_id) !== templateId) continue;
      result.damage_multiplier *= Math.max(0, Number(modifier.damage_multiplier) || 1);
      result.turn_cost_delta += Math.floor(Number(modifier.turn_cost_delta) || 0);
      result.charge_turns_delta += Math.floor(Number(modifier.charge_turns_delta) || 0);
      result.sources.push(gear.name);
    }
  }
  result.damage_multiplier = Math.round(result.damage_multiplier * 10000) / 10000;
  result.turn_cost_delta = Math.max(-1, Math.min(1, result.turn_cost_delta));
  result.charge_turns_delta = Math.max(-20, Math.min(20, result.charge_turns_delta));
  return result;
}

async function consumeDirectGrantingGearUseCost(userId, character, skillTemplateId, selectedAmmoRaw = null) {
  const templateId = Math.max(0, Number(skillTemplateId) || 0);
  if (!templateId) return null;
  const equippedNames = new Set([
    ...(Array.isArray(character && character.equipped_equipment) ? character.equipped_equipment : []),
    ...(Array.isArray(character && character.equipped_weapons) ? character.equipped_weapons : [])
  ].map(item => normalizeGearEntry(item).name));

  const selectedAmmo = selectedAmmoRaw && typeof selectedAmmoRaw === 'object' && !Array.isArray(selectedAmmoRaw)
    ? {
        name:String(selectedAmmoRaw.name || '').trim().slice(0,120),
        category:String(selectedAmmoRaw.category || '').trim().slice(0,80)
      }
    : null;

  const candidates = [];
  for (const candidate of [['equipment', normalizeGearList(character && character.equipment)], ['weapons', normalizeGearList(character && character.weapons)]]) {
    const [candidateKind, candidateList] = candidate;
    candidateList.forEach((item, foundIndex) => {
      if (!equippedNames.has(item.name)) return;
      if (item.grant_mode !== 'equipped') return;
      if (!(item.granted_skill_template_ids || []).map(Number).includes(templateId)) return;
      candidates.push({kind:candidateKind,index:foundIndex,gear:item,list:candidateList});
    });
  }
  if (!candidates.length) return null; // 套裝授予技能不會額外扣某一件裝備的使用成本。

  let picked = candidates[0];
  if (selectedAmmo && selectedAmmo.name) {
    const matching = candidates.find(candidate => (candidate.gear.ammo_options || []).some(option =>
      String(option.name).toLocaleLowerCase() === selectedAmmo.name.toLocaleLowerCase() &&
      (!selectedAmmo.category || String(option.category || '').toLocaleLowerCase() === selectedAmmo.category.toLocaleLowerCase())
    ));
    if (matching) picked = matching;
  }

  const {kind,index,list} = picked;
  const gear = picked.gear;
  let itemName = String(gear.consume_item_name || '').trim();
  let category = String(gear.consume_item_category || '').trim();
  let quantity = Math.max(0, Math.floor(Number(gear.consume_quantity) || 0));
  let ammo = null;

  const ammoOptions = Array.isArray(gear.ammo_options) ? gear.ammo_options : [];
  if (ammoOptions.length) {
    let option = null;
    if (selectedAmmo && selectedAmmo.name) {
      option = ammoOptions.find(entry =>
        String(entry.name).toLocaleLowerCase() === selectedAmmo.name.toLocaleLowerCase() &&
        (!selectedAmmo.category || String(entry.category || '').toLocaleLowerCase() === selectedAmmo.category.toLocaleLowerCase())
      ) || null;
      if (!option) throw Object.assign(new Error(`${gear.name} 不支援選擇的彈藥`), {status:400});
    } else if (ammoOptions.length === 1) {
      option = ammoOptions[0];
    } else {
      throw Object.assign(new Error(`${gear.name} 有多種可用彈藥，請先選擇要使用的彈藥`), {status:400});
    }
    itemName = String(option.name || '').trim();
    category = String(option.category || '').trim();
    quantity = Math.max(1, Math.floor(Number(option.quantity) || 1));
    ammo = {
      name:itemName,
      category,
      quantity,
      damage_multiplier:Math.max(0, Number(option.damage_multiplier) || 1),
      note:String(option.note || '').trim()
    };
  }

  let inventory = normalizeInventory(character.inventory);
  if (itemName && quantity > 0) {
    if (inventoryQuantity(inventory,itemName,category) < quantity) throw Object.assign(new Error(`${gear.name} 的技能需要 ${itemName} ×${quantity}`), {status:400});
    inventory = changeInventoryQuantity(inventory,itemName,-quantity,category);
  }
  const chargeCost = gear.charges_max > 0 ? Math.max(0, Math.floor(Number(gear.charge_cost) || 1)) : 0;
  if (chargeCost > 0 && gear.charges_current < chargeCost) throw Object.assign(new Error(`${gear.name} 充能不足（${gear.charges_current}/${gear.charges_max}）`), {status:400});
  const durabilityCost = gear.durability_max > 0 ? Math.max(0, Math.floor(Number(gear.durability_cost) || 1)) : 0;
  if (durabilityCost > 0 && gear.durability_current < durabilityCost) throw Object.assign(new Error(`${gear.name} 耐久不足（${gear.durability_current}/${gear.durability_max}）`), {status:400});
  if (chargeCost > 0) gear.charges_current = Math.max(0, gear.charges_current - chargeCost);
  if (durabilityCost > 0) gear.durability_current = Math.max(0, gear.durability_current - durabilityCost);
  list[index] = gear;
  await run(`UPDATE character_cards SET inventory=?, ${kind}=?::jsonb, updated_at=CURRENT_TIMESTAMP WHERE user_id=?`, [JSON.stringify(inventory), JSON.stringify(list), userId]);
  return {
    gear_name:gear.name,
    gear_kind:kind,
    consumed:itemName&&quantity>0?{name:itemName,category,quantity}:null,
    ammo,
    charge_change:chargeCost>0?{current:gear.charges_current,max:gear.charges_max,cost:chargeCost}:null,
    durability_change:durabilityCost>0?{current:gear.durability_current,max:gear.durability_max,cost:durabilityCost}:null
  };
}


function inventorySlotLimit(character) {
  return Math.max(5, Math.floor(Number(character && character.inventory_slots) || 5));
}

function inventoryUsedSlots(inventory) {
  return normalizeInventory(inventory).length;
}

function inventoryWouldNeedNewSlot(inventory, name, category = '') {
  const safeName = String(name || '').trim().toLocaleLowerCase();
  const safeCategory = String(category || '').trim().toLocaleLowerCase();
  if (!safeName) return false;
  return !normalizeInventory(inventory).some(item =>
    String(item.name || '').trim().toLocaleLowerCase() === safeName &&
    String(item.category || '').trim().toLocaleLowerCase() === safeCategory
  );
}

function ensureInventoryCanAdd(character, inventory, name, category = '') {
  if (!inventoryWouldNeedNewSlot(inventory, name, category)) return;
  const used = inventoryUsedSlots(inventory);
  const limit = inventorySlotLimit(character);
  if (used >= limit) {
    throw Object.assign(new Error(`物品欄已滿（${used}/${limit}）`), {status:400});
  }
}


async function applyStatusTemplateWithClient(client, userId, templateOrName, source = '系統', durationOverride = null) {
  let template = null;
  if (templateOrName && typeof templateOrName === 'object') {
    template = templateOrName;
  } else if (Number(templateOrName)) {
    const result = await client.query('SELECT * FROM status_templates WHERE id=$1', [Number(templateOrName)]);
    template = result.rows[0] || null;
  } else {
    const name = String(templateOrName || '').trim();
    if (name) {
      const result = await client.query('SELECT * FROM status_templates WHERE LOWER(name)=LOWER($1) ORDER BY id DESC LIMIT 1', [name]);
      template = result.rows[0] || null;
    }
  }
  if (!template) throw Object.assign(new Error('找不到要套用的狀態模板'), {status:404});

  const stackMode = ['refresh','stack','independent','replace','ignore'].includes(String(template.stack_mode || ''))
    ? String(template.stack_mode)
    : 'refresh';
  const maxStacks = Math.max(1, Math.min(99, Math.floor(Number(template.max_stacks) || 1)));
  const duration = durationOverride === null || durationOverride === undefined
    ? Math.max(0, Math.floor(Number(template.duration_rounds) || 0))
    : Math.max(0, Math.floor(Number(durationOverride) || 0));
  const effects = parseJsonField(template.effects, {});
  const dispellable = template.dispellable === undefined ? true : Boolean(template.dispellable);

  const existingResult = await client.query(
    `SELECT * FROM character_statuses WHERE user_id=$1 AND status_template_id=$2 AND active=TRUE ORDER BY id DESC LIMIT 1 FOR UPDATE`,
    [userId, template.id]
  );
  const existing = existingResult.rows[0] || null;

  if (existing && stackMode === 'ignore') return existing;
  if (existing && stackMode === 'refresh') {
    const updated = await client.query(
      `UPDATE character_statuses SET name=$1,description=$2,remaining_rounds=$3,effects=$4::jsonb,source=$5,stacks=1,stack_mode=$6,max_stacks=$7,dispellable=$8,expires_by_round=$9,updated_at=CURRENT_TIMESTAMP WHERE id=$10 RETURNING *`,
      [template.name,template.description,duration,JSON.stringify(effects),String(source||'系統'),stackMode,maxStacks,dispellable,duration>0,existing.id]
    );
    return updated.rows[0];
  }
  if (existing && stackMode === 'stack') {
    const stacks = Math.min(maxStacks, Math.max(1, Number(existing.stacks) || 1) + 1);
    const nextDuration = duration > 0 ? Math.max(duration, Math.max(0, Number(existing.remaining_rounds) || 0)) : 0;
    const updated = await client.query(
      `UPDATE character_statuses SET name=$1,description=$2,remaining_rounds=$3,effects=$4::jsonb,source=$5,stacks=$6,stack_mode=$7,max_stacks=$8,dispellable=$9,expires_by_round=$10,updated_at=CURRENT_TIMESTAMP WHERE id=$11 RETURNING *`,
      [template.name,template.description,nextDuration,JSON.stringify(effects),String(source||'系統'),stacks,stackMode,maxStacks,dispellable,duration>0,existing.id]
    );
    return updated.rows[0];
  }
  if (stackMode === 'replace') {
    await client.query(`UPDATE character_statuses SET active=FALSE,updated_at=CURRENT_TIMESTAMP WHERE user_id=$1 AND status_template_id=$2 AND active=TRUE`,[userId,template.id]);
  }

  const inserted = await client.query(
    `INSERT INTO character_statuses(user_id,status_template_id,name,description,remaining_rounds,effects,source,stacks,stack_mode,max_stacks,dispellable,expires_by_round) VALUES($1,$2,$3,$4,$5,$6::jsonb,$7,1,$8,$9,$10,$11) RETURNING *`,
    [userId,template.id,template.name,template.description,duration,JSON.stringify(effects),String(source||'系統'),stackMode,maxStacks,dispellable,duration>0]
  );
  return inserted.rows[0];
}

async function applyAdHocModifierStatusWithClient(client, userId, effects, source, durationRounds) {
  const safeSource = String(source || '效果').slice(0,240);
  const name = safeSource.slice(0,120) || '效果';
  const duration = Math.max(0, Math.floor(Number(durationRounds) || 0));
  const passiveList = normalizeUnifiedEffects(effects).map(effect => ({...effect,timing:'passive',duration_rounds:0}));
  if (!passiveList.length) return null;
  const container = {list:passiveList};
  const existing = await client.query(
    `SELECT * FROM character_statuses WHERE user_id=$1 AND status_template_id IS NULL AND source=$2 AND name=$3 AND active=TRUE ORDER BY id DESC LIMIT 1 FOR UPDATE`,
    [userId,safeSource,name]
  );
  if (existing.rows[0]) {
    const updated = await client.query(
      `UPDATE character_statuses SET effects=$1::jsonb,remaining_rounds=$2,stacks=1,stack_mode='refresh',max_stacks=1,dispellable=TRUE,expires_by_round=$3,updated_at=CURRENT_TIMESTAMP WHERE id=$4 RETURNING *`,
      [JSON.stringify(container),duration,duration>0,existing.rows[0].id]
    );
    return updated.rows[0];
  }
  const inserted = await client.query(
    `INSERT INTO character_statuses(user_id,name,description,remaining_rounds,effects,source,stacks,stack_mode,max_stacks,dispellable,expires_by_round) VALUES($1,$2,$3,$4,$5::jsonb,$6,1,'refresh',1,TRUE,$7) RETURNING *`,
    [userId,name,'由統一效果系統產生',duration,JSON.stringify(container),safeSource,duration>0]
  );
  return inserted.rows[0];
}

function effectOperationNumber(before, effect, min = 0, max = Number.MAX_SAFE_INTEGER) {
  const value = Number(effect.value) || 0;
  let next = Number(before) || 0;
  if (effect.operation === 'set') next = value;
  else if (effect.operation === 'spend') next -= Math.abs(value);
  else next += value;
  return Math.max(min, Math.min(max, next));
}

async function applyUnifiedEffectsWithClient(client, {userId,roomId=null,effects=[],source='效果',allowedTimings=null}) {
  const list = normalizeUnifiedEffects(effects).filter(effect => !allowedTimings || allowedTimings.has(effect.timing));
  if (!list.length) return {applied:[],message_parts:[]};

  const charResult = await client.query('SELECT * FROM character_cards WHERE user_id=$1 FOR UPDATE',[userId]);
  const rawCharacter = charResult.rows[0];
  if (!rawCharacter) throw Object.assign(new Error('找不到角色'),{status:404});
  const character = parseCharacter(rawCharacter);

  let member = null;
  if (roomId) {
    const memberResult = await client.query('SELECT * FROM room_members WHERE room_id=$1 AND user_id=$2 FOR UPDATE',[roomId,userId]);
    member = memberResult.rows[0] || null;
    if (!member) throw Object.assign(new Error('角色不在這個房間'),{status:403});
  }

  let inventory = normalizeInventory(character.inventory);
  let weirdCoins = Math.max(0,Math.floor(Number(character.weird_coins)||0));
  let gameCoins = Math.max(0,Math.floor(Number(character.game_coins)||0));
  let inventorySlots = Math.max(5,Math.floor(Number(character.inventory_slots)||5));
  let classResources = {...parseJsonField(character.class_resources,{})};
  const fifthField = character.faction === '西國' ? 'current_faith' : 'current_great_way';
  const fifthMax = character.faction === '西國' ? Math.max(0,Number(character.final_faith)||0) : Math.max(0,Number(character.final_great_way)||0);
  let fifthCurrent = Math.max(0,Number(character[fifthField])||0);
  let hp = member ? Math.max(0,Number(member.hp)||0) : null;
  let spirit = member ? Math.max(0,Number(member.spirit)||0) : null;
  const sanityMax = member ? Math.max(0,Math.floor((Number(character.final_spirit??character.spirit)||0)*1.2)) : null;
  let sanity = member ? (member.sanity_initialized ? Math.max(0,Math.min(sanityMax,Number(member.current_sanity)||0)) : sanityMax) : null;
  const sanityBeforeAll = sanity;
  let sevenSin = member ? (member.seven_sin||null) : null;

  const applied = [];
  const messageParts = [];
  const modifierEffects = [];
  let modifierDuration = 0;

  for (const effect of list) {
    if (effect.chance < 100 && Math.random()*100 >= effect.chance) continue;
    const value = Number(effect.value) || 0;

    if (['stat_flat','stat_percent','all_attributes_percent','element_affinity'].includes(effect.type)) {
      if (effect.timing === 'passive') continue;
      modifierEffects.push(effect);
      modifierDuration = Math.max(modifierDuration,effect.duration_rounds);
      applied.push(effect);
      continue;
    }

    if (effect.type === 'heal_hp' || effect.type === 'damage_hp') {
      if (!member) throw Object.assign(new Error('生命效果需要在房間中使用'),{status:400});
      const amount = Math.max(0,Math.floor(Math.abs(value)));
      const before = hp;
      hp = effect.type === 'heal_hp' ? Math.min(Number(member.max_hp)||character.max_hp||0,hp+amount) : Math.max(0,hp-amount);
      messageParts.push(`HP ${before}→${hp}`);
    } else if (effect.type === 'heal_spirit' || effect.type === 'damage_spirit') {
      if (!member) throw Object.assign(new Error('精神值效果需要在房間中使用'),{status:400});
      const amount = Math.max(0,Math.floor(Math.abs(value)));
      const before = spirit;
      spirit = effect.type === 'heal_spirit' ? Math.min(Number(member.max_spirit)||100,spirit+amount) : Math.max(0,spirit-amount);
      messageParts.push(`精神值 ${before}→${spirit}`);
    } else if (effect.type === 'heal_sanity' || effect.type === 'damage_sanity') {
      if (!member) throw Object.assign(new Error('理智效果需要在房間中使用'),{status:400});
      const amount = Math.max(0,Math.floor(Math.abs(value)));
      const before = sanity;
      sanity = effect.type === 'heal_sanity' ? Math.min(sanityMax,sanity+amount) : Math.max(0,sanity-amount);
      if(effect.type==='damage_sanity'&&!sevenSin&&sanityMax>0&&(before-sanity)>=sanityMax*0.25)sevenSin=SEVEN_SINS[Math.floor(Math.random()*SEVEN_SINS.length)];
      messageParts.push(`理智 ${before}→${sanity}`);
    } else if (effect.type === 'fifth_change') {
      const before = fifthCurrent;
      fifthCurrent = effectOperationNumber(fifthCurrent,effect,0,fifthMax);
      messageParts.push(`${character.fifth_resource_name||'第五維'} ${before}→${fifthCurrent}`);
    } else if (effect.type === 'weird_coin') {
      const before = weirdCoins;
      weirdCoins = Math.floor(effectOperationNumber(weirdCoins,effect,0));
      messageParts.push(`詭幣 ${before}→${weirdCoins}`);
    } else if (effect.type === 'game_coin') {
      if (!character.game_coin_unlocked && effect.operation !== 'gain') throw Object.assign(new Error('遊戲幣尚未解鎖'),{status:403});
      const before = gameCoins;
      gameCoins = Math.floor(effectOperationNumber(gameCoins,effect,0));
      messageParts.push(`遊戲幣 ${before}→${gameCoins}`);
    } else if (effect.type === 'inventory') {
      const itemName = effect.extra;
      if (!itemName) throw Object.assign(new Error('道具效果缺少道具名稱'),{status:400});
      const amount = Math.max(1,Math.floor(Math.abs(value)||1));
      const before = inventoryQuantity(inventory,itemName,effect.category);
      if (effect.operation === 'spend' && before < amount) throw Object.assign(new Error(`缺少「${itemName}」×${amount}`),{status:400});
      if (effect.operation === 'gain') {
        ensureInventoryCanAdd({...character,inventory_slots:inventorySlots},inventory,itemName,effect.category);
        inventory = changeInventoryQuantity(inventory,itemName,amount,effect.category);
      } else if (effect.operation === 'spend') {
        inventory = changeInventoryQuantity(inventory,itemName,-amount,effect.category);
      } else {
        const current = inventoryQuantity(inventory,itemName,effect.category);
        inventory = changeInventoryQuantity(inventory,itemName,-current,effect.category);
        if (amount > 0) {
          ensureInventoryCanAdd({...character,inventory_slots:inventorySlots},inventory,itemName,effect.category);
          inventory = changeInventoryQuantity(inventory,itemName,amount,effect.category);
        }
      }
      const after = inventoryQuantity(inventory,itemName,effect.category);
      messageParts.push(`${itemName} ${before}→${after}`);
    } else if (effect.type === 'status_apply') {
      if (!effect.extra) throw Object.assign(new Error('套用狀態效果缺少狀態名稱'),{status:400});
      const status = await applyStatusTemplateWithClient(client,userId,effect.extra,source,effect.duration_rounds || null);
      messageParts.push(`獲得狀態「${status.name}」${Number(status.stacks)>1?`×${status.stacks}`:''}`);
    } else if (effect.type === 'revive') {
      if (!member) throw Object.assign(new Error('復活效果需要在房間中使用'),{status:400});
      if (hp > 0) throw Object.assign(new Error('目前生命值不為 0，無法使用復活效果'),{status:400});
      const pct = Math.max(1,Math.min(100,Math.floor(Math.abs(value)||20)));
      hp = Math.max(1,Math.floor((Number(member.max_hp)||character.max_hp||1)*pct/100));
      messageParts.push(`復活至 ${hp} HP`);
    } else if (effect.type === 'inventory_slots') {
      const before = inventorySlots;
      inventorySlots = Math.max(5,Math.floor(effectOperationNumber(inventorySlots,effect,5,500)));
      if (inventorySlots < inventoryUsedSlots(inventory)) throw Object.assign(new Error('物品欄上限不能低於目前已使用格數'),{status:400});
      messageParts.push(`物品欄 ${before}→${inventorySlots}`);
    } else if (effect.type === 'class_resource') {
      const target = effect.extra.toLocaleLowerCase();
      const entry = Object.entries(classResources).find(([key,res]) => key.toLocaleLowerCase()===target || String(res&&res.name||'').toLocaleLowerCase()===target);
      if (!entry) throw Object.assign(new Error(`找不到職業資源「${effect.extra||'未指定'}」`),{status:400});
      const [key,res] = entry;
      const min = Number(res.min)||0;
      const max = Math.max(min,Number(res.max)||0);
      const before = Number(res.current)||0;
      const next = Math.floor(effectOperationNumber(before,effect,min,max));
      classResources[key] = {...res,current:next};
      messageParts.push(`${res.name||effect.extra} ${before}→${next}`);
    }
    applied.push(effect);
  }

  if (modifierEffects.length) {
    const status = await applyAdHocModifierStatusWithClient(client,userId,modifierEffects,source,modifierDuration);
    if (status) messageParts.push(`獲得「${status.name}」${modifierDuration?` ${modifierDuration} 回合`:'（持續）'}`);
  }

  await client.query(
    `UPDATE character_cards SET inventory=$1::jsonb,weird_coins=$2,game_coins=$3,inventory_slots=$4,class_resources=$5::jsonb,${fifthField}=$6,updated_at=CURRENT_TIMESTAMP WHERE user_id=$7`,
    [JSON.stringify(inventory),weirdCoins,gameCoins,inventorySlots,JSON.stringify(classResources),fifthCurrent,userId]
  );
  let sanityTransition=null;
  if (member) {
    const oldStage=sanityStage(sanityBeforeAll,sanityMax),newStage=sanityStage(sanity,sanityMax);
    sanityTransition={old_stage:oldStage.key,new_stage:newStage.key,worsened:(SANITY_STAGE_ORDER[newStage.key]||0)>(SANITY_STAGE_ORDER[oldStage.key]||0),new_sin:sevenSin&&!member.seven_sin?sevenSin:null};
    await client.query(`UPDATE room_members SET hp=$1,spirit=$2,current_sanity=$3,max_sanity=$4,sanity_initialized=TRUE,sanity_stage=$5,seven_sin=$6,combat_state=CASE WHEN $1>0 THEN 'active' ELSE 'downed' END WHERE room_id=$7 AND user_id=$8`,[hp,spirit,sanity,sanityMax,newStage.key,sevenSin,roomId,userId]);
  }

  return {applied,message_parts:messageParts,sanity_transition:sanityTransition};
}

async function applyUnifiedEffects({userId,roomId=null,effects=[],source='效果',allowedTimings=null}) {
  const client = await pool.connect();
  try {
    await client.query('BEGIN');
    const result = await applyUnifiedEffectsWithClient(client,{userId,roomId,effects,source,allowedTimings});
    await client.query('COMMIT');
    if(roomId&&result.sanity_transition?.worsened)await createSanityStageVision(roomId,userId,result.sanity_transition.new_stage);
    if(roomId&&result.sanity_transition?.new_sin)await get(`INSERT INTO personal_visions(room_id,user_id,title,content,vision_type,shareable) VALUES(?,?,?,?,?,?) RETURNING *`,[roomId,userId,'七宗罪',`你的理智在一次衝擊中崩落了四分之一以上。你陷入了「${result.sanity_transition.new_sin}」。`,'sanity',false]);
    return result;
  } catch (error) {
    await client.query('ROLLBACK').catch(()=>{});
    throw error;
  } finally {
    client.release();
  }
}

async function processCharacterStatusesRoundEnd(roomId,userId) {
  const client = await pool.connect();
  let messageParts = [];
  try {
    await client.query('BEGIN');
    const result = await client.query(`SELECT * FROM character_statuses WHERE user_id=$1 AND active=TRUE ORDER BY id FOR UPDATE`,[userId]);
    const tickEffects = [];
    for (const status of result.rows) {
      const stacks = Math.max(1,Number(status.stacks)||1);
      for (const effect of normalizeUnifiedEffects(parseJsonField(status.effects,{}))) {
        if (effect.timing !== 'round_end') continue;
        tickEffects.push({...effect,value:(Number(effect.value)||0)*stacks});
      }
    }
    if (tickEffects.length) {
      const applied = await applyUnifiedEffectsWithClient(client,{userId,roomId,effects:tickEffects,source:'狀態回合效果',allowedTimings:new Set(['round_end'])});
      messageParts = applied.message_parts || [];
    }
    await client.query(`UPDATE character_statuses SET remaining_rounds=remaining_rounds-1,updated_at=CURRENT_TIMESTAMP WHERE user_id=$1 AND active=TRUE AND expires_by_round=TRUE AND remaining_rounds>0`,[userId]);
    await client.query(`UPDATE character_statuses SET active=FALSE,updated_at=CURRENT_TIMESTAMP WHERE user_id=$1 AND active=TRUE AND expires_by_round=TRUE AND remaining_rounds<=0`,[userId]);
    await client.query('COMMIT');
  } catch (error) {
    await client.query('ROLLBACK').catch(()=>{});
    console.error('STATUS ROUND TICK ERROR:',error);
  } finally {
    client.release();
  }
  if (messageParts.length) {
    const character = await getCharacterByUser(userId);
    await addEvent(roomId,userId,'status_tick',{text:`${character?.name||'角色'} 的狀態效果：${messageParts.join('、')}`});
  }
}

function hasInventoryExpansionSkill(character) {
  const skills = Array.isArray(character && character.skills) ? character.skills : [];
  return skills.some(skill => {
    const data = skill && skill.data && typeof skill.data === 'object' && !Array.isArray(skill.data) ? skill.data : {};
    return Boolean(data.unlock_inventory_expansion) || String(skill && skill.name || '').trim() === '玩家背包';
  });
}

function inventoryBackpackBaseLimit(character) {
  return Math.max(5, Math.floor(Number(character && character.inventory_slots) || 5));
}

async function ensureBackpackBonusForUser(userId, rawCharacter = null) {
  const character = rawCharacter ? parseCharacter(rawCharacter) : await getCharacterByUser(userId);
  if (!character || !hasInventoryExpansionSkill(character) || character.backpack_bonus_applied) return character;
  const slots = Math.max(5, Math.floor(Number(character.inventory_slots) || 5));
  const nextSlots = slots + 20;
  await run(`UPDATE character_cards SET inventory_slots=?,backpack_bonus_applied=TRUE,updated_at=CURRENT_TIMESTAMP WHERE user_id=?`,[nextSlots,userId]);
  return getCharacterByUser(userId);
}

function parsedRecipe(row) {
  if (!row) return null;
  return {
    ...row,
    materials: parseJsonField(row.materials, []),
    output: parseJsonField(row.output, {}),
    config: parseJsonField(row.config, {})
  };
}

function characterHasTemplateSkill(character, templateId) {
  if (!templateId) return true;
  const skills = Array.isArray(character.skills) ? character.skills : [];
  return skills.some(skill => String(skill && skill.template_id || '') === String(templateId));
}


function characterCraftSuccessBonus(character, craftCategory) {
  const target = String(craftCategory || '').trim().toLocaleLowerCase();
  const skills = Array.isArray(character && character.skills) ? character.skills : [];
  let total = 0;

  for (const skill of skills) {
    const data = skill && skill.data && typeof skill.data === 'object' && !Array.isArray(skill.data)
      ? skill.data
      : {};
    const bonus = Number(data.craft_success_bonus) || 0;
    if (!bonus) continue;

    const category = String(data.craft_success_category || '').trim().toLocaleLowerCase();
    if (!category || category === '全部' || category === 'all' || category === '*' || category === target) {
      total += bonus;
    }
  }

  return Math.floor(total);
}

/* =========================================================
   EQUIPMENT / WEAPON CONSUMABLE USE
========================================================= */

app.post('/api/character/gear/:kind/:index/use', auth, async function(req,res){
  const client = await pool.connect();
  try{
    await client.query('BEGIN');
    const kind = req.params.kind === 'equipment' ? 'equipment' : req.params.kind === 'weapon' ? 'weapons' : null;
    if(!kind){
      await client.query('ROLLBACK');
      return res.status(400).json({error:'裝備類型錯誤'});
    }

    const index = Math.max(0, Math.floor(Number(req.params.index)||0));
    const result = await client.query('SELECT * FROM character_cards WHERE user_id=$1 FOR UPDATE',[req.user.id]);
    const character = parseCharacter(result.rows[0]);
    if(!character){
      await client.query('ROLLBACK');
      return res.status(404).json({error:'你還沒有角色卡'});
    }

    const list = normalizeGearList(character[kind]);
    const gear = list[index];
    if(!gear){
      await client.query('ROLLBACK');
      return res.status(404).json({error:'找不到這件裝備／武器'});
    }

    const requirement = gearRequirementResult(character, gear);
    if (!requirement.ok) {
      await client.query('ROLLBACK');
      return res.status(400).json({error:`${gear.name} 目前無法使用：${requirement.reason}`});
    }

    let itemName = String(gear.consume_item_name || '').trim();
    let category = String(gear.consume_item_category || '').trim();
    let quantity = Math.max(0, Math.floor(Number(gear.consume_quantity)||0));
    let selectedAmmo = null;
    const ammoOptions = Array.isArray(gear.ammo_options) ? gear.ammo_options : [];
    if (kind === 'weapons' && ammoOptions.length) {
      const rawAmmo = req.body && req.body.ammo && typeof req.body.ammo === 'object' ? req.body.ammo : null;
      let option = null;
      if (rawAmmo && rawAmmo.name) {
        option = ammoOptions.find(entry =>
          String(entry.name).toLocaleLowerCase() === String(rawAmmo.name).trim().toLocaleLowerCase() &&
          (!rawAmmo.category || String(entry.category || '').toLocaleLowerCase() === String(rawAmmo.category).trim().toLocaleLowerCase())
        ) || null;
        if (!option) {
          await client.query('ROLLBACK');
          return res.status(400).json({error:`${gear.name} 不支援選擇的彈藥`});
        }
      } else if (ammoOptions.length === 1) {
        option = ammoOptions[0];
      } else {
        await client.query('ROLLBACK');
        return res.status(400).json({error:`${gear.name} 有多種可用彈藥，請先選擇要使用的彈藥`});
      }
      itemName = String(option.name || '').trim();
      category = String(option.category || '').trim();
      quantity = Math.max(1, Math.floor(Number(option.quantity)||1));
      selectedAmmo = {name:itemName,category,quantity,damage_multiplier:Math.max(0,Number(option.damage_multiplier)||1),note:String(option.note||'').trim()};
    }
    let inventory = normalizeInventory(character.inventory);

    if(itemName && quantity > 0){
      if(inventoryQuantity(inventory,itemName,category) < quantity){
        await client.query('ROLLBACK');
        return res.status(400).json({error:`${gear.name} 使用失敗：${itemName} 不足，需要 ${quantity}`});
      }
      inventory = changeInventoryQuantity(inventory,itemName,-quantity,category);
    }

    const chargeCost = gear.charges_max > 0 ? Math.max(0, Math.floor(Number(gear.charge_cost) || 1)) : 0;
    if (chargeCost > 0 && gear.charges_current < chargeCost) {
      await client.query('ROLLBACK');
      return res.status(400).json({error:`${gear.name} 充能不足（${gear.charges_current}/${gear.charges_max}）`});
    }
    const durabilityCost = gear.durability_max > 0 ? Math.max(0, Math.floor(Number(gear.durability_cost) || 1)) : 0;
    if (durabilityCost > 0 && gear.durability_current < durabilityCost) {
      await client.query('ROLLBACK');
      return res.status(400).json({error:`${gear.name} 耐久不足（${gear.durability_current}/${gear.durability_max}）`});
    }

    if (chargeCost > 0) gear.charges_current = Math.max(0, gear.charges_current - chargeCost);
    if (durabilityCost > 0) gear.durability_current = Math.max(0, gear.durability_current - durabilityCost);
    list[index] = gear;

    await client.query(
      `UPDATE character_cards SET inventory=$1::jsonb, ${kind}=$2::jsonb, updated_at=CURRENT_TIMESTAMP WHERE user_id=$3`,
      [JSON.stringify(inventory),JSON.stringify(list),req.user.id]
    );

    const roomId = Math.max(0, Number(req.body && req.body.room_id) || 0) || null;
    if (roomId) await requireRoomMember(req.user.id, roomId);
    const effectResult = await applyUnifiedEffectsWithClient(client, {
      userId:req.user.id,
      roomId,
      effects:gear.effects || [],
      source:`${kind==='equipment'?'裝備':'武器'}：${gear.name}`,
      allowedTimings:new Set(['immediate'])
    });

    await client.query('COMMIT');
    const updatedCharacter = await getCharacterByUser(req.user.id);
    const snapshot = roomId ? await roomSnapshot(roomId) : null;
    if (snapshot) io.to('room:' + roomId).emit('room:snapshot', snapshot);
    res.json({
      ok:true,
      gear,
      consumed:itemName&&quantity>0?{name:itemName,category,quantity}:null,
      selected_ammo:selectedAmmo,
      charge_change:chargeCost>0?{cost:chargeCost,current:gear.charges_current,max:gear.charges_max}:null,
      durability_change:durabilityCost>0?{cost:durabilityCost,current:gear.durability_current,max:gear.durability_max}:null,
      effects_applied:effectResult.applied||[],
      effect_messages:effectResult.message_parts||[],
      character:updatedCharacter,
      room:snapshot
    });
  }catch(error){
    await client.query('ROLLBACK').catch(()=>{});
    console.error('USE GEAR ERROR:',error);
    res.status(error.status||500).json({error:error.message||'使用裝備失敗'});
  }finally{
    client.release();
  }
});

app.get('/api/recipes', auth, async function(req,res){
  try{
    const rows = await all(`SELECT * FROM recipes ORDER BY id ASC`);
    res.json({recipes: rows.map(parsedRecipe)});
  }catch(error){
    console.error('RECIPE LIST ERROR:', error);
    res.status(500).json({error:'讀取合成配方失敗'});
  }
});

app.post('/api/recipes/:id/craft', auth, async function(req,res){
  const client = await pool.connect();
  try{
    await client.query('BEGIN');

    const recipeResult = await client.query(
      'SELECT * FROM recipes WHERE id = $1 FOR SHARE',
      [req.params.id]
    );
    const recipe = parsedRecipe(recipeResult.rows[0]);
    if(!recipe){
      await client.query('ROLLBACK');
      return res.status(404).json({error:'找不到配方'});
    }

    const charResult = await client.query(
      'SELECT * FROM character_cards WHERE user_id = $1 FOR UPDATE',
      [req.user.id]
    );
    const character = parseCharacter(charResult.rows[0]);
    if(!character){
      await client.query('ROLLBACK');
      return res.status(404).json({error:'你還沒有角色卡'});
    }

    const config = recipe.config || {};
    const access = config.access === 'skill' ? 'skill' : 'public';
    const requiredSkillId = Number(config.required_skill_template_id) || 0;

    if(access === 'skill' && !characterHasTemplateSkill(character, requiredSkillId)){
      await client.query('ROLLBACK');
      return res.status(403).json({error:'你還沒有解鎖這個配方需要的製作技能'});
    }

    const materials = Array.isArray(recipe.materials) ? recipe.materials : [];
    let inventory = normalizeInventory(character.inventory);

    for(const material of materials){
      const name = String(material && material.name || '').trim();
      const category = String(material && material.category || '').trim();
      const quantity = Math.max(1, Math.floor(Number(material && (material.quantity ?? material.qty)) || 1));
      if(!name) continue;
      if(inventoryQuantity(inventory, name, category) < quantity){
        await client.query('ROLLBACK');
        return res.status(400).json({error:`材料不足：${name} 需要 ${quantity}`});
      }
    }

    const currencyType = ['weird','game'].includes(config.currency_type) ? config.currency_type : 'none';
    const currencyCost = Math.max(0, Math.floor(Number(config.currency_cost) || 0));
    let weirdCoins = Math.max(0, Number(character.weird_coins) || 0);
    let gameCoins = Math.max(0, Number(character.game_coins) || 0);

    if(currencyType === 'game' && !character.game_coin_unlocked){
      await client.query('ROLLBACK');
      return res.status(403).json({error:'遊戲幣尚未解鎖'});
    }
    if(currencyType === 'weird' && weirdCoins < currencyCost){
      await client.query('ROLLBACK');
      return res.status(400).json({error:'詭幣不足'});
    }
    if(currencyType === 'game' && gameCoins < currencyCost){
      await client.query('ROLLBACK');
      return res.status(400).json({error:'遊戲幣不足'});
    }

    const craftCategory = String(config.craft_category || recipe.recipe_type || '一般').trim().slice(0,80) || '一般';
    const baseSuccessRate = Math.max(0, Math.min(100, Math.floor(Number(config.success_rate ?? 100) || 0)));
    const skillBonus = characterCraftSuccessBonus(character, craftCategory);
    const successRate = Math.max(0, Math.min(100, baseSuccessRate + skillBonus));
    const roll = 1 + Math.floor(Math.random() * 100);
    const success = roll <= successRate;
    const failureConsumes = config.failure_consumes === undefined ? true : Boolean(config.failure_consumes);

    const consumeCosts = success || failureConsumes;

    if(consumeCosts){
      for(const material of materials){
        const name = String(material && material.name || '').trim();
        const category = String(material && material.category || '').trim();
        const quantity = Math.max(1, Math.floor(Number(material && (material.quantity ?? material.qty)) || 1));
        if(name) inventory = changeInventoryQuantity(inventory, name, -quantity, category);
      }
      if(currencyType === 'weird') weirdCoins -= currencyCost;
      if(currencyType === 'game') gameCoins -= currencyCost;
    }

    const output = recipe.output && typeof recipe.output === 'object' ? recipe.output : {};
    const outputType = ['item','equipment','weapon'].includes(output.type) ? output.type : 'item';
    const outputName = String(output.name || recipe.name || '未命名成品').trim().slice(0, 120);
    const outputQuantity = Math.max(1, Math.floor(Number(output.quantity) || 1));
    const outputCategory = String(output.category || craftCategory || '').trim().slice(0,80);
    const equipment = Array.isArray(character.equipment) ? [...character.equipment] : [];
    const weapons = Array.isArray(character.weapons) ? [...character.weapons] : [];

    if(success){
      if(outputType === 'equipment'){
        for(let i=0;i<outputQuantity;i+=1) equipment.push(outputName);
      }else if(outputType === 'weapon'){
        for(let i=0;i<outputQuantity;i+=1) weapons.push(outputName);
      }else{
        ensureInventoryCanAdd(character, inventory, outputName, outputCategory);
        inventory = changeInventoryQuantity(inventory, outputName, outputQuantity, outputCategory);
      }
    }

    await client.query(
      `UPDATE character_cards
       SET inventory=$1::jsonb,
           equipment=$2::jsonb,
           weapons=$3::jsonb,
           weird_coins=$4,
           game_coins=$5,
           updated_at=CURRENT_TIMESTAMP
       WHERE user_id=$6`,
      [JSON.stringify(inventory), JSON.stringify(equipment), JSON.stringify(weapons), weirdCoins, gameCoins, req.user.id]
    );

    await client.query('COMMIT');
    const updated = await getCharacterByUser(req.user.id);
    res.json({
      ok:true,
      success,
      roll,
      base_success_rate:baseSuccessRate,
      skill_bonus:skillBonus,
      success_rate:successRate,
      craft_category:craftCategory,
      failure_consumed:!success && consumeCosts,
      character:updated,
      recipe
    });
  }catch(error){
    try{ await client.query('ROLLBACK'); }catch{}
    console.error('CRAFT ERROR:', error);
    res.status(error.status||500).json({error:error.message||'合成失敗'});
  }finally{
    client.release();
  }
});


/* =========================================================
   INVENTORY EXPANSION / 詭源商店
========================================================= */

app.post('/api/character/inventory-slot', auth, async function(req,res){
  const client = await pool.connect();
  try{
    await client.query('BEGIN');
    const result = await client.query('SELECT * FROM character_cards WHERE user_id=$1 FOR UPDATE',[req.user.id]);
    const character = parseCharacter(result.rows[0]);
    if(!character){
      await client.query('ROLLBACK');
      return res.status(404).json({error:'你還沒有角色卡'});
    }
    if(!hasInventoryExpansionSkill(character)){
      await client.query('ROLLBACK');
      return res.status(403).json({error:'需要先取得「玩家背包」技能才能擴充物品欄'});
    }
    if(!character.game_coin_unlocked){
      await client.query('ROLLBACK');
      return res.status(403).json({error:'遊戲幣尚未解鎖'});
    }
    const cost = 20;
    const gameCoins = Math.max(0, Math.floor(Number(character.game_coins)||0));
    if(gameCoins < cost){
      await client.query('ROLLBACK');
      return res.status(400).json({error:`遊戲幣不足，擴充 1 格需要 ${cost} 遊戲幣`});
    }
    const nextSlots = inventorySlotLimit(character) + 1;
    await client.query(
      `UPDATE character_cards SET inventory_slots=$1,game_coins=$2,updated_at=CURRENT_TIMESTAMP WHERE user_id=$3`,
      [nextSlots, gameCoins-cost, req.user.id]
    );
    await client.query('COMMIT');
    res.json({ok:true,cost,character:await getCharacterByUser(req.user.id)});
  }catch(error){
    await client.query('ROLLBACK').catch(()=>{});
    console.error('BUY INVENTORY SLOT ERROR:',error);
    res.status(error.status||500).json({error:error.message||'擴充物品欄失敗'});
  }finally{client.release();}
});


/* =========================================================
   GAME ROOM / 遊戲間
========================================================= */

app.get('/api/game-room', auth, async function(req,res){
  try{
    const character = await requireCharacter(req.user.id);
    const ticketCount = inventoryQuantity(character.inventory, '入場卷');

    res.json({
      unlocked: Boolean(character.game_room_unlocked),
      ticket_count: ticketCount,
      npcs: [],
      npc_ready: false
    });
  }catch(error){
    console.error('GAME ROOM STATUS ERROR:', error);
    res.status(error.status||500).json({error:error.message||'讀取遊戲間失敗'});
  }
});

app.post('/api/game-room/enter', auth, async function(req,res){
  const client = await pool.connect();
  try{
    await client.query('BEGIN');

    const result = await client.query(
      'SELECT * FROM character_cards WHERE user_id=$1 FOR UPDATE',
      [req.user.id]
    );

    const character = parseCharacter(result.rows[0]);
    if(!character){
      await client.query('ROLLBACK');
      return res.status(404).json({error:'你還沒有角色卡'});
    }

    if(character.game_room_unlocked){
      await client.query('COMMIT');
      return res.json({
        ok:true,
        first_entry:false,
        character:await getCharacterByUser(req.user.id),
        npcs:[]
      });
    }

    let inventory = normalizeInventory(character.inventory);
    const ticketName = '入場卷';
    const ticketCount = inventoryQuantity(inventory, ticketName);

    if(ticketCount < 1){
      await client.query('ROLLBACK');
      return res.status(400).json({error:'進入遊戲間需要「入場卷」×1'});
    }

    inventory = changeInventoryQuantity(inventory, ticketName, -1);

    await client.query(
      `UPDATE character_cards
       SET inventory=$1::jsonb,
           game_room_unlocked=TRUE,
           updated_at=CURRENT_TIMESTAMP
       WHERE user_id=$2`,
      [JSON.stringify(inventory), req.user.id]
    );

    await client.query('COMMIT');

    res.json({
      ok:true,
      first_entry:true,
      consumed:{name:ticketName,quantity:1},
      character:await getCharacterByUser(req.user.id),
      npcs:[]
    });
  }catch(error){
    await client.query('ROLLBACK').catch(()=>{});
    console.error('GAME ROOM ENTER ERROR:', error);
    res.status(error.status||500).json({error:error.message||'進入遊戲間失敗'});
  }finally{
    client.release();
  }
});

app.get('/api/shop', auth, async function(req,res){
  try{
    const rows = await all(`SELECT * FROM shop_items WHERE active=TRUE ORDER BY id ASC`);
    res.json({items:rows});
  }catch(error){
    console.error('SHOP LIST ERROR:',error);
    res.status(500).json({error:'讀取詭源商店失敗'});
  }
});

app.get('/api/admin/shop', auth, adminAuth, async function(req,res){
  try{
    const rows = await all(`SELECT * FROM shop_items ORDER BY id ASC`);
    res.json({items:rows});
  }catch(error){
    console.error('ADMIN SHOP LIST ERROR:',error);
    res.status(500).json({error:'讀取詭源商店失敗'});
  }
});

app.post('/api/admin/shop', auth, adminAuth, async function(req,res){
  try{
    const name = String(req.body.name||'').trim().slice(0,120);
    if(!name)return res.status(400).json({error:'商品名稱不能為空'});
    const itemType = ['item','equipment','weapon'].includes(req.body.item_type) ? req.body.item_type : 'item';
    const row = await get(
      `INSERT INTO shop_items (name,item_type,item_category,description,price_weird,quantity,active) VALUES (?,?,?,?,?,?,?) RETURNING *`,
      [name,itemType,String(req.body.item_category||'').trim().slice(0,80),String(req.body.description||''),Math.max(0,Math.floor(Number(req.body.price_weird)||0)),Math.max(1,Math.floor(Number(req.body.quantity)||1)),req.body.active===undefined?true:Boolean(req.body.active)]
    );
    res.json({item:row});
  }catch(error){
    console.error('CREATE SHOP ITEM ERROR:',error);
    res.status(500).json({error:'建立商店商品失敗'});
  }
});

app.patch('/api/admin/shop/:id', auth, adminAuth, async function(req,res){
  try{
    const current = await get(`SELECT * FROM shop_items WHERE id=?`,[req.params.id]);
    if(!current)return res.status(404).json({error:'找不到商品'});
    const itemType = req.body.item_type===undefined ? current.item_type : (['item','equipment','weapon'].includes(req.body.item_type)?req.body.item_type:'item');
    const row = await get(
      `UPDATE shop_items SET name=?,item_type=?,item_category=?,description=?,price_weird=?,quantity=?,active=?,updated_at=CURRENT_TIMESTAMP WHERE id=? RETURNING *`,
      [String(req.body.name===undefined?current.name:req.body.name).trim().slice(0,120),itemType,String(req.body.item_category===undefined?current.item_category:req.body.item_category).trim().slice(0,80),String(req.body.description===undefined?current.description:req.body.description),Math.max(0,Math.floor(Number(req.body.price_weird===undefined?current.price_weird:req.body.price_weird)||0)),Math.max(1,Math.floor(Number(req.body.quantity===undefined?current.quantity:req.body.quantity)||1)),req.body.active===undefined?Boolean(current.active):Boolean(req.body.active),req.params.id]
    );
    res.json({item:row});
  }catch(error){
    console.error('UPDATE SHOP ITEM ERROR:',error);
    res.status(500).json({error:'修改商店商品失敗'});
  }
});

app.delete('/api/admin/shop/:id', auth, adminAuth, async function(req,res){
  try{
    const result = await run(`DELETE FROM shop_items WHERE id=?`,[req.params.id]);
    if(!result.changes)return res.status(404).json({error:'找不到商品'});
    res.json({ok:true});
  }catch(error){
    console.error('DELETE SHOP ITEM ERROR:',error);
    res.status(500).json({error:'刪除商店商品失敗'});
  }
});

app.post('/api/shop/:id/buy', auth, async function(req,res){
  const client = await pool.connect();
  try{
    await client.query('BEGIN');
    const shopResult = await client.query('SELECT * FROM shop_items WHERE id=$1 AND active=TRUE FOR SHARE',[req.params.id]);
    const item = shopResult.rows[0];
    if(!item){
      await client.query('ROLLBACK');
      return res.status(404).json({error:'找不到商品'});
    }
    const charResult = await client.query('SELECT * FROM character_cards WHERE user_id=$1 FOR UPDATE',[req.user.id]);
    const character = parseCharacter(charResult.rows[0]);
    if(!character){
      await client.query('ROLLBACK');
      return res.status(404).json({error:'你還沒有角色卡'});
    }
    const price = Math.max(0,Math.floor(Number(item.price_weird)||0));
    const weirdCoins = Math.max(0,Math.floor(Number(character.weird_coins)||0));
    if(weirdCoins < price){
      await client.query('ROLLBACK');
      return res.status(400).json({error:`詭幣不足，需要 ${price}`});
    }
    const quantity = Math.max(1,Math.floor(Number(item.quantity)||1));
    let inventory = normalizeInventory(character.inventory);
    const equipment = Array.isArray(character.equipment)?[...character.equipment]:[];
    const weapons = Array.isArray(character.weapons)?[...character.weapons]:[];
    if(item.item_type==='equipment'){
      for(let i=0;i<quantity;i+=1)equipment.push(item.name);
    }else if(item.item_type==='weapon'){
      for(let i=0;i<quantity;i+=1)weapons.push(item.name);
    }else{
      ensureInventoryCanAdd(character, inventory, item.name, item.item_category||'');
      inventory = changeInventoryQuantity(inventory,item.name,quantity,item.item_category||'');
    }
    await client.query(
      `UPDATE character_cards SET weird_coins=$1,inventory=$2::jsonb,equipment=$3::jsonb,weapons=$4::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$5`,
      [weirdCoins-price,JSON.stringify(inventory),JSON.stringify(equipment),JSON.stringify(weapons),req.user.id]
    );
    await client.query('COMMIT');
    res.json({ok:true,item,character:await getCharacterByUser(req.user.id)});
  }catch(error){
    await client.query('ROLLBACK').catch(()=>{});
    console.error('BUY SHOP ITEM ERROR:',error);
    res.status(error.status||500).json({error:error.message||'購買失敗'});
  }finally{client.release();}
});

app.post('/api/character/skills/from-template', auth, async function(req,res){
  const client = await pool.connect();
  try{
    await client.query('BEGIN');
    const templateId = Math.max(0, Number(req.body.template_id) || 0);
    if(!templateId){
      await client.query('ROLLBACK');
      return res.status(400).json({error:'請選擇技能'});
    }

    const templateResult = await client.query('SELECT * FROM skill_templates WHERE id = $1 FOR SHARE', [templateId]);
    const template = templateResult.rows[0];
    if(!template){
      await client.query('ROLLBACK');
      return res.status(404).json({error:'找不到技能資料'});
    }

    const characterResult = await client.query('SELECT * FROM character_cards WHERE user_id = $1 FOR UPDATE', [req.user.id]);
    const character = parseCharacter(characterResult.rows[0]);
    if(!character){
      await client.query('ROLLBACK');
      return res.status(404).json({error:'你還沒有角色卡'});
    }

    const skills = Array.isArray(character.skills) ? [...character.skills] : [];
    if(skills.some(skill=>String(skill.template_id||'')===String(template.id))){
      await client.query('ROLLBACK');
      return res.status(409).json({error:'角色已經擁有這個技能'});
    }

    const templateData = parseJsonField(template.data, {});
    const cost = normalizeSkillLearnCost(templateData.learn_cost);
    if(cost.mode !== 'cost'){
      await client.query('ROLLBACK');
      return res.status(403).json({error:'這個技能不能直接學習，只能透過職業升級、任務獎勵或 DM 授予取得'});
    }
    if(!skillLearnCostHasPayment(cost)){
      await client.query('ROLLBACK');
      return res.status(403).json({error:'這個技能尚未設定學習代價，因此不能免費取得'});
    }

    if(cost.required_profession_id){
      const progress = normalizeProfessionProgress(character.profession_progress);
      const entry = progress[String(cost.required_profession_id)];
      if(!entry || !entry.active){
        await client.query('ROLLBACK');
        return res.status(403).json({error:'你沒有學習這個技能所需的職業'});
      }
      const currentRankIndex = PROFESSION_RANKS.indexOf(normalizeProfessionRank(entry.rank));
      const requiredRankIndex = PROFESSION_RANKS.indexOf(normalizeProfessionRank(cost.required_profession_rank));
      if(currentRankIndex < requiredRankIndex || (currentRankIndex === requiredRankIndex && Number(entry.level||1) < cost.required_profession_level)){
        await client.query('ROLLBACK');
        return res.status(403).json({error:`職業需求不足：至少 ${cost.required_profession_rank} 階 Lv.${cost.required_profession_level}`});
      }
    }

    let inventory = normalizeInventory(character.inventory);
    if(cost.item_name && inventoryQuantity(inventory, cost.item_name) < cost.item_quantity){
      await client.query('ROLLBACK');
      return res.status(400).json({error:`缺少學習道具「${cost.item_name}」×${cost.item_quantity}`});
    }
    let weirdCoins = Math.max(0, Number(character.weird_coins)||0);
    let gameCoins = Math.max(0, Number(character.game_coins)||0);
    let skillPoints = Math.max(0, Number(character.skill_points)||0);
    if(weirdCoins < cost.weird_coins){
      await client.query('ROLLBACK');
      return res.status(400).json({error:`詭幣不足，需要 ${cost.weird_coins}`});
    }
    if(cost.game_coins > 0 && !character.game_coin_unlocked){
      await client.query('ROLLBACK');
      return res.status(403).json({error:'遊戲幣尚未解鎖'});
    }
    if(gameCoins < cost.game_coins){
      await client.query('ROLLBACK');
      return res.status(400).json({error:`遊戲幣不足，需要 ${cost.game_coins}`});
    }
    if(skillPoints < cost.skill_points){
      await client.query('ROLLBACK');
      return res.status(400).json({error:`技能點不足，需要 ${cost.skill_points}`});
    }

    if(cost.item_name) inventory = changeInventoryQuantity(inventory, cost.item_name, -cost.item_quantity);
    weirdCoins -= cost.weird_coins;
    gameCoins -= cost.game_coins;
    skillPoints -= cost.skill_points;
    skills.push(buildCharacterSkillFromTemplate(template, {type:'learned', cost}));
    let gameCoinUnlocked = Boolean(character.game_coin_unlocked || templateData.unlock_game_coin);

    await client.query(
      `UPDATE character_cards
       SET skills=$1::jsonb,
           inventory=$2::jsonb,
           weird_coins=$3,
           game_coins=$4,
           game_coin_unlocked=$5,
           skill_points=$6,
           inventory_slots=$7,
           backpack_bonus_applied=$8,
           updated_at=CURRENT_TIMESTAMP
       WHERE user_id=$9`,
      [
        JSON.stringify(skills), JSON.stringify(inventory), weirdCoins, gameCoins, gameCoinUnlocked, skillPoints,
        (templateData.unlock_inventory_expansion || String(template.name||'').trim()==='玩家背包') && !character.backpack_bonus_applied
          ? Math.max(5,Math.floor(Number(character.inventory_slots)||5)) + 20
          : Math.max(5,Math.floor(Number(character.inventory_slots)||5)),
        Boolean(character.backpack_bonus_applied || templateData.unlock_inventory_expansion || String(template.name||'').trim()==='玩家背包'),
        req.user.id
      ]
    );

    await client.query('COMMIT');
    const characterUpdated = await getCharacterByUser(req.user.id);
    res.json({ok:true, character:characterUpdated, paid_cost:cost});
  }catch(error){
    await client.query('ROLLBACK').catch(()=>{});
    console.error('ADD TEMPLATE SKILL ERROR:',error);
    res.status(error.status||500).json({error:error.message||'新增技能失敗'});
  }finally{
    client.release();
  }
});

app.delete('/api/character/skills/:skillId', auth, async function(req,res){
  try{
    const c=await requireCharacter(req.user.id);
    const skills=Array.isArray(c.skills)?c.skills:[];
    const target=skills.find(skill=>String(skill.id)===String(req.params.skillId));
    if(!target) return res.status(404).json({error:'找不到角色技能'});

    const acquisition = target && target.acquisition && typeof target.acquisition === 'object'
      ? target.acquisition
      : {};

    if(acquisition.type === 'profession'){
      return res.status(403).json({error:'這是職業自動取得技能，不能由玩家自行移除'});
    }

    const next=skills.filter(skill=>String(skill.id)!==String(req.params.skillId));
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
      const character = await requireCharacter(req.user.id);
      let skill = (Array.isArray(character.skills)?character.skills:[]).find(item=>String(item&&item.id)===String(req.params.skillId));
      const gearMatch = /^gear_(\d+)$/.exec(String(req.params.skillId || ''));
      if (!skill && gearMatch) {
        const templateId = Math.max(0, Number(gearMatch[1]) || 0);
        const allowed = new Set(gearGrantedSkillTemplateIds(character).map(String));
        if (!allowed.has(String(templateId))) return res.status(403).json({error:'目前沒有裝備提供這個技能'});
        const template = await get(`SELECT * FROM skill_templates WHERE id = ?`, [templateId]);
        if (!template) return res.status(404).json({error:'裝備提供的技能已不存在'});
        skill = buildCharacterSkillFromTemplate(template, {type:'equipment', temporary:true});
        skill.id = 'gear_' + templateId;
      }
      if(!skill)return res.status(404).json({error:'找不到角色技能'});
      if(skill.summon_tag)return res.status(400).json({error:'召喚技能請使用「召喚」按鈕'});

      const roomId=Number(req.body&&req.body.room_id)||null;
      if(roomId){
        const currentUser=await getUser(req.user.id);
        if(!currentUser||!currentUser.is_admin)await requireRoomMember(req.user.id,roomId);
      }

      const gate=await prepareBattleSkillUse(roomId,req.user.id,character,skill);
      if(gate.charging){
        const snapshot=roomId?await roomSnapshot(roomId):null;
        if(snapshot)io.to('room:'+roomId).emit('room:snapshot',snapshot);
        return res.json({ok:true,charging:true,character:await getCharacterByUser(req.user.id),room:snapshot,charge_turns:gate.config.charge_turns});
      }

      const configForUse=gate.config||skillCombatConfig(skill,character);
      const skillEffects=parseJsonField(skill.data,{}).effects||[];
      const effectTraits=skillEffectTraits(skill);
      let selectedTargets=[];
      if(gate.battle){
        if(configForUse.combat_attack&&!String(configForUse.combat_target_mode||'').startsWith('enemy')){
          return res.status(400).json({error:'攻擊技能的戰鬥目標必須設定為敵方單體或敵方全體'});
        }
        selectedTargets=await playerRequestedSkillTargets(roomId,req.user.id,configForUse,req.body||{},{includeDowned:effectTraits.revives});
      }

      const change=calculateSkillClassResourceChange(character,skill);
      const gearUseCost = skill.acquisition && skill.acquisition.type === 'equipment'
        ? await consumeDirectGrantingGearUseCost(req.user.id, character, skill.template_id, req.body && req.body.ammo)
        : null;
      if(change.changed)await saveCharacterClassResources(req.user.id,change.next_resources);

      const effectResults=[];
      const combatResults=[];
      let hitTargets=[];
      const sourceLabel=`技能：${skill.name||'未命名技能'}`;

      if(!gate.battle){
        const selfEffect=await applyUnifiedEffects({
          userId:req.user.id,
          roomId,
          effects:skillEffects,
          source:sourceLabel,
          allowedTimings:new Set(['immediate'])
        });
        effectResults.push({target_name:character.name,...selfEffect});
      }else{
        if(configForUse.effects_target_mode==='self'&&!configForUse.effects_on_hit){
          const selfTarget=await getCombatTarget(roomId,'player',req.user.id,{allowDowned:true});
          const result=await applyEffectsToCombatTarget({roomId,target:selfTarget,effects:skillEffects,source:sourceLabel});
          effectResults.push({target_name:selfTarget.name,...result});
        }
        if(configForUse.combat_attack){
          const ammoMultiplier=gearUseCost&&gearUseCost.ammo?Math.max(0,Number(gearUseCost.ammo.damage_multiplier)||1):1;
          const attackTargets=configForUse.combat_target_mode==='enemy_all'?selectedTargets:selectedTargets.slice(0,1);
          for(const target of attackTargets){
            const combat=await resolvePlayerAttack({roomId,userId:req.user.id,character,skill,config:configForUse,targetId:target.id,targetType:target.type,damageMultiplier:ammoMultiplier});
            combatResults.push(combat);if(combat.hit)hitTargets.push(target);
          }
        }
        if(configForUse.effects_target_mode==='target'){
          const effectTargets=configForUse.effects_on_hit&&configForUse.combat_attack?hitTargets:selectedTargets;
          for(const target of effectTargets){
            const result=await applyEffectsToCombatTarget({roomId,target,effects:skillEffects,source:sourceLabel});
            effectResults.push({target_name:target.name,...result});
          }
        }else if(configForUse.effects_target_mode==='self'&&configForUse.effects_on_hit&&(!configForUse.combat_attack||hitTargets.length)){
          const selfTarget=await getCombatTarget(roomId,'player',req.user.id,{allowDowned:true});
          const result=await applyEffectsToCombatTarget({roomId,target:selfTarget,effects:skillEffects,source:sourceLabel});
          effectResults.push({target_name:selfTarget.name,...result});
        }
      }

      const mergedEffectMessages=effectResults.flatMap(r=>(r.message_parts||[]).map(msg=>effectResults.length>1?`${r.target_name}：${msg}`:msg));
      const effectResult={applied:effectResults.flatMap(r=>r.applied||[]),message_parts:mergedEffectMessages};
      const combatResult=combatResults[0]||null;

      if(roomId){
        const resourceText=change.effect?`（${change.effect.name} ${change.before} → ${change.after}）`:'';
        const effectText=effectResult.message_parts&&effectResult.message_parts.length?`【${effectResult.message_parts.join('、')}】`:'';
        const config=configForUse;
        const ammoMultiplier=gearUseCost&&gearUseCost.ammo?Math.max(0,Number(gearUseCost.ammo.damage_multiplier)||1):1;
        const effectiveDamageMultiplier=Math.round((Math.max(0,Number(config.damage_multiplier)||1)*ammoMultiplier)*10000)/10000;
        const enhanceText=effectiveDamageMultiplier!==1?`（有效傷害倍率 ×${effectiveDamageMultiplier}）`:'';
        const ammoText=gearUseCost&&gearUseCost.ammo?`（彈藥：${gearUseCost.ammo.name}${gearUseCost.ammo.note?`－${gearUseCost.ammo.note}`:''}）`:'';
        const gearCostText=gearUseCost?`（來源裝備：${gearUseCost.gear_name}${gearUseCost.consumed?`，消耗 ${gearUseCost.consumed.name} ×${gearUseCost.consumed.quantity}`:''}）`:'';
        const combatText=combatResults.length?`【${combatResults.map(r=>r.hit?`命中 ${r.target.name}，傷害 ${r.damage}`:`攻擊 ${r.target.name} 未命中`).join('；')}】`:'';
        await addEvent(roomId,req.user.id,'skill',{
          text:`${character.name} 使用技能「${skill.name||'未命名技能'}」${resourceText}${effectText}${enhanceText}${ammoText}${gearCostText}${combatText}${config.turn_cost===0?'（不消耗回合數）':''}`,
          skill_id:skill.id,
          turn_cost:config.turn_cost,
          charged:Boolean(gate.charged),
          targets:selectedTargets.map(t=>({type:t.type,id:t.id,name:t.name})),
          resource_change:change.effect?{name:change.effect.name,before:change.before,after:change.after}:null
        });
        if(gate.battle){if(config.turn_cost>0)await advanceBattleAction(roomId,req.user.id,`使用「${skill.name||'技能'}」`);else await endBattleIfResolved(roomId);}
        const snapshot=await roomSnapshot(roomId);
        io.to('room:'+roomId).emit('room:snapshot',snapshot);
      }

      res.json({
        ok:true,
        charging:false,
        character:await getCharacterByUser(req.user.id),
        resource_change:change.effect?{name:change.effect.name,before:change.before,after:change.after}:null,
        effects_applied:effectResult.applied||[],
        effect_messages:effectResult.message_parts||[],
        effective_combat_config:{
          ...(gate.config||skillCombatConfig(skill, character)),
          damage_multiplier:Math.round(((Math.max(0,Number((gate.config||skillCombatConfig(skill, character)).damage_multiplier)||1))*(gearUseCost&&gearUseCost.ammo?Math.max(0,Number(gearUseCost.ammo.damage_multiplier)||1):1))*10000)/10000
        },
        selected_ammo:gearUseCost&&gearUseCost.ammo?gearUseCost.ammo:null,
        gear_use_cost:gearUseCost,
        combat_result:combatResult,
        combat_results:combatResults
      });
    }catch(error){
      console.error('USE CHARACTER SKILL ERROR:',error);
      res.status(error.status||500).json({error:error.message||'使用技能失敗'});
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
      ),
    ...calculateEntityEffectStats(
      parseJsonField(row.attributes, {}),
      parseJsonField(row.element_affinity, {}),
      parseJsonField(row.effects, {})
    ),
    traits: parseJsonField(row.traits, []),
    skills: parseJsonField(row.skills, []),
    config: parseJsonField(row.config, {})
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
          st.attributes,
          st.element_affinity,
          st.traits,
          st.skills,
          st.effects,
          st.config,

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

      let skill =
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

      const gearMatch = /^gear_(\d+)$/.exec(String(req.params.skillId || ''));
      if (!skill && gearMatch) {
        const templateId = Math.max(0, Number(gearMatch[1]) || 0);
        const allowed = new Set(gearGrantedSkillTemplateIds(character).map(String));
        if (!allowed.has(String(templateId))) return res.status(403).json({error:'目前沒有裝備提供這個召喚技能'});
        const template = await get(`SELECT * FROM skill_templates WHERE id = ?`, [templateId]);
        if (template) {
          skill = buildCharacterSkillFromTemplate(template, {type:'equipment', temporary:true});
          skill.id = 'gear_' + templateId;
        }
      }

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

      const gate = await prepareBattleSkillUse(roomId, req.user.id, character, skill);
      if (gate.charging) {
        const snapshot = roomId ? await roomSnapshot(roomId) : null;
        if (snapshot) io.to('room:' + roomId).emit('room:snapshot', snapshot);
        return res.json({ok:true, charging:true, room:snapshot, character:await getCharacterByUser(req.user.id)});
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

      const summonSkillEffects = await applyUnifiedEffects({
        userId:req.user.id,
        roomId,
        effects:parseJsonField(skill.data,{}).effects||[],
        source:`技能：${skill.name||'召喚技能'}`,
        allowedTimings:new Set(['immediate'])
      });

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
              ) +
              (
                summonSkillEffects.message_parts && summonSkillEffects.message_parts.length
                  ? '【' + summonSkillEffects.message_parts.join('、') + '】'
                  : ''
              ),
            summon_id:
              row.id,
            template_id:
              template.id
          }
        );
      }

      if (roomId && gate.battle && gate.config.turn_cost > 0) {
        await advanceBattleAction(roomId, req.user.id, '使用召喚技能');
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

app.post('/api/character/summons/:id/skills/:skillTemplateId/use',auth,async function(req,res){
  try{
    const summon=await get(`SELECT si.*,st.skills,st.attributes,st.element_affinity,st.effects,st.config FROM summon_instances si LEFT JOIN summon_templates st ON st.id=si.template_id WHERE si.id=? AND si.owner_user_id=? AND si.status='active'`,[req.params.id,req.user.id]);
    if(!summon)return res.status(404).json({error:'找不到你的召喚物'});
    const skillIds=parseJsonField(summon.skills,[]).map(Number);const skillTemplateId=Number(req.params.skillTemplateId);
    if(!skillIds.includes(skillTemplateId))return res.status(403).json({error:'這個召喚物沒有此技能'});
    const template=await get(`SELECT * FROM skill_templates WHERE id=?`,[skillTemplateId]);if(!template)return res.status(404).json({error:'召喚物技能模板已不存在'});
    const skill=buildCharacterSkillFromTemplate(template,{type:'summon_template',summon_id:summon.id});const character=await requireCharacter(req.user.id);const roomId=Number(summon.room_id)||null;
    const gate=await prepareBattleSkillUse(roomId,req.user.id,character,skill);
    if(gate.charging){const snapshot=roomId?await roomSnapshot(roomId):null;if(snapshot)io.to('room:'+roomId).emit('room:snapshot',snapshot);return res.json({ok:true,charging:true,room:snapshot,charge_turns:gate.config.charge_turns});}
    if(roomId){
      const config=gate.config||skillCombatConfig(skill);const traits=skillEffectTraits(skill);const effects=parseJsonField(skill.data,{}).effects||[];
      if(config.combat_attack&&!String(config.combat_target_mode||'').startsWith('enemy'))return res.status(400).json({error:'召喚物攻擊技能的目標必須是敵方'});
      const selectedTargets=gate.battle?await playerRequestedSkillTargets(roomId,req.user.id,config,req.body||{},{includeDowned:traits.revives}):[];
      const summonEntity=calculateEntityEffectStats(parseJsonField(summon.attributes,{}),parseJsonField(summon.element_affinity,{}),parseJsonField(summon.effects,{}));
      const combatResults=[];const effectResults=[];const hitTargets=[];
      if(gate.battle&&config.combat_attack){
        const attackTargets=config.combat_target_mode==='enemy_all'?selectedTargets:selectedTargets.slice(0,1);
        for(const target of attackTargets){const r=await resolveCombatAttack({roomId,attackerType:'summon',attackerId:summon.id,attackerName:summon.name,attackerSource:summonEntity,attackerUserId:req.user.id,skill,config,targetType:target.type,targetId:target.id,damageMultiplier:1});combatResults.push(r);if(r.hit)hitTargets.push(target);}
      }
      if(gate.battle&&config.effects_target_mode==='target'){
        const effectTargets=config.effects_on_hit&&config.combat_attack?hitTargets:selectedTargets;
        for(const target of effectTargets){const r=await applyEffectsToCombatTarget({roomId,target,effects,source:`召喚物 ${summon.name}：${template.name}`});effectResults.push({target_name:target.name,...r});}
      }else if(gate.battle&&config.effects_target_mode==='self'&&(!config.effects_on_hit||!config.combat_attack||hitTargets.length)){
        // 召喚物目前沒有獨立 HP 資源列；自我型立即效果先作用於主人，召喚物的被動效果仍由召喚物模板計算。
        const selfTarget=await getCombatTarget(roomId,'player',req.user.id,{allowDowned:true});const r=await applyEffectsToCombatTarget({roomId,target:selfTarget,effects,source:`召喚物 ${summon.name}：${template.name}`});effectResults.push({target_name:selfTarget.name,...r});
      }
      const effectMessages=effectResults.flatMap(x=>(x.message_parts||[]).map(m=>`${x.target_name}：${m}`));
      await addEvent(roomId,req.user.id,'summon_skill',{text:`召喚物「${summon.name}」使用技能「${template.name}」${combatResults.length?`【${combatResults.map(r=>r.hit?`命中 ${r.target.name}，傷害 ${r.damage}`:`攻擊 ${r.target.name} 未命中`).join('；')}】`:''}${effectMessages.length?`【${effectMessages.join('；')}】`:''}${config.turn_cost===0?'（不消耗回合數）':''}`,summon_id:summon.id,skill_template_id:template.id,turn_cost:config.turn_cost,charged:Boolean(gate.charged)});
      if(gate.battle){if(config.turn_cost>0)await advanceBattleAction(roomId,req.user.id,`指揮「${summon.name}」使用技能「${template.name}」`);else await endBattleIfResolved(roomId);}
      const snapshot=await roomSnapshot(roomId);io.to('room:'+roomId).emit('room:snapshot',snapshot);return res.json({ok:true,charging:false,room:snapshot,combat_result:combatResults[0]||null,combat_results:combatResults,effect_messages:effectMessages});
    }
    res.json({ok:true,charging:false});
  }catch(error){console.error('USE SUMMON SKILL ERROR:',error);res.status(error.status||500).json({error:error.message||'召喚物使用技能失敗'});}
});

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
      res.json({ summon_templates: rows.map(row=>({
        ...row,
        ...calculateEntityEffectStats(parseJsonField(row.attributes,{}),parseJsonField(row.element_affinity,{}),parseJsonField(row.effects,{})),
        traits:parseJsonField(row.traits,[]),skills:parseJsonField(row.skills,[]),config:parseJsonField(row.config,{})
      })) });
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
      const normalizedSkills = [...new Set(
        (Array.isArray(req.body.skills) ? req.body.skills : [])
          .map(Number)
          .filter(Boolean)
      )].slice(0, 100);

      if (normalizedSkills.length) {
        const rows = await all(
          `SELECT id FROM skill_templates WHERE id = ANY(?::bigint[])`,
          [normalizedSkills]
        );
        const existing = new Set(rows.map(row => Number(row.id)));
        const missing = normalizedSkills.filter(id => !existing.has(id));
        if (missing.length) {
          return res.status(400).json({
            error: '召喚物技能包含不存在的技能模板：' + missing.join(', ')
          });
        }
      }

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
              effects,
              config
            )
          VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?::jsonb, ?::jsonb, ?::jsonb, ?::jsonb, ?::jsonb, ?::jsonb)
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
          JSON.stringify(normalizedSkills),
          JSON.stringify(effectContainerWithList(req.body.effects, normalizeUnifiedEffects(req.body.effects))),
          JSON.stringify(req.body.config || {})
        ]
      );
      const calc = calculateEntityEffectStats(parseJsonField(row.attributes,{}),parseJsonField(row.element_affinity,{}),parseJsonField(row.effects,{}));
      res.json({ summon_template: {...row,...calc,traits:parseJsonField(row.traits,[]),skills:parseJsonField(row.skills,[]),config:parseJsonField(row.config,{})} });
    } catch (error) {
      console.error(error);
      res.status(500).json({ error: '建立召喚物模板失敗' });
    }
  }
);


app.patch('/api/admin/summon-templates/:id',auth,adminAuth,async function(req,res){
  try{
    const c=await get(`SELECT * FROM summon_templates WHERE id = ?`,[req.params.id]);
    if(!c)return res.status(404).json({error:'找不到召喚物模板'});

    const currentSkills=parseJsonField(c.skills,[]);
    const nextSkills=(req.body.skills===undefined?currentSkills:req.body.skills);
    const normalizedSkills=[...new Set((Array.isArray(nextSkills)?nextSkills:[]).map(Number).filter(Boolean))].slice(0,100);

    if(normalizedSkills.length){
      const rows=await all(`SELECT id FROM skill_templates WHERE id = ANY(?::bigint[])`,[normalizedSkills]);
      const existing=new Set(rows.map(row=>Number(row.id)));
      const missing=normalizedSkills.filter(id=>!existing.has(id));
      if(missing.length)return res.status(400).json({error:'召喚物技能包含不存在的技能模板：'+missing.join(', ')});
    }

    const currentEffects=parseJsonField(c.effects,{});
    const nextEffects=req.body.effects===undefined?currentEffects:req.body.effects;
    const normalizedEffects=effectContainerWithList(nextEffects,normalizeUnifiedEffects(nextEffects));
    const row=await get(`UPDATE summon_templates SET name=?,description=?,growth_mode=?,control_mode=?,behavior_mode=?,self_ai_level=?,language_level=?,speech_enabled=?,loyalty=?,attributes=?::jsonb,element_affinity=?::jsonb,traits=?::jsonb,skills=?::jsonb,effects=?::jsonb,config=?::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=? RETURNING *`,[
      String(req.body.name===undefined?c.name:req.body.name).slice(0,120),
      String(req.body.description===undefined?c.description:req.body.description),
      String(req.body.growth_mode===undefined?c.growth_mode:req.body.growth_mode).slice(0,30),
      ['manual','assist','auto'].includes(req.body.control_mode)?req.body.control_mode:c.control_mode,
      String(req.body.behavior_mode===undefined?c.behavior_mode:req.body.behavior_mode).slice(0,30),
      Math.max(0,Number(req.body.self_ai_level===undefined?c.self_ai_level:req.body.self_ai_level)||0),
      Math.max(0,Number(req.body.language_level===undefined?c.language_level:req.body.language_level)||0),
      req.body.speech_enabled===undefined?Boolean(c.speech_enabled):Boolean(req.body.speech_enabled),
      Math.max(0,Number(req.body.loyalty===undefined?c.loyalty:req.body.loyalty)||0),
      JSON.stringify(req.body.attributes===undefined?parseJsonField(c.attributes,{}):req.body.attributes||{}),
      JSON.stringify(req.body.element_affinity===undefined?parseJsonField(c.element_affinity,{}):req.body.element_affinity||{}),
      JSON.stringify(req.body.traits===undefined?parseJsonField(c.traits,[]):Array.isArray(req.body.traits)?req.body.traits:[]),
      JSON.stringify(normalizedSkills),
      JSON.stringify(normalizedEffects),
      JSON.stringify(req.body.config===undefined?parseJsonField(c.config,{}):req.body.config||{}),
      req.params.id
    ]);
    const calc=calculateEntityEffectStats(parseJsonField(row.attributes,{}),parseJsonField(row.element_affinity,{}),parseJsonField(row.effects,{}));
    res.json({summon_template:{...row,...calc,traits:parseJsonField(row.traits,[]),skills:parseJsonField(row.skills,[]),config:parseJsonField(row.config,{})}});
  }catch(error){console.error(error);res.status(error.status||500).json({error:error.message||'修改召喚物模板失敗'});}
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
      res.json({ recipes: rows.map(parsedRecipe) });
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
          JSON.stringify(
            (Array.isArray(req.body.materials) ? req.body.materials : [])
              .map(item => ({
                name: String(item && item.name || '').trim().slice(0,120),
                category: String(item && item.category || '').trim().slice(0,80),
                quantity: Math.max(1, Math.floor(Number(item && (item.quantity ?? item.qty)) || 1))
              }))
              .filter(item => item.name)
          ),
          JSON.stringify({
            type: ['item','equipment','weapon'].includes(req.body.output && req.body.output.type)
              ? req.body.output.type
              : 'item',
            name: String(req.body.output && req.body.output.name || req.body.name || '未命名成品').trim().slice(0,120),
            quantity: Math.max(1, Math.floor(Number(req.body.output && req.body.output.quantity) || 1)),
            category: String(req.body.output && req.body.output.category || '').trim().slice(0,80)
          }),
          JSON.stringify({
            access: req.body.config && req.body.config.access === 'skill' ? 'skill' : 'public',
            required_skill_template_id: Math.max(0, Number(req.body.config && req.body.config.required_skill_template_id) || 0),
            currency_type: ['weird','game'].includes(req.body.config && req.body.config.currency_type)
              ? req.body.config.currency_type
              : 'none',
            currency_cost: Math.max(0, Math.floor(Number(req.body.config && req.body.config.currency_cost) || 0)),
            craft_category: String(req.body.config && req.body.config.craft_category || '一般').trim().slice(0,80) || '一般',
            success_rate: Math.max(0, Math.min(100, Math.floor(Number((req.body.config && req.body.config.success_rate) ?? 100) || 0))),
            failure_consumes: req.body.config && req.body.config.failure_consumes === undefined
              ? true
              : Boolean(req.body.config && req.body.config.failure_consumes)
          })
        ]
      );
      res.json({ recipe: row });
    } catch (error) {
      console.error(error);
      res.status(500).json({ error: '建立配方失敗' });
    }
  }
);



app.delete('/api/admin/recipes/:id', auth, adminAuth, async function(req,res){
  try{
    const result = await run(`DELETE FROM recipes WHERE id = ?`, [req.params.id]);
    if(!result.changes) return res.status(404).json({error:'找不到配方'});
    res.json({ok:true});
  }catch(error){
    console.error('DELETE RECIPE ERROR:', error);
    res.status(500).json({error:'刪除配方失敗'});
  }
});

/* =========================================================
   PROFESSION PROGRESSION
========================================================= */
app.post('/api/admin/players/:id/professions/:professionId/level-up', auth, adminAuth, async function(req,res){
  const client = await pool.connect();
  try{
    await client.query('BEGIN');
    const userId = Math.max(0, Number(req.params.id)||0);
    const charResult = await client.query('SELECT * FROM character_cards WHERE user_id=$1 FOR UPDATE', [userId]);
    const character = parseCharacter(charResult.rows[0]);
    if(!character){
      await client.query('ROLLBACK');
      return res.status(404).json({error:'找不到玩家角色卡'});
    }
    const result = await levelUpProfessionOnCharacterWithClient(client, character, req.params.professionId);
    await persistCharacterProgressWithClient(client, character, userId);
    await client.query('COMMIT');
    const updated = await getCharacterByUser(userId);
    res.json({ok:true, character:updated, progression:result});
  }catch(error){
    await client.query('ROLLBACK').catch(()=>{});
    console.error('PROFESSION LEVEL UP ERROR:', error);
    res.status(error.status||500).json({error:error.message||'職業升級失敗'});
  }finally{
    client.release();
  }
});

/* =========================================================
   V17 BLOODLINE DATABASE
========================================================= */

function parsedBloodline(row) {
  if (!row) return null;
  const effects = effectContainerWithList(parseJsonField(row.effects,{}), normalizeUnifiedEffects(parseJsonField(row.effects,{})));
  return {...row,effects};
}

app.get('/api/bloodlines',auth,async function(req,res){
  try{const rows=await all(`SELECT * FROM bloodline_templates ORDER BY name ASC`);res.json({bloodlines:rows.map(parsedBloodline)});}catch(error){res.status(500).json({error:'讀取血脈資料庫失敗'});}
});
app.get('/api/admin/bloodlines',auth,adminAuth,async function(req,res){
  try{const rows=await all(`SELECT * FROM bloodline_templates ORDER BY name ASC`);res.json({bloodlines:rows.map(parsedBloodline)});}catch(error){res.status(500).json({error:'讀取血脈資料庫失敗'});}
});
app.post('/api/admin/bloodlines',auth,adminAuth,async function(req,res){
  try{
    const name=String(req.body.name||'').trim().slice(0,120);if(!name)return res.status(400).json({error:'血脈名稱不能為空'});
    const effects=effectContainerWithList(req.body.effects,normalizeUnifiedEffects(req.body.effects));
    const row=await get(`INSERT INTO bloodline_templates(name,description,effects) VALUES(?,?,?::jsonb) RETURNING *`,[name,String(req.body.description||''),JSON.stringify(effects)]);
    await syncAllCharacterSourceEffects();
    res.json({bloodline:parsedBloodline(row)});
  }catch(error){console.error(error);res.status(error.code==='23505'?409:500).json({error:error.code==='23505'?'這個血脈名稱已存在':'建立血脈失敗'});}
});
app.patch('/api/admin/bloodlines/:id',auth,adminAuth,async function(req,res){
  try{
    const current=await get(`SELECT * FROM bloodline_templates WHERE id=?`,[req.params.id]);if(!current)return res.status(404).json({error:'找不到血脈'});
    const oldName=current.name;const name=String(req.body.name===undefined?current.name:req.body.name).trim().slice(0,120);if(!name)return res.status(400).json({error:'血脈名稱不能為空'});
    const raw=req.body.effects===undefined?parseJsonField(current.effects,{}):req.body.effects;const effects=effectContainerWithList(raw,normalizeUnifiedEffects(raw));
    const row=await get(`UPDATE bloodline_templates SET name=?,description=?,effects=?::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=? RETURNING *`,[name,String(req.body.description===undefined?current.description:req.body.description),JSON.stringify(effects),req.params.id]);
    if(oldName!==name)await run(`UPDATE character_cards SET bloodline=?,updated_at=CURRENT_TIMESTAMP WHERE bloodline=?`,[name,oldName]);
    await syncAllCharacterSourceEffects();
    res.json({bloodline:parsedBloodline(row)});
  }catch(error){console.error(error);res.status(error.code==='23505'?409:500).json({error:error.code==='23505'?'這個血脈名稱已存在':'修改血脈失敗'});}
});
app.delete('/api/admin/bloodlines/:id',auth,adminAuth,async function(req,res){
  try{await run(`DELETE FROM bloodline_templates WHERE id=?`,[req.params.id]);await syncAllCharacterSourceEffects();res.json({ok:true});}catch(error){res.status(500).json({error:'刪除血脈失敗'});}
});

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
    baseConfig.auto_skills=normalizeProfessionAutoSkills(
      req.body.auto_skills===undefined
        ? baseConfig.auto_skills
        : req.body.auto_skills
    );
    const professionEffectsInput = req.body.effects===undefined ? baseConfig.effects : req.body.effects;
    baseConfig.effects = effectContainerWithList(professionEffectsInput, normalizeUnifiedEffects(professionEffectsInput));
    baseConfig.book_name=String(req.body.book_name || baseConfig.book_name || (name + '職業書')).trim().slice(0,120);
    baseConfig.book_quantity=Math.max(1,Math.floor(Number(req.body.book_quantity ?? baseConfig.book_quantity ?? 1)||1));

    const row=await get(
      `INSERT INTO profession_templates (name,category,rank,description,config) VALUES (?,?,?,?,?::jsonb) RETURNING *`,
      [name,category,String(req.body.rank||'G').slice(0,10),String(req.body.description||''),JSON.stringify(baseConfig)]
    );

    await syncAllCharacterSourceEffects();
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
    configInput.auto_skills=normalizeProfessionAutoSkills(
      req.body.auto_skills===undefined
        ? configInput.auto_skills
        : req.body.auto_skills
    );
    const professionEffectsInput = req.body.effects===undefined ? configInput.effects : req.body.effects;
    configInput.effects = effectContainerWithList(professionEffectsInput, normalizeUnifiedEffects(professionEffectsInput));
    configInput.book_name=String(req.body.book_name===undefined?(configInput.book_name || (name + '職業書')):req.body.book_name).trim().slice(0,120);
    configInput.book_quantity=Math.max(1,Math.floor(Number(req.body.book_quantity===undefined?(configInput.book_quantity||1):req.body.book_quantity)||1));

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
    await syncAllCharacterProfessionProgress();
    await syncAllCharacterSourceEffects();

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
    await syncAllCharacterProfessionProgress();
    await syncAllCharacterSourceEffects();
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

    skillData.learn_cost = normalizeSkillLearnCost(
      req.body.learn_cost === undefined
        ? skillData.learn_cost
        : req.body.learn_cost
    );

    skillData.effects = normalizeUnifiedEffects(
      req.body.data && req.body.data.effects !== undefined
        ? req.body.data.effects
        : skillData.effects
    );

    if (skillData.learn_cost.mode === 'cost' && !skillLearnCostHasPayment(skillData.learn_cost)) {
      return res.status(400).json({
        error:'可自行學習的技能至少要設定一種代價（技能書／道具、詭幣或遊戲幣）'
      });
    }

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

    nextData.learn_cost = normalizeSkillLearnCost(
      req.body.learn_cost === undefined
        ? nextData.learn_cost
        : req.body.learn_cost
    );

    nextData.effects = normalizeUnifiedEffects(nextData.effects || []);

    if (nextData.learn_cost.mode === 'cost' && !skillLearnCostHasPayment(nextData.learn_cost)) {
      return res.status(400).json({
        error:'可自行學習的技能至少要設定一種代價（技能書／道具、詭幣或遊戲幣）'
      });
    }

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

        const professionSync =
          await syncCharacterProfessionProgress(
            character.user_id,
            character
          );

        parsedCharacters.push(
          parseCharacter({
            ...character,
            class_resources:
              classResources,
            profession_progress:
              professionSync.progress,
            skills:
              professionSync.skills,
            game_coin_unlocked:
              professionSync.game_coin_unlocked
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
            battle_active = FALSE,
            current_actor_id = NULL,
            current_actor_ref_id = NULL,
            combat_order = '[]'::jsonb,
            combat_index = 0,
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
            battle_active = FALSE,
            current_actor_id = NULL,
            current_actor_ref_id = NULL,
            combat_order = '[]'::jsonb,
            combat_index = 0,
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
   START ROOM / EXPLORATION
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

      await run(
        `
          UPDATE rooms
          SET
            status = 'active',
            saved = FALSE,
            battle_active = FALSE,
            round = 1,
            current_actor_id = NULL,
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
            'DM 開始跑團，現在進入探索階段。'
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

      res.json(snapshot);
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
   ROOM SCENE / MAP / IN-GAME TIME
========================================================= */

app.patch('/api/rooms/:id/scene', auth, adminAuth, async function(req,res){
  try{
    await requireRoomAdmin(req.user.id, req.params.id);

    const current = await get(`SELECT * FROM rooms WHERE id = ?`, [req.params.id]);
    if(!current) return res.status(404).json({error:'找不到房間'});

    const mapName = req.body.map_name === undefined
      ? current.map_name
      : String(req.body.map_name || '').trim().slice(0,120) || '未設定地點';
    const mapImageUrl = req.body.map_image_url === undefined
      ? current.map_image_url
      : String(req.body.map_image_url || '').trim().slice(0,2000);
    const mapDescription = req.body.map_description === undefined
      ? current.map_description
      : String(req.body.map_description || '').trim().slice(0,4000);
    const gameDay = req.body.game_day === undefined
      ? Math.max(1, Number(current.game_day)||1)
      : Math.max(1, Math.floor(Number(req.body.game_day)||1));
    const gameHour = req.body.game_hour === undefined
      ? Math.max(0, Math.min(23, Number(current.game_hour)||0))
      : Math.max(0, Math.min(23, Math.floor(Number(req.body.game_hour)||0)));
    const gameMinute = req.body.game_minute === undefined
      ? Math.max(0, Math.min(59, Number(current.game_minute)||0))
      : Math.max(0, Math.min(59, Math.floor(Number(req.body.game_minute)||0)));
    const timeHidden = req.body.time_hidden === undefined
      ? Boolean(current.time_hidden)
      : Boolean(req.body.time_hidden);
    const timeDistortionOffset = req.body.time_distortion_offset_minutes === undefined
      ? clampInt(current.time_distortion_offset_minutes,-10080,10080,0)
      : clampInt(req.body.time_distortion_offset_minutes,-10080,10080,0);
    const timeRate = req.body.time_rate === undefined
      ? (Number.isFinite(Number(current.time_rate)) ? Number(current.time_rate) : 1)
      : Math.max(-4,Math.min(8,Number(req.body.time_rate)||0));

    const previousTotal=roomGameTotal(current);
    await run(
      `UPDATE rooms SET map_name=?,map_image_url=?,map_description=?,game_day=?,game_hour=?,game_minute=?,time_hidden=?,time_distortion_offset_minutes=?,time_rate=?,updated_at=CURRENT_TIMESTAMP WHERE id=?`,
      [mapName,mapImageUrl,mapDescription,gameDay,gameHour,gameMinute,timeHidden,timeDistortionOffset,timeRate,req.params.id]
    );
    const nextTotal=((gameDay-1)*1440)+(gameHour*60)+gameMinute;
    if(nextTotal>previousTotal){await decayWorldNoise(Number(req.params.id),nextTotal-previousTotal);await processScheduledWorldEvents(Number(req.params.id),previousTotal,nextTotal);await checkAllBossTimePhases(Number(req.params.id),nextTotal);}

    await addEvent(req.params.id, req.user.id, 'scene', {
      text:`場景更新：${mapName}｜第 ${gameDay} 天 ${String(gameHour).padStart(2,'0')}:${String(gameMinute).padStart(2,'0')}`,
      map_name:mapName,
      game_day:gameDay,
      game_hour:gameHour,
      game_minute:gameMinute
    });

    const snapshot=await roomSnapshot(req.params.id);
    io.to('room:'+req.params.id).emit('room:snapshot',snapshot);
    res.json(snapshot);
  }catch(error){
    console.error('UPDATE SCENE ERROR:',error);
    res.status(error.status||500).json({error:error.message||'更新地圖／時間失敗'});
  }
});

app.post('/api/rooms/:id/time/advance', auth, adminAuth, async function(req,res){
  try{
    await requireRoomAdmin(req.user.id, req.params.id);
    const room=await get(`SELECT * FROM rooms WHERE id = ?`,[req.params.id]);
    if(!room)return res.status(404).json({error:'找不到房間'});

    const delta=Math.floor(Number(req.body.minutes)||0);
    if(!delta)return res.status(400).json({error:'請輸入要推進的分鐘數'});

    const currentTotal=((Math.max(1,Number(room.game_day)||1)-1)*1440)+(Math.max(0,Number(room.game_hour)||0)*60)+Math.max(0,Number(room.game_minute)||0);
    const nextTotal=Math.max(0,currentTotal+delta);
    const gameDay=Math.floor(nextTotal/1440)+1;
    const dayMinutes=nextTotal%1440;
    const gameHour=Math.floor(dayMinutes/60);
    const gameMinute=dayMinutes%60;

    const rawDelta=nextTotal-currentTotal;
    const roomRate=Number.isFinite(Number(room.time_rate))?Number(room.time_rate):1;
    const roomFlowDelta=Math.round(rawDelta*(roomRate-1));
    await run(`UPDATE rooms SET game_day=?,game_hour=?,game_minute=?,time_flow_offset_minutes=COALESCE(time_flow_offset_minutes,0)+?,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[gameDay,gameHour,gameMinute,roomFlowDelta,req.params.id]);
    if(rawDelta!==0){
      const nodes=await all(`SELECT id,time_rate FROM room_map_nodes WHERE room_id=?`,[req.params.id]);
      for(const node of nodes){const rate=Number.isFinite(Number(node.time_rate))?Number(node.time_rate):1;const flowDelta=Math.round(rawDelta*(rate-1));if(flowDelta)await run(`UPDATE room_map_nodes SET time_flow_offset_minutes=COALESCE(time_flow_offset_minutes,0)+?,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[flowDelta,node.id]);}
    }
    if(nextTotal>currentTotal){await decayWorldNoise(Number(req.params.id),nextTotal-currentTotal);await processScheduledWorldEvents(Number(req.params.id),currentTotal,nextTotal,true);await checkAllBossTimePhases(Number(req.params.id),nextTotal);}
    await addEvent(req.params.id,req.user.id,'time',{text:`遊戲時間推進 ${delta>0?'+':''}${delta} 分鐘 → 第 ${gameDay} 天 ${String(gameHour).padStart(2,'0')}:${String(gameMinute).padStart(2,'0')}`,minutes:delta});
    const snapshot=await roomSnapshot(req.params.id);
    io.to('room:'+req.params.id).emit('room:snapshot',snapshot);
    res.json(snapshot);
  }catch(error){
    console.error('ADVANCE TIME ERROR:',error);
    res.status(error.status||500).json({error:error.message||'推進遊戲時間失敗'});
  }
});



/* =========================================================
   V26 HIDDEN / DISTORTED TIME
========================================================= */

function totalMinutesToGameTime(total) {
  const safe = Math.max(0, Math.floor(Number(total) || 0));
  const day = Math.floor(safe / 1440) + 1;
  const dayMinutes = safe % 1440;
  const hour = Math.floor(dayMinutes / 60);
  const minute = dayMinutes % 60;
  return {
    day,
    hour,
    minute,
    text: `第 ${day} 天 ${String(hour).padStart(2,'0')}:${String(minute).padStart(2,'0')}`
  };
}

async function characterTimeCapability(userId) {
  const character = await getCharacterByUser(userId);
  if (!character) return {can_view:false,true_time:false,item_name:null};
  const inventory = normalizeInventory(character.inventory).filter(item => Number(item.quantity) > 0);
  const names = [...new Set(inventory.map(item => String(item.name || '').trim()).filter(Boolean))];
  if (!names.length) return {can_view:false,true_time:false,item_name:null};

  const result = await query(
    `SELECT name,grants_time_view,grants_true_time
       FROM item_templates
      WHERE name = ANY(?::text[])
        AND (grants_time_view=TRUE OR grants_true_time=TRUE)
      ORDER BY grants_true_time DESC,id ASC`,
    [names]
  );
  const rows = result.rows || [];
  if (!rows.length) return {can_view:false,true_time:false,item_name:null};
  const truth = rows.find(row => row.grants_true_time);
  const chosen = truth || rows[0];
  return {
    can_view:true,
    true_time:Boolean(truth),
    item_name:chosen?.name || null
  };
}

async function buildRoomTimeView(roomId,userId) {
  const room = await get(
    `SELECT id,game_day,game_hour,game_minute,time_hidden,time_distortion_offset_minutes,time_rate,time_flow_offset_minutes,current_map_node_id
       FROM rooms
      WHERE id=?`,
    [roomId]
  );
  if (!room) throw Object.assign(new Error('找不到房間'),{status:404});

  const user = await getUser(userId);
  if (!user) throw Object.assign(new Error('帳號不存在'),{status:401});
  const isAdmin = Boolean(user.is_admin);
  if (!isAdmin) await requireRoomMember(userId,roomId);

  const capability = isAdmin
    ? {can_view:true,true_time:true,item_name:null}
    : await characterTimeCapability(userId);

  const viewerNodeId = isAdmin
    ? (room.current_map_node_id || null)
    : await getViewerMapNodeId(userId,Number(roomId));

  let nodeOffset = 0;
  let nodeFlowOffset = 0;
  let nodeRate = 1;
  let nodeName = '';
  if (viewerNodeId) {
    const node = await get(
      `SELECT name,time_offset_minutes,time_rate,time_flow_offset_minutes FROM room_map_nodes WHERE id=? AND room_id=?`,
      [viewerNodeId,roomId]
    );
    if (node) {
      nodeOffset = clampInt(node.time_offset_minutes,-10080,10080,0);
      nodeFlowOffset = clampInt(node.time_flow_offset_minutes,-525600,525600,0);
      nodeRate = Number.isFinite(Number(node.time_rate)) ? Number(node.time_rate) : 1;
      nodeName = String(node.name || '');
    }
  }

  const globalOffset = clampInt(room.time_distortion_offset_minutes,-10080,10080,0);
  const globalFlowOffset = clampInt(room.time_flow_offset_minutes,-525600,525600,0);
  const globalRate = Number.isFinite(Number(room.time_rate)) ? Number(room.time_rate) : 1;
  const actualTotal = roomGameTotal(room);
  const actual = totalMinutesToGameTime(actualTotal);
  const canView = isAdmin || !Boolean(room.time_hidden) || capability.can_view;
  const seesTrueTime = isAdmin || capability.true_time;
  const observedOffset = seesTrueTime ? 0 : globalOffset + globalFlowOffset + nodeOffset + nodeFlowOffset;
  const observed = totalMinutesToGameTime(actualTotal + observedOffset);

  if (!canView) {
    return {
      visible:false,
      display_text:'時間未知',
      source:'hidden',
      item_name:null,
      truthful:false,
      room_time_hidden:Boolean(room.time_hidden),
      viewer_map_node_id:viewerNodeId || null,
      viewer_map_node_name:nodeName || ''
    };
  }

  const response = {
    visible:true,
    display_text:observed.text,
    day:observed.day,
    hour:observed.hour,
    minute:observed.minute,
    source:isAdmin?'dm':(!Boolean(room.time_hidden)?'public':'item'),
    item_name:capability.item_name,
    truthful:Boolean(seesTrueTime),
    room_time_hidden:Boolean(room.time_hidden),
    viewer_map_node_id:viewerNodeId || null,
    viewer_map_node_name:nodeName || ''
  };

  // 普通玩家若只持有一般時計，不會被告知時間已經被扭曲。
  if (isAdmin) {
    response.admin = {
      actual,
      global_offset_minutes:globalOffset,
      global_flow_offset_minutes:globalFlowOffset,
      global_time_rate:globalRate,
      node_offset_minutes:nodeOffset,
      node_flow_offset_minutes:nodeFlowOffset,
      node_time_rate:nodeRate,
      player_observed:totalMinutesToGameTime(actualTotal + globalOffset + globalFlowOffset + nodeOffset + nodeFlowOffset)
    };
  } else if (capability.true_time) {
    response.actual = actual;
    response.distortion_detected = (globalOffset + globalFlowOffset + nodeOffset + nodeFlowOffset) !== 0 || globalRate !== 1 || nodeRate !== 1;
  }

  return response;
}

app.get('/api/rooms/:id/time-view',auth,async(req,res)=>{
  try{
    const view=await buildRoomTimeView(Number(req.params.id),req.user.id);
    res.json(view);
  }catch(error){
    res.status(error.status||500).json({error:error.message||'讀取時間失敗'});
  }
});


/* =========================================================
   V20 COMBAT ENGINE
========================================================= */

const COMBAT_LEVEL_RANK = {
  '失敗': 0,
  '普通成功': 1,
  '困難成功': 2,
  '極難成功': 3,
  '大成功': 4
};

const COMBAT_ATTRIBUTE_ALIASES = {
  strength:'strength', 力量:'strength',
  agility:'agility', 敏捷:'agility',
  constitution:'constitution', 體質:'constitution',
  spirit:'spirit', 精神:'spirit',
  luck:'luck', 幸運:'luck',
  fifth:'fifth', 大道:'fifth', 信仰:'fifth',
  endurance:'endurance', 耐力:'endurance',
  will:'will', 意志:'will',
  sanity:'sanity', 理智:'sanity'
};

function normalizeCombatAttribute(value, fallback='strength') {
  const key = String(value || '').trim();
  return COMBAT_ATTRIBUTE_ALIASES[key] || COMBAT_ATTRIBUTE_ALIASES[key.toLowerCase()] || fallback;
}

function combatAttributeLabel(key) {
  return ({strength:'力量',agility:'敏捷',constitution:'體質',spirit:'精神',luck:'幸運',fifth:'大道／信仰',endurance:'耐力',will:'意志',sanity:'理智'})[key] || key;
}

function skillCombatConfig(skill, character = null) {
  const data = parseJsonField(skill && skill.data, {});
  const modifier = character ? equipmentSkillModifierForCharacter(character, skill) : {damage_multiplier:1,turn_cost_delta:0,charge_turns_delta:0,sources:[]};
  const baseTurnCost = data.turn_cost === 0 ? 0 : 1;
  const baseChargeTurns = Math.max(0, Math.min(20, Math.floor(Number(data.charge_turns) || 0)));
  const baseDamageMultiplier = Math.max(0, Number(data.damage_multiplier) || 1);
  const combatAttack = Boolean(data.combat_attack || String(skill && skill.damage_formula || '').trim());
  const targetMode = ['enemy_single','enemy_all','ally_single','ally_all','self'].includes(String(data.combat_target_mode || ''))
    ? String(data.combat_target_mode)
    : (combatAttack ? 'enemy_single' : 'self');
  const effectsTargetMode = ['self','target'].includes(String(data.effects_target_mode || ''))
    ? String(data.effects_target_mode)
    : 'self';
  return {
    turn_cost: Math.max(0, Math.min(1, baseTurnCost + (Number(modifier.turn_cost_delta) || 0))),
    charge_turns: Math.max(0, Math.min(20, baseChargeTurns + (Number(modifier.charge_turns_delta) || 0))),
    damage_multiplier: Math.max(0, baseDamageMultiplier * (Number(modifier.damage_multiplier) || 1)),
    equipment_modifier_sources: Array.isArray(modifier.sources) ? modifier.sources : [],
    riding_skill: Boolean(data.riding_skill),
    investigation_skill: Boolean(data.investigation_skill),
    investigation_attribute: ['spirit','agility','luck','fifth'].includes(String(data.investigation_attribute || ''))
      ? String(data.investigation_attribute)
      : 'spirit',
    investigation_base_rate: Math.max(5, Math.min(95, Math.floor(Number(data.investigation_base_rate) || 40))),
    investigation_bonus: Math.max(-100, Math.min(100, Math.floor(Number(data.investigation_bonus) || 0))),
    combat_attack: combatAttack,
    combat_target_mode: targetMode,
    effects_target_mode: effectsTargetMode,
    effects_on_hit: Boolean(data.effects_on_hit),
    reaction_trigger: ['none','self_attacked','ally_attacked','dodge_success','defense_success','hit_taken','ally_downed'].includes(String(data.reaction_trigger||'')) ? String(data.reaction_trigger) : 'none',
    reaction_kind: ['none','counter','guard','intercept','substitute','effect'].includes(String(data.reaction_kind||'')) ? String(data.reaction_kind) : 'none',
    reaction_limit_per_round: Math.max(1,Math.min(9,Math.floor(Number(data.reaction_limit_per_round)||1))),
    reaction_damage_multiplier: Math.max(0,Math.min(10,Number(data.reaction_damage_multiplier)||1)),
    reaction_guard_percent: Math.max(0,Math.min(95,Number(data.reaction_guard_percent)||30)),
    combat_check_attribute: normalizeCombatAttribute(data.combat_check_attribute || skill?.check_name || 'strength','strength'),
    combat_accuracy_bonus: Math.max(-10000, Math.min(10000, Number(data.combat_accuracy_bonus) || 0)),
    combat_element: ELEMENTS.includes(String(data.combat_element || '')) ? String(data.combat_element) : '',
    combat_damage_target: ['hp','spirit','sanity','fifth'].includes(String(data.combat_damage_target || '')) ? String(data.combat_damage_target) : 'hp'
  };
}

function combatPlayerStat(character, key) {
  const k = normalizeCombatAttribute(key,'strength');
  if (k === 'strength') return Math.max(0, Number(character.final_strength) || 0);
  if (k === 'agility') return Math.max(0, Number(character.final_agility) || 0);
  if (k === 'constitution') return Math.max(0, Number(character.final_constitution) || 0);
  if (k === 'spirit') return Math.max(0, Number(character.final_spirit) || 0);
  if (k === 'luck') return Math.max(0, Number(character.final_luck ?? character.luck) || 0);
  if (k === 'fifth') return Math.max(0, Number(character.fifth_current ?? character.fifth_max) || 0);
  if (k === 'endurance') return Math.max(0, Number(character.endurance) || 0);
  if (k === 'will') return Math.max(0, Number(character.will) || 0);
  if (k === 'sanity') return Math.max(0, Number(character.sanity) || 0);
  return 0;
}

function combatEntityStat(entity, key) {
  const k = normalizeCombatAttribute(key,'strength');
  const attrs = entity && entity.final_attributes && typeof entity.final_attributes === 'object' ? entity.final_attributes : (entity && entity.attributes) || {};
  if (k === 'fifth') return Math.max(0, Number(attrs.great_way ?? attrs.faith ?? attrs.fifth) || 0);
  if (k === 'endurance') return Math.max(0, Math.floor(((Number(attrs.constitution)||0)+(Number(attrs.spirit)||0))/2));
  if (k === 'will') return Math.max(0, Math.floor((Number(attrs.spirit)||0)*0.8));
  if (k === 'sanity') return Math.max(0, Math.floor((Number(attrs.spirit)||0)*1.2));
  return Math.max(0, Number(attrs[k]) || 0);
}

function combatSuccessLevel(roll, target) {
  const t = Math.max(0, Number(target) || 0);
  if (roll === 1) return '大成功';
  if (t <= 0) return '失敗';
  if (roll <= Math.max(1, Math.floor(t / 5))) return '極難成功';
  if (roll <= Math.max(1, Math.floor(t / 2))) return '困難成功';
  if (roll <= t) return '普通成功';
  return '失敗';
}

function rollCombatCheck(targetValue, bonus = 0) {
  const target = Math.max(0, Math.floor((Number(targetValue) || 0) + (Number(bonus) || 0)));
  const roll = 1 + Math.floor(Math.random() * 100);
  return {roll, target, level:combatSuccessLevel(roll,target)};
}

function combatAttackHits(attackLevel, reactionLevel, reactionType) {
  const a = COMBAT_LEVEL_RANK[attackLevel] ?? 0;
  const d = COMBAT_LEVEL_RANK[reactionLevel] ?? 0;
  if (a <= 0) return false;
  if (d <= 0) return true;
  // 自訂規則：大成功攻擊 VS 大成功防禦，攻擊方勝。
  if (attackLevel === '大成功' && reactionLevel === '大成功' && reactionType === 'defense') return true;
  return a > d;
}

function safeArithmeticEval(expression) {
  const tokens = String(expression||'').match(/\d+(?:\.\d+)?|[()+\-*/]/g) || [];
  let i=0;
  function primary(){
    const t=tokens[i++];
    if(t==='('){const v=expr();if(tokens[i]!==')')throw new Error('括號不完整');i++;return v;}
    if(t==='-')return -primary();
    if(t==='+')return primary();
    const n=Number(t);if(!Number.isFinite(n))throw new Error('公式包含無法辨識內容');return n;
  }
  function term(){let v=primary();while(tokens[i]==='*'||tokens[i]==='/'){const op=tokens[i++];const r=primary();v=op==='*'?v*r:(r===0?0:v/r);}return v;}
  function expr(){let v=term();while(tokens[i]==='+'||tokens[i]==='-'){const op=tokens[i++];const r=term();v=op==='+'?v+r:v-r;}return v;}
  if(!tokens.length)return 0;
  const v=expr();
  if(i!==tokens.length)throw new Error('公式格式錯誤');
  return Number.isFinite(v)?v:0;
}

function combatFormulaStats(source, type='player') {
  const getter = type==='player' ? key=>combatPlayerStat(source,key) : key=>combatEntityStat(source,key);
  return {
    力量:getter('strength'),敏捷:getter('agility'),體質:getter('constitution'),精神:getter('spirit'),幸運:getter('luck'),
    大道:getter('fifth'),信仰:getter('fifth'),耐力:getter('endurance'),意志:getter('will'),理智:getter('sanity')
  };
}

function rollDamageFormula(formula, source, type='player') {
  let text=String(formula||'').trim();
  if(!text)return {total:0,formula:'',rolls:[]};
  text=text.replace(/[×xX]/g,'*').replace(/÷/g,'/');
  const stats=combatFormulaStats(source,type);
  const rolls=[];
  text=text.replace(/(\d{1,2})\s*[dD]\s*(力量|敏捷|體質|精神|幸運|大道|信仰|耐力|意志|理智|\d{1,6})/g,(m,countRaw,sidesRaw)=>{
    const count=Math.max(1,Math.min(20,Number(countRaw)||1));
    const sides=stats[sidesRaw]!==undefined?Math.max(1,Math.floor(stats[sidesRaw])):Math.max(1,Math.min(100000,Number(sidesRaw)||1));
    const values=Array.from({length:count},()=>1+Math.floor(Math.random()*sides));
    rolls.push({notation:`${count}D${sidesRaw}`,values,total:values.reduce((a,b)=>a+b,0)});
    return String(values.reduce((a,b)=>a+b,0));
  });
  for(const [name,value] of Object.entries(stats)) text=text.replace(new RegExp(name,'g'),String(value));
  if(!/^[\d\s.+\-*/()]+$/.test(text))throw Object.assign(new Error('傷害公式只支援數字、屬性、骰式與 + - × ÷ 括號'),{status:400});
  const total=Math.max(0,Math.floor(safeArithmeticEval(text)));
  return {total,formula:String(formula||''),rolls};
}

function monsterReactionMode(monster) {
  const cfg=parseWorldConfig(monster && monster.config);
  const behavior=String(monster && monster.ai_behavior || 'balanced');
  if(behavior==='defensive')return 'defense';
  if(behavior==='aggressive')return 'dodge';
  const defense=combatEntityStat(monster,normalizeCombatAttribute(cfg.combat_defense_attribute||'constitution','constitution'));
  const dodge=combatEntityStat(monster,normalizeCombatAttribute(cfg.combat_dodge_attribute||'agility','agility'));
  return defense>=dodge?'defense':'dodge';
}

async function loadCombatEntityStatuses(roomId, entityType, entityId) {
  return all(
    `SELECT * FROM combat_entity_statuses WHERE room_id=? AND entity_type=? AND entity_id=? AND active=TRUE ORDER BY id ASC`,
    [roomId, entityType, entityId]
  );
}

function combatStatusPassiveEffects(statuses) {
  const list=[];
  for(const status of Array.isArray(statuses)?statuses:[]){
    const stacks=Math.max(1,Number(status.stacks)||1);
    for(const effect of normalizeUnifiedEffects(parseJsonField(status.effects,{}))){
      if(effect.timing!=='passive')continue;
      list.push({...effect,value:(Number(effect.value)||0)*stacks});
    }
  }
  return list;
}

async function decorateCombatWorldEntity(row, entityType, entityId, roomId) {
  const statuses=await loadCombatEntityStatuses(roomId,entityType,entityId);
  const templateEffects=normalizeUnifiedEffects(parseJsonField(row.effects,{}));
  const combinedEffects={list:[...templateEffects,...combatStatusPassiveEffects(statuses)]};
  const parsed=parseWorldEntityRow(row);
  const recalculated=calculateEntityEffectStats(
    parseJsonField(row.attributes,{}),
    parseJsonField(row.element_affinity,{}),
    combinedEffects
  );
  const finalAttrs=recalculated.final_attributes||{};
  const derivedSpirit=Math.max(0,Number(finalAttrs.spirit)||0);
  const derivedFifth=Math.max(0,Number(finalAttrs.great_way??finalAttrs.faith??finalAttrs.fifth)||0);
  const maxHp=Math.max(1,Number(row.max_hp)||Math.max(1,(Number(finalAttrs.constitution)||0)*2)||100);
  const maxSpirit=Math.max(0,Number(row.max_spirit)>0?Number(row.max_spirit):derivedSpirit);
  const maxFifth=Math.max(0,Number(row.max_fifth)>0?Number(row.max_fifth):derivedFifth);
  const currentSpirit=Number(row.max_spirit)>0?Math.max(0,Number(row.current_spirit)||0):maxSpirit;
  const currentFifth=Number(row.max_fifth)>0?Math.max(0,Number(row.current_fifth)||0):maxFifth;
  return {
    ...parsed,
    ...recalculated,
    combat_statuses:statuses.map(st=>({...st,effects:parseJsonField(st.effects,{})})),
    current_hp:Math.max(0,Number(row.current_hp)||0),
    max_hp:maxHp,
    current_spirit:currentSpirit,
    max_spirit:maxSpirit,
    current_fifth:currentFifth,
    max_fifth:maxFifth
  };
}

async function getCombatMonster(roomId, monsterId, {allowDefeated=false}={}) {
  const row=await get(`SELECT rm.*,mt.name,mt.category,mt.rank,mt.attributes,mt.element_affinity,mt.effects,mt.skills,mt.ai_level,mt.ai_behavior,mt.config FROM room_monsters rm JOIN monster_templates mt ON mt.id=rm.monster_template_id WHERE rm.id=? AND rm.room_id=?`,[monsterId,roomId]);
  if(!row)throw Object.assign(new Error('找不到戰鬥目標'),{status:404});
  if(!allowDefeated && row.status!=='encountered')throw Object.assign(new Error('這個怪物目前不是可攻擊目標'),{status:400});
  if(!allowDefeated && Number(row.current_hp)<=0)throw Object.assign(new Error('這個怪物已經失去戰鬥能力'),{status:400});
  return {...await decorateCombatWorldEntity(row,'monster',row.id,roomId),room_monster_id:row.id,combat_side:'enemy'};
}

async function getCombatNpc(roomId, npcTemplateId, {allowDowned=false}={}) {
  const row=await get(`SELECT rn.*,nt.name,nt.role_name,nt.attributes,nt.element_affinity,nt.effects,nt.skills,nt.ai_level,nt.ai_behavior,nt.config,nt.max_hp AS template_max_hp FROM room_npcs rn JOIN npc_templates nt ON nt.id=rn.npc_template_id WHERE rn.room_id=? AND rn.npc_template_id=?`,[roomId,npcTemplateId]);
  if(!row)throw Object.assign(new Error('找不到 NPC 戰鬥單位'),{status:404});
  if(!allowDowned && row.status!=='active')throw Object.assign(new Error('這個 NPC 目前無法行動'),{status:400});
  if(!allowDowned && Number(row.current_hp)<=0)throw Object.assign(new Error('這個 NPC 已經倒地'),{status:400});
  const entity=await decorateCombatWorldEntity({...row,max_hp:Number(row.max_hp)||Number(row.template_max_hp)||100},'npc',row.npc_template_id,roomId);
  return {...entity,npc_template_id:row.npc_template_id,combat_side:['ally','enemy'].includes(String(row.combat_side))?String(row.combat_side):'neutral'};
}

async function getBattlePlayers(roomId) {
  const rows=await all(`SELECT rm.*,cc.* FROM room_members rm JOIN character_cards cc ON cc.user_id=rm.user_id WHERE rm.room_id=? AND rm.role='player'`,[roomId]);
  return rows.map(row=>({member:row,character:parseCharacter(row)}));
}

async function getBattlePlayer(roomId,userId,{allowDowned=false}={}){
  const players=await getBattlePlayers(roomId);
  const target=players.find(p=>String(p.member.user_id)===String(userId));
  if(!target)throw Object.assign(new Error('找不到玩家戰鬥單位'),{status:404});
  if(!allowDowned && Number(target.member.hp)<=0)throw Object.assign(new Error('這名玩家目前已倒地'),{status:400});
  return target;
}

function combatTargetSide(target){
  if(!target)return 'neutral';
  if(target.type==='player')return 'ally';
  if(target.type==='monster')return 'enemy';
  if(target.type==='npc')return target.entity?.combat_side||target.combat_side||'neutral';
  return 'neutral';
}

function combatActorSide(actor){
  if(!actor)return 'neutral';
  if(actor.type==='player'||actor.type==='summon')return 'ally';
  if(actor.type==='monster')return 'enemy';
  if(actor.type==='npc')return actor.side||actor.combat_side||'neutral';
  return 'neutral';
}

async function getCombatTarget(roomId,targetType,targetId,{allowDowned=false}={}){
  if(targetType==='player'){
    const p=await getBattlePlayer(roomId,targetId,{allowDowned});
    return {type:'player',id:Number(p.member.user_id),name:p.character.name,member:p.member,character:p.character,side:'ally'};
  }
  if(targetType==='npc'){
    const entity=await getCombatNpc(roomId,targetId,{allowDowned});
    return {type:'npc',id:Number(entity.npc_template_id),name:entity.name,entity,side:entity.combat_side};
  }
  const entity=await getCombatMonster(roomId,targetId,{allowDefeated:allowDowned});
  return {type:'monster',id:Number(entity.room_monster_id),name:entity.name,entity,side:'enemy'};
}

async function listCombatTargets(roomId){
  const out=[];
  for(const p of await getBattlePlayers(roomId)){
    out.push({type:'player',id:Number(p.member.user_id),name:p.character.name,side:'ally',alive:Number(p.member.hp)>0,member:p.member,character:p.character});
  }
  const monsters=await all(`SELECT id FROM room_monsters WHERE room_id=? AND status IN ('encountered','defeated')`,[roomId]);
  for(const row of monsters){
    const m=await getCombatMonster(roomId,row.id,{allowDefeated:true});
    out.push({type:'monster',id:Number(row.id),name:m.name,side:'enemy',alive:m.status==='encountered'&&Number(m.current_hp)>0,entity:m});
  }
  const npcs=await all(`SELECT npc_template_id FROM room_npcs WHERE room_id=? AND combat_side IN ('ally','enemy')`,[roomId]);
  for(const row of npcs){
    const n=await getCombatNpc(roomId,row.npc_template_id,{allowDowned:true});
    out.push({type:'npc',id:Number(row.npc_template_id),name:n.name,side:n.combat_side,alive:n.status==='active'&&Number(n.current_hp)>0,entity:n});
  }
  return out;
}

async function combatTargetsForActor(roomId,actor,{relation='enemy',includeDowned=false}={}){
  const side=combatActorSide(actor);
  const allTargets=await listCombatTargets(roomId);
  return allTargets.filter(target=>{
    const same=target.side===side;
    if(relation==='self')return target.type===actor.type&&String(target.id)===String(actor.id);
    if(relation==='ally'&&!same)return false;
    if(relation==='enemy'&&same)return false;
    if(target.side==='neutral')return false;
    return includeDowned?true:Boolean(target.alive);
  });
}

async function adjustCombatActions(roomId,type,id,delta) {
  const amount=Math.floor(Number(delta)||0);if(!amount)return;
  if(type==='player')await run(`UPDATE room_members SET turn_actions_remaining=GREATEST(0,turn_actions_remaining+?) WHERE room_id=? AND user_id=?`,[amount,roomId,id]);
  else if(type==='monster')await run(`UPDATE room_monsters SET turn_actions_remaining=GREATEST(0,turn_actions_remaining+?) WHERE room_id=? AND id=?`,[amount,roomId,id]);
  else if(type==='npc')await run(`UPDATE room_npcs SET turn_actions_remaining=GREATEST(0,turn_actions_remaining+?) WHERE room_id=? AND npc_template_id=?`,[amount,roomId,id]);
}

function combatEntityReactionMode(entity) {
  const cfg=parseWorldConfig(entity&&entity.config);
  const behavior=String(entity&&entity.ai_behavior||'balanced');
  if(behavior==='defensive')return 'defense';
  if(behavior==='aggressive')return 'dodge';
  const defense=combatEntityStat(entity,normalizeCombatAttribute(cfg.combat_defense_attribute||'constitution','constitution'));
  const dodge=combatEntityStat(entity,normalizeCombatAttribute(cfg.combat_dodge_attribute||'agility','agility'));
  return defense>=dodge?'defense':'dodge';
}

function combatTargetReaction(target){
  if(target.type==='player'){
    const type=String(target.member.combat_reaction||'dodge')==='defense'?'defense':'dodge';
    const attr=type==='defense'?'constitution':'agility';
    return {type,attribute:attr,check:rollCombatCheck(combatPlayerStat(target.character,attr),0)};
  }
  const type=combatEntityReactionMode(target.entity);
  const cfg=parseWorldConfig(target.entity.config);
  const attr=type==='defense'
    ? normalizeCombatAttribute(cfg.combat_defense_attribute||'constitution','constitution')
    : normalizeCombatAttribute(cfg.combat_dodge_attribute||'agility','agility');
  return {type,attribute:attr,check:rollCombatCheck(combatEntityStat(target.entity,attr),0)};
}


function combatSkillIdentity(skill){
  return String(skill?.id??skill?.template_id??'').trim();
}
function isConfiguredReactionSkill(skill){
  const cfg=skillCombatConfig(skill);
  return ['reaction'].includes(String(skill?.skill_type||skill?.type||''))||cfg.reaction_kind!=='none'||cfg.reaction_trigger!=='none';
}
async function selectedPlayerReactionSkill(roomId,userId){
  const member=await get(`SELECT combat_reaction_skill_id,combat_reaction_round,combat_reaction_uses,hp,combat_state FROM room_members WHERE room_id=? AND user_id=?`,[roomId,userId]);
  if(!member||Number(member.hp)<=0||String(member.combat_state||'active')==='escaped')return null;
  const selected=String(member.combat_reaction_skill_id||'').trim();if(!selected)return null;
  const character=await getCharacterByUser(userId);if(!character)return null;
  const skills=Array.isArray(character.skills)?character.skills:[];
  let skill=skills.find(sk=>combatSkillIdentity(sk)===selected||String(sk?.template_id||'')===selected);
  if(!skill){const granted=new Set((character.equipment_granted_skill_template_ids||[]).map(x=>String(x)));if(granted.has(selected)){const template=await get(`SELECT * FROM skill_templates WHERE id=?`,[Number(selected)]);if(template)skill=buildCharacterSkillFromTemplate(template,{type:'equipment_reaction'});}}
  if(!skill||!isConfiguredReactionSkill(skill))return null;
  return {member,character,skill,config:skillCombatConfig(skill,character)};
}
async function reactionUseAvailable(roomId,userId,config){
  const room=await get(`SELECT round FROM rooms WHERE id=?`,[roomId]);const round=Math.max(1,Number(room?.round)||1);
  const row=await get(`SELECT combat_reaction_round,combat_reaction_uses FROM room_members WHERE room_id=? AND user_id=?`,[roomId,userId]);
  const uses=Number(row?.combat_reaction_round)===round?Math.max(0,Number(row?.combat_reaction_uses)||0):0;
  return {round,uses,available:uses<Math.max(1,Number(config?.reaction_limit_per_round)||1)};
}
async function consumeReactionUse(roomId,userId,config){
  const state=await reactionUseAvailable(roomId,userId,config);if(!state.available)return false;
  await run(`UPDATE room_members SET combat_reaction_round=?,combat_reaction_uses=? WHERE room_id=? AND user_id=?`,[state.round,state.uses+1,roomId,userId]);return true;
}
async function resolveProtectivePlayerReaction(roomId,target){
  if(combatTargetSide(target)!=='ally')return {target,guard:null,reaction:null};
  const players=await getBattlePlayers(roomId);const guards=[];
  for(const p of players){
    const uid=Number(p.member.user_id);if(Number(p.member.hp)<=0||String(p.member.combat_state||'active')==='escaped'||(target.type==='player'&&String(uid)===String(target.id)))continue;
    const selected=await selectedPlayerReactionSkill(roomId,uid);if(!selected)continue;
    const cfg=selected.config;if(cfg.reaction_trigger!=='ally_attacked'||!['guard','intercept','substitute'].includes(cfg.reaction_kind))continue;
    const use=await reactionUseAvailable(roomId,uid,cfg);if(!use.available)continue;
    if(cfg.reaction_kind==='guard'){
      guards.push({userId:uid,character:selected.character,skill:selected.skill,config:cfg,percent:cfg.reaction_guard_percent});continue;
    }
    if(cfg.reaction_kind==='intercept'){
      const attr=cfg.combat_check_attribute||'agility';const check=rollCombatCheck(combatPlayerStat(selected.character,attr),cfg.combat_accuracy_bonus||0);
      if((COMBAT_LEVEL_RANK[check.level]||0)<=0){await addEvent(roomId,uid,'reaction',{text:`🛡️ ${selected.character.name} 嘗試以「${selected.skill.name||'攔截'}」攔截攻擊，但判定 ${check.roll}/${check.target} 失敗。`,reaction_kind:'intercept',success:false});continue;}
      if(!(await consumeReactionUse(roomId,uid,cfg)))continue;
      const redirected=await getCombatTarget(roomId,'player',uid);
      await addEvent(roomId,uid,'reaction',{text:`🛡️ ${selected.character.name} 發動「${selected.skill.name||'攔截'}」成功攔截，代替 ${target.name} 成為攻擊目標。`,reaction_kind:'intercept',success:true,original_target:{type:target.type,id:target.id},new_target:{type:'player',id:uid}});
      return {target:redirected,guard:null,reaction:{kind:'intercept',user_id:uid,skill:selected.skill}};
    }
    if(cfg.reaction_kind==='substitute'){
      if(!(await consumeReactionUse(roomId,uid,cfg)))continue;
      const redirected=await getCombatTarget(roomId,'player',uid);
      await addEvent(roomId,uid,'reaction',{text:`🩸 ${selected.character.name} 發動「${selected.skill.name||'替命'}」，代替 ${target.name} 承受這次攻擊。`,reaction_kind:'substitute',original_target:{type:target.type,id:target.id},new_target:{type:'player',id:uid}});
      return {target:redirected,guard:null,reaction:{kind:'substitute',user_id:uid,skill:selected.skill}};
    }
  }
  guards.sort((a,b)=>Number(b.percent)-Number(a.percent));
  return {target,guard:guards[0]||null,reaction:null};
}
async function triggerPlayerReactionEffect({roomId,userId,selected,trigger,attackerTarget=null,allyTarget=null}){
  if(!selected||selected.config.reaction_trigger!==trigger)return null;
  if(!(await consumeReactionUse(roomId,userId,selected.config)))return null;
  const kind=selected.config.reaction_kind;const skill=selected.skill;const effects=parseJsonField(skill.data,{}).effects||[];
  if(kind==='counter'&&attackerTarget){
    const cfg={...selected.config,combat_attack:true,combat_target_mode:'enemy_single',damage_multiplier:Math.max(0,Number(selected.config.damage_multiplier)||1)*Math.max(0,Number(selected.config.reaction_damage_multiplier)||1)};
    const result=await resolveCombatAttack({roomId,attackerType:'player',attackerId:userId,attackerUserId:userId,attackerName:selected.character.name,attackerSource:selected.character,skill:{...skill,damage_formula:skill.damage_formula||'1D力量'},config:cfg,targetType:attackerTarget.type,targetId:attackerTarget.id,damageMultiplier:1,allowReactions:false});
    await addEvent(roomId,userId,'reaction',{text:`⚔️ ${selected.character.name} 觸發反擊「${skill.name||'反擊'}」${result.hit?`，造成 ${result.damage} 傷害`:'，但未命中'}。`,reaction_kind:'counter',trigger});return result;
  }
  if(kind==='effect'){
    const target=selected.config.effects_target_mode==='target'?(allyTarget||attackerTarget):await getCombatTarget(roomId,'player',userId,{allowDowned:true});
    if(!target)return null;const result=await applyEffectsToCombatTarget({roomId,target,effects,source:`反應：${skill.name||'技能'}`});
    await addEvent(roomId,userId,'reaction',{text:`✨ ${selected.character.name} 觸發「${skill.name||'反應技能'}」${result.message_parts?.length?`【${result.message_parts.join('、')}】`:''}`,reaction_kind:'effect',trigger});return result;
  }
  return null;
}
async function processPostAttackPlayerReactions({roomId,target,reactionInfo,hit,damageChange,attackerType,attackerId}){
  const attackerTarget=['monster','npc'].includes(attackerType)?await getCombatTarget(roomId,attackerType,attackerId,{allowDowned:true}).catch(()=>null):null;
  if(target.type==='player'){
    const selected=await selectedPlayerReactionSkill(roomId,target.id);
    if(selected){
      let trigger='self_attacked';
      if(hit)trigger='hit_taken';
      else if(reactionInfo.type==='dodge'&&(COMBAT_LEVEL_RANK[reactionInfo.check.level]||0)>0)trigger='dodge_success';
      else if(reactionInfo.type==='defense'&&(COMBAT_LEVEL_RANK[reactionInfo.check.level]||0)>0)trigger='defense_success';
      if(selected.config.reaction_trigger===trigger)await triggerPlayerReactionEffect({roomId,userId:target.id,selected,trigger,attackerTarget});
      else if(selected.config.reaction_trigger==='self_attacked')await triggerPlayerReactionEffect({roomId,userId:target.id,selected,trigger:'self_attacked',attackerTarget});
    }
  }
  if(damageChange?.downed&&combatTargetSide(target)==='ally'){
    const players=await getBattlePlayers(roomId);
    for(const p of players){const uid=Number(p.member.user_id);if(target.type==='player'&&String(uid)===String(target.id))continue;if(Number(p.member.hp)<=0||String(p.member.combat_state||'active')==='escaped')continue;const selected=await selectedPlayerReactionSkill(roomId,uid);if(!selected||selected.config.reaction_trigger!=='ally_downed')continue;await triggerPlayerReactionEffect({roomId,userId:uid,selected,trigger:'ally_downed',attackerTarget,allyTarget:await getCombatTarget(roomId,target.type,target.id,{allowDowned:true})});}
  }
}

async function persistCombatEntityResources(roomId,type,id,values){
  const safe={};
  for(const key of ['current_hp','current_spirit','current_fifth'])if(values[key]!==undefined)safe[key]=Math.max(0,Math.floor(Number(values[key])||0));
  const keys=Object.keys(safe);if(!keys.length)return;
  const assignments=keys.map(k=>`${k}=?`);
  const params=keys.map(k=>safe[k]);
  if(type==='monster'){
    if(safe.current_hp!==undefined){assignments.push(`status=CASE WHEN ?<=0 THEN 'defeated' ELSE CASE WHEN status='defeated' THEN 'encountered' ELSE status END END`);params.push(safe.current_hp);}
    params.push(roomId,id);
    await run(`UPDATE room_monsters SET ${assignments.join(',')},updated_at=CURRENT_TIMESTAMP WHERE room_id=? AND id=?`,params);
    if(safe.current_hp!==undefined)await checkMonsterBossPhase(roomId,id,'hp');
  }else if(type==='npc'){
    if(safe.current_hp!==undefined){assignments.push(`status=CASE WHEN ?<=0 THEN 'downed' ELSE 'active' END`);params.push(safe.current_hp);}
    params.push(roomId,id);
    await run(`UPDATE room_npcs SET ${assignments.join(',')},updated_at=CURRENT_TIMESTAMP WHERE room_id=? AND npc_template_id=?`,params);
  }
}

async function applyCombatDamageToTarget(roomId,target,damageTarget,amount){
  const dmg=Math.max(0,Math.floor(Number(amount)||0));
  const kind=['hp','spirit','sanity','fifth'].includes(String(damageTarget))?String(damageTarget):'hp';
  if(target.type==='player'){
    if(kind==='sanity'){
      const result=await applyPlayerSanityChange(roomId,target.id,{delta:-dmg,source:'combat'});return {kind,before:result.before,after:result.after,sanity_stage:result.stage?.key,seven_sin:result.seven_sin};
    }
    if(kind==='spirit'){
      const next=Math.max(0,Number(target.member.spirit)-dmg);await run(`UPDATE room_members SET spirit=? WHERE room_id=? AND user_id=?`,[next,roomId,target.id]);return {kind,before:Number(target.member.spirit)||0,after:next};
    }
    if(kind==='fifth'){
      const field=target.character.faction==='西國'?'current_faith':'current_great_way';
      const before=Math.max(0,Number(target.character[field])||0);const next=Math.max(0,before-dmg);await run(`UPDATE character_cards SET ${field}=?,updated_at=CURRENT_TIMESTAMP WHERE user_id=?`,[next,target.id]);return {kind,before,after:next};
    }
    const before=Math.max(0,Number(target.member.hp)||0),next=Math.max(0,before-dmg);
    await run(`UPDATE room_members SET hp=?,combat_state=CASE WHEN ?>0 THEN 'active' ELSE 'downed' END WHERE room_id=? AND user_id=?`,[next,next,roomId,target.id]);
    return {kind:'hp',before,after:next,downed:next<=0};
  }
  const entity=target.entity;
  const field=(kind==='spirit'||kind==='sanity')?'current_spirit':kind==='fifth'?'current_fifth':'current_hp';
  const before=Math.max(0,Number(entity[field])||0),next=Math.max(0,before-dmg);
  await persistCombatEntityResources(roomId,target.type,target.id,{[field]:next});
  return {kind,before,after:next,downed:kind==='hp'&&next<=0};
}

async function resolveCombatAttack({roomId,attackerType,attackerId=null,attackerUserId=null,attackerName,attackerSource,skill,config,targetType,targetId,damageMultiplier=1,allowReactions=true}){
  const originalTarget=await getCombatTarget(roomId,targetType,targetId);
  const attackerActor={type:attackerType==='summon'?'summon':attackerType,id:attackerId||attackerUserId,side:attackerType==='npc'?attackerSource?.combat_side:undefined};
  if(combatActorSide(attackerActor)===combatTargetSide(originalTarget))throw Object.assign(new Error('不能攻擊同一陣營的目標'),{status:400});
  const protection=allowReactions?await resolveProtectivePlayerReaction(roomId,originalTarget):{target:originalTarget,guard:null,reaction:null};
  const target=protection.target;
  const checkAttr=normalizeCombatAttribute(config.combat_check_attribute||skill?.check_name||'strength','strength');
  const attackStat=attackerType==='player'?combatPlayerStat(attackerSource,checkAttr):combatEntityStat(attackerSource,checkAttr);
  const attack=rollCombatCheck(attackStat,config.combat_accuracy_bonus||0);
  const reactionInfo=combatTargetReaction(target);const reaction=reactionInfo.check;
  const hit=combatAttackHits(attack.level,reaction.level,reactionInfo.type);
  let baseDamage=0,formulaResult={total:0,formula:'',rolls:[]},finalDamage=0,damageChange=null;
  const element=ELEMENTS.includes(config.combat_element)?config.combat_element:'';
  let affinityMultiplier=1,counterMultiplier=1,guardMultiplier=1;
  if(hit){
    formulaResult=rollDamageFormula(skill?.damage_formula||'1D力量',attackerSource,attackerType==='player'?'player':'entity');
    baseDamage=formulaResult.total;
    if(element){
      affinityMultiplier=attackerType==='player'
        ? Math.max(0,Number(attackerSource?.element_damage_multiplier?.[element])||1)
        : Math.max(0,1+(Number(attackerSource?.element_damage_bonus_pct?.[element])||0)/100);
      if(target.type!=='player')counterMultiplier=getElementDamageMultiplier(element,String(parseWorldConfig(target.entity.config).element||''));
    }
    const reactionMultiplier=reactionInfo.type==='defense'?0.8:1;
    if(protection.guard&&await consumeReactionUse(roomId,protection.guard.userId,protection.guard.config)){
      guardMultiplier=Math.max(0,1-(Number(protection.guard.percent)||0)/100);
      await addEvent(roomId,protection.guard.userId,'reaction',{text:`🛡️ ${protection.guard.character.name} 發動「${protection.guard.skill.name||'援護'}」，使 ${target.name} 本次受到的傷害降低 ${Math.round((1-guardMultiplier)*100)}%。`,reaction_kind:'guard',target_type:target.type,target_id:target.id});
    }
    finalDamage=Math.max(0,Math.floor(baseDamage*Math.max(0,Number(config.damage_multiplier)||1)*Math.max(0,Number(damageMultiplier)||1)*affinityMultiplier*counterMultiplier*reactionMultiplier*guardMultiplier));
    damageChange=await applyCombatDamageToTarget(roomId,target,config.combat_damage_target||'hp',finalDamage);
  }
  const attackerActionType=attackerType==='summon'?'player':attackerType;
  const attackerActionId=attackerType==='summon'?attackerUserId:attackerId;
  if(attack.level==='大成功'&&attackerActionId)await adjustCombatActions(roomId,attackerActionType,attackerActionId,1);
  if(reaction.level==='大成功')await adjustCombatActions(roomId,target.type,target.id,1);
  const reactionLabel=reactionInfo.type==='defense'?'防禦':'閃避';
  const targetStateText=damageChange?.downed?'，目標倒地／失去戰鬥能力':'';
  const redirectText=String(originalTarget.id)!==String(target.id)||originalTarget.type!==target.type?`（原目標 ${originalTarget.name}，由 ${target.name} 代替承受）`:'';
  const guardText=guardMultiplier<1?`（援護減傷 ${Math.round((1-guardMultiplier)*100)}%）`:'';
  const text=`${attackerType==='monster'?'👹 ':attackerType==='npc'?'🧑 ':''}${attackerName} 攻擊「${target.name}」${redirectText}：攻擊 ${attack.roll}/${attack.target} ${attack.level} VS ${reactionLabel} ${reaction.roll}/${reaction.target} ${reaction.level} → ${hit?`命中，造成 ${finalDamage} ${config.combat_damage_target==='sanity'?'理智':config.combat_damage_target==='spirit'?'精神':config.combat_damage_target==='fifth'?'大道／信仰':'HP'}傷害${element?`（${element}）`:''}${guardText}`:'未命中'}${targetStateText}`;
  await addEvent(roomId,attackerType==='player'||attackerType==='summon'?attackerUserId:null,'combat_attack',{text,attacker_type:attackerType,attacker_id:attackerId,attacker_name:attackerName,target_type:target.type,target_id:target.id,target_name:target.name,original_target:{type:originalTarget.type,id:originalTarget.id,name:originalTarget.name},attack,reaction:{...reaction,type:reactionInfo.type},hit,base_damage:baseDamage,damage:finalDamage,damage_target:config.combat_damage_target||'hp',damage_change:damageChange,formula:formulaResult,element,affinity_multiplier:affinityMultiplier,counter_multiplier:counterMultiplier,guard_multiplier:guardMultiplier});
  if(allowReactions)await processPostAttackPlayerReactions({roomId,target,reactionInfo,hit,damageChange,attackerType,attackerId});
  return {attacker:{type:attackerType,id:attackerId,name:attackerName},target:{type:target.type,id:target.id,name:target.name},original_target:{type:originalTarget.type,id:originalTarget.id,name:originalTarget.name},attack,reaction:{...reaction,type:reactionInfo.type},hit,base_damage:baseDamage,damage:finalDamage,damage_target:config.combat_damage_target||'hp',damage_change:damageChange,formula:formulaResult,element,affinity_multiplier:affinityMultiplier,counter_multiplier:counterMultiplier,guard_multiplier:guardMultiplier};
}

async function applyStatusTemplateToCombatEntity(roomId,entityType,entityId,templateOrName,source='系統',durationOverride=null){
  let template=null;
  if(Number(templateOrName))template=await get(`SELECT * FROM status_templates WHERE id=?`,[Number(templateOrName)]);
  else if(templateOrName&&typeof templateOrName==='object')template=templateOrName;
  else if(String(templateOrName||'').trim())template=await get(`SELECT * FROM status_templates WHERE LOWER(name)=LOWER(?) ORDER BY id DESC LIMIT 1`,[String(templateOrName).trim()]);
  if(!template)throw Object.assign(new Error('找不到要套用的狀態模板'),{status:404});
  const stackMode=['refresh','stack','independent','replace','ignore'].includes(String(template.stack_mode||''))?String(template.stack_mode):'refresh';
  const maxStacks=Math.max(1,Math.min(99,Math.floor(Number(template.max_stacks)||1)));
  const duration=durationOverride===null||durationOverride===undefined?Math.max(0,Math.floor(Number(template.duration_rounds)||0)):Math.max(0,Math.floor(Number(durationOverride)||0));
  const existing=await get(`SELECT * FROM combat_entity_statuses WHERE room_id=? AND entity_type=? AND entity_id=? AND status_template_id=? AND active=TRUE ORDER BY id DESC LIMIT 1`,[roomId,entityType,entityId,template.id]);
  if(existing&&stackMode==='ignore')return existing;
  if(existing&&stackMode==='refresh')return get(`UPDATE combat_entity_statuses SET name=?,description=?,remaining_rounds=?,effects=?::jsonb,source=?,stacks=1,stack_mode=?,max_stacks=?,dispellable=?,expires_by_round=?,updated_at=CURRENT_TIMESTAMP WHERE id=? RETURNING *`,[template.name,template.description,duration,JSON.stringify(parseJsonField(template.effects,{})),source,stackMode,maxStacks,template.dispellable!==false,duration>0,existing.id]);
  if(existing&&stackMode==='stack'){
    const stacks=Math.min(maxStacks,Math.max(1,Number(existing.stacks)||1)+1);const nextDuration=duration>0?Math.max(duration,Math.max(0,Number(existing.remaining_rounds)||0)):0;
    return get(`UPDATE combat_entity_statuses SET remaining_rounds=?,effects=?::jsonb,source=?,stacks=?,stack_mode=?,max_stacks=?,dispellable=?,expires_by_round=?,updated_at=CURRENT_TIMESTAMP WHERE id=? RETURNING *`,[nextDuration,JSON.stringify(parseJsonField(template.effects,{})),source,stacks,stackMode,maxStacks,template.dispellable!==false,duration>0,existing.id]);
  }
  if(stackMode==='replace')await run(`UPDATE combat_entity_statuses SET active=FALSE,updated_at=CURRENT_TIMESTAMP WHERE room_id=? AND entity_type=? AND entity_id=? AND status_template_id=? AND active=TRUE`,[roomId,entityType,entityId,template.id]);
  return get(`INSERT INTO combat_entity_statuses(room_id,entity_type,entity_id,status_template_id,name,description,remaining_rounds,effects,source,stacks,stack_mode,max_stacks,dispellable,expires_by_round) VALUES(?,?,?,?,?,?,?,?,?,1,?,?,?,?) RETURNING *`,[roomId,entityType,entityId,template.id,template.name,template.description,duration,JSON.stringify(parseJsonField(template.effects,{})),String(source||'系統'),stackMode,maxStacks,template.dispellable!==false,duration>0]);
}

async function applyAdHocCombatEntityModifier(roomId,entityType,entityId,effects,source,duration){
  const list=normalizeUnifiedEffects(effects).map(effect=>({...effect,timing:'passive',duration_rounds:0}));if(!list.length)return null;
  const name=String(source||'效果').slice(0,120);const safeDuration=Math.max(0,Math.floor(Number(duration)||0));
  const existing=await get(`SELECT * FROM combat_entity_statuses WHERE room_id=? AND entity_type=? AND entity_id=? AND status_template_id IS NULL AND source=? AND active=TRUE ORDER BY id DESC LIMIT 1`,[roomId,entityType,entityId,String(source||'效果')]);
  if(existing)return get(`UPDATE combat_entity_statuses SET name=?,remaining_rounds=?,effects=?::jsonb,stacks=1,stack_mode='refresh',max_stacks=1,dispellable=TRUE,expires_by_round=?,updated_at=CURRENT_TIMESTAMP WHERE id=? RETURNING *`,[name,safeDuration,JSON.stringify({list}),safeDuration>0,existing.id]);
  return get(`INSERT INTO combat_entity_statuses(room_id,entity_type,entity_id,name,description,remaining_rounds,effects,source,stacks,stack_mode,max_stacks,dispellable,expires_by_round) VALUES(?,?,?,?,?,?,?::jsonb,?,1,'refresh',1,TRUE,?) RETURNING *`,[roomId,entityType,entityId,name,'由統一效果系統產生',safeDuration,JSON.stringify({list}),String(source||'效果'),safeDuration>0]);
}

async function applyCombatEntityEffects({roomId,entityType,entityId,effects=[],source='效果',allowedTimings=null}){
  const entity=entityType==='monster'?await getCombatMonster(roomId,entityId,{allowDefeated:true}):await getCombatNpc(roomId,entityId,{allowDowned:true});
  const list=normalizeUnifiedEffects(effects).filter(effect=>!allowedTimings||allowedTimings.has(effect.timing));
  let hp=entity.current_hp,spirit=entity.current_spirit,fifth=entity.current_fifth;const applied=[],parts=[],mods=[];let modDuration=0;
  for(const effect of list){
    if(effect.chance<100&&Math.random()*100>=effect.chance)continue;
    const value=Number(effect.value)||0;
    if(['stat_flat','stat_percent','all_attributes_percent','element_affinity'].includes(effect.type)){
      if(effect.timing==='passive')continue;mods.push(effect);modDuration=Math.max(modDuration,effect.duration_rounds);applied.push(effect);continue;
    }
    if(effect.type==='heal_hp'||effect.type==='damage_hp'){
      const before=hp,amount=Math.max(0,Math.floor(Math.abs(value)));hp=effect.type==='heal_hp'?Math.min(entity.max_hp,hp+amount):Math.max(0,hp-amount);parts.push(`HP ${before}→${hp}`);
    }else if(effect.type==='heal_spirit'||effect.type==='damage_spirit'||effect.type==='heal_sanity'||effect.type==='damage_sanity'){
      const before=spirit,amount=Math.max(0,Math.floor(Math.abs(value)));const healing=effect.type==='heal_spirit'||effect.type==='heal_sanity';spirit=healing?Math.min(entity.max_spirit,spirit+amount):Math.max(0,spirit-amount);parts.push(`${effect.type.includes('sanity')?'理智':'精神'} ${before}→${spirit}`);
    }else if(effect.type==='fifth_value'){
      const before=fifth;fifth=Math.max(0,Math.min(entity.max_fifth,effectOperationNumber(fifth,effect,0,entity.max_fifth)));parts.push(`第五維 ${before}→${fifth}`);
    }else if(effect.type==='status_apply'){
      const st=await applyStatusTemplateToCombatEntity(roomId,entityType,entityId,effect.extra,source,effect.duration_rounds||null);parts.push(`獲得狀態「${st.name}」`);
    }else if(effect.type==='revive'){
      if(hp>0)continue;const pct=Math.max(1,Math.min(100,Math.floor(Math.abs(value)||20)));hp=Math.max(1,Math.floor(entity.max_hp*pct/100));parts.push(`復活至 ${hp} HP`);
    }else{
      continue;
    }
    applied.push(effect);
  }
  if(mods.length){const st=await applyAdHocCombatEntityModifier(roomId,entityType,entityId,mods,source,modDuration);if(st)parts.push(`獲得「${st.name}」${modDuration?` ${modDuration} 回合`:''}`);}
  await persistCombatEntityResources(roomId,entityType,entityId,{current_hp:hp,current_spirit:spirit,current_fifth:fifth});
  return {applied,message_parts:parts};
}

async function applyEffectsToCombatTarget({roomId,target,effects,source,allowedTimings=new Set(['immediate'])}){
  if(target.type==='player')return applyUnifiedEffects({userId:target.id,roomId,effects,source,allowedTimings});
  return applyCombatEntityEffects({roomId,entityType:target.type,entityId:target.id,effects,source,allowedTimings});
}

async function processCombatEntityStatusesRoundEnd(roomId,entityType,entityId){
  try{
    const statuses=await loadCombatEntityStatuses(roomId,entityType,entityId);
    const tick=[];
    for(const status of statuses){
      const stacks=Math.max(1,Number(status.stacks)||1);
      for(const effect of normalizeUnifiedEffects(parseJsonField(status.effects,{}))){
        if(effect.timing!=='round_end')continue;
        tick.push({...effect,value:(Number(effect.value)||0)*stacks});
      }
    }
    let parts=[];
    if(tick.length){
      const result=await applyCombatEntityEffects({roomId,entityType,entityId,effects:tick,source:'狀態回合效果',allowedTimings:new Set(['round_end'])});
      parts=result.message_parts||[];
    }
    await run(`UPDATE combat_entity_statuses SET remaining_rounds=remaining_rounds-1,updated_at=CURRENT_TIMESTAMP WHERE room_id=? AND entity_type=? AND entity_id=? AND active=TRUE AND expires_by_round=TRUE AND remaining_rounds>0`,[roomId,entityType,entityId]);
    await run(`UPDATE combat_entity_statuses SET active=FALSE,updated_at=CURRENT_TIMESTAMP WHERE room_id=? AND entity_type=? AND entity_id=? AND active=TRUE AND expires_by_round=TRUE AND remaining_rounds<=0`,[roomId,entityType,entityId]);
    if(parts.length){
      const entity=entityType==='monster'?await getCombatMonster(roomId,entityId,{allowDefeated:true}):await getCombatNpc(roomId,entityId,{allowDowned:true});
      await addEvent(roomId,null,'status_tick',{text:`${entityType==='monster'?'👹 ':'🧑 '}${entity.name} 的狀態效果：${parts.join('、')}`});
    }
  }catch(error){console.error('ENTITY STATUS ROUND TICK ERROR:',error);}
}

function chooseWeightedCombatTarget(targets){
  const alive=(targets||[]).filter(t=>t.alive!==false);if(!alive.length)return null;
  const weighted=alive.map(t=>{
    let w=100;
    if(t.type==='player')w=Math.max(1,Math.floor(Number(t.character?.weird_hatred)||100));
    else if(t.type==='npc')w=Math.max(1,Math.floor(Number(parseWorldConfig(t.entity?.config).combat_hatred)||80));
    return {t,w};
  });
  const total=weighted.reduce((sum,x)=>sum+x.w,0);let r=Math.random()*total;
  for(const x of weighted){r-=x.w;if(r<=0)return x.t;}return weighted[weighted.length-1].t;
}

async function resolvePlayerAttack({roomId,userId,character,skill,config,targetId,targetType='monster',damageMultiplier=1,attackerName=null,attackerEntity=null,attackerType='player'}) {
  const source=attackerType==='summon'?attackerEntity:character;
  return resolveCombatAttack({roomId,attackerType,attackerId:attackerType==='summon'?null:userId,attackerUserId:userId,attackerName:attackerName||character.name,attackerSource:source,skill,config,targetType,targetId,damageMultiplier});
}

async function resolveMonsterAttack(roomId, monsterId, requestedTargetId=null, requestedTargetType='player') {
  const monster=await getCombatMonster(roomId,monsterId);
  let target=null;
  if(requestedTargetId){try{const t=await getCombatTarget(roomId,requestedTargetType,requestedTargetId);if(combatTargetSide(t)==='ally')target=t;}catch{}}
  if(!target){const candidates=await combatTargetsForActor(roomId,{type:'monster',id:monsterId},{relation:'enemy'});target=chooseWeightedCombatTarget(candidates);}
  if(!target)throw Object.assign(new Error('沒有仍可戰鬥的敵對目標'),{status:400});
  const cfg=parseWorldConfig(monster.config);
  const skill={name:'怪物基本攻擊',damage_formula:String(cfg.combat_damage_formula||'1D力量'),check_name:combatAttributeLabel(normalizeCombatAttribute(cfg.combat_attack_attribute||'strength','strength'))};
  const config={combat_check_attribute:normalizeCombatAttribute(cfg.combat_attack_attribute||'strength','strength'),combat_accuracy_bonus:Number(cfg.combat_accuracy_bonus)||0,combat_element:ELEMENTS.includes(String(cfg.element||''))?String(cfg.element):'',combat_damage_target:['hp','spirit','sanity','fifth'].includes(String(cfg.combat_damage_target||''))?String(cfg.combat_damage_target):'hp',damage_multiplier:1};
  return resolveCombatAttack({roomId,attackerType:'monster',attackerId:monsterId,attackerName:monster.name,attackerSource:monster,skill,config,targetType:target.type,targetId:target.id,damageMultiplier:bossDamageMultiplier(monster)});
}

async function resolveNpcBasicAttack(roomId,npcId,requestedTargetId=null,requestedTargetType=null){
  const npc=await getCombatNpc(roomId,npcId);if(!['ally','enemy'].includes(npc.combat_side))throw Object.assign(new Error('這個 NPC 沒有設定戰鬥陣營'),{status:400});
  let target=null;
  if(requestedTargetId&&requestedTargetType){try{const t=await getCombatTarget(roomId,requestedTargetType,requestedTargetId);if(combatTargetSide(t)!==npc.combat_side)target=t;}catch{}}
  if(!target){const candidates=await combatTargetsForActor(roomId,{type:'npc',id:npcId,side:npc.combat_side},{relation:'enemy'});target=chooseWeightedCombatTarget(candidates);}
  if(!target)throw Object.assign(new Error('沒有可攻擊目標'),{status:400});
  const cfg=parseWorldConfig(npc.config);
  const skill={name:'NPC 基本攻擊',damage_formula:String(cfg.combat_damage_formula||'1D力量'),check_name:combatAttributeLabel(normalizeCombatAttribute(cfg.combat_attack_attribute||'strength','strength'))};
  const config={combat_check_attribute:normalizeCombatAttribute(cfg.combat_attack_attribute||'strength','strength'),combat_accuracy_bonus:Number(cfg.combat_accuracy_bonus)||0,combat_element:ELEMENTS.includes(String(cfg.element||''))?String(cfg.element):'',combat_damage_target:['hp','spirit','sanity','fifth'].includes(String(cfg.combat_damage_target||''))?String(cfg.combat_damage_target):'hp',damage_multiplier:1};
  return resolveCombatAttack({roomId,attackerType:'npc',attackerId:npcId,attackerName:npc.name,attackerSource:npc,skill,config,targetType:target.type,targetId:target.id,damageMultiplier:1});
}

async function buildCombatOrder(roomId) {
  const players=await getBattlePlayers(roomId);
  const monsters=await all(`SELECT rm.*,mt.name,mt.attributes,mt.element_affinity,mt.effects,mt.ai_behavior,mt.config FROM room_monsters rm JOIN monster_templates mt ON mt.id=rm.monster_template_id WHERE rm.room_id=? AND rm.status='encountered' AND rm.current_hp>0`,[roomId]);
  const npcs=await all(`SELECT rn.*,nt.name,nt.attributes,nt.element_affinity,nt.effects,nt.ai_behavior,nt.config,nt.max_hp AS template_max_hp FROM room_npcs rn JOIN npc_templates nt ON nt.id=rn.npc_template_id WHERE rn.room_id=? AND rn.status='active' AND rn.current_hp>0 AND rn.combat_side IN ('ally','enemy')`,[roomId]);
  const order=[];
  for(const p of players){
    if(Number(p.member.hp)<=0||String(p.member.combat_state||'active')==='escaped')continue;
    order.push({type:'player',id:Number(p.member.user_id),name:p.character.name,initiative:combatPlayerStat(p.character,'agility'),actions_max:Math.max(1,Number(p.character.round_count)||1),joined_at:p.member.joined_at});
  }
  for(const raw of monsters){
    const m={...parseWorldEntityRow(raw),current_hp:Number(raw.current_hp)||0};
    const agility=combatEntityStat(m,'agility');
    order.push({type:'monster',id:Number(raw.id),name:m.name,initiative:agility,actions_max:Math.max(1,1+Math.floor(agility/50)),joined_at:raw.created_at});
  }
  for(const raw of npcs){
    const n=await getCombatNpc(roomId,raw.npc_template_id);
    const agility=combatEntityStat(n,'agility');
    order.push({type:'npc',id:Number(raw.npc_template_id),name:n.name,side:n.combat_side,initiative:agility,actions_max:Math.max(1,1+Math.floor(agility/50)),joined_at:raw.created_at});
  }
  order.sort((a,b)=>{if(b.initiative!==a.initiative)return b.initiative-a.initiative;if(a.type!==b.type)return a.type==='player'?-1:1;return Number(a.id)-Number(b.id);});
  return order;
}

async function combatActorAlive(roomId,actor) {
  if(!actor)return false;
  if(actor.type==='player'){const row=await get(`SELECT hp,combat_state FROM room_members WHERE room_id=? AND user_id=?`,[roomId,actor.id]);return Boolean(row&&Number(row.hp)>0&&!['dead','escaped'].includes(String(row.combat_state||'active')));}
  if(actor.type==='monster'){const row=await get(`SELECT current_hp,status FROM room_monsters WHERE room_id=? AND id=?`,[roomId,actor.id]);return Boolean(row&&row.status==='encountered'&&Number(row.current_hp)>0);}
  if(actor.type==='npc'){const row=await get(`SELECT current_hp,status,combat_side FROM room_npcs WHERE room_id=? AND npc_template_id=?`,[roomId,actor.id]);return Boolean(row&&row.status==='active'&&['ally','enemy'].includes(String(row.combat_side))&&Number(row.current_hp)>0);}
  return false;
}

async function setCombatActor(roomId,actor,index,round) {
  await run(`UPDATE rooms SET round=?,current_actor_type=?,current_actor_ref_id=?,current_actor_id=?,combat_index=?,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[
    Math.max(1,Number(round)||1),actor?actor.type:'player',actor?actor.id:null,actor&&actor.type==='player'?actor.id:null,Math.max(0,Number(index)||0),roomId
  ]);
}

async function resetCombatActorActions(roomId,order) {
  for(const actor of order){
    if(!(await combatActorAlive(roomId,actor)))continue;
    if(actor.type==='player'){
      const ch=await getCharacterByUser(actor.id);await run(`UPDATE room_members SET turn_actions_remaining=? WHERE room_id=? AND user_id=?`,[Math.max(1,Number(ch?.round_count)||1),roomId,actor.id]);
    }else{
      if(actor.type==='monster'){
        const monster=await getCombatMonster(roomId,actor.id);const agi=combatEntityStat(monster,'agility');await run(`UPDATE room_monsters SET turn_actions_remaining=? WHERE room_id=? AND id=?`,[Math.max(1,1+Math.floor(agi/50)),roomId,actor.id]);
      }else if(actor.type==='npc'){
        const npc=await getCombatNpc(roomId,actor.id);const agi=combatEntityStat(npc,'agility');await run(`UPDATE room_npcs SET turn_actions_remaining=? WHERE room_id=? AND npc_template_id=?`,[Math.max(1,1+Math.floor(agi/50)),roomId,actor.id]);
      }
    }
  }
}

async function initializeBattleTurnState(roomId) {
  await run(`UPDATE room_members SET combat_state=CASE WHEN hp>0 THEN 'active' ELSE 'downed' END,combat_reaction_round=0,combat_reaction_uses=0 WHERE room_id=? AND role='player'`,[roomId]);
  const order=await buildCombatOrder(roomId);
  for(const actor of order){
    if(actor.type==='player'){
      const ch=await getCharacterByUser(actor.id);
      await run(`UPDATE room_members SET turn_actions_remaining=?,skill_charge_state='{}'::jsonb,max_hp=?,hp=LEAST(hp,?),max_spirit=?,spirit=LEAST(spirit,?),combat_reaction_round=0,combat_reaction_uses=0,combat_state=CASE WHEN hp>0 THEN 'active' ELSE 'downed' END WHERE room_id=? AND user_id=?`,[Math.max(1,Number(ch?.round_count)||1),Math.max(1,Number(ch?.max_hp)||1),Math.max(1,Number(ch?.max_hp)||1),Math.max(0,Number(ch?.final_spirit)||0),Math.max(0,Number(ch?.final_spirit)||0),roomId,actor.id]);
    }else{
      if(actor.type==='monster')await run(`UPDATE room_monsters SET turn_actions_remaining=?,skill_charge_state='{}'::jsonb WHERE room_id=? AND id=?`,[Math.max(1,Number(actor.actions_max)||1),roomId,actor.id]);
      else if(actor.type==='npc')await run(`UPDATE room_npcs SET turn_actions_remaining=?,skill_charge_state='{}'::jsonb WHERE room_id=? AND npc_template_id=?`,[Math.max(1,Number(actor.actions_max)||1),roomId,actor.id]);
    }
  }
  await run(`UPDATE rooms SET combat_order=?::jsonb,combat_index=0 WHERE id=?`,[JSON.stringify(order),roomId]);
  return order;
}

async function currentCombatActor(roomId) {
  const room=await get(`SELECT * FROM rooms WHERE id=?`,[roomId]);if(!room)return {room:null,order:[],actor:null,index:0};
  let order=parseJsonField(room.combat_order,[]);if(!Array.isArray(order)||!order.length){order=await buildCombatOrder(roomId);await run(`UPDATE rooms SET combat_order=?::jsonb WHERE id=?`,[JSON.stringify(order),roomId]);}
  let index=Math.max(0,Math.min(Math.max(0,order.length-1),Number(room.combat_index)||0));
  let actor=order[index]||null;
  if(actor && !(await combatActorAlive(roomId,actor))){
    for(let step=1;step<=order.length;step++){const idx=(index+step)%order.length;if(await combatActorAlive(roomId,order[idx])){index=idx;actor=order[idx];break;}}
    if(actor)await setCombatActor(roomId,actor,index,room.round);
  }
  return {room,order,actor,index};
}

async function endBattleIfResolved(roomId){
  const room=await get(`SELECT battle_active FROM rooms WHERE id=?`,[roomId]);
  if(!room||!room.battle_active)return {ended:false};
  const order=await buildCombatOrder(roomId);const alive=[];
  for(const actor of order)if(await combatActorAlive(roomId,actor))alive.push(actor);
  const allies=alive.filter(a=>combatActorSide(a)==='ally');const enemies=alive.filter(a=>combatActorSide(a)==='enemy');
  if(allies.length&&enemies.length)return {ended:false};
  await run(`UPDATE rooms SET battle_active=FALSE,current_actor_id=NULL,current_actor_ref_id=NULL,combat_order='[]'::jsonb,combat_index=0,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[roomId]);
  await run(`UPDATE room_members SET combat_state=CASE WHEN hp>0 THEN 'active' ELSE 'downed' END,turn_actions_remaining=0,skill_charge_state='{}'::jsonb WHERE room_id=? AND role='player'`,[roomId]);
  const text=!enemies.length?'🏁 戰鬥結束：敵方已無可戰鬥單位。':'⚠️ 戰鬥結束：玩家／友方已無可戰鬥單位。';
  await addEvent(roomId,null,'system',{text});return {ended:true,text};
}

async function advanceCurrentCombatAction(roomId, actorType, actorId, reason) {
  const state=await currentCombatActor(roomId);const room=state.room;
  if(!room||!room.battle_active)return {advanced:false};
  const actor=state.actor;if(!actor)return {advanced:false};
  if(String(actor.type)!==String(actorType)||String(actor.id)!==String(actorId))throw Object.assign(new Error('現在不是這個單位的行動'),{status:403});

  let remaining=0;
  if(actor.type==='player'){
    const row=await get(`SELECT turn_actions_remaining,skill_charge_state FROM room_members WHERE room_id=? AND user_id=?`,[roomId,actor.id]);remaining=Math.max(0,Number(row?.turn_actions_remaining)||0);if(!remaining){const ch=await getCharacterByUser(actor.id);remaining=Math.max(1,Number(ch?.round_count)||1);}remaining=Math.max(0,remaining-1);
    const charge=parseJsonField(row?.skill_charge_state,{});if(charge&&charge.status==='charging'&&Number(charge.remaining_turns)>0){charge.remaining_turns=Math.max(0,Number(charge.remaining_turns)-1);if(charge.remaining_turns<=0)charge.status='ready';}
    await run(`UPDATE room_members SET turn_actions_remaining=?,skill_charge_state=?::jsonb WHERE room_id=? AND user_id=?`,[remaining,JSON.stringify(charge||{}),roomId,actor.id]);
  }else if(actor.type==='monster'){
    const row=await get(`SELECT turn_actions_remaining,skill_charge_state FROM room_monsters WHERE room_id=? AND id=?`,[roomId,actor.id]);remaining=Math.max(0,Number(row?.turn_actions_remaining)||0);if(!remaining)remaining=Math.max(1,Number(actor.actions_max)||1);remaining=Math.max(0,remaining-1);
    const charge=parseJsonField(row?.skill_charge_state,{});if(charge&&charge.status==='charging'&&Number(charge.remaining_turns)>0){charge.remaining_turns=Math.max(0,Number(charge.remaining_turns)-1);if(charge.remaining_turns<=0)charge.status='ready';}
    await run(`UPDATE room_monsters SET turn_actions_remaining=?,skill_charge_state=?::jsonb WHERE room_id=? AND id=?`,[remaining,JSON.stringify(charge||{}),roomId,actor.id]);
  }else if(actor.type==='npc'){
    const row=await get(`SELECT turn_actions_remaining,skill_charge_state FROM room_npcs WHERE room_id=? AND npc_template_id=?`,[roomId,actor.id]);remaining=Math.max(0,Number(row?.turn_actions_remaining)||0);if(!remaining)remaining=Math.max(1,Number(actor.actions_max)||1);remaining=Math.max(0,remaining-1);
    const charge=parseJsonField(row?.skill_charge_state,{});if(charge&&charge.status==='charging'&&Number(charge.remaining_turns)>0){charge.remaining_turns=Math.max(0,Number(charge.remaining_turns)-1);if(charge.remaining_turns<=0)charge.status='ready';}
    await run(`UPDATE room_npcs SET turn_actions_remaining=?,skill_charge_state=?::jsonb WHERE room_id=? AND npc_template_id=?`,[remaining,JSON.stringify(charge||{}),roomId,actor.id]);
  }

  const resolvedNow=await endBattleIfResolved(roomId);if(resolvedNow.ended)return {advanced:true,ended:true};
  if(remaining>0){await addEvent(roomId,actor.type==='player'?actor.id:null,'turn_action',{text:`${actor.name} 消耗 1 回合數${reason?`（${reason}）`:''}，本輪剩餘 ${remaining}`,remaining,actor_type:actor.type,actor_id:actor.id});return {advanced:true,actor_type:actor.type,actor_id:actor.id,round:room.round,remaining};}

  let order=state.order;
  const alive=[];for(const item of order)if(await combatActorAlive(roomId,item))alive.push(item);
  const aliveAllies=alive.filter(x=>combatActorSide(x)==='ally'), aliveEnemies=alive.filter(x=>combatActorSide(x)==='enemy');
  if(!aliveAllies.length||!aliveEnemies.length){await run(`UPDATE rooms SET battle_active=FALSE,current_actor_id=NULL,current_actor_ref_id=NULL,combat_order='[]'::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[roomId]);await run(`UPDATE room_members SET combat_state=CASE WHEN hp>0 THEN 'active' ELSE 'downed' END,turn_actions_remaining=0,skill_charge_state='{}'::jsonb WHERE room_id=? AND role='player'`,[roomId]);await addEvent(roomId,null,'system',{text:!aliveEnemies.length?'🏁 戰鬥結束：敵方已無可戰鬥單位。':'⚠️ 戰鬥結束：玩家／友方已無可戰鬥單位。'});return {advanced:true,ended:true};}

  let nextIndex=-1,wrapped=false;
  for(let step=1;step<=order.length;step++){
    const idx=(state.index+step)%order.length;
    if(await combatActorAlive(roomId,order[idx])){nextIndex=idx;wrapped=idx<=state.index;break;}
  }
  if(nextIndex<0)return {advanced:false};
  let nextRound=Math.max(1,Number(room.round)||1);
  if(wrapped){
    nextRound+=1;
    for(const p of aliveAllies.filter(x=>x.type==='player')){await applyCharacterClassResourceRoundDelta(p.id);await processCharacterStatusesRoundEnd(roomId,p.id);}
    for(const entity of alive.filter(x=>x.type==='monster'||x.type==='npc'))await processCombatEntityStatusesRoundEnd(roomId,entity.type,entity.id);
    const resolvedAfterTicks=await endBattleIfResolved(roomId);if(resolvedAfterTicks.ended)return {advanced:true,ended:true};
    await resetCombatActorActions(roomId,order);
  }
  const next=order[nextIndex];await setCombatActor(roomId,next,nextIndex,nextRound);
  await addEvent(roomId,null,'turn',{round:nextRound,actor_type:next.type,actor_id:next.id,text:`輪到 ${next.type==='monster'?'👹 ':next.type==='npc'?'🧑 ':''}${next.name} 行動`});
  return {advanced:true,actor_type:next.type,actor_id:next.id,round:nextRound,remaining:null};
}

async function advanceBattleAction(roomId, actingUserId, reason) {
  return advanceCurrentCombatAction(roomId,'player',actingUserId,reason);
}

async function prepareBattleSkillUse(roomId, userId, character, skill) {
  const config=skillCombatConfig(skill,character);if(!roomId)return {ready:true,battle:false,config};
  const room=await get(`SELECT * FROM rooms WHERE id=?`,[roomId]);if(!room)throw Object.assign(new Error('找不到房間'),{status:404});if(!room.battle_active)return {ready:true,battle:false,config};
  const member=await get(`SELECT * FROM room_members WHERE room_id=? AND user_id=?`,[roomId,userId]);if(!member)throw Object.assign(new Error('你不是房間成員'),{status:403});
  if(config.turn_cost>0 && !(room.current_actor_type==='player' && String(room.current_actor_ref_id||room.current_actor_id)===String(userId)))throw Object.assign(new Error('現在不是你的行動'),{status:403});
  if(config.charge_turns<=0)return {ready:true,battle:true,config};
  const charge=parseJsonField(member.skill_charge_state,{});
  if(charge&&charge.skill_id&&String(charge.skill_id)!==String(skill.id))throw Object.assign(new Error(`目前正在為「${charge.skill_name||'其他技能'}」蓄力，請先完成或取消`),{status:400});
  if(charge&&String(charge.skill_id)===String(skill.id)&&charge.status==='ready'){await run(`UPDATE room_members SET skill_charge_state='{}'::jsonb WHERE room_id=? AND user_id=?`,[roomId,userId]);return {ready:true,battle:true,config,charged:true};}
  if(charge&&String(charge.skill_id)===String(skill.id)&&charge.status==='charging')throw Object.assign(new Error(`技能仍在蓄力，剩餘 ${Math.max(0,Number(charge.remaining_turns)||0)} 回合數`),{status:400});
  const nextCharge={skill_id:skill.id,skill_name:skill.name||'未命名技能',status:'charging',remaining_turns:config.charge_turns,started_at_round:Math.max(1,Number(room.round)||1)};
  await run(`UPDATE room_members SET skill_charge_state=?::jsonb WHERE room_id=? AND user_id=?`,[JSON.stringify(nextCharge),roomId,userId]);
  await addEvent(roomId,userId,'charge',{text:`${character.name} 開始為技能「${skill.name||'未命名技能'}」蓄力（需要 ${config.charge_turns} 回合數）`,skill_id:skill.id,remaining_turns:config.charge_turns});
  if(config.turn_cost>0)await advanceBattleAction(roomId,userId,'開始蓄力');
  return {ready:false,charging:true,battle:true,config};
}

async function resolveWorldSkillTemplateRef(ref){
  const raw=String(ref??'').trim();if(!raw)return null;
  if(/^\d+$/.test(raw))return get(`SELECT * FROM skill_templates WHERE id=?`,[Number(raw)]);
  return get(`SELECT * FROM skill_templates WHERE LOWER(name)=LOWER(?) ORDER BY id DESC LIMIT 1`,[raw]);
}

async function loadWorldSkillTemplates(refs){
  const out=[];const seen=new Set();
  for(const ref of parseWorldList(refs)){
    const template=await resolveWorldSkillTemplateRef(ref);if(!template||seen.has(String(template.id)))continue;
    seen.add(String(template.id));out.push(template);
  }
  return out;
}

function skillEffectTraits(skill){
  const data=parseJsonField(skill&&skill.data,{});const effects=normalizeUnifiedEffects(data.effects||[]);
  return {
    heals:effects.some(e=>e.type==='heal_hp'||e.type==='heal_spirit'||e.type==='heal_sanity'||e.type==='revive'),
    revives:effects.some(e=>e.type==='revive'),
    buffs:effects.some(e=>['stat_flat','stat_percent','all_attributes_percent','element_affinity','status_apply'].includes(e.type)),
    attacks:Boolean(data.combat_attack||String(skill&&skill.damage_formula||'').trim())
  };
}

function chooseAiCombatSkill(entity,skills){
  if(!skills.length)return null;
  const hpRatio=Math.max(0,Number(entity.current_hp)||0)/Math.max(1,Number(entity.max_hp)||1);
  const behavior=String(entity.ai_behavior||'balanced');
  const enriched=skills.map(template=>({template,skill:buildCharacterSkillFromTemplate(template,{type:'ai'}),traits:skillEffectTraits(template)}));
  if(hpRatio<=0.4){const heal=enriched.find(x=>x.traits.heals);if(heal)return heal;}
  if(behavior==='aggressive'){
    const attacks=enriched.filter(x=>x.traits.attacks);if(attacks.length)return attacks[Math.floor(Math.random()*attacks.length)];
  }
  if(behavior==='defensive'){
    const support=enriched.filter(x=>x.traits.heals||x.traits.buffs);if(support.length)return support[Math.floor(Math.random()*support.length)];
  }
  return enriched[Math.floor(Math.random()*enriched.length)];
}

async function getEntityChargeState(roomId,type,id){
  if(type==='monster')return parseJsonField((await get(`SELECT skill_charge_state FROM room_monsters WHERE room_id=? AND id=?`,[roomId,id]))?.skill_charge_state,{});
  if(type==='npc')return parseJsonField((await get(`SELECT skill_charge_state FROM room_npcs WHERE room_id=? AND npc_template_id=?`,[roomId,id]))?.skill_charge_state,{});
  return {};
}

async function setEntityChargeState(roomId,type,id,state){
  if(type==='monster')await run(`UPDATE room_monsters SET skill_charge_state=?::jsonb WHERE room_id=? AND id=?`,[JSON.stringify(state||{}),roomId,id]);
  else if(type==='npc')await run(`UPDATE room_npcs SET skill_charge_state=?::jsonb WHERE room_id=? AND npc_template_id=?`,[JSON.stringify(state||{}),roomId,id]);
}

async function combatSkillTargetsForActor(roomId,actor,config,{includeDowned=false}={}){
  const mode=config.combat_target_mode||'enemy_single';
  if(mode==='self')return [await getCombatTarget(roomId,actor.type,actor.id,{allowDowned:includeDowned})];
  const relation=mode.startsWith('ally')?'ally':'enemy';
  const candidates=await combatTargetsForActor(roomId,actor,{relation,includeDowned});
  if(mode.endsWith('_all'))return candidates;
  if(!candidates.length)return [];
  if(relation==='enemy')return [chooseWeightedCombatTarget(candidates)];
  const sorted=[...candidates].sort((a,b)=>{
    const ratio=t=>t.type==='player'?(Number(t.member.hp)||0)/Math.max(1,Number(t.member.max_hp)||1):(Number(t.entity.current_hp)||0)/Math.max(1,Number(t.entity.max_hp)||1);
    return ratio(a)-ratio(b);
  });
  return [sorted[0]];
}

async function playerRequestedSkillTargets(roomId,userId,config,body,{includeDowned=false}={}){
  const actor={type:'player',id:userId};const mode=config.combat_target_mode||'enemy_single';
  if(mode==='self')return [await getCombatTarget(roomId,'player',userId,{allowDowned:includeDowned})];
  if(mode.endsWith('_all'))return combatSkillTargetsForActor(roomId,actor,config,{includeDowned});
  const targetType=['player','monster','npc'].includes(String(body&&body.target_type||''))?String(body.target_type):null;
  const targetId=Number(body&&body.target_id)||0;
  if(!targetType||!targetId)throw Object.assign(new Error('請先選擇技能目標'),{status:400});
  const target=await getCombatTarget(roomId,targetType,targetId,{allowDowned:includeDowned});
  const side=combatTargetSide(target);
  if(mode==='enemy_single'&&side!=='enemy')throw Object.assign(new Error('這個技能必須選擇敵方目標'),{status:400});
  if(mode==='ally_single'&&side!=='ally')throw Object.assign(new Error('這個技能必須選擇友方目標'),{status:400});
  return [target];
}

async function executeAiCombatActor(roomId,actor){
  const entity=actor.type==='monster'?await getCombatMonster(roomId,actor.id):await getCombatNpc(roomId,actor.id);
  let charge=await getEntityChargeState(roomId,actor.type,actor.id);
  let chosen=null;
  if(charge&&charge.status==='charging'){
    await addEvent(roomId,null,'charge',{text:`${entity.name} 持續蓄力「${charge.skill_name||'技能'}」（剩餘 ${Math.max(0,Number(charge.remaining_turns)||0)} 回合數）`});
    await advanceCurrentCombatAction(roomId,actor.type,actor.id,'AI 蓄力');
    return {charging:true,actor:{type:actor.type,id:actor.id,name:entity.name},skill_name:charge.skill_name||''};
  }
  if(charge&&charge.status==='ready'&&charge.skill_id){
    const template=await get(`SELECT * FROM skill_templates WHERE id=?`,[Number(charge.skill_id)]);if(template)chosen={template,skill:buildCharacterSkillFromTemplate(template,{type:'ai'}),traits:skillEffectTraits(template)};
    await setEntityChargeState(roomId,actor.type,actor.id,{});
  }
  if(!chosen){
    const phase=actor.type==='monster'?bossPhaseFromEntity(entity):null;const skillRefs=[...(Array.isArray(entity.skills)?entity.skills:[]),...(Array.isArray(phase?.skills)?phase.skills:[])];const templates=await loadWorldSkillTemplates(skillRefs);const aiEntity=phase?{...entity,ai_behavior:phase.ai_behavior||entity.ai_behavior}:entity;chosen=chooseAiCombatSkill(aiEntity,templates);
  }
  if(!chosen){
    const combat=actor.type==='monster'?await resolveMonsterAttack(roomId,actor.id):await resolveNpcBasicAttack(roomId,actor.id);
    await advanceCurrentCombatAction(roomId,actor.type,actor.id,'AI 基本攻擊');
    return {skill_name:'基本攻擊',combat_results:[combat],actor:{type:actor.type,id:actor.id,name:entity.name}};
  }
  const {template,skill}=chosen;const config=skillCombatConfig(skill);
  if(config.charge_turns>0&&!charge?.status){
    await setEntityChargeState(roomId,actor.type,actor.id,{skill_id:template.id,skill_name:template.name,status:'charging',remaining_turns:config.charge_turns,started_at_round:(await get(`SELECT round FROM rooms WHERE id=?`,[roomId]))?.round||1});
    await addEvent(roomId,null,'charge',{text:`${entity.name} 開始為「${template.name}」蓄力（需要 ${config.charge_turns} 回合數）`});
    if(config.turn_cost>0)await advanceCurrentCombatAction(roomId,actor.type,actor.id,'開始蓄力');
    return {charging:true,actor:{type:actor.type,id:actor.id,name:entity.name},skill_name:template.name};
  }
  const effects=parseJsonField(skill.data,{}).effects||[];
  const includeDowned=skillEffectTraits(skill).revives;
  const targets=await combatSkillTargetsForActor(roomId,actor,config,{includeDowned});
  if(!targets.length&&config.combat_target_mode!=='self'){
    const combat=actor.type==='monster'?await resolveMonsterAttack(roomId,actor.id):await resolveNpcBasicAttack(roomId,actor.id);
    await advanceCurrentCombatAction(roomId,actor.type,actor.id,'AI 找不到技能目標，改用基本攻擊');
    return {skill_name:'基本攻擊',combat_results:[combat],actor:{type:actor.type,id:actor.id,name:entity.name}};
  }
  const combatResults=[];const effectMessages=[];
  let hitTargets=[];
  if(config.combat_attack){
    const attackTargets=config.combat_target_mode==='enemy_all'?targets:targets.slice(0,1);
    for(const target of attackTargets){
      const result=await resolveCombatAttack({roomId,attackerType:actor.type,attackerId:actor.id,attackerName:entity.name,attackerSource:entity,skill,config,targetType:target.type,targetId:target.id,damageMultiplier:actor.type==='monster'?bossDamageMultiplier(entity):1});
      combatResults.push(result);if(result.hit)hitTargets.push(target);
    }
  }
  let effectTargets=[];
  if(config.effects_target_mode==='target')effectTargets=config.effects_on_hit&&config.combat_attack?hitTargets:targets;
  else effectTargets=[await getCombatTarget(roomId,actor.type,actor.id,{allowDowned:true})];
  for(const target of effectTargets){
    const effectResult=await applyEffectsToCombatTarget({roomId,target,effects,source:`${entity.name}：${template.name}`});
    if(effectResult.message_parts?.length)effectMessages.push(`${target.name}：${effectResult.message_parts.join('、')}`);
  }
  await addEvent(roomId,null,'skill',{text:`${actor.type==='monster'?'👹':'🧑'} ${entity.name} 使用技能「${template.name}」${effectMessages.length?`【${effectMessages.join('；')}】`:''}${config.turn_cost===0?'（不消耗回合數）':''}`,skill_template_id:template.id,actor_type:actor.type,actor_id:actor.id});
  if(config.turn_cost>0)await advanceCurrentCombatAction(roomId,actor.type,actor.id,`使用「${template.name}」`);else await endBattleIfResolved(roomId);
  return {skill_name:template.name,combat_results:combatResults,effect_messages:effectMessages,actor:{type:actor.type,id:actor.id,name:entity.name}};
}

app.patch('/api/rooms/:id/combat/reaction',auth,async(req,res)=>{try{
  await requireRoomMember(req.user.id,req.params.id);const mode=String(req.body.mode||'dodge')==='defense'?'defense':'dodge';let skillId=String(req.body.skill_id??'').trim().slice(0,120);
  if(skillId){const ch=await requireCharacter(req.user.id);let skill=(Array.isArray(ch.skills)?ch.skills:[]).find(sk=>combatSkillIdentity(sk)===skillId||String(sk?.template_id||'')===skillId);if(!skill&&(ch.equipment_granted_skill_template_ids||[]).map(String).includes(skillId)){const template=await get(`SELECT * FROM skill_templates WHERE id=?`,[Number(skillId)]);if(template)skill=buildCharacterSkillFromTemplate(template,{type:'equipment_reaction'});}if(!skill||!isConfiguredReactionSkill(skill))return res.status(400).json({error:'找不到可使用的反應技能'});}
  await run(`UPDATE room_members SET combat_reaction=?,combat_reaction_skill_id=?,combat_reaction_round=0,combat_reaction_uses=0 WHERE room_id=? AND user_id=?`,[mode,skillId,req.params.id,req.user.id]);const snapshot=await roomSnapshot(req.params.id);io.to('room:'+req.params.id).emit('room:snapshot',snapshot);res.json({ok:true,mode,skill_id:skillId,room:snapshot});
}catch(e){res.status(e.status||500).json({error:e.message||'設定戰鬥反應失敗'});}});

app.post('/api/rooms/:id/combat/pass',auth,async(req,res)=>{try{
  await requireRoomMember(req.user.id,req.params.id);
  const state=await currentCombatActor(req.params.id);
  if(!state.room?.battle_active)return res.status(400).json({error:'目前不是戰鬥階段'});
  if(!state.actor||state.actor.type!=='player'||String(state.actor.id)!==String(req.user.id))return res.status(403).json({error:'現在不是你的行動'});
  await advanceBattleAction(req.params.id,req.user.id,'放棄這次行動');
  const snapshot=await roomSnapshot(req.params.id);io.to('room:'+req.params.id).emit('room:snapshot',snapshot);res.json({ok:true,room:snapshot});
}catch(e){console.error('PASS COMBAT ACTION ERROR',e);res.status(e.status||500).json({error:e.message||'略過行動失敗'});}});

app.post('/api/rooms/:id/combat/basic-attack',auth,async(req,res)=>{try{
  await requireRoomMember(req.user.id,req.params.id);const character=await requireCharacter(req.user.id);const room=await get(`SELECT * FROM rooms WHERE id=?`,[req.params.id]);if(!room?.battle_active)return res.status(400).json({error:'目前不是戰鬥階段'});if(!(room.current_actor_type==='player'&&String(room.current_actor_ref_id||room.current_actor_id)===String(req.user.id)))return res.status(403).json({error:'現在不是你的行動'});
  const targetId=Number(req.body.target_id)||0;const targetType=['monster','npc'].includes(String(req.body.target_type||''))?String(req.body.target_type):'monster';if(!targetId)return res.status(400).json({error:'請選擇攻擊目標'});
  const skill={name:'基本攻擊',damage_formula:'1D力量',check_name:'力量'};const config={combat_check_attribute:'strength',combat_accuracy_bonus:0,combat_element:'',combat_damage_target:'hp',damage_multiplier:1};
  const combat=await resolvePlayerAttack({roomId:req.params.id,userId:req.user.id,character,skill,config,targetId,targetType});await advanceBattleAction(req.params.id,req.user.id,'基本攻擊');const snapshot=await roomSnapshot(req.params.id);io.to('room:'+req.params.id).emit('room:snapshot',snapshot);res.json({ok:true,combat,room:snapshot});
}catch(e){console.error('BASIC ATTACK ERROR',e);res.status(e.status||500).json({error:e.message||'基本攻擊失敗'});}});


async function transitionEscapedTeamToChase(roomId,userId){
  const team=await getUserRoomTeam(userId,roomId);if(!team)return null;
  const activeTeam=await all(`SELECT rm.user_id,rm.hp,rm.combat_state FROM room_team_members tm JOIN room_members rm ON rm.room_id=tm.room_id AND rm.user_id=tm.user_id WHERE tm.team_id=?`,[team.id]);
  if(activeTeam.some(m=>Number(m.hp)>0&&String(m.combat_state||'active')!=='escaped'))return null;
  const nodeId=Number(team.current_map_node_id)||null;
  const monsters=await all(`SELECT rm.id,rm.map_node_id,rm.status,rm.current_hp,mt.name,mt.attributes,mt.element_affinity,mt.effects FROM room_monsters rm JOIN monster_templates mt ON mt.id=rm.monster_template_id WHERE rm.room_id=? AND rm.status='encountered' AND rm.current_hp>0`,[roomId]);
  const local=monsters.filter(m=>!nodeId||!m.map_node_id||String(m.map_node_id)===String(nodeId));if(!local.length)return null;
  let chosen=null,best=-1;for(const m of local){const e=calculateEntityEffectStats(parseJsonField(m.attributes,{}),parseJsonField(m.element_affinity,{}),parseJsonField(m.effects,{}));const agi=Math.max(0,Number(e.final_attributes?.agility)||0);if(agi>best){best=agi;chosen=m;}}
  if(!chosen)return null;
  await run(`UPDATE room_chases SET status='ended',updated_at=CURRENT_TIMESTAMP WHERE room_id=? AND team_id=? AND status='active'`,[roomId,team.id]);
  const chase=await get(`INSERT INTO room_chases(room_id,team_id,monster_id,distance,escape_distance,log) VALUES(?,?,?,?,?,?::jsonb) RETURNING *`,[roomId,team.id,chosen.id,2,6,JSON.stringify([`由戰鬥脫離轉入追逐：${chosen.name}`])]);
  await run(`UPDATE room_monsters SET chase_team_id=? WHERE id=?`,[team.id,chosen.id]);
  await addEvent(roomId,userId,'chase',{text:`🏃 隊伍「${team.name}」脫離正面戰鬥，但「${chosen.name}」仍在追趕！追逐開始。`,chase_id:chase.id});return chase;
}

app.post('/api/rooms/:id/combat/escape',auth,async(req,res)=>{try{
  const roomId=Number(req.params.id);await requireRoomMember(req.user.id,roomId);const state=await currentCombatActor(roomId);if(!state.room?.battle_active)return res.status(400).json({error:'目前不是戰鬥階段'});if(!state.actor||state.actor.type!=='player'||String(state.actor.id)!==String(req.user.id))return res.status(403).json({error:'現在不是你的行動'});
  const player=await getBattlePlayer(roomId,req.user.id);const enemies=await combatTargetsForActor(roomId,{type:'player',id:req.user.id},{relation:'enemy'});if(!enemies.length)return res.status(400).json({error:'目前沒有需要脫離的敵人'});
  const playerCheck=rollCombatCheck(combatPlayerStat(player.character,'agility'),0);let enemyStat=0,enemyName='敵人';for(const t of enemies){const v=t.type==='player'?combatPlayerStat(t.character,'agility'):combatEntityStat(t.entity,'agility');if(v>enemyStat){enemyStat=v;enemyName=t.name;}}const enemyCheck=rollCombatCheck(enemyStat,0);const success=(COMBAT_LEVEL_RANK[playerCheck.level]||0)>(COMBAT_LEVEL_RANK[enemyCheck.level]||0);
  if(success){await run(`UPDATE room_members SET turn_actions_remaining=1 WHERE room_id=? AND user_id=?`,[roomId,req.user.id]);await advanceBattleAction(roomId,req.user.id,'嘗試脫離戰鬥');await run(`UPDATE room_members SET combat_state='escaped',turn_actions_remaining=0,skill_charge_state='{}'::jsonb WHERE room_id=? AND user_id=?`,[roomId,req.user.id]);const chase=await transitionEscapedTeamToChase(roomId,req.user.id);await endBattleIfResolved(roomId);await addEvent(roomId,req.user.id,'combat_escape',{text:`🏃 ${player.character.name} 脫離戰鬥成功：${playerCheck.roll}/${playerCheck.target} ${playerCheck.level} VS ${enemyName} ${enemyCheck.roll}/${enemyCheck.target} ${enemyCheck.level}${chase?'，並轉入追逐階段':''}`,success:true,chase_id:chase?.id||null});}
  else{await addEvent(roomId,req.user.id,'combat_escape',{text:`🏃 ${player.character.name} 嘗試脫離戰鬥失敗：${playerCheck.roll}/${playerCheck.target} ${playerCheck.level} VS ${enemyName} ${enemyCheck.roll}/${enemyCheck.target} ${enemyCheck.level}`,success:false});await advanceBattleAction(roomId,req.user.id,'脫離失敗');}
  const snapshot=await roomSnapshot(roomId);io.to('room:'+roomId).emit('room:snapshot',snapshot);res.json({ok:true,success,player_check:playerCheck,enemy_check:enemyCheck,room:snapshot});
}catch(e){console.error('COMBAT ESCAPE ERROR',e);res.status(e.status||500).json({error:e.message||'脫離戰鬥失敗'});}});

app.post('/api/admin/rooms/:id/combat/monster-act',auth,adminAuth,async(req,res)=>{try{
  await requireRoomAdmin(req.user.id,req.params.id);const state=await currentCombatActor(req.params.id);if(!state.room?.battle_active)return res.status(400).json({error:'目前不是戰鬥階段'});if(!state.actor||state.actor.type!=='monster')return res.status(400).json({error:'目前不是怪物行動'});
  const result=await executeAiCombatActor(req.params.id,state.actor);const snapshot=await roomSnapshot(req.params.id);io.to('room:'+req.params.id).emit('room:snapshot',snapshot);res.json({ok:true,combat:result.combat_results?.[0]||null,ai_result:result,room:snapshot});
}catch(e){console.error('MONSTER ACT ERROR',e);res.status(e.status||500).json({error:e.message||'怪物行動失敗'});}});

app.post('/api/admin/rooms/:id/combat/ai-act',auth,adminAuth,async(req,res)=>{try{
  await requireRoomAdmin(req.user.id,req.params.id);const state=await currentCombatActor(req.params.id);if(!state.room?.battle_active)return res.status(400).json({error:'目前不是戰鬥階段'});if(!state.actor||!['monster','npc'].includes(state.actor.type))return res.status(400).json({error:'目前不是 AI 單位行動'});
  const result=await executeAiCombatActor(req.params.id,state.actor);const snapshot=await roomSnapshot(req.params.id);io.to('room:'+req.params.id).emit('room:snapshot',snapshot);res.json({ok:true,ai_result:result,combat:result.combat_results?.[0]||null,room:snapshot});
}catch(e){console.error('AI COMBAT ACT ERROR',e);res.status(e.status||500).json({error:e.message||'AI 行動失敗'});}});


app.post('/api/admin/rooms/:id/combat/revive',auth,adminAuth,async(req,res)=>{try{
  await requireRoomAdmin(req.user.id,req.params.id);
  const targetType=['player','monster','npc'].includes(String(req.body.target_type||''))?String(req.body.target_type):'';
  const targetId=Number(req.body.target_id)||0;const hpPercent=Math.max(1,Math.min(100,Number(req.body.hp_percent)||30));
  if(!targetType||!targetId)return res.status(400).json({error:'請選擇要復甦的目標'});
  const target=await getCombatTarget(req.params.id,targetType,targetId,{allowDowned:true});
  let recovered=0;
  if(targetType==='player'){
    const maxHp=Math.max(1,Number(target.member.max_hp)||Number(calculateCharacter(target.character).max_hp)||1);recovered=Math.max(1,Math.ceil(maxHp*hpPercent/100));
    await run(`UPDATE room_members SET hp=?,combat_state='active' WHERE room_id=? AND user_id=?`,[recovered,req.params.id,targetId]);
  }else{
    const maxHp=Math.max(1,Number(target.entity.max_hp)||100);recovered=Math.max(1,Math.ceil(maxHp*hpPercent/100));
    await persistCombatEntityResources(req.params.id,targetType,targetId,{hp:recovered});
  }
  await addEvent(req.params.id,req.user.id,'revive',{text:`✨ DM 使 ${target.name} 復甦，恢復 ${recovered} HP。`,target_type:targetType,target_id:targetId,hp:recovered});
  const snapshot=await roomSnapshot(req.params.id);io.to('room:'+req.params.id).emit('room:snapshot',snapshot);res.json({ok:true,target:{type:targetType,id:targetId,name:target.name},hp:recovered,room:snapshot});
}catch(e){console.error('DM REVIVE ERROR',e);res.status(e.status||500).json({error:e.message||'復甦失敗'});}});

/* =========================================================
   BATTLE CONTROL
========================================================= */

app.post('/api/rooms/:id/battle/start', auth, adminAuth, async function(req,res){
  try{
    const roomInfo=await requireRoomAdmin(req.user.id,req.params.id);
    if(roomInfo.room.status!=='active')return res.status(400).json({error:'請先開始跑團'});
    if(roomInfo.room.battle_active)return res.status(400).json({error:'目前已經在戰鬥中'});

    const order=await initializeBattleTurnState(req.params.id);
    if(!order.some(actor=>combatActorSide(actor)==='ally'))return res.status(400).json({error:'至少需要一名仍可戰鬥的玩家或友方 NPC'});
    if(!order.some(actor=>combatActorSide(actor)==='enemy'))return res.status(400).json({error:'請先讓至少一隻怪物遭遇，或把 NPC 設為敵方'});
    for(const actor of order){if(actor.type==='player')await resetCharacterClassResourcesForBattle(actor.id);}
    const first=order[0]||null;
    await run(`UPDATE rooms SET battle_active=TRUE,round=1,current_actor_type=?,current_actor_ref_id=?,current_actor_id=?,combat_index=0,combat_order=?::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[first?first.type:'player',first?first.id:null,first&&first.type==='player'?first.id:null,JSON.stringify(order),req.params.id]);
    await addEvent(req.params.id,req.user.id,'system',{text:first?`⚔️ DM 開始戰鬥。由 ${first.type==='monster'?'👹 ':first.type==='npc'?'🧑 ':''}${first.name} 先行動。`:'⚔️ DM 開始戰鬥，但目前沒有可戰鬥單位。'});
    const snapshot=await roomSnapshot(req.params.id);
    io.to('room:'+req.params.id).emit('room:snapshot',snapshot);
    res.json(snapshot);
  }catch(error){
    console.error('START BATTLE ERROR:',error);
    res.status(error.status||500).json({error:error.message||'無法開始戰鬥'});
  }
});

app.post('/api/rooms/:id/battle/end', auth, adminAuth, async function(req,res){
  try{
    await requireRoomAdmin(req.user.id,req.params.id);
    await run(`UPDATE rooms SET battle_active=FALSE,current_actor_id=NULL,current_actor_ref_id=NULL,combat_order='[]'::jsonb,combat_index=0,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[req.params.id]);
    await run(`UPDATE room_members SET combat_state=CASE WHEN hp>0 THEN 'active' ELSE 'downed' END,turn_actions_remaining=0,skill_charge_state='{}'::jsonb WHERE room_id=? AND role='player'`,[req.params.id]);
    await addEvent(req.params.id,req.user.id,'system',{text:'🕯️ 戰鬥結束，回到探索階段。'});
    const snapshot=await roomSnapshot(req.params.id);
    io.to('room:'+req.params.id).emit('room:snapshot',snapshot);
    res.json(snapshot);
  }catch(error){
    console.error('END BATTLE ERROR:',error);
    res.status(error.status||500).json({error:error.message||'無法結束戰鬥'});
  }
});

/* =========================================================
   NEXT TURN
========================================================= */

app.post(
  '/api/rooms/:id/next-turn',
  auth,
  adminAuth,
  async function (req, res) {
    try {
      const roomInfo = await requireRoomAdmin(req.user.id, req.params.id);
      if (roomInfo.room.status !== 'active') return res.status(400).json({error:'跑團尚未開始'});
      if (!roomInfo.room.battle_active) return res.status(400).json({error:'目前不是戰鬥階段'});

      const current=await currentCombatActor(req.params.id);
      if(!current.actor)return res.status(400).json({error:'目前沒有可行動單位'});
      await advanceCurrentCombatAction(req.params.id,current.actor.type,current.actor.id,'DM 略過／推進');

      const snapshot=await roomSnapshot(req.params.id);
      io.to('room:'+req.params.id).emit('room:snapshot',snapshot);
      res.json(snapshot);
    } catch (error) {
      res.status(error.status || 500).json({error:error.message || '切換回合失敗'});
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
            battle_active = FALSE,
            current_actor_id = NULL,
            current_actor_ref_id = NULL,
            combat_order = '[]'::jsonb,
            combat_index = 0,
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
            battle_active = FALSE,
            current_actor_id = NULL,
            current_actor_ref_id = NULL,
            combat_order = '[]'::jsonb,
            combat_index = 0,
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
            battle_active = FALSE,
            current_actor_id = NULL,
            current_actor_ref_id = NULL,
            combat_order = '[]'::jsonb,
            combat_index = 0,
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
   V13 WORLD CARDS / INVESTIGATION / AI / RIDING
========================================================= */

function parseWorldConfig(value){
  const parsed=parseJsonField(value,{});
  return parsed&&typeof parsed==='object'&&!Array.isArray(parsed)?parsed:{};
}
function parseWorldList(value){
  const parsed=parseJsonField(value,[]);
  return Array.isArray(parsed)?parsed:[];
}
function splitLines(value,limit=50){
  if(Array.isArray(value))return value.map(v=>String(v||'').trim()).filter(Boolean).slice(0,limit);
  return String(value||'').split(/\r?\n/).map(v=>v.trim()).filter(Boolean).slice(0,limit);
}
function worldAiChoice(list,behavior){
  const actions=Array.isArray(list)?list.filter(Boolean):[];
  if(!actions.length)return '';
  if(behavior==='aggressive'){
    const hit=actions.find(x=>/攻擊|追擊|襲擊|撲|咬|射擊/.test(String(x)));
    if(hit)return hit;
  }
  if(behavior==='defensive'){
    const hit=actions.find(x=>/防禦|防守|退|觀察|警戒/.test(String(x)));
    if(hit)return hit;
  }
  return actions[Math.floor(Math.random()*actions.length)]||actions[0];
}

function parseWorldEntityRow(row){
  if(!row)return null;
  const attributes=parseJsonField(row.attributes,{});
  const elementAffinity=parseJsonField(row.element_affinity,{});
  const effects=parseJsonField(row.effects,{});
  return {
    ...row,
    ...calculateEntityEffectStats(attributes,elementAffinity,effects),
    skills:parseWorldList(row.skills),
    drops:parseWorldList(row.drops),
    config:parseWorldConfig(row.config)
  };
}

async function adminListWorldTable(table){
  const rows=await all(`SELECT * FROM ${table} ORDER BY id ASC`);
  return rows.map(parseWorldEntityRow);
}

app.get('/api/admin/monster-templates',auth,adminAuth,async(req,res)=>{try{res.json({monsters:await adminListWorldTable('monster_templates')});}catch(e){res.status(500).json({error:'讀取怪物卡失敗'});}});
app.get('/api/admin/npc-templates',auth,adminAuth,async(req,res)=>{try{res.json({npcs:await adminListWorldTable('npc_templates')});}catch(e){res.status(500).json({error:'讀取 NPC 卡失敗'});}});
app.get('/api/admin/dungeon-templates',auth,adminAuth,async(req,res)=>{try{res.json({dungeons:await adminListWorldTable('dungeon_templates')});}catch(e){res.status(500).json({error:'讀取副本卡失敗'});}});

app.post('/api/admin/monster-templates',auth,adminAuth,async(req,res)=>{try{
  const name=String(req.body.name||'').trim().slice(0,120);if(!name)return res.status(400).json({error:'請填怪物名稱'});
  const config={
    ...parseWorldConfig(req.body.config),
    ai_actions:splitLines(req.body.ai_actions||req.body.config?.ai_actions),
    secret:String(req.body.secret||req.body.config?.secret||'').slice(0,4000),
    element:String(req.body.element||req.body.config?.element||'').slice(0,20),
    combat_attack_attribute:normalizeCombatAttribute(req.body.combat_attack_attribute||req.body.config?.combat_attack_attribute||'strength','strength'),
    combat_damage_formula:String(req.body.combat_damage_formula||req.body.config?.combat_damage_formula||'1D力量').slice(0,200),
    combat_damage_target:['hp','spirit','sanity','fifth'].includes(String(req.body.combat_damage_target||req.body.config?.combat_damage_target||''))?String(req.body.combat_damage_target||req.body.config?.combat_damage_target):'hp',
    combat_accuracy_bonus:Math.max(-10000,Math.min(10000,Number(req.body.combat_accuracy_bonus||req.body.config?.combat_accuracy_bonus)||0)),
    combat_defense_attribute:normalizeCombatAttribute(req.body.combat_defense_attribute||req.body.config?.combat_defense_attribute||'constitution','constitution'),
    combat_dodge_attribute:normalizeCombatAttribute(req.body.combat_dodge_attribute||req.body.config?.combat_dodge_attribute||'agility','agility'),
    hearing_threshold:Math.max(0,Math.min(100,Number(req.body.hearing_threshold??req.body.config?.hearing_threshold??30)||30)),
    boss_enabled:Boolean(req.body.boss_enabled??req.body.config?.boss_enabled),
    boss_phases:normalizeBossPhases(req.body.boss_phases??req.body.config?.boss_phases)
  };
  const effects=effectContainerWithList(req.body.effects,normalizeUnifiedEffects(req.body.effects));
  const row=await get(`INSERT INTO monster_templates(name,category,rank,description,public_hint,image_url,max_hp,investigation_dc,attributes,element_affinity,effects,skills,drops,ai_level,ai_behavior,config) VALUES(?,?,?,?,?,?,?,?,?::jsonb,?::jsonb,?::jsonb,?::jsonb,?::jsonb,?,?,?::jsonb) RETURNING *`,[
    name,String(req.body.category||'怪物').slice(0,80),String(req.body.rank||'G').slice(0,10),String(req.body.description||''),String(req.body.public_hint||'').slice(0,2000),String(req.body.image_url||''),Math.max(1,Number(req.body.max_hp)||100),Math.max(1,Math.min(100,Number(req.body.investigation_dc)||50)),JSON.stringify(req.body.attributes||{}),JSON.stringify(req.body.element_affinity||{}),JSON.stringify(effects),JSON.stringify(splitLines(req.body.skills)),JSON.stringify(splitLines(req.body.drops)),Math.max(0,Math.min(10,Number(req.body.ai_level)||1)),String(req.body.ai_behavior||'balanced').slice(0,40),JSON.stringify(config)
  ]);res.json({monster:parseWorldEntityRow(row)});
}catch(e){console.error(e);res.status(500).json({error:'建立怪物卡失敗'});}});

app.patch('/api/admin/monster-templates/:id',auth,adminAuth,async(req,res)=>{try{
  const cur=await get(`SELECT * FROM monster_templates WHERE id=?`,[req.params.id]);if(!cur)return res.status(404).json({error:'找不到怪物卡'});
  const config={...parseWorldConfig(cur.config),...parseWorldConfig(req.body.config)};
  if(req.body.ai_actions!==undefined)config.ai_actions=splitLines(req.body.ai_actions);
  if(req.body.secret!==undefined)config.secret=String(req.body.secret||'').slice(0,4000);
  if(req.body.element!==undefined)config.element=String(req.body.element||'').slice(0,20);
  if(req.body.combat_attack_attribute!==undefined)config.combat_attack_attribute=normalizeCombatAttribute(req.body.combat_attack_attribute,'strength');
  if(req.body.combat_damage_formula!==undefined)config.combat_damage_formula=String(req.body.combat_damage_formula||'1D力量').slice(0,200);
  if(req.body.combat_damage_target!==undefined)config.combat_damage_target=['hp','spirit','sanity','fifth'].includes(String(req.body.combat_damage_target))?String(req.body.combat_damage_target):'hp';
  if(req.body.combat_accuracy_bonus!==undefined)config.combat_accuracy_bonus=Math.max(-10000,Math.min(10000,Number(req.body.combat_accuracy_bonus)||0));
  if(req.body.combat_defense_attribute!==undefined)config.combat_defense_attribute=normalizeCombatAttribute(req.body.combat_defense_attribute,'constitution');
  if(req.body.combat_dodge_attribute!==undefined)config.combat_dodge_attribute=normalizeCombatAttribute(req.body.combat_dodge_attribute,'agility');
  if(req.body.hearing_threshold!==undefined)config.hearing_threshold=Math.max(0,Math.min(100,Number(req.body.hearing_threshold)||30));
  if(req.body.boss_enabled!==undefined)config.boss_enabled=Boolean(req.body.boss_enabled);
  if(req.body.boss_phases!==undefined)config.boss_phases=normalizeBossPhases(req.body.boss_phases);
  const rawEffects=req.body.effects===undefined?parseJsonField(cur.effects,{}):req.body.effects;
  const effects=effectContainerWithList(rawEffects,normalizeUnifiedEffects(rawEffects));
  const row=await get(`UPDATE monster_templates SET name=?,category=?,rank=?,description=?,public_hint=?,image_url=?,max_hp=?,investigation_dc=?,attributes=?::jsonb,element_affinity=?::jsonb,effects=?::jsonb,skills=?::jsonb,drops=?::jsonb,ai_level=?,ai_behavior=?,config=?::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=? RETURNING *`,[
    String(req.body.name??cur.name).trim().slice(0,120),String(req.body.category??cur.category).slice(0,80),String(req.body.rank??cur.rank).slice(0,10),String(req.body.description??cur.description),String(req.body.public_hint??cur.public_hint).slice(0,2000),String(req.body.image_url??cur.image_url),Math.max(1,Number(req.body.max_hp??cur.max_hp)||100),Math.max(1,Math.min(100,Number(req.body.investigation_dc??cur.investigation_dc)||50)),JSON.stringify(req.body.attributes??parseJsonField(cur.attributes,{})),JSON.stringify(req.body.element_affinity??parseJsonField(cur.element_affinity,{})),JSON.stringify(effects),JSON.stringify(req.body.skills===undefined?parseWorldList(cur.skills):splitLines(req.body.skills)),JSON.stringify(req.body.drops===undefined?parseWorldList(cur.drops):splitLines(req.body.drops)),Math.max(0,Math.min(10,Number(req.body.ai_level??cur.ai_level)||1)),String(req.body.ai_behavior??cur.ai_behavior).slice(0,40),JSON.stringify(config),req.params.id
  ]);res.json({monster:parseWorldEntityRow(row)});
}catch(e){console.error(e);res.status(500).json({error:'修改怪物卡失敗'});}});
app.delete('/api/admin/monster-templates/:id',auth,adminAuth,async(req,res)=>{try{await run(`DELETE FROM monster_templates WHERE id=?`,[req.params.id]);res.json({ok:true});}catch(e){res.status(500).json({error:'刪除怪物卡失敗'});}});

app.post('/api/admin/npc-templates',auth,adminAuth,async(req,res)=>{try{
  const name=String(req.body.name||'').trim().slice(0,120);if(!name)return res.status(400).json({error:'請填 NPC 名稱'});
  const config={
    ...parseWorldConfig(req.body.config),
    ai_lines:splitLines(req.body.ai_lines||req.body.config?.ai_lines),
    ai_actions:splitLines(req.body.ai_actions||req.body.config?.ai_actions),
    combat_side:['ally','enemy','neutral'].includes(String(req.body.combat_side||req.body.config?.combat_side||''))?String(req.body.combat_side||req.body.config?.combat_side):'neutral',
    element:String(req.body.element||req.body.config?.element||'').slice(0,20),
    combat_attack_attribute:normalizeCombatAttribute(req.body.combat_attack_attribute||req.body.config?.combat_attack_attribute||'strength','strength'),
    combat_damage_formula:String(req.body.combat_damage_formula||req.body.config?.combat_damage_formula||'1D力量').slice(0,200),
    combat_damage_target:['hp','spirit','sanity','fifth'].includes(String(req.body.combat_damage_target||req.body.config?.combat_damage_target||''))?String(req.body.combat_damage_target||req.body.config?.combat_damage_target):'hp',
    combat_accuracy_bonus:Math.max(-10000,Math.min(10000,Number(req.body.combat_accuracy_bonus||req.body.config?.combat_accuracy_bonus)||0)),
    combat_defense_attribute:normalizeCombatAttribute(req.body.combat_defense_attribute||req.body.config?.combat_defense_attribute||'constitution','constitution'),
    combat_dodge_attribute:normalizeCombatAttribute(req.body.combat_dodge_attribute||req.body.config?.combat_dodge_attribute||'agility','agility'),
    combat_hatred:Math.max(1,Math.floor(Number(req.body.combat_hatred||req.body.config?.combat_hatred)||80))
  };
  const effects=effectContainerWithList(req.body.effects,normalizeUnifiedEffects(req.body.effects));
  const row=await get(`INSERT INTO npc_templates(name,role_name,description,public_hint,investigation_dc,requires_investigation,image_url,max_hp,attributes,element_affinity,effects,skills,ai_level,ai_behavior,speech_enabled,in_game_room,config) VALUES(?,?,?,?,?,?,?, ?,?::jsonb,?::jsonb,?::jsonb,?::jsonb,?,?,?,?,?::jsonb) RETURNING *`,[
    name,String(req.body.role_name||'').slice(0,120),String(req.body.description||''),String(req.body.public_hint||'').slice(0,2000),clampInt(req.body.investigation_dc,1,100,50),Boolean(req.body.requires_investigation),String(req.body.image_url||''),Math.max(1,Number(req.body.max_hp)||100),JSON.stringify(req.body.attributes||{}),JSON.stringify(req.body.element_affinity||{}),JSON.stringify(effects),JSON.stringify(splitLines(req.body.skills)),Math.max(0,Math.min(10,Number(req.body.ai_level)||1)),String(req.body.ai_behavior||'balanced').slice(0,40),req.body.speech_enabled!==false,Boolean(req.body.in_game_room),JSON.stringify(config)
  ]);res.json({npc:parseWorldEntityRow(row)});
}catch(e){console.error(e);res.status(500).json({error:'建立 NPC 卡失敗'});}});
app.patch('/api/admin/npc-templates/:id',auth,adminAuth,async(req,res)=>{try{
  const cur=await get(`SELECT * FROM npc_templates WHERE id=?`,[req.params.id]);if(!cur)return res.status(404).json({error:'找不到 NPC 卡'});
  const config={...parseWorldConfig(cur.config),...parseWorldConfig(req.body.config)};if(req.body.ai_lines!==undefined)config.ai_lines=splitLines(req.body.ai_lines);if(req.body.ai_actions!==undefined)config.ai_actions=splitLines(req.body.ai_actions);
  if(req.body.combat_side!==undefined)config.combat_side=['ally','enemy','neutral'].includes(String(req.body.combat_side))?String(req.body.combat_side):'neutral';
  if(req.body.element!==undefined)config.element=String(req.body.element||'').slice(0,20);
  if(req.body.combat_attack_attribute!==undefined)config.combat_attack_attribute=normalizeCombatAttribute(req.body.combat_attack_attribute,'strength');
  if(req.body.combat_damage_formula!==undefined)config.combat_damage_formula=String(req.body.combat_damage_formula||'1D力量').slice(0,200);
  if(req.body.combat_damage_target!==undefined)config.combat_damage_target=['hp','spirit','sanity','fifth'].includes(String(req.body.combat_damage_target))?String(req.body.combat_damage_target):'hp';
  if(req.body.combat_accuracy_bonus!==undefined)config.combat_accuracy_bonus=Math.max(-10000,Math.min(10000,Number(req.body.combat_accuracy_bonus)||0));
  if(req.body.combat_defense_attribute!==undefined)config.combat_defense_attribute=normalizeCombatAttribute(req.body.combat_defense_attribute,'constitution');
  if(req.body.combat_dodge_attribute!==undefined)config.combat_dodge_attribute=normalizeCombatAttribute(req.body.combat_dodge_attribute,'agility');
  if(req.body.combat_hatred!==undefined)config.combat_hatred=Math.max(1,Math.floor(Number(req.body.combat_hatred)||80));
  const rawEffects=req.body.effects===undefined?parseJsonField(cur.effects,{}):req.body.effects;
  const effects=effectContainerWithList(rawEffects,normalizeUnifiedEffects(rawEffects));
  const row=await get(`UPDATE npc_templates SET name=?,role_name=?,description=?,public_hint=?,investigation_dc=?,requires_investigation=?,image_url=?,max_hp=?,attributes=?::jsonb,element_affinity=?::jsonb,effects=?::jsonb,skills=?::jsonb,ai_level=?,ai_behavior=?,speech_enabled=?,in_game_room=?,config=?::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=? RETURNING *`,[
    String(req.body.name??cur.name).trim().slice(0,120),String(req.body.role_name??cur.role_name).slice(0,120),String(req.body.description??cur.description),String(req.body.public_hint??cur.public_hint??'').slice(0,2000),clampInt(req.body.investigation_dc??cur.investigation_dc,1,100,50),req.body.requires_investigation===undefined?Boolean(cur.requires_investigation):Boolean(req.body.requires_investigation),String(req.body.image_url??cur.image_url),Math.max(1,Number(req.body.max_hp??cur.max_hp)||100),JSON.stringify(req.body.attributes??parseJsonField(cur.attributes,{})),JSON.stringify(req.body.element_affinity??parseJsonField(cur.element_affinity,{})),JSON.stringify(effects),JSON.stringify(req.body.skills===undefined?parseWorldList(cur.skills):splitLines(req.body.skills)),Math.max(0,Math.min(10,Number(req.body.ai_level??cur.ai_level)||1)),String(req.body.ai_behavior??cur.ai_behavior).slice(0,40),req.body.speech_enabled===undefined?cur.speech_enabled:Boolean(req.body.speech_enabled),req.body.in_game_room===undefined?cur.in_game_room:Boolean(req.body.in_game_room),JSON.stringify(config),req.params.id
  ]);res.json({npc:parseWorldEntityRow(row)});
}catch(e){console.error(e);res.status(500).json({error:'修改 NPC 卡失敗'});}});
app.delete('/api/admin/npc-templates/:id',auth,adminAuth,async(req,res)=>{try{await run(`DELETE FROM npc_templates WHERE id=?`,[req.params.id]);res.json({ok:true});}catch(e){res.status(500).json({error:'刪除 NPC 卡失敗'});}});

app.post('/api/admin/dungeon-templates',auth,adminAuth,async(req,res)=>{try{
  const name=String(req.body.name||'').trim().slice(0,120);if(!name)return res.status(400).json({error:'請填副本名稱'});
  const config={...parseWorldConfig(req.body.config),ai_events:splitLines(req.body.ai_events||req.body.config?.ai_events)};
  const row=await get(`INSERT INTO dungeon_templates(name,rank,description,map_name,map_image_url,ai_level,ai_behavior,config) VALUES(?,?,?,?,?,?,?,?::jsonb) RETURNING *`,[
    name,String(req.body.rank||'G').slice(0,10),String(req.body.description||''),String(req.body.map_name||'').slice(0,120),String(req.body.map_image_url||''),Math.max(0,Math.min(10,Number(req.body.ai_level)||1)),String(req.body.ai_behavior||'balanced').slice(0,40),JSON.stringify(config)
  ]);res.json({dungeon:{...row,config:parseWorldConfig(row.config)}});
}catch(e){console.error(e);res.status(500).json({error:'建立副本卡失敗'});}});
app.patch('/api/admin/dungeon-templates/:id',auth,adminAuth,async(req,res)=>{try{
  const cur=await get(`SELECT * FROM dungeon_templates WHERE id=?`,[req.params.id]);if(!cur)return res.status(404).json({error:'找不到副本卡'});
  const config={...parseWorldConfig(cur.config),...parseWorldConfig(req.body.config)};if(req.body.ai_events!==undefined)config.ai_events=splitLines(req.body.ai_events);
  const row=await get(`UPDATE dungeon_templates SET name=?,rank=?,description=?,map_name=?,map_image_url=?,ai_level=?,ai_behavior=?,config=?::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=? RETURNING *`,[
    String(req.body.name??cur.name).trim().slice(0,120),String(req.body.rank??cur.rank).slice(0,10),String(req.body.description??cur.description),String(req.body.map_name??cur.map_name).slice(0,120),String(req.body.map_image_url??cur.map_image_url),Math.max(0,Math.min(10,Number(req.body.ai_level??cur.ai_level)||1)),String(req.body.ai_behavior??cur.ai_behavior).slice(0,40),JSON.stringify(config),req.params.id
  ]);res.json({dungeon:{...row,config:parseWorldConfig(row.config)}});
}catch(e){console.error(e);res.status(500).json({error:'修改副本卡失敗'});}});
app.delete('/api/admin/dungeon-templates/:id',auth,adminAuth,async(req,res)=>{try{await run(`DELETE FROM dungeon_templates WHERE id=?`,[req.params.id]);res.json({ok:true});}catch(e){res.status(500).json({error:'刪除副本卡失敗'});}});

app.get('/api/rooms/:id/world',auth,async(req,res)=>{try{
  const roomId=Number(req.params.id);const currentUser=await getUser(req.user.id);const isAdmin=Boolean(currentUser&&currentUser.is_admin);if(!isAdmin)await requireRoomMember(req.user.id,roomId);
  const viewerNodeId=isAdmin?null:await getViewerMapNodeId(req.user.id,roomId);
  const monsters=await all(`SELECT rm.*,mn.name AS map_node_name,mt.name,mt.category,mt.rank,mt.description,mt.public_hint,mt.image_url,mt.investigation_dc,mt.attributes,mt.element_affinity,mt.effects,mt.skills,mt.drops,mt.ai_level,mt.ai_behavior,mt.config,md.encountered,md.investigated,md.last_roll,md.last_rate FROM room_monsters rm JOIN monster_templates mt ON mt.id=rm.monster_template_id LEFT JOIN room_map_nodes mn ON mn.id=rm.map_node_id LEFT JOIN monster_discoveries md ON md.room_monster_id=rm.id AND md.user_id=? WHERE rm.room_id=? ORDER BY rm.id`,[req.user.id,roomId]);
  const visibleMonsters=[];
  for(const m of monsters){
    if(!isAdmin && m.map_node_id && String(m.map_node_id)!==String(viewerNodeId||''))continue;
    const full=isAdmin||Boolean(m.investigated);const encountered=isAdmin||Boolean(m.encountered)||m.status==='encountered';
    if(!encountered&&!isAdmin)continue;
    if(full){visibleMonsters.push({...await decorateCombatWorldEntity(m,'monster',m.id,roomId),revealed:true,map_node_id:m.map_node_id||null,map_node_name:m.map_node_name||'',alert_level:Number(m.alert_level)||0,chase_team_id:m.chase_team_id||null,boss_phase_index:Number(m.boss_phase_index)||0,boss_phase_name:m.boss_phase_name||''});}
    else{visibleMonsters.push({id:m.id,monster_template_id:m.monster_template_id,status:m.status,revealed:false,encountered:true,name:'未知怪物',public_hint:m.public_hint||'你只知道這裡有某種異常存在。',image_url:'',turn_actions_remaining:Number(m.turn_actions_remaining)||0,last_roll:m.last_roll,last_rate:m.last_rate,map_node_id:m.map_node_id||null,map_node_name:m.map_node_name||'',boss_phase_index:Number(m.boss_phase_index)||0,boss_phase_name:m.boss_phase_name||''});}
  }
  const npcRows=await all(`SELECT rn.status AS room_status,rn.state,rn.combat_side,rn.current_hp,rn.max_hp AS room_max_hp,rn.current_spirit,rn.max_spirit,rn.current_fifth,rn.max_fifth,rn.turn_actions_remaining,rn.skill_charge_state,rn.map_node_id,mn.name AS map_node_name,nt.* FROM room_npcs rn JOIN npc_templates nt ON nt.id=rn.npc_template_id LEFT JOIN room_map_nodes mn ON mn.id=rn.map_node_id WHERE rn.room_id=? ORDER BY nt.id`,[roomId]);
  const npcs=[];
  for(const n of npcRows){
    if(!isAdmin && n.map_node_id && String(n.map_node_id)!==String(viewerNodeId||''))continue;
    const discovery=isAdmin?{investigated:true}:await get(`SELECT * FROM npc_discoveries WHERE room_id=? AND npc_template_id=? AND user_id=?`,[roomId,n.id,req.user.id]);
    const revealed=isAdmin||!Boolean(n.requires_investigation)||Boolean(discovery?.investigated);
    if(!revealed){npcs.push({id:n.id,npc_template_id:n.id,name:'未知人物',role_name:'身份未知',public_hint:n.public_hint||'你只能辨認出這個人的外觀與動作。',requires_investigation:true,revealed:false,last_roll:discovery?.last_roll||null,last_rate:discovery?.last_rate||null,map_node_id:n.map_node_id||null,map_node_name:n.map_node_name||'',status:n.room_status,combat_side:'neutral'});continue;}
    const entity=await decorateCombatWorldEntity({...n,status:n.room_status,max_hp:Number(n.room_max_hp)||Number(n.max_hp)||100},'npc',n.id,roomId);
    const relationship=isAdmin?null:await get(`SELECT affinity,trust,fear,hostility,memories FROM npc_relationships WHERE room_id=? AND npc_template_id=? AND user_id=?`,[roomId,n.id,req.user.id]);
    npcs.push({...entity,id:n.id,npc_template_id:n.id,status:n.room_status,state:parseJsonField(n.state,{}),combat_side:n.combat_side,current_hp:Number(n.current_hp)||0,max_hp:Number(n.room_max_hp)||Number(n.max_hp)||100,turn_actions_remaining:Number(n.turn_actions_remaining)||0,skill_charge_state:parseJsonField(n.skill_charge_state,{}),map_node_id:n.map_node_id||null,map_node_name:n.map_node_name||'',revealed:true,relationship:relationship?{...relationship,memories:parseJsonField(relationship.memories,[])}:null});
  }
  const dungeonRows=await all(`SELECT rd.status,rd.state,rd.map_node_id,mn.name AS map_node_name,dt.* FROM room_dungeons rd JOIN dungeon_templates dt ON dt.id=rd.dungeon_template_id LEFT JOIN room_map_nodes mn ON mn.id=rd.map_node_id WHERE rd.room_id=? ORDER BY dt.id`,[roomId]);
  const dungeons=dungeonRows.filter(d=>isAdmin||!d.map_node_id||String(d.map_node_id)===String(viewerNodeId||'')).map(d=>({...d,state:parseJsonField(d.state,{}),config:parseWorldConfig(d.config),map_node_id:d.map_node_id||null,map_node_name:d.map_node_name||''}));
  res.json({monsters:visibleMonsters,npcs,dungeons,viewer_map_node_id:viewerNodeId||null});
}catch(e){console.error(e);res.status(e.status||500).json({error:e.message||'讀取世界資料失敗'});}});

app.post('/api/admin/rooms/:id/monsters',auth,adminAuth,async(req,res)=>{try{
  const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);const t=await get(`SELECT * FROM monster_templates WHERE id=?`,[req.body.template_id]);if(!t)return res.status(404).json({error:'找不到怪物卡'});
  const mapNodeId=await validRoomMapNodeId(roomId,req.body.map_node_id);
  const entity=parseWorldEntityRow(t);const attrs=entity.final_attributes||{};const spirit=Math.max(0,Number(attrs.spirit)||0);const fifth=Math.max(0,Number(attrs.great_way??attrs.faith??attrs.fifth)||0);const hp=Math.max(1,Number(t.max_hp)||100);
  const row=await get(`INSERT INTO room_monsters(room_id,monster_template_id,status,current_hp,max_hp,current_spirit,max_spirit,current_fifth,max_fifth,map_node_id) VALUES(?,?, 'hidden', ?, ?, ?, ?, ?, ?, ?) RETURNING *`,[roomId,t.id,hp,hp,spirit,spirit,fifth,fifth,mapNodeId]);res.json({ok:true,instance:row});
}catch(e){res.status(e.status||500).json({error:e.message||'加入怪物失敗'});}});
app.post('/api/admin/rooms/:id/monsters/:monsterId/encounter',auth,adminAuth,async(req,res)=>{try{
  const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);const inst=await get(`SELECT * FROM room_monsters WHERE id=? AND room_id=?`,[req.params.monsterId,roomId]);if(!inst)return res.status(404).json({error:'找不到房間怪物'});
  await run(`UPDATE room_monsters SET status='encountered',updated_at=CURRENT_TIMESTAMP WHERE id=?`,[inst.id]);const members=await usersAtMapNode(roomId,inst.map_node_id);
  for(const userId of members){await run(`INSERT INTO monster_discoveries(room_monster_id,user_id,encountered,investigated) VALUES(?,?,TRUE,FALSE) ON CONFLICT(room_monster_id,user_id) DO UPDATE SET encountered=TRUE,updated_at=CURRENT_TIMESTAMP`,[inst.id,userId]);}
  await addEvent(roomId,req.user.id,'encounter',{text:'⚠️ 此地點的玩家遭遇了未知怪物。',monster_id:inst.id,map_node_id:inst.map_node_id||null});res.json({ok:true});
}catch(e){res.status(e.status||500).json({error:e.message||'設定遭遇失敗'});}});
app.delete('/api/admin/rooms/:id/monsters/:monsterId',auth,adminAuth,async(req,res)=>{try{await requireRoomAdmin(req.user.id,req.params.id);await run(`DELETE FROM room_monsters WHERE id=? AND room_id=?`,[req.params.monsterId,req.params.id]);res.json({ok:true});}catch(e){res.status(e.status||500).json({error:e.message||'移除怪物失敗'});}});

app.post('/api/admin/rooms/:id/npcs',auth,adminAuth,async(req,res)=>{try{
  const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);const t=await get(`SELECT * FROM npc_templates WHERE id=?`,[req.body.template_id]);if(!t)return res.status(404).json({error:'找不到 NPC 卡'});
  const mapNodeId=await validRoomMapNodeId(roomId,req.body.map_node_id);
  const entity=parseWorldEntityRow(t);const attrs=entity.final_attributes||{};const spirit=Math.max(0,Number(attrs.spirit)||0);const fifth=Math.max(0,Number(attrs.great_way??attrs.faith??attrs.fifth)||0);const hp=Math.max(1,Number(t.max_hp)||Math.max(1,(Number(attrs.constitution)||0)*2)||100);const side=['ally','enemy','neutral'].includes(String(parseWorldConfig(t.config).combat_side||''))?String(parseWorldConfig(t.config).combat_side):'neutral';
  await run(`INSERT INTO room_npcs(room_id,npc_template_id,combat_side,current_hp,max_hp,current_spirit,max_spirit,current_fifth,max_fifth,map_node_id) VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(room_id,npc_template_id) DO UPDATE SET combat_side=EXCLUDED.combat_side,map_node_id=EXCLUDED.map_node_id,current_hp=CASE WHEN room_npcs.current_hp<=0 THEN EXCLUDED.current_hp ELSE room_npcs.current_hp END,max_hp=EXCLUDED.max_hp,max_spirit=EXCLUDED.max_spirit,max_fifth=EXCLUDED.max_fifth,updated_at=CURRENT_TIMESTAMP`,[roomId,t.id,side,hp,hp,spirit,spirit,fifth,fifth,mapNodeId]);res.json({ok:true});
}catch(e){res.status(e.status||500).json({error:e.message||'加入 NPC 失敗'});}});
app.patch('/api/admin/rooms/:id/npcs/:templateId/combat-side',auth,adminAuth,async(req,res)=>{try{
  await requireRoomAdmin(req.user.id,req.params.id);const side=['ally','enemy','neutral'].includes(String(req.body.side||''))?String(req.body.side):'neutral';const row=await get(`UPDATE room_npcs SET combat_side=?,updated_at=CURRENT_TIMESTAMP WHERE room_id=? AND npc_template_id=? RETURNING *`,[side,req.params.id,req.params.templateId]);if(!row)return res.status(404).json({error:'找不到房間 NPC'});res.json({ok:true,npc:row});
}catch(e){res.status(e.status||500).json({error:e.message||'設定 NPC 戰鬥立場失敗'});}});
app.delete('/api/admin/rooms/:id/npcs/:templateId',auth,adminAuth,async(req,res)=>{try{await requireRoomAdmin(req.user.id,req.params.id);await run(`DELETE FROM room_npcs WHERE room_id=? AND npc_template_id=?`,[req.params.id,req.params.templateId]);res.json({ok:true});}catch(e){res.status(e.status||500).json({error:e.message||'移除 NPC 失敗'});}});
app.post('/api/admin/rooms/:id/dungeons',auth,adminAuth,async(req,res)=>{try{const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);const mapNodeId=await validRoomMapNodeId(roomId,req.body.map_node_id);await run(`INSERT INTO room_dungeons(room_id,dungeon_template_id,map_node_id) VALUES(?,?,?) ON CONFLICT(room_id,dungeon_template_id) DO UPDATE SET map_node_id=EXCLUDED.map_node_id`,[roomId,req.body.template_id,mapNodeId]);res.json({ok:true});}catch(e){res.status(e.status||500).json({error:e.message||'加入副本失敗'});}});
app.delete('/api/admin/rooms/:id/dungeons/:templateId',auth,adminAuth,async(req,res)=>{try{await requireRoomAdmin(req.user.id,req.params.id);await run(`DELETE FROM room_dungeons WHERE room_id=? AND dungeon_template_id=?`,[req.params.id,req.params.templateId]);res.json({ok:true});}catch(e){res.status(e.status||500).json({error:e.message||'移除副本失敗'});}});

app.post('/api/rooms/:id/monsters/:monsterId/investigate',auth,async(req,res)=>{try{
  await requireRoomMember(req.user.id,req.params.id);const character=await requireCharacter(req.user.id);const skill=(Array.isArray(character.skills)?character.skills:[]).find(x=>String(x.id)===String(req.body.skill_id));if(!skill)return res.status(404).json({error:'找不到探查技能'});
  const cfg=skillCombatConfig(skill);if(!cfg.investigation_skill)return res.status(400).json({error:'這個技能不是探查類技能'});
  const row=await get(`SELECT rm.*,mt.investigation_dc,mt.name FROM room_monsters rm JOIN monster_templates mt ON mt.id=rm.monster_template_id WHERE rm.id=? AND rm.room_id=?`,[req.params.monsterId,req.params.id]);if(!row)return res.status(404).json({error:'找不到怪物'});
  const viewerNodeId=await getViewerMapNodeId(req.user.id,Number(req.params.id));if(row.map_node_id&&String(row.map_node_id)!==String(viewerNodeId||''))return res.status(403).json({error:'你目前不在這個怪物所在的地點'});
  const disc=await get(`SELECT * FROM monster_discoveries WHERE room_monster_id=? AND user_id=?`,[row.id,req.user.id]);if(!(disc&&disc.encountered) && row.status!=='encountered')return res.status(403).json({error:'你尚未真正遭遇這個怪物'});
  let attr=0;if(cfg.investigation_attribute==='agility')attr=Number(character.final_agility)||0;else if(cfg.investigation_attribute==='luck')attr=Number(character.final_luck)||0;else if(cfg.investigation_attribute==='fifth')attr=Number(character.fifth_current)||0;else attr=Number(character.final_spirit)||0;
  const attrBonus=Math.min(40,Math.floor(Math.sqrt(Math.max(0,attr))*3));const rate=Math.max(5,Math.min(95,cfg.investigation_base_rate+cfg.investigation_bonus+attrBonus-(Math.max(1,Number(row.investigation_dc)||50)-50)));const roll=1+Math.floor(Math.random()*100);const success=roll<=rate;
  await run(`INSERT INTO monster_discoveries(room_monster_id,user_id,encountered,investigated,last_roll,last_rate) VALUES(?,?,TRUE,?,?,?) ON CONFLICT(room_monster_id,user_id) DO UPDATE SET encountered=TRUE,investigated=(monster_discoveries.investigated OR EXCLUDED.investigated),last_roll=EXCLUDED.last_roll,last_rate=EXCLUDED.last_rate,updated_at=CURRENT_TIMESTAMP`,[row.id,req.user.id,success,roll,rate]);
  await addEvent(req.params.id,req.user.id,'investigation',{text:`${character.name} 使用「${skill.name}」探查未知怪物：${success?'成功，怪物卡完整顯現':'失敗'}（${roll}/${rate}）`,monster_id:row.id,roll,rate,success});
  res.json({ok:true,success,roll,rate});
}catch(e){console.error(e);res.status(e.status||500).json({error:e.message||'探查失敗'});}});

app.post('/api/rooms/:id/ai/step',auth,adminAuth,async(req,res)=>{try{
  const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);const messages=await advanceWorldAi(roomId,req.user.id);if(!messages.length)messages.push('AI 本次沒有觸發新的行動。');
  for(const text of messages)await addEvent(roomId,req.user.id,'ai',{text});const snapshot=await roomSnapshot(roomId);io.to('room:'+roomId).emit('room:snapshot',snapshot);emitTeamUpdate(roomId,null);res.json({ok:true,messages,snapshot});
}catch(e){console.error('WORLD AI STEP ERROR',e);res.status(e.status||500).json({error:e.message||'AI 推進失敗'});}});

app.get('/api/game-room/npcs',auth,async(req,res)=>{try{const rows=await all(`SELECT * FROM npc_templates WHERE in_game_room=TRUE ORDER BY id`);res.json({npcs:rows.map(parseWorldEntityRow)});}catch(e){res.status(500).json({error:'讀取遊戲間 NPC 失敗'});}});

app.post('/api/rooms/:id/summons/:summonId/mount',auth,async(req,res)=>{try{
  await requireRoomMember(req.user.id,req.params.id);const character=await requireCharacter(req.user.id);const hasRide=(Array.isArray(character.skills)?character.skills:[]).some(skill=>skillCombatConfig(skill).riding_skill);if(!hasRide)return res.status(403).json({error:'你需要先取得騎乘類技能'});
  const summon=await get(`SELECT * FROM summon_instances WHERE id=? AND owner_user_id=? AND room_id=? AND status='active'`,[req.params.summonId,req.user.id,req.params.id]);if(!summon)return res.status(404).json({error:'找不到可騎乘的召喚物'});
  await run(`UPDATE room_members SET mounted_summon_id=? WHERE room_id=? AND user_id=?`,[summon.id,req.params.id,req.user.id]);await addEvent(req.params.id,req.user.id,'mount',{text:`${character.name} 騎乘了「${summon.name}」`,summon_id:summon.id});const snapshot=await roomSnapshot(req.params.id);io.to('room:'+req.params.id).emit('room:snapshot',snapshot);res.json({ok:true,room:snapshot});
}catch(e){res.status(e.status||500).json({error:e.message||'騎乘失敗'});}});
app.post('/api/rooms/:id/summons/unmount',auth,async(req,res)=>{try{await requireRoomMember(req.user.id,req.params.id);const character=await requireCharacter(req.user.id);await run(`UPDATE room_members SET mounted_summon_id=NULL WHERE room_id=? AND user_id=?`,[req.params.id,req.user.id]);await addEvent(req.params.id,req.user.id,'mount',{text:`${character.name} 結束騎乘`});const snapshot=await roomSnapshot(req.params.id);io.to('room:'+req.params.id).emit('room:snapshot',snapshot);res.json({ok:true,room:snapshot});}catch(e){res.status(e.status||500).json({error:e.message||'解除騎乘失敗'});}});
app.post('/api/rooms/:id/charge/cancel',auth,async(req,res)=>{try{await requireRoomMember(req.user.id,req.params.id);await run(`UPDATE room_members SET skill_charge_state='{}'::jsonb WHERE room_id=? AND user_id=?`,[req.params.id,req.user.id]);const snapshot=await roomSnapshot(req.params.id);io.to('room:'+req.params.id).emit('room:snapshot',snapshot);res.json({ok:true,room:snapshot});}catch(e){res.status(e.status||500).json({error:e.message||'取消蓄力失敗'});}});

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
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS class_resources JSONB NOT NULL DEFAULT '{}'::jsonb`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS inventory JSONB NOT NULL DEFAULT '[]'::jsonb`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS weird_coins BIGINT NOT NULL DEFAULT 0`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS game_coins BIGINT NOT NULL DEFAULT 0`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS game_coin_unlocked BOOLEAN NOT NULL DEFAULT FALSE`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS profession_progress JSONB NOT NULL DEFAULT '{}'::jsonb`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS inventory_slots INTEGER NOT NULL DEFAULT 5`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS bloodline VARCHAR(120) NOT NULL DEFAULT ''`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS bloodline_effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS profession_effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS great_way_effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS faith_effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS self_intro TEXT NOT NULL DEFAULT ''`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS game_room_unlocked BOOLEAN NOT NULL DEFAULT FALSE`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS name_locked BOOLEAN NOT NULL DEFAULT FALSE`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS backpack_bonus_applied BOOLEAN NOT NULL DEFAULT FALSE`,
    `ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS gear_loadout JSONB NOT NULL DEFAULT '{"equipment":[],"weapons":[]}'::jsonb`,
    `ALTER TABLE rooms ADD COLUMN IF NOT EXISTS map_name VARCHAR(120) NOT NULL DEFAULT '未設定地點'`,
    `ALTER TABLE rooms ADD COLUMN IF NOT EXISTS map_image_url TEXT NOT NULL DEFAULT ''`,
    `ALTER TABLE rooms ADD COLUMN IF NOT EXISTS map_description TEXT NOT NULL DEFAULT ''`,
    `ALTER TABLE rooms ADD COLUMN IF NOT EXISTS game_day INTEGER NOT NULL DEFAULT 1`,
    `ALTER TABLE rooms ADD COLUMN IF NOT EXISTS game_hour INTEGER NOT NULL DEFAULT 8`,
    `ALTER TABLE rooms ADD COLUMN IF NOT EXISTS game_minute INTEGER NOT NULL DEFAULT 0`,
    `ALTER TABLE rooms ADD COLUMN IF NOT EXISTS battle_active BOOLEAN NOT NULL DEFAULT FALSE`,
    `ALTER TABLE rooms ADD COLUMN IF NOT EXISTS current_actor_type VARCHAR(20) NOT NULL DEFAULT 'player'`,
    `ALTER TABLE rooms ADD COLUMN IF NOT EXISTS current_actor_ref_id BIGINT`,
    `ALTER TABLE rooms ADD COLUMN IF NOT EXISTS combat_order JSONB NOT NULL DEFAULT '[]'::jsonb`,
    `ALTER TABLE rooms ADD COLUMN IF NOT EXISTS combat_index INTEGER NOT NULL DEFAULT 0`,
    `ALTER TABLE room_members ADD COLUMN IF NOT EXISTS turn_actions_remaining INTEGER NOT NULL DEFAULT 0`,
    `ALTER TABLE room_members ADD COLUMN IF NOT EXISTS skill_charge_state JSONB NOT NULL DEFAULT '{}'::jsonb`,
    `ALTER TABLE room_members ADD COLUMN IF NOT EXISTS combat_reaction VARCHAR(12) NOT NULL DEFAULT 'dodge'`,
    `ALTER TABLE room_members ADD COLUMN IF NOT EXISTS combat_reaction_skill_id TEXT NOT NULL DEFAULT ''`,
    `ALTER TABLE room_members ADD COLUMN IF NOT EXISTS combat_reaction_round INTEGER NOT NULL DEFAULT 0`,
    `ALTER TABLE room_members ADD COLUMN IF NOT EXISTS combat_reaction_uses INTEGER NOT NULL DEFAULT 0`,
    `ALTER TABLE room_members ADD COLUMN IF NOT EXISTS combat_state VARCHAR(20) NOT NULL DEFAULT 'active'`
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
    `CREATE TABLE IF NOT EXISTS bloodline_templates (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(120) NOT NULL UNIQUE,
      description TEXT NOT NULL DEFAULT '',
      effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb,
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


  statements.push(
    `CREATE TABLE IF NOT EXISTS shop_items (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(120) NOT NULL,
      item_type VARCHAR(30) NOT NULL DEFAULT 'item',
      description TEXT NOT NULL DEFAULT '',
      price_weird BIGINT NOT NULL DEFAULT 0,
      quantity INTEGER NOT NULL DEFAULT 1,
      active BOOLEAN NOT NULL DEFAULT TRUE,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );


  statements.push(
    `CREATE TABLE IF NOT EXISTS monster_templates (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(120) NOT NULL,
      category VARCHAR(80) NOT NULL DEFAULT '怪物',
      rank VARCHAR(10) NOT NULL DEFAULT 'G',
      description TEXT NOT NULL DEFAULT '',
      public_hint TEXT NOT NULL DEFAULT '',
      image_url TEXT NOT NULL DEFAULT '',
      max_hp INTEGER NOT NULL DEFAULT 100,
      investigation_dc INTEGER NOT NULL DEFAULT 50,
      attributes JSONB NOT NULL DEFAULT '{}'::jsonb,
      skills JSONB NOT NULL DEFAULT '[]'::jsonb,
      drops JSONB NOT NULL DEFAULT '[]'::jsonb,
      ai_level INTEGER NOT NULL DEFAULT 1,
      ai_behavior VARCHAR(40) NOT NULL DEFAULT 'balanced',
      config JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );

  statements.push(
    `CREATE TABLE IF NOT EXISTS npc_templates (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(120) NOT NULL,
      role_name VARCHAR(120) NOT NULL DEFAULT '',
      description TEXT NOT NULL DEFAULT '',
      image_url TEXT NOT NULL DEFAULT '',
      ai_level INTEGER NOT NULL DEFAULT 1,
      ai_behavior VARCHAR(40) NOT NULL DEFAULT 'balanced',
      speech_enabled BOOLEAN NOT NULL DEFAULT TRUE,
      in_game_room BOOLEAN NOT NULL DEFAULT FALSE,
      config JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );

  statements.push(
    `CREATE TABLE IF NOT EXISTS dungeon_templates (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(120) NOT NULL,
      rank VARCHAR(10) NOT NULL DEFAULT 'G',
      description TEXT NOT NULL DEFAULT '',
      map_name VARCHAR(120) NOT NULL DEFAULT '',
      map_image_url TEXT NOT NULL DEFAULT '',
      ai_level INTEGER NOT NULL DEFAULT 1,
      ai_behavior VARCHAR(40) NOT NULL DEFAULT 'balanced',
      config JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );

  statements.push(
    `CREATE TABLE IF NOT EXISTS room_monsters (
      id BIGSERIAL PRIMARY KEY,
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      monster_template_id BIGINT NOT NULL REFERENCES monster_templates(id) ON DELETE CASCADE,
      status VARCHAR(30) NOT NULL DEFAULT 'hidden',
      current_hp INTEGER NOT NULL DEFAULT 100,
      max_hp INTEGER NOT NULL DEFAULT 100,
      state JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );

  statements.push(`ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS turn_actions_remaining INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS skill_charge_state JSONB NOT NULL DEFAULT '{}'::jsonb`);
  statements.push(`ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS current_spirit INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS max_spirit INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS current_fifth INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS max_fifth INTEGER NOT NULL DEFAULT 0`);

  statements.push(
    `CREATE TABLE IF NOT EXISTS monster_discoveries (
      room_monster_id BIGINT NOT NULL REFERENCES room_monsters(id) ON DELETE CASCADE,
      user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      encountered BOOLEAN NOT NULL DEFAULT FALSE,
      investigated BOOLEAN NOT NULL DEFAULT FALSE,
      last_roll INTEGER,
      last_rate INTEGER,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      PRIMARY KEY(room_monster_id, user_id)
    )`
  );

  statements.push(
    `CREATE TABLE IF NOT EXISTS room_npcs (
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      npc_template_id BIGINT NOT NULL REFERENCES npc_templates(id) ON DELETE CASCADE,
      status VARCHAR(30) NOT NULL DEFAULT 'active',
      state JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      PRIMARY KEY(room_id, npc_template_id)
    )`
  );

  statements.push(
    `CREATE TABLE IF NOT EXISTS room_dungeons (
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      dungeon_template_id BIGINT NOT NULL REFERENCES dungeon_templates(id) ON DELETE CASCADE,
      status VARCHAR(30) NOT NULL DEFAULT 'available',
      state JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      PRIMARY KEY(room_id, dungeon_template_id)
    )`
  );

  statements.push(`ALTER TABLE summon_templates ADD COLUMN IF NOT EXISTS control_mode VARCHAR(20) NOT NULL DEFAULT 'manual'`);
  statements.push(`ALTER TABLE summon_templates ADD COLUMN IF NOT EXISTS behavior_mode VARCHAR(30) NOT NULL DEFAULT 'balanced'`);
  statements.push(`ALTER TABLE summon_templates ADD COLUMN IF NOT EXISTS speech_enabled BOOLEAN NOT NULL DEFAULT FALSE`);
  statements.push(`ALTER TABLE summon_templates ADD COLUMN IF NOT EXISTS effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb`);
  statements.push(`ALTER TABLE monster_templates ADD COLUMN IF NOT EXISTS element_affinity JSONB NOT NULL DEFAULT '{}'::jsonb`);
  statements.push(`ALTER TABLE monster_templates ADD COLUMN IF NOT EXISTS effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb`);
  statements.push(`ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS attributes JSONB NOT NULL DEFAULT '{}'::jsonb`);
  statements.push(`ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS element_affinity JSONB NOT NULL DEFAULT '{}'::jsonb`);
  statements.push(`ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb`);
  statements.push(`ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS skills JSONB NOT NULL DEFAULT '[]'::jsonb`);
  statements.push(`ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS max_hp INTEGER NOT NULL DEFAULT 100`);
  statements.push(`ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS combat_side VARCHAR(20) NOT NULL DEFAULT 'neutral'`);
  statements.push(`ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP`);
  statements.push(`ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS current_hp INTEGER NOT NULL DEFAULT 100`);
  statements.push(`ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS max_hp INTEGER NOT NULL DEFAULT 100`);
  statements.push(`ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS current_spirit INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS max_spirit INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS current_fifth INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS max_fifth INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS turn_actions_remaining INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS skill_charge_state JSONB NOT NULL DEFAULT '{}'::jsonb`);
  statements.push(`ALTER TABLE summon_instances ADD COLUMN IF NOT EXISTS room_id BIGINT REFERENCES rooms(id) ON DELETE SET NULL`);
  statements.push(`ALTER TABLE summon_instances ADD COLUMN IF NOT EXISTS source_skill_id VARCHAR(100)`);
  statements.push(`ALTER TABLE room_members ADD COLUMN IF NOT EXISTS mounted_summon_id BIGINT REFERENCES summon_instances(id) ON DELETE SET NULL`);
  statements.push(`ALTER TABLE shop_items ADD COLUMN IF NOT EXISTS item_category VARCHAR(80) NOT NULL DEFAULT ''`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_summon_instances_owner_status ON summon_instances(owner_user_id, status)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_summon_instances_room_status ON summon_instances(room_id, status)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_room_monsters_room_status ON room_monsters(room_id, status)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_room_npcs_room_side ON room_npcs(room_id, combat_side, status)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_monster_discoveries_user ON monster_discoveries(user_id, investigated)`);

  statements.push(
    `CREATE TABLE IF NOT EXISTS combat_entity_statuses (
      id BIGSERIAL PRIMARY KEY,
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      entity_type VARCHAR(20) NOT NULL,
      entity_id BIGINT NOT NULL,
      status_template_id BIGINT,
      name VARCHAR(120) NOT NULL,
      description TEXT NOT NULL DEFAULT '',
      remaining_rounds INTEGER NOT NULL DEFAULT 0,
      effects JSONB NOT NULL DEFAULT '{}'::jsonb,
      source TEXT NOT NULL DEFAULT '',
      active BOOLEAN NOT NULL DEFAULT TRUE,
      stacks INTEGER NOT NULL DEFAULT 1,
      stack_mode VARCHAR(20) NOT NULL DEFAULT 'refresh',
      max_stacks INTEGER NOT NULL DEFAULT 1,
      dispellable BOOLEAN NOT NULL DEFAULT TRUE,
      expires_by_round BOOLEAN NOT NULL DEFAULT FALSE,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );
  statements.push(`CREATE INDEX IF NOT EXISTS idx_combat_entity_statuses_lookup ON combat_entity_statuses(room_id,entity_type,entity_id,active)`);

  /* =========================
     V15: 規則手冊 / 線索 / 狀態 / 地圖節點 / 隱藏判定 / 道具模板
  ========================= */
  statements.push(
    `CREATE TABLE IF NOT EXISTS clue_templates (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(160) NOT NULL,
      category VARCHAR(80) NOT NULL DEFAULT '一般線索',
      description TEXT NOT NULL DEFAULT '',
      image_url TEXT NOT NULL DEFAULT '',
      linked_rule_id BIGINT,
      reveal_level INTEGER NOT NULL DEFAULT 1,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );

  statements.push(
    `CREATE TABLE IF NOT EXISTS character_clues (
      clue_id BIGINT NOT NULL REFERENCES clue_templates(id) ON DELETE CASCADE,
      user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      note TEXT NOT NULL DEFAULT '',
      acquired_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      PRIMARY KEY(clue_id,user_id)
    )`
  );

  statements.push(
    `CREATE TABLE IF NOT EXISTS status_templates (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(120) NOT NULL,
      category VARCHAR(60) NOT NULL DEFAULT '狀態',
      description TEXT NOT NULL DEFAULT '',
      duration_rounds INTEGER NOT NULL DEFAULT 0,
      effects JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );

  statements.push(
    `CREATE TABLE IF NOT EXISTS character_statuses (
      id BIGSERIAL PRIMARY KEY,
      user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      status_template_id BIGINT REFERENCES status_templates(id) ON DELETE SET NULL,
      name VARCHAR(120) NOT NULL,
      description TEXT NOT NULL DEFAULT '',
      remaining_rounds INTEGER NOT NULL DEFAULT 0,
      effects JSONB NOT NULL DEFAULT '{}'::jsonb,
      source TEXT NOT NULL DEFAULT '',
      active BOOLEAN NOT NULL DEFAULT TRUE,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );

  statements.push(
    `CREATE TABLE IF NOT EXISTS room_map_nodes (
      id BIGSERIAL PRIMARY KEY,
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      name VARCHAR(120) NOT NULL,
      description TEXT NOT NULL DEFAULT '',
      image_url TEXT NOT NULL DEFAULT '',
      connections JSONB NOT NULL DEFAULT '[]'::jsonb,
      hidden BOOLEAN NOT NULL DEFAULT FALSE,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );
  statements.push(`ALTER TABLE rooms ADD COLUMN IF NOT EXISTS current_map_node_id BIGINT REFERENCES room_map_nodes(id) ON DELETE SET NULL`);

  statements.push(
    `CREATE TABLE IF NOT EXISTS hidden_rolls (
      id BIGSERIAL PRIMARY KEY,
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      character_name VARCHAR(120) NOT NULL DEFAULT '',
      check_name VARCHAR(80) NOT NULL,
      target_value INTEGER NOT NULL DEFAULT 0,
      roll_value INTEGER NOT NULL,
      success_level VARCHAR(30) NOT NULL DEFAULT '失敗',
      note TEXT NOT NULL DEFAULT '',
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );

  statements.push(
    `CREATE TABLE IF NOT EXISTS item_templates (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(120) NOT NULL UNIQUE,
      category VARCHAR(80) NOT NULL DEFAULT '道具',
      item_type VARCHAR(30) NOT NULL DEFAULT 'normal',
      description TEXT NOT NULL DEFAULT '',
      effects JSONB NOT NULL DEFAULT '{}'::jsonb,
      consume_on_use BOOLEAN NOT NULL DEFAULT TRUE,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );
  statements.push(`ALTER TABLE status_templates ADD COLUMN IF NOT EXISTS stack_mode VARCHAR(20) NOT NULL DEFAULT 'refresh'`);
  statements.push(`ALTER TABLE status_templates ADD COLUMN IF NOT EXISTS max_stacks INTEGER NOT NULL DEFAULT 1`);
  statements.push(`ALTER TABLE status_templates ADD COLUMN IF NOT EXISTS dispellable BOOLEAN NOT NULL DEFAULT TRUE`);
  statements.push(`ALTER TABLE status_templates ADD COLUMN IF NOT EXISTS tags JSONB NOT NULL DEFAULT '[]'::jsonb`);
  statements.push(`ALTER TABLE character_statuses ADD COLUMN IF NOT EXISTS stacks INTEGER NOT NULL DEFAULT 1`);
  statements.push(`ALTER TABLE character_statuses ADD COLUMN IF NOT EXISTS stack_mode VARCHAR(20) NOT NULL DEFAULT 'refresh'`);
  statements.push(`ALTER TABLE character_statuses ADD COLUMN IF NOT EXISTS max_stacks INTEGER NOT NULL DEFAULT 1`);
  statements.push(`ALTER TABLE character_statuses ADD COLUMN IF NOT EXISTS dispellable BOOLEAN NOT NULL DEFAULT TRUE`);
  statements.push(`ALTER TABLE character_statuses ADD COLUMN IF NOT EXISTS expires_by_round BOOLEAN NOT NULL DEFAULT FALSE`);
  statements.push(`UPDATE character_statuses SET expires_by_round=TRUE WHERE remaining_rounds>0 AND expires_by_round=FALSE`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_character_statuses_user_active ON character_statuses(user_id,active)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_character_clues_user ON character_clues(user_id)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_room_map_nodes_room ON room_map_nodes(room_id)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_hidden_rolls_room_created ON hidden_rolls(room_id,created_at DESC)`);

  statements.push(
    `CREATE TABLE IF NOT EXISTS horror_rules (
      id BIGSERIAL PRIMARY KEY,
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      name VARCHAR(160) NOT NULL,
      rule_text TEXT NOT NULL DEFAULT '',
      public_hint TEXT NOT NULL DEFAULT '',
      location_contains VARCHAR(160) NOT NULL DEFAULT '',
      start_minute INTEGER NOT NULL DEFAULT 0,
      end_minute INTEGER NOT NULL DEFAULT 1439,
      keywords JSONB NOT NULL DEFAULT '[]'::jsonb,
      match_mode VARCHAR(10) NOT NULL DEFAULT 'any',
      consequence TEXT NOT NULL DEFAULT '',
      active BOOLEAN NOT NULL DEFAULT TRUE,
      trigger_count INTEGER NOT NULL DEFAULT 0,
      last_triggered_at TIMESTAMPTZ,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );
  statements.push(
    `CREATE TABLE IF NOT EXISTS rule_discoveries (
      rule_id BIGINT NOT NULL REFERENCES horror_rules(id) ON DELETE CASCADE,
      user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      knowledge_level INTEGER NOT NULL DEFAULT 1,
      note TEXT NOT NULL DEFAULT '',
      discovered_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      PRIMARY KEY(rule_id,user_id)
    )`
  );
  statements.push(`CREATE INDEX IF NOT EXISTS idx_rule_discoveries_user ON rule_discoveries(user_id)`);

  statements.push(`CREATE INDEX IF NOT EXISTS idx_horror_rules_room_active ON horror_rules(room_id, active)`);


  /* =========================
     V22: 正式組隊 / 個別視野 / 隊伍共享情報
  ========================= */
  statements.push(
    `CREATE TABLE IF NOT EXISTS room_teams (
      id BIGSERIAL PRIMARY KEY,
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      name VARCHAR(120) NOT NULL,
      leader_user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,
      current_map_node_id BIGINT REFERENCES room_map_nodes(id) ON DELETE SET NULL,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );
  statements.push(
    `CREATE TABLE IF NOT EXISTS room_team_members (
      team_id BIGINT NOT NULL REFERENCES room_teams(id) ON DELETE CASCADE,
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      joined_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      PRIMARY KEY(team_id,user_id),
      UNIQUE(room_id,user_id)
    )`
  );
  statements.push(
    `CREATE TABLE IF NOT EXISTS team_shared_clues (
      team_id BIGINT NOT NULL REFERENCES room_teams(id) ON DELETE CASCADE,
      clue_id BIGINT NOT NULL REFERENCES clue_templates(id) ON DELETE CASCADE,
      shared_by_user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,
      note TEXT NOT NULL DEFAULT '',
      shared_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      PRIMARY KEY(team_id,clue_id)
    )`
  );
  statements.push(
    `CREATE TABLE IF NOT EXISTS team_shared_rules (
      team_id BIGINT NOT NULL REFERENCES room_teams(id) ON DELETE CASCADE,
      rule_id BIGINT NOT NULL REFERENCES horror_rules(id) ON DELETE CASCADE,
      knowledge_level INTEGER NOT NULL DEFAULT 1,
      shared_by_user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,
      note TEXT NOT NULL DEFAULT '',
      shared_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      PRIMARY KEY(team_id,rule_id)
    )`
  );
  statements.push(
    `CREATE TABLE IF NOT EXISTS personal_visions (
      id BIGSERIAL PRIMARY KEY,
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      title VARCHAR(180) NOT NULL DEFAULT '私人視野',
      content TEXT NOT NULL DEFAULT '',
      vision_type VARCHAR(40) NOT NULL DEFAULT 'perception',
      shareable BOOLEAN NOT NULL DEFAULT TRUE,
      is_read BOOLEAN NOT NULL DEFAULT FALSE,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );
  statements.push(
    `CREATE TABLE IF NOT EXISTS team_shared_notes (
      id BIGSERIAL PRIMARY KEY,
      team_id BIGINT NOT NULL REFERENCES room_teams(id) ON DELETE CASCADE,
      shared_by_user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,
      title VARCHAR(180) NOT NULL DEFAULT '共享情報',
      content TEXT NOT NULL DEFAULT '',
      note_type VARCHAR(40) NOT NULL DEFAULT 'intel',
      source_vision_id BIGINT REFERENCES personal_visions(id) ON DELETE SET NULL,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );
  statements.push(
    `CREATE TABLE IF NOT EXISTS team_messages (
      id BIGSERIAL PRIMARY KEY,
      team_id BIGINT NOT NULL REFERENCES room_teams(id) ON DELETE CASCADE,
      user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,
      content TEXT NOT NULL,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );
  statements.push(`CREATE INDEX IF NOT EXISTS idx_room_teams_room ON room_teams(room_id)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_room_team_members_room ON room_team_members(room_id)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_room_team_members_user ON room_team_members(user_id)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_personal_visions_user_room ON personal_visions(user_id,room_id,created_at DESC)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_team_messages_team_created ON team_messages(team_id,created_at DESC)`);


  /* =========================
     V24: 地圖實體 / 噪音警戒 / 追逐 / 污染 / 定時事件 / Boss 階段
  ========================= */
  statements.push(`ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS pollution INTEGER NOT NULL DEFAULT 0`);

  statements.push(`ALTER TABLE room_members ADD COLUMN IF NOT EXISTS current_sanity INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE room_members ADD COLUMN IF NOT EXISTS max_sanity INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE room_members ADD COLUMN IF NOT EXISTS sanity_initialized BOOLEAN NOT NULL DEFAULT FALSE`);
  statements.push(`ALTER TABLE room_members ADD COLUMN IF NOT EXISTS sanity_stage VARCHAR(40) NOT NULL DEFAULT 'stable'`);
  statements.push(`ALTER TABLE room_members ADD COLUMN IF NOT EXISTS seven_sin VARCHAR(40)`);

  statements.push(`ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS noise_level INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS alert_level INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS noise_threshold INTEGER NOT NULL DEFAULT 30`);
  statements.push(`ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS map_node_id BIGINT REFERENCES room_map_nodes(id) ON DELETE SET NULL`);
  statements.push(`ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS alert_level INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS chase_team_id BIGINT`);
  statements.push(`ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS boss_phase_index INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS boss_phase_name VARCHAR(160) NOT NULL DEFAULT ''`);
  statements.push(`ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS map_node_id BIGINT REFERENCES room_map_nodes(id) ON DELETE SET NULL`);
  statements.push(`ALTER TABLE room_dungeons ADD COLUMN IF NOT EXISTS map_node_id BIGINT REFERENCES room_map_nodes(id) ON DELETE SET NULL`);
  statements.push(
    `CREATE TABLE IF NOT EXISTS scheduled_world_events (
      id BIGSERIAL PRIMARY KEY,
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      name VARCHAR(180) NOT NULL,
      trigger_day INTEGER NOT NULL DEFAULT 1,
      trigger_minute INTEGER NOT NULL DEFAULT 0,
      repeat_mode VARCHAR(20) NOT NULL DEFAULT 'once',
      audience VARCHAR(20) NOT NULL DEFAULT 'room',
      target_team_id BIGINT REFERENCES room_teams(id) ON DELETE CASCADE,
      target_user_id BIGINT REFERENCES users(id) ON DELETE CASCADE,
      map_node_id BIGINT REFERENCES room_map_nodes(id) ON DELETE SET NULL,
      content TEXT NOT NULL DEFAULT '',
      active BOOLEAN NOT NULL DEFAULT TRUE,
      trigger_count INTEGER NOT NULL DEFAULT 0,
      last_trigger_total BIGINT,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );
  statements.push(
    `CREATE TABLE IF NOT EXISTS room_chases (
      id BIGSERIAL PRIMARY KEY,
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      team_id BIGINT NOT NULL REFERENCES room_teams(id) ON DELETE CASCADE,
      monster_id BIGINT NOT NULL REFERENCES room_monsters(id) ON DELETE CASCADE,
      status VARCHAR(20) NOT NULL DEFAULT 'active',
      distance INTEGER NOT NULL DEFAULT 3,
      escape_distance INTEGER NOT NULL DEFAULT 6,
      round INTEGER NOT NULL DEFAULT 1,
      last_actor_user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,
      log JSONB NOT NULL DEFAULT '[]'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    )`
  );
  statements.push(`CREATE INDEX IF NOT EXISTS idx_room_monsters_node ON room_monsters(room_id,map_node_id,status)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_room_npcs_node ON room_npcs(room_id,map_node_id,status)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_room_dungeons_node ON room_dungeons(room_id,map_node_id,status)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_scheduled_world_events_room ON scheduled_world_events(room_id,active)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_room_chases_room_team ON room_chases(room_id,team_id,status)`);



  /* =========================
     V25: DM 控制台 / 搜索掉落 / NPC 關係記憶 / 規則真偽變動 / 存檔回溯
  ========================= */
  statements.push(`ALTER TABLE horror_rules ADD COLUMN IF NOT EXISTS truth_state VARCHAR(20) NOT NULL DEFAULT 'true'`);
  statements.push(`ALTER TABLE horror_rules ADD COLUMN IF NOT EXISTS active_from_total BIGINT`);
  statements.push(`ALTER TABLE horror_rules ADD COLUMN IF NOT EXISTS active_until_total BIGINT`);
  statements.push(`ALTER TABLE horror_rules ADD COLUMN IF NOT EXISTS change_note TEXT NOT NULL DEFAULT ''`);
  statements.push(`CREATE TABLE IF NOT EXISTS npc_relationships (
    room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
    npc_template_id BIGINT NOT NULL REFERENCES npc_templates(id) ON DELETE CASCADE,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    affinity INTEGER NOT NULL DEFAULT 0,
    trust INTEGER NOT NULL DEFAULT 0,
    fear INTEGER NOT NULL DEFAULT 0,
    hostility INTEGER NOT NULL DEFAULT 0,
    memories JSONB NOT NULL DEFAULT '[]'::jsonb,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(room_id,npc_template_id,user_id)
  )`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_npc_relationships_room ON npc_relationships(room_id,npc_template_id)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_npc_relationships_user ON npc_relationships(user_id,room_id)`);
  statements.push(`CREATE TABLE IF NOT EXISTS search_containers (
    id BIGSERIAL PRIMARY KEY,
    room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
    map_node_id BIGINT REFERENCES room_map_nodes(id) ON DELETE SET NULL,
    name VARCHAR(160) NOT NULL,
    container_type VARCHAR(60) NOT NULL DEFAULT '容器',
    description TEXT NOT NULL DEFAULT '',
    hidden BOOLEAN NOT NULL DEFAULT FALSE,
    search_attribute VARCHAR(30) NOT NULL DEFAULT 'spirit',
    search_dc INTEGER NOT NULL DEFAULT 50,
    required_item VARCHAR(120) NOT NULL DEFAULT '',
    one_time BOOLEAN NOT NULL DEFAULT TRUE,
    loot JSONB NOT NULL DEFAULT '[]'::jsonb,
    active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  )`);
  statements.push(`CREATE TABLE IF NOT EXISTS search_records (
    container_id BIGINT NOT NULL REFERENCES search_containers(id) ON DELETE CASCADE,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    attempts INTEGER NOT NULL DEFAULT 0,
    success BOOLEAN NOT NULL DEFAULT FALSE,
    claimed BOOLEAN NOT NULL DEFAULT FALSE,
    last_roll INTEGER,
    last_target INTEGER,
    found JSONB NOT NULL DEFAULT '[]'::jsonb,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(container_id,user_id)
  )`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_search_containers_room_node ON search_containers(room_id,map_node_id,active)`);
  statements.push(`CREATE TABLE IF NOT EXISTS room_savepoints (
    id BIGSERIAL PRIMARY KEY,
    room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
    label VARCHAR(180) NOT NULL DEFAULT '手動存檔',
    snapshot JSONB NOT NULL,
    created_by BIGINT REFERENCES users(id) ON DELETE SET NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  )`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_room_savepoints_room_created ON room_savepoints(room_id,created_at DESC)`);

  /* =========================
     V26: 隱藏時間 / 計時道具 / 時間扭曲
  ========================= */
  statements.push(`ALTER TABLE rooms ADD COLUMN IF NOT EXISTS time_hidden BOOLEAN NOT NULL DEFAULT TRUE`);
  statements.push(`ALTER TABLE rooms ADD COLUMN IF NOT EXISTS time_distortion_offset_minutes INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS time_offset_minutes INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS grants_time_view BOOLEAN NOT NULL DEFAULT FALSE`);
  statements.push(`ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS grants_true_time BOOLEAN NOT NULL DEFAULT FALSE`);

  /* =========================
     V27: 探索 / 情報揭露 / 線索推理 / 高級時間扭曲 / 世界事件動作
  ========================= */
  statements.push(`ALTER TABLE rooms ADD COLUMN IF NOT EXISTS time_rate NUMERIC(6,2) NOT NULL DEFAULT 1`);
  statements.push(`ALTER TABLE rooms ADD COLUMN IF NOT EXISTS time_flow_offset_minutes INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS time_rate NUMERIC(6,2) NOT NULL DEFAULT 1`);
  statements.push(`ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS time_flow_offset_minutes INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS investigation_dc INTEGER NOT NULL DEFAULT 50`);
  statements.push(`ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS discovery_hint TEXT NOT NULL DEFAULT ''`);
  statements.push(`ALTER TABLE search_records ADD COLUMN IF NOT EXISTS discovered BOOLEAN NOT NULL DEFAULT FALSE`);

  statements.push(`CREATE TABLE IF NOT EXISTS map_node_discoveries (
    node_id BIGINT NOT NULL REFERENCES room_map_nodes(id) ON DELETE CASCADE,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    discovered_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(node_id,user_id)
  )`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_map_node_discoveries_user ON map_node_discoveries(user_id)`);

  statements.push(`ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS public_hint TEXT NOT NULL DEFAULT ''`);
  statements.push(`ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS investigation_dc INTEGER NOT NULL DEFAULT 50`);
  statements.push(`ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS requires_investigation BOOLEAN NOT NULL DEFAULT FALSE`);
  statements.push(`CREATE TABLE IF NOT EXISTS npc_discoveries (
    room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
    npc_template_id BIGINT NOT NULL REFERENCES npc_templates(id) ON DELETE CASCADE,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    investigated BOOLEAN NOT NULL DEFAULT FALSE,
    last_roll INTEGER,
    last_rate INTEGER,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(room_id,npc_template_id,user_id)
  )`);

  statements.push(`ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS requires_identification BOOLEAN NOT NULL DEFAULT FALSE`);
  statements.push(`ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS unknown_name VARCHAR(120) NOT NULL DEFAULT '未知物品'`);
  statements.push(`ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS identification_attribute VARCHAR(30) NOT NULL DEFAULT 'spirit'`);
  statements.push(`ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS identification_dc INTEGER NOT NULL DEFAULT 50`);
  statements.push(`CREATE TABLE IF NOT EXISTS item_identifications (
    item_template_id BIGINT NOT NULL REFERENCES item_templates(id) ON DELETE CASCADE,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    identified_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(item_template_id,user_id)
  )`);

  statements.push(`ALTER TABLE clue_templates ADD COLUMN IF NOT EXISTS room_id BIGINT REFERENCES rooms(id) ON DELETE CASCADE`);
  statements.push(`ALTER TABLE clue_templates ADD COLUMN IF NOT EXISTS map_node_id BIGINT REFERENCES room_map_nodes(id) ON DELETE SET NULL`);
  statements.push(`ALTER TABLE clue_templates ADD COLUMN IF NOT EXISTS auto_discover BOOLEAN NOT NULL DEFAULT FALSE`);
  statements.push(`ALTER TABLE clue_templates ADD COLUMN IF NOT EXISTS investigation_dc INTEGER NOT NULL DEFAULT 50`);

  statements.push(`CREATE TABLE IF NOT EXISTS clue_combinations (
    id BIGSERIAL PRIMARY KEY,
    room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
    name VARCHAR(180) NOT NULL,
    required_clue_ids JSONB NOT NULL DEFAULT '[]'::jsonb,
    result_type VARCHAR(30) NOT NULL DEFAULT 'vision',
    result_id BIGINT,
    result_title VARCHAR(180) NOT NULL DEFAULT '推理結果',
    result_content TEXT NOT NULL DEFAULT '',
    active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  )`);
  statements.push(`CREATE TABLE IF NOT EXISTS clue_combination_unlocks (
    combination_id BIGINT NOT NULL REFERENCES clue_combinations(id) ON DELETE CASCADE,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    unlocked_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(combination_id,user_id)
  )`);

  statements.push(`ALTER TABLE horror_rules ADD COLUMN IF NOT EXISTS required_profession VARCHAR(100) NOT NULL DEFAULT ''`);
  statements.push(`ALTER TABLE horror_rules ADD COLUMN IF NOT EXISTS required_bloodline VARCHAR(120) NOT NULL DEFAULT ''`);
  statements.push(`ALTER TABLE horror_rules ADD COLUMN IF NOT EXISTS required_item VARCHAR(120) NOT NULL DEFAULT ''`);
  statements.push(`ALTER TABLE horror_rules ADD COLUMN IF NOT EXISTS min_pollution INTEGER NOT NULL DEFAULT 0`);
  statements.push(`ALTER TABLE horror_rules ADD COLUMN IF NOT EXISTS max_pollution INTEGER NOT NULL DEFAULT 100`);

  statements.push(`ALTER TABLE scheduled_world_events ADD COLUMN IF NOT EXISTS time_basis VARCHAR(20) NOT NULL DEFAULT 'world'`);
  statements.push(`ALTER TABLE scheduled_world_events ADD COLUMN IF NOT EXISTS action_type VARCHAR(30) NOT NULL DEFAULT 'message'`);
  statements.push(`ALTER TABLE scheduled_world_events ADD COLUMN IF NOT EXISTS action_payload JSONB NOT NULL DEFAULT '{}'::jsonb`);


  /* =========================
     V29: 大道 / 信仰 / 諸神殿 / 自訂輪盤
  ========================= */
  statements.push(`CREATE TABLE IF NOT EXISTS great_way_templates (
    id BIGSERIAL PRIMARY KEY,
    name VARCHAR(160) NOT NULL UNIQUE,
    principle TEXT NOT NULL DEFAULT '',
    description TEXT NOT NULL DEFAULT '',
    max_rank VARCHAR(10) NOT NULL DEFAULT 'SSS',
    effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb,
    granted_skill_template_ids JSONB NOT NULL DEFAULT '[]'::jsonb,
    active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  )`);
  statements.push(`CREATE TABLE IF NOT EXISTS faith_deities (
    id BIGSERIAL PRIMARY KEY,
    name VARCHAR(160) NOT NULL UNIQUE,
    title VARCHAR(180) NOT NULL DEFAULT '',
    domains JSONB NOT NULL DEFAULT '[]'::jsonb,
    doctrine TEXT NOT NULL DEFAULT '',
    description TEXT NOT NULL DEFAULT '',
    max_rank VARCHAR(10) NOT NULL DEFAULT 'SSS',
    effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb,
    granted_skill_template_ids JSONB NOT NULL DEFAULT '[]'::jsonb,
    active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  )`);
  statements.push(`CREATE TABLE IF NOT EXISTS character_great_ways (
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    great_way_id BIGINT NOT NULL REFERENCES great_way_templates(id) ON DELETE CASCADE,
    rank VARCHAR(10) NOT NULL DEFAULT 'G',
    progress INTEGER NOT NULL DEFAULT 0,
    active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(user_id,great_way_id)
  )`);
  statements.push(`CREATE TABLE IF NOT EXISTS character_faiths (
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    deity_id BIGINT NOT NULL REFERENCES faith_deities(id) ON DELETE CASCADE,
    rank VARCHAR(10) NOT NULL DEFAULT 'G',
    devotion INTEGER NOT NULL DEFAULT 0,
    favor INTEGER NOT NULL DEFAULT 0,
    active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(user_id,deity_id)
  )`);
  statements.push(`CREATE TABLE IF NOT EXISTS custom_wheels (
    id BIGSERIAL PRIMARY KEY,
    room_id BIGINT REFERENCES rooms(id) ON DELETE CASCADE,
    name VARCHAR(180) NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    audience VARCHAR(20) NOT NULL DEFAULT 'all',
    options JSONB NOT NULL DEFAULT '[]'::jsonb,
    active BOOLEAN NOT NULL DEFAULT TRUE,
    created_by BIGINT REFERENCES users(id) ON DELETE SET NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  )`);
  statements.push(`CREATE TABLE IF NOT EXISTS wheel_spins (
    id BIGSERIAL PRIMARY KEY,
    wheel_id BIGINT NOT NULL REFERENCES custom_wheels(id) ON DELETE CASCADE,
    room_id BIGINT REFERENCES rooms(id) ON DELETE CASCADE,
    user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,
    result_index INTEGER NOT NULL DEFAULT 0,
    result_label VARCHAR(300) NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  )`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_character_great_ways_user ON character_great_ways(user_id,active)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_character_faiths_user ON character_faiths(user_id,active)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_custom_wheels_room ON custom_wheels(room_id,active)`);
  statements.push(`CREATE INDEX IF NOT EXISTS idx_wheel_spins_wheel ON wheel_spins(wheel_id,created_at DESC)`);

  for (const sql of statements) {
    await run(sql);
  }

  await run(`UPDATE character_cards SET name_locked=TRUE WHERE name_locked=FALSE AND BTRIM(COALESCE(name,''))<>'' AND name<>'未命名角色'`);
}

async function updatePlayerCharacter(userId, body) {
  const current = await requireCharacter(userId);
  const allowed = new Set([
    'name','self_intro','agility','strength','constitution','spirit','attribute_points','gear_loadout'
  ]);
  const forbidden = Object.keys(body || {}).filter(key => !allowed.has(key));
  if (forbidden.length) {
    throw Object.assign(new Error('玩家只能修改可分配屬性、自我介紹、第一次角色姓名與裝備分配；職業、血脈、大道／信仰、技能、貨幣與其他資料由系統或 DM 管理'), {status:403});
  }

  const fields = ['agility','strength','constitution','spirit'];
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
    if (next[field] > cap) throw Object.assign(new Error(`目前 ${tier} 階單項屬性上限為 ${cap}`), {status:400});
  }

  const newTotal = fields.reduce((sum, field) => sum + next[field], 0);
  const newPoints = body.attribute_points === undefined ? oldPoints : Math.max(0, Math.floor(Number(body.attribute_points) || 0));
  if (newTotal + newPoints !== oldTotal + oldPoints) {
    throw Object.assign(new Error('屬性點數不正確，力量、敏捷、體質、精神與剩餘屬性點的總和必須保持不變'), {status:400});
  }

  const updates=[];
  const values=[];
  for (const field of fields) {
    if (body[field] !== undefined) { updates.push(`${field} = ?`); values.push(next[field]); }
  }
  if (body.attribute_points !== undefined || fields.some(field => body[field] !== undefined)) {
    updates.push('attribute_points = ?'); values.push(newPoints);
  }

  if (body.name !== undefined) {
    const name=String(body.name||'').trim().slice(0,50);
    if(!name) throw Object.assign(new Error('角色名稱不能為空'),{status:400});
    const changed=name!==String(current.name||'');
    if(changed && current.name_locked) throw Object.assign(new Error('角色名稱第一次設定後就不能再由玩家修改'),{status:403});
    if(changed){ updates.push('name = ?'); values.push(name); updates.push('name_locked = TRUE'); }
  }

  if (body.self_intro !== undefined) {
    updates.push('self_intro = ?'); values.push(String(body.self_intro||'').trim().slice(0,4000));
  }

  let permanentGearSkillIds = [];
  if (body.gear_loadout !== undefined) {
    const raw=body.gear_loadout&&typeof body.gear_loadout==='object'&&!Array.isArray(body.gear_loadout)?body.gear_loadout:{};
    const wantedEquipment=(Array.isArray(raw.equipment)?raw.equipment:[]).map(v=>String(v||'').trim()).filter(Boolean).slice(0,4);
    const wantedWeapons=(Array.isArray(raw.weapons)?raw.weapons:[]).map(v=>String(v||'').trim()).filter(Boolean).slice(0,2);
    const ownedEquipment=normalizeGearList(current.equipment);
    const ownedWeapons=normalizeGearList(current.weapons);
    const validate=(wanted,owned,label)=>{
      const pool=[...owned];
      const selected=[];
      for(const name of wanted){
        const idx=pool.findIndex(item=>String(item.name)===name);
        if(idx<0)throw Object.assign(new Error(`你沒有可裝備的${label}「${name}」`),{status:400});
        const gear=pool[idx];
        const requirement=gearRequirementResult(current,gear);
        if(!requirement.ok)throw Object.assign(new Error(`無法裝備「${name}」：${requirement.reason}`),{status:400});
        selected.push(gear);
        pool.splice(idx,1);
      }
      return selected;
    };
    const selectedEquipment=validate(wantedEquipment,ownedEquipment,'裝備');
    const selectedWeapons=validate(wantedWeapons,ownedWeapons,'武器');
    permanentGearSkillIds=[...new Set([...selectedEquipment,...selectedWeapons]
      .filter(gear=>gear.grant_mode==='permanent_first_equip')
      .flatMap(gear=>gear.granted_skill_template_ids||[])
      .map(Number).filter(Boolean))];
    updates.push('gear_loadout = ?::jsonb'); values.push(JSON.stringify({configured:true,equipment:wantedEquipment,weapons:wantedWeapons}));
  }

  if(!updates.length) throw Object.assign(new Error('沒有可更新欄位'),{status:400});
  updates.push('updated_at = CURRENT_TIMESTAMP'); values.push(userId);
  await run(`UPDATE character_cards SET ${updates.join(', ')} WHERE user_id = ?`, values);

  if(permanentGearSkillIds.length){
    const client=await pool.connect();
    try{
      await client.query('BEGIN');
      const result=await client.query('SELECT * FROM character_cards WHERE user_id=$1 FOR UPDATE',[userId]);
      const character=parseCharacter(result.rows[0]);
      await grantSkillTemplateIdsToCharacterWithClient(client,character,permanentGearSkillIds,{type:'equipment_permanent'});
      await persistCharacterProgressWithClient(client,character,userId);
      await client.query('COMMIT');
    }catch(error){await client.query('ROLLBACK').catch(()=>{});throw error;}finally{client.release();}
  }

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

  await run(`ALTER TABLE tasks ADD COLUMN IF NOT EXISTS profession_id BIGINT REFERENCES profession_templates(id) ON DELETE SET NULL`);
  await run(`ALTER TABLE tasks ADD COLUMN IF NOT EXISTS profession_rank VARCHAR(10)`);
  await run(`CREATE INDEX IF NOT EXISTS idx_tasks_status_type ON tasks(status, task_type)`);
  await run(`CREATE INDEX IF NOT EXISTS idx_tasks_target_user ON tasks(target_user_id)`);
  await run(`CREATE INDEX IF NOT EXISTS idx_tasks_profession ON tasks(profession_id)`);
  await run(`CREATE INDEX IF NOT EXISTS idx_task_participants_user ON task_participants(user_id)`);
}


/* =========================================================
   TASK SYSTEM
========================================================= */

function normalizeTaskRewards(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return {};
  const rewards = {};

  for (const field of ['experience','attribute_points','skill_points','weird_coins','game_coins']) {
    if (value[field] !== undefined) rewards[field] = Math.max(0, Math.floor(Number(value[field]) || 0));
  }

  rewards.inventory = normalizeInventory(value.inventory || value.items || []);
  rewards.equipment = Array.isArray(value.equipment)
    ? value.equipment.map(x=>String(x||'').trim().slice(0,120)).filter(Boolean).slice(0,100)
    : [];
  rewards.weapons = Array.isArray(value.weapons)
    ? value.weapons.map(x=>String(x||'').trim().slice(0,120)).filter(Boolean).slice(0,100)
    : [];
  rewards.skill_template_ids = [...new Set((Array.isArray(value.skill_template_ids) ? value.skill_template_ids : []).map(Number).filter(Boolean))].slice(0,100);
  rewards.profession_level_ups = [...new Set((Array.isArray(value.profession_level_ups) ? value.profession_level_ups : []).map(Number).filter(Boolean))].slice(0,20);
  rewards.profession_grants = (Array.isArray(value.profession_grants) ? value.profession_grants : [])
    .map(raw=>{
      const profession_id = Math.max(0, Number(raw && raw.profession_id)||0);
      if(!profession_id) return null;
      return {profession_id, role: raw && raw.role === 'main' ? 'main' : 'sub'};
    })
    .filter(Boolean)
    .slice(0,10);
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
  const client = await pool.connect();
  try{
    await client.query('BEGIN');
    const result = await client.query('SELECT * FROM character_cards WHERE user_id=$1 FOR UPDATE',[userId]);
    const character = parseCharacter(result.rows[0]);
    if(!character) throw Object.assign(new Error('找不到角色卡'),{status:404});
    const normalized = normalizeTaskRewards(rewards);

    for(const field of ['experience','attribute_points','skill_points','weird_coins','game_coins']) {
      character[field] = Math.max(0, Number(character[field])||0) + Math.max(0, Number(normalized[field])||0);
    }
    let inventory = normalizeInventory(character.inventory);
    for(const item of normalized.inventory||[]) {
      ensureInventoryCanAdd(character, inventory, item.name);
      inventory = changeInventoryQuantity(inventory,item.name,item.quantity);
    }
    character.inventory = inventory;
    character.equipment = [...(Array.isArray(character.equipment)?character.equipment:[]), ...(normalized.equipment||[])];
    character.weapons = [...(Array.isArray(character.weapons)?character.weapons:[]), ...(normalized.weapons||[])];
    await grantSkillTemplateIdsToCharacterWithClient(client, character, normalized.skill_template_ids, {type:'task_reward'});
    for(const grant of normalized.profession_grants||[]) {
      const profession=await resolveProfessionTemplateWithClient(client,grant.profession_id);
      if(!profession) throw Object.assign(new Error('找不到任務獎勵指定的職業'),{status:404});
      const cfg=profession.config&&typeof profession.config==='object'?profession.config:{};
      const bookName=String(cfg.book_name||`${profession.name}職業書`).trim();
      const bookQty=Math.max(1,Math.floor(Number(cfg.book_quantity)||1));
      ensureInventoryCanAdd(character,inventory,bookName,'職業書');
      inventory=changeInventoryQuantity(inventory,bookName,bookQty,'職業書');
      character.inventory=inventory;
    }
    for(const professionId of normalized.profession_level_ups||[]) await levelUpProfessionOnCharacterWithClient(client, character, professionId);

    await client.query(
      `UPDATE character_cards SET experience=$1,attribute_points=$2,skill_points=$3,weird_coins=$4,game_coins=$5,inventory=$6::jsonb,equipment=$7::jsonb,weapons=$8::jsonb,main_class=$9,sub_classes=$10::jsonb,profession_progress=$11::jsonb,profession_rank=$12,skills=$13::jsonb,game_coin_unlocked=$14,inventory_slots=$15,backpack_bonus_applied=$16,updated_at=CURRENT_TIMESTAMP WHERE user_id=$17`,
      [character.experience,character.attribute_points,character.skill_points,character.weird_coins,character.game_coins,JSON.stringify(character.inventory),JSON.stringify(character.equipment),JSON.stringify(character.weapons),character.main_class||'未設定',JSON.stringify(character.sub_classes||[]),JSON.stringify(normalizeProfessionProgress(character.profession_progress)),character.profession_rank||'G',JSON.stringify(character.skills||[]),Boolean(character.game_coin_unlocked),Math.max(5,Math.floor(Number(character.inventory_slots)||5)),Boolean(character.backpack_bonus_applied),userId]
    );
    await client.query('COMMIT');
  }catch(error){
    await client.query('ROLLBACK').catch(()=>{});
    throw error;
  }finally{client.release();}
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

      const taskType = ['private','profession'].includes(req.body.task_type)
        ? req.body.task_type
        : 'public';

      const targetUserId = taskType === 'public'
        ? null
        : Number(req.body.target_user_id || 0);

      if (taskType !== 'public') {
        if (!targetUserId) {
          return res.status(400).json({
            error: taskType === 'profession' ? '職業升階任務必須指定玩家' : '私人任務必須指定玩家'
          });
        }

        const target = await getUser(targetUserId);
        if (!target || target.is_admin) {
          return res.status(400).json({
            error: taskType === 'profession' ? '職業升階任務只能指定玩家帳號' : '私人任務只能指定玩家帳號'
          });
        }
      }

      let professionId = null;
      let professionRank = null;
      if (taskType === 'profession') {
        professionId = Math.max(0, Number(req.body.profession_id) || 0);
        if (!professionId) return res.status(400).json({error:'請選擇要升階的職業'});
        const targetCharacter = await getCharacterByUser(targetUserId);
        if (!targetCharacter) return res.status(400).json({error:'指定玩家沒有角色卡'});
        const profession = await get(`SELECT * FROM profession_templates WHERE id = ?`, [professionId]);
        if (!profession) return res.status(404).json({error:'找不到職業'});
        const progress = normalizeProfessionProgress(targetCharacter.profession_progress);
        const entry = progress[String(professionId)];
        if (!entry || !entry.active) return res.status(400).json({error:'指定玩家目前沒有裝備這個職業'});
        professionRank = entry.rank;
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
            rewards,
            profession_id,
            profession_rank
          )
          VALUES (?, ?, ?, 'open', ?, ?, ?, ?, ?::jsonb, ?, ?)
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
          JSON.stringify(rewards),
          professionId,
          professionRank
        ]
      );

      if (taskType !== 'public') {
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

      const rewards = normalizeTaskRewards(parseJsonField(task.rewards, {}));

      const characterResult = await client.query(
        'SELECT * FROM character_cards WHERE user_id = $1 FOR UPDATE',
        [targetUserId]
      );
      const character = parseCharacter(characterResult.rows[0]);
      if (!character) {
        await client.query('ROLLBACK');
        return res.status(404).json({error:'找不到玩家角色卡'});
      }

      const professionEvents = [];
      const grantedSkills = [];

      if (task.task_type === 'profession') {
        const promoted = await promoteProfessionOnCharacterWithClient(
          client,
          character,
          task.profession_id,
          task.profession_rank
        );
        professionEvents.push({
          type:'promotion',
          profession_id: promoted.profession.id,
          profession_name: promoted.profession.name,
          from_rank: promoted.from_rank,
          to_rank: promoted.to_rank
        });
        grantedSkills.push(...promoted.granted);
      }

      for (const field of ['experience','attribute_points','skill_points','weird_coins','game_coins']) {
        character[field] = Math.max(0, Number(character[field])||0) + Math.max(0, Number(rewards[field])||0);
      }

      let inventory = normalizeInventory(character.inventory);
      for (const item of rewards.inventory || []) {
        ensureInventoryCanAdd(character, inventory, item.name, item.category || '');
        inventory = changeInventoryQuantity(inventory, item.name, item.quantity, item.category || '');
      }
      character.inventory = inventory;
      character.equipment = [...(Array.isArray(character.equipment)?character.equipment:[]), ...(rewards.equipment||[])];
      character.weapons = [...(Array.isArray(character.weapons)?character.weapons:[]), ...(rewards.weapons||[])];

      grantedSkills.push(...await grantSkillTemplateIdsToCharacterWithClient(
        client,
        character,
        rewards.skill_template_ids,
        {type:'task_reward', task_id:task.id, task_title:task.title}
      ));

      for (const grant of rewards.profession_grants || []) {
        const profession=await resolveProfessionTemplateWithClient(client,grant.profession_id);
        if(!profession) throw Object.assign(new Error('找不到任務獎勵指定的職業'),{status:404});
        const cfg=profession.config&&typeof profession.config==='object'?profession.config:{};
        const bookName=String(cfg.book_name||`${profession.name}職業書`).trim();
        const bookQty=Math.max(1,Math.floor(Number(cfg.book_quantity)||1));
        ensureInventoryCanAdd(character,inventory,bookName,'職業書');
        inventory=changeInventoryQuantity(inventory,bookName,bookQty,'職業書');
        character.inventory=inventory;
        professionEvents.push({type:'profession_book',profession_id:profession.id,profession_name:profession.name,book_name:bookName,book_quantity:bookQty});
      }

      for (const professionId of rewards.profession_level_ups || []) {
        const result = await levelUpProfessionOnCharacterWithClient(client, character, professionId);
        professionEvents.push({type:'level_up',profession_id:result.profession.id,profession_name:result.profession.name,rank:result.entry.rank,level:result.entry.level});
        grantedSkills.push(...result.granted);
      }

      let mainProfessionRank = character.profession_rank || 'G';
      for (const entry of Object.values(normalizeProfessionProgress(character.profession_progress))) {
        if (entry.active && entry.role === 'main') { mainProfessionRank = entry.rank; break; }
      }

      await client.query(
        `UPDATE character_cards
         SET experience=$1,
             attribute_points=$2,
             skill_points=$3,
             weird_coins=$4,
             game_coins=$5,
             inventory=$6::jsonb,
             equipment=$7::jsonb,
             weapons=$8::jsonb,
             main_class=$9,
             sub_classes=$10::jsonb,
             profession_progress=$11::jsonb,
             profession_rank=$12,
             skills=$13::jsonb,
             game_coin_unlocked=$14,
             inventory_slots=$15,
             backpack_bonus_applied=$16,
             updated_at=CURRENT_TIMESTAMP
         WHERE user_id=$17`,
        [
          character.experience,
          character.attribute_points,
          character.skill_points,
          character.weird_coins,
          character.game_coins,
          JSON.stringify(character.inventory),
          JSON.stringify(character.equipment),
          JSON.stringify(character.weapons),
          character.main_class || '未設定',
          JSON.stringify(Array.isArray(character.sub_classes)?character.sub_classes:[]),
          JSON.stringify(normalizeProfessionProgress(character.profession_progress)),
          mainProfessionRank,
          JSON.stringify(Array.isArray(character.skills)?character.skills:[]),
          Boolean(character.game_coin_unlocked),
          Math.max(5,Math.floor(Number(character.inventory_slots)||5)),
          Boolean(character.backpack_bonus_applied),
          targetUserId
        ]
      );

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
        rewards,
        profession_events: professionEvents,
        granted_skills: grantedSkills
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
   V14 RULE-HORROR SELF JUDGEMENT
========================================================= */

function parseClockMinute(value, fallback) {
  const text=String(value??'').trim();
  const match=text.match(/^(\d{1,2}):(\d{2})$/);
  if(match){
    const hour=Math.max(0,Math.min(23,Number(match[1])||0));
    const minute=Math.max(0,Math.min(59,Number(match[2])||0));
    return hour*60+minute;
  }
  const n=Number(value);
  return Number.isFinite(n)?Math.max(0,Math.min(1439,Math.floor(n))):fallback;
}

function ruleTimeMatches(nowMinute,startMinute,endMinute){
  const start=Math.max(0,Math.min(1439,Number(startMinute)||0));
  const end=Math.max(0,Math.min(1439,Number(endMinute)||0));
  if(start<=end)return nowMinute>=start&&nowMinute<=end;
  return nowMinute>=start||nowMinute<=end;
}

function ruleActionMatches(action,keywords,mode){
  const text=String(action||'').toLocaleLowerCase();
  const list=(Array.isArray(keywords)?keywords:[]).map(x=>String(x||'').trim().toLocaleLowerCase()).filter(Boolean);
  if(!list.length)return false;
  return mode==='all'?list.every(keyword=>text.includes(keyword)):list.some(keyword=>text.includes(keyword));
}

function parsedHorrorRule(row){
  return row?{...row,keywords:parseJsonField(row.keywords,[])}:null;
}

app.get('/api/admin/horror-rules',auth,adminAuth,async(req,res)=>{try{
  const rows=await all(`SELECT hr.*,r.name AS room_name,r.code AS room_code FROM horror_rules hr JOIN rooms r ON r.id=hr.room_id ORDER BY hr.room_id,hr.id`);
  res.json({rules:rows.map(parsedHorrorRule)});
}catch(error){console.error(error);res.status(500).json({error:'讀取規則怪談失敗'});}});

app.post('/api/admin/horror-rules',auth,adminAuth,async(req,res)=>{try{
  const roomId=Number(req.body.room_id);await requireRoomAdmin(req.user.id,roomId);
  const name=String(req.body.name||'').trim().slice(0,160);if(!name)return res.status(400).json({error:'規則名稱不能為空'});
  const keywords=(Array.isArray(req.body.keywords)?req.body.keywords:String(req.body.keywords||'').split(/\r?\n/)).map(x=>String(x).trim()).filter(Boolean).slice(0,60);
  if(!keywords.length)return res.status(400).json({error:'至少設定一個行動關鍵字／片語'});
  const truthState=['true','false','partial','corrupted','expired'].includes(String(req.body.truth_state||''))?String(req.body.truth_state):'true';
  const activeFrom=req.body.active_from_total===null||req.body.active_from_total===''||req.body.active_from_total===undefined?null:Math.max(0,Math.floor(Number(req.body.active_from_total)||0));
  const activeUntil=req.body.active_until_total===null||req.body.active_until_total===''||req.body.active_until_total===undefined?null:Math.max(0,Math.floor(Number(req.body.active_until_total)||0));
  const row=await get(`INSERT INTO horror_rules(room_id,name,rule_text,public_hint,location_contains,start_minute,end_minute,keywords,match_mode,consequence,active,truth_state,active_from_total,active_until_total,change_note,required_profession,required_bloodline,required_item,min_pollution,max_pollution) VALUES(?,?,?,?,?,?,?,?::jsonb,?,?,?,?,?,?,?,?,?,?,?,?) RETURNING *`,[
    roomId,name,String(req.body.rule_text||''),String(req.body.public_hint||''),String(req.body.location_contains||'').trim().slice(0,160),parseClockMinute(req.body.start_time,0),parseClockMinute(req.body.end_time,1439),JSON.stringify(keywords),req.body.match_mode==='all'?'all':'any',String(req.body.consequence||''),req.body.active===undefined?true:Boolean(req.body.active),truthState,activeFrom,activeUntil,String(req.body.change_note||'').slice(0,4000),String(req.body.required_profession||'').trim().slice(0,100),String(req.body.required_bloodline||'').trim().slice(0,120),String(req.body.required_item||'').trim().slice(0,120),clampInt(req.body.min_pollution,0,100,0),clampInt(req.body.max_pollution,0,100,100)
  ]);res.json({rule:parsedHorrorRule(row)});
}catch(error){console.error(error);res.status(error.status||500).json({error:error.message||'建立規則失敗'});}});

app.patch('/api/admin/horror-rules/:id',auth,adminAuth,async(req,res)=>{try{
  const cur=await get(`SELECT * FROM horror_rules WHERE id=?`,[req.params.id]);if(!cur)return res.status(404).json({error:'找不到規則'});await requireRoomAdmin(req.user.id,cur.room_id);
  const keywords=req.body.keywords===undefined?parseJsonField(cur.keywords,[]):(Array.isArray(req.body.keywords)?req.body.keywords:String(req.body.keywords||'').split(/\r?\n/)).map(x=>String(x).trim()).filter(Boolean).slice(0,60);
  const truthState=req.body.truth_state===undefined?String(cur.truth_state||'true'):(['true','false','partial','corrupted','expired'].includes(String(req.body.truth_state||''))?String(req.body.truth_state):'true');
  const activeFrom=req.body.active_from_total===undefined?cur.active_from_total:(req.body.active_from_total===null||req.body.active_from_total===''?null:Math.max(0,Math.floor(Number(req.body.active_from_total)||0)));
  const activeUntil=req.body.active_until_total===undefined?cur.active_until_total:(req.body.active_until_total===null||req.body.active_until_total===''?null:Math.max(0,Math.floor(Number(req.body.active_until_total)||0)));
  const row=await get(`UPDATE horror_rules SET name=?,rule_text=?,public_hint=?,location_contains=?,start_minute=?,end_minute=?,keywords=?::jsonb,match_mode=?,consequence=?,active=?,truth_state=?,active_from_total=?,active_until_total=?,change_note=?,required_profession=?,required_bloodline=?,required_item=?,min_pollution=?,max_pollution=?,updated_at=CURRENT_TIMESTAMP WHERE id=? RETURNING *`,[
    String(req.body.name??cur.name).trim().slice(0,160),String(req.body.rule_text??cur.rule_text),String(req.body.public_hint??cur.public_hint),String(req.body.location_contains??cur.location_contains).trim().slice(0,160),req.body.start_time===undefined?cur.start_minute:parseClockMinute(req.body.start_time,cur.start_minute),req.body.end_time===undefined?cur.end_minute:parseClockMinute(req.body.end_time,cur.end_minute),JSON.stringify(keywords),req.body.match_mode===undefined?cur.match_mode:(req.body.match_mode==='all'?'all':'any'),String(req.body.consequence??cur.consequence),req.body.active===undefined?Boolean(cur.active):Boolean(req.body.active),truthState,activeFrom,activeUntil,String(req.body.change_note??cur.change_note??'').slice(0,4000),String(req.body.required_profession??cur.required_profession??'').trim().slice(0,100),String(req.body.required_bloodline??cur.required_bloodline??'').trim().slice(0,120),String(req.body.required_item??cur.required_item??'').trim().slice(0,120),clampInt(req.body.min_pollution??cur.min_pollution,0,100,0),clampInt(req.body.max_pollution??cur.max_pollution,0,100,100),req.params.id
  ]);res.json({rule:parsedHorrorRule(row)});
}catch(error){console.error(error);res.status(error.status||500).json({error:error.message||'修改規則失敗'});}});

app.delete('/api/admin/horror-rules/:id',auth,adminAuth,async(req,res)=>{try{
  const cur=await get(`SELECT * FROM horror_rules WHERE id=?`,[req.params.id]);if(!cur)return res.status(404).json({error:'找不到規則'});await requireRoomAdmin(req.user.id,cur.room_id);await run(`DELETE FROM horror_rules WHERE id=?`,[cur.id]);res.json({ok:true});
}catch(error){res.status(error.status||500).json({error:error.message||'刪除規則失敗'});}});

app.post('/api/rooms/:id/actions',auth,async(req,res)=>{try{
  const user=await getUser(req.user.id);const isAdmin=Boolean(user&&user.is_admin);if(!isAdmin)await requireRoomMember(req.user.id,req.params.id);
  const room=await get(`SELECT * FROM rooms WHERE id=?`,[req.params.id]);if(!room)return res.status(404).json({error:'找不到房間'});
  const action=String(req.body.action||'').trim().slice(0,2000);if(!action)return res.status(400).json({error:'請輸入角色要做的行動'});
  const character=await getCharacterByUser(req.user.id);const actorName=character?.name||user?.username||'玩家';
  let actorLocation=String(room.map_name||''),actorNodeId=room.current_map_node_id||null,actorTeamId=null;
  if(!isAdmin){
    const team=await getUserRoomTeam(req.user.id,room.id);actorTeamId=team?.id||null;
    if(team&&team.current_map_node_id){actorNodeId=team.current_map_node_id;const node=await get(`SELECT name FROM room_map_nodes WHERE id=? AND room_id=?`,[team.current_map_node_id,room.id]);if(node?.name)actorLocation=String(node.name);}
  }
  const noise=await applyActionNoise(room.id,actorNodeId,actorTeamId,req.user.id,action,req.body.noise);
  await addEvent(room.id,req.user.id,'action',{text:`🗣️ ${actorName} 宣告行動：${action}${noise.amount>0?`（噪音：${noise.label}）`:''}`,action,location:actorLocation,map_node_id:actorNodeId,noise:noise.amount});
  const nowMinute=(Math.max(0,Number(room.game_hour)||0)*60)+Math.max(0,Number(room.game_minute)||0);
  const nowTotal=(Math.max(1,Number(room.game_day)||1)-1)*1440+nowMinute;
  const rules=await all(`SELECT * FROM horror_rules WHERE room_id=? AND active=TRUE ORDER BY id`,[room.id]);
  const triggered=[];
  for(const raw of rules){
    const rule=parsedHorrorRule(raw);
    const truthState=String(rule.truth_state||'true');
    if(['false','expired'].includes(truthState))continue;
    if(rule.active_from_total!==null&&rule.active_from_total!==undefined&&nowTotal<Number(rule.active_from_total))continue;
    if(rule.active_until_total!==null&&rule.active_until_total!==undefined&&nowTotal>Number(rule.active_until_total))continue;
    if(!isAdmin&&character){
      const reqProfession=String(rule.required_profession||'').trim().toLocaleLowerCase();
      if(reqProfession){const progressNames=Object.values(normalizeProfessionProgress(character.profession_progress)).filter(x=>x&&x.active!==false).map(x=>String(x.profession_name||'').trim().toLocaleLowerCase());const professions=[String(character.main_class||''),...(Array.isArray(character.sub_classes)?character.sub_classes:[]),...progressNames].map(x=>String(x||'').trim().toLocaleLowerCase()).filter(Boolean);if(!professions.includes(reqProfession))continue;}
      const reqBloodline=String(rule.required_bloodline||'').trim().toLocaleLowerCase();if(reqBloodline&&String(character.bloodline||'').trim().toLocaleLowerCase()!==reqBloodline)continue;
      const reqItem=String(rule.required_item||'').trim();if(reqItem&&inventoryQuantity(character.inventory,reqItem)<=0)continue;
      const pollution=clampInt(character.pollution,0,100,0);if(pollution<clampInt(rule.min_pollution,0,100,0)||pollution>clampInt(rule.max_pollution,0,100,100))continue;
    }
    const location=String(rule.location_contains||'').trim().toLocaleLowerCase();
    if(location&&!actorLocation.toLocaleLowerCase().includes(location))continue;
    if(!ruleTimeMatches(nowMinute,rule.start_minute,rule.end_minute))continue;
    if(!ruleActionMatches(action,rule.keywords,rule.match_mode))continue;
    const consequence=String(rule.consequence||'某條隱藏規則產生了反應。').trim();
    triggered.push({id:rule.id,consequence,public_hint:rule.public_hint||''});
    await run(`UPDATE horror_rules SET trigger_count=trigger_count+1,last_triggered_at=CURRENT_TIMESTAMP,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[rule.id]);
    if(!isAdmin){await run(`INSERT INTO rule_discoveries(rule_id,user_id,knowledge_level) VALUES(?,?,1) ON CONFLICT(rule_id,user_id) DO UPDATE SET knowledge_level=GREATEST(rule_discoveries.knowledge_level,1),updated_at=CURRENT_TIMESTAMP`,[rule.id,req.user.id]);}
    await addEvent(room.id,null,'rule_effect',{text:`⚠️ ${consequence}`,rule_id:rule.id,actor_user_id:req.user.id});
  }
  if(!isAdmin)await maybeCreatePollutionVision(req.user.id,room.id,actorLocation);
  const snapshot=await roomSnapshot(room.id);io.to('room:'+room.id).emit('room:snapshot',snapshot);emitTeamUpdate(room.id,actorTeamId);
  res.json({ok:true,triggered,noise,snapshot});
}catch(error){console.error('RULE ACTION ERROR:',error);res.status(error.status||500).json({error:error.message||'行動判應失敗'});}});


/* =========================================================
   V15: 調查手冊 / 線索 / 狀態 / 地圖節點 / 隱藏判定 / 道具模板
========================================================= */

function parsedStatus(row){return row?{...row,effects:parseJsonField(row.effects,{}),tags:parseJsonField(row.tags,[]),stacks:Math.max(1,Number(row.stacks)||1),max_stacks:Math.max(1,Number(row.max_stacks)||1),dispellable:row.dispellable===undefined?true:Boolean(row.dispellable)}:null;}
function parsedItemTemplate(row){return row?{...row,effects:parseJsonField(row.effects,{})}:null;}

app.get('/api/journal',auth,async(req,res)=>{try{
  const rules=await all(`SELECT hr.id,hr.name,hr.public_hint,hr.rule_text,rd.knowledge_level,rd.note,rd.discovered_at FROM rule_discoveries rd JOIN horror_rules hr ON hr.id=rd.rule_id WHERE rd.user_id=? ORDER BY rd.updated_at DESC`,[req.user.id]);
  const clues=await all(`SELECT ct.*,cc.note AS player_note,cc.acquired_at FROM character_clues cc JOIN clue_templates ct ON ct.id=cc.clue_id WHERE cc.user_id=? ORDER BY cc.acquired_at DESC`,[req.user.id]);
  const statuses=await all(`SELECT * FROM character_statuses WHERE user_id=? AND active=TRUE ORDER BY id DESC`,[req.user.id]);
  res.json({rules:rules.map(r=>({...r,rule_text:Number(r.knowledge_level)>=3?r.rule_text:''})),clues,statuses:statuses.map(parsedStatus)});
}catch(error){console.error(error);res.status(500).json({error:'讀取調查手冊失敗'});}});

app.get('/api/admin/clues',auth,adminAuth,async(req,res)=>{try{const rows=await all(`SELECT ct.*,hr.name AS linked_rule_name,r.name AS room_name,mn.name AS map_node_name FROM clue_templates ct LEFT JOIN horror_rules hr ON hr.id=ct.linked_rule_id LEFT JOIN rooms r ON r.id=ct.room_id LEFT JOIN room_map_nodes mn ON mn.id=ct.map_node_id ORDER BY ct.id DESC`);res.json({clues:rows});}catch(error){res.status(500).json({error:'讀取線索失敗'});}});
app.post('/api/admin/clues',auth,adminAuth,async(req,res)=>{try{const name=String(req.body.name||'').trim().slice(0,160);if(!name)return res.status(400).json({error:'線索名稱不能為空'});const roomId=Number(req.body.room_id)||null;if(roomId)await requireRoomAdmin(req.user.id,roomId);const nodeId=roomId?await validRoomMapNodeId(roomId,req.body.map_node_id):null;const row=await get(`INSERT INTO clue_templates(name,category,description,image_url,linked_rule_id,reveal_level,room_id,map_node_id,auto_discover,investigation_dc) VALUES(?,?,?,?,?,?,?,?,?,?) RETURNING *`,[name,String(req.body.category||'一般線索').slice(0,80),String(req.body.description||''),String(req.body.image_url||''),Number(req.body.linked_rule_id)||null,Math.max(1,Math.min(3,Number(req.body.reveal_level)||1)),roomId,nodeId,Boolean(req.body.auto_discover),clampInt(req.body.investigation_dc,1,100,50)]);res.json({clue:row});}catch(error){console.error(error);res.status(error.status||500).json({error:error.message||'建立線索失敗'});}});
app.delete('/api/admin/clues/:id',auth,adminAuth,async(req,res)=>{try{await run(`DELETE FROM clue_templates WHERE id=?`,[req.params.id]);res.json({ok:true});}catch(error){res.status(500).json({error:'刪除線索失敗'});}});
app.post('/api/admin/players/:id/clues/:clueId',auth,adminAuth,async(req,res)=>{try{
  const userId=Number(req.params.id);const clue=await get(`SELECT * FROM clue_templates WHERE id=?`,[req.params.clueId]);if(!clue)return res.status(404).json({error:'找不到線索'});
  await run(`INSERT INTO character_clues(clue_id,user_id,note) VALUES(?,?,?) ON CONFLICT(clue_id,user_id) DO UPDATE SET note=EXCLUDED.note`,[clue.id,userId,String(req.body.note||'')]);
  if(clue.linked_rule_id){await run(`INSERT INTO rule_discoveries(rule_id,user_id,knowledge_level) VALUES(?,?,?) ON CONFLICT(rule_id,user_id) DO UPDATE SET knowledge_level=GREATEST(rule_discoveries.knowledge_level,EXCLUDED.knowledge_level),updated_at=CURRENT_TIMESTAMP`,[clue.linked_rule_id,userId,Math.max(1,Math.min(3,Number(clue.reveal_level)||1))]);}
  res.json({ok:true});
}catch(error){console.error(error);res.status(500).json({error:'發放線索失敗'});}});
app.post('/api/admin/players/:id/rules/:ruleId/reveal',auth,adminAuth,async(req,res)=>{try{const level=Math.max(1,Math.min(3,Number(req.body.level)||1));await run(`INSERT INTO rule_discoveries(rule_id,user_id,knowledge_level,note) VALUES(?,?,?,?) ON CONFLICT(rule_id,user_id) DO UPDATE SET knowledge_level=EXCLUDED.knowledge_level,note=EXCLUDED.note,updated_at=CURRENT_TIMESTAMP`,[req.params.ruleId,req.params.id,level,String(req.body.note||'')]);res.json({ok:true});}catch(error){res.status(500).json({error:'揭露規則失敗'});}});

app.get('/api/admin/status-templates',auth,adminAuth,async(req,res)=>{try{
  const rows=await all(`SELECT * FROM status_templates ORDER BY id DESC`);
  res.json({statuses:rows.map(parsedStatus)});
}catch(error){res.status(500).json({error:'讀取狀態模板失敗'});}});

app.post('/api/admin/status-templates',auth,adminAuth,async(req,res)=>{try{
  const name=String(req.body.name||'').trim().slice(0,120);
  if(!name)return res.status(400).json({error:'狀態名稱不能為空'});
  const stackMode=['refresh','stack','independent','replace','ignore'].includes(req.body.stack_mode)?req.body.stack_mode:'refresh';
  const maxStacks=Math.max(1,Math.min(99,Math.floor(Number(req.body.max_stacks)||1)));
  const rawEffects=req.body.effects&&typeof req.body.effects==='object'?req.body.effects:{};
  const effects=effectContainerWithList(rawEffects,normalizeUnifiedEffects(rawEffects));
  const row=await get(`INSERT INTO status_templates(name,category,description,duration_rounds,effects,stack_mode,max_stacks,dispellable,tags) VALUES(?,?,?,?,?::jsonb,?,?,?,?::jsonb) RETURNING *`,[
    name,String(req.body.category||'狀態').slice(0,60),String(req.body.description||''),Math.max(0,Math.floor(Number(req.body.duration_rounds)||0)),JSON.stringify(effects),stackMode,maxStacks,req.body.dispellable===undefined?true:Boolean(req.body.dispellable),JSON.stringify(Array.isArray(req.body.tags)?req.body.tags.map(x=>String(x).trim()).filter(Boolean).slice(0,20):[])
  ]);
  res.json({status:parsedStatus(row)});
}catch(error){console.error(error);res.status(500).json({error:'建立狀態模板失敗'});}});

app.patch('/api/admin/status-templates/:id',auth,adminAuth,async(req,res)=>{try{
  const current=await get(`SELECT * FROM status_templates WHERE id=?`,[req.params.id]);
  if(!current)return res.status(404).json({error:'找不到狀態模板'});
  const rawEffects=req.body.effects===undefined?parseJsonField(current.effects,{}):(req.body.effects&&typeof req.body.effects==='object'?req.body.effects:{});
  const effects=effectContainerWithList(rawEffects,normalizeUnifiedEffects(rawEffects));
  const stackMode=['refresh','stack','independent','replace','ignore'].includes(req.body.stack_mode)?req.body.stack_mode:String(current.stack_mode||'refresh');
  const row=await get(`UPDATE status_templates SET name=?,category=?,description=?,duration_rounds=?,effects=?::jsonb,stack_mode=?,max_stacks=?,dispellable=?,tags=?::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=? RETURNING *`,[
    String(req.body.name===undefined?current.name:req.body.name).trim().slice(0,120),String(req.body.category===undefined?current.category:req.body.category).slice(0,60),String(req.body.description===undefined?current.description:req.body.description),Math.max(0,Math.floor(Number(req.body.duration_rounds===undefined?current.duration_rounds:req.body.duration_rounds)||0)),JSON.stringify(effects),stackMode,Math.max(1,Math.min(99,Math.floor(Number(req.body.max_stacks===undefined?current.max_stacks:req.body.max_stacks)||1))),req.body.dispellable===undefined?Boolean(current.dispellable):Boolean(req.body.dispellable),JSON.stringify(req.body.tags===undefined?parseJsonField(current.tags,[]):(Array.isArray(req.body.tags)?req.body.tags.map(x=>String(x).trim()).filter(Boolean).slice(0,20):[])),req.params.id
  ]);
  res.json({status:parsedStatus(row)});
}catch(error){console.error(error);res.status(500).json({error:'修改狀態模板失敗'});}});

app.delete('/api/admin/status-templates/:id',auth,adminAuth,async(req,res)=>{try{await run(`DELETE FROM status_templates WHERE id=?`,[req.params.id]);res.json({ok:true});}catch(error){res.status(500).json({error:'刪除狀態模板失敗'});}});

app.post('/api/admin/players/:id/statuses',auth,adminAuth,async(req,res)=>{const client=await pool.connect();try{
  await client.query('BEGIN');
  const templateResult=await client.query(`SELECT * FROM status_templates WHERE id=$1`,[req.body.template_id]);
  const template=templateResult.rows[0];
  if(!template){await client.query('ROLLBACK');return res.status(404).json({error:'找不到狀態模板'});}
  const row=await applyStatusTemplateWithClient(client,Number(req.params.id),template,String(req.body.source||'DM'),req.body.remaining_rounds===undefined?null:Math.max(0,Number(req.body.remaining_rounds)||0));
  await client.query('COMMIT');
  res.json({status:parsedStatus(row),character:await getCharacterByUser(req.params.id)});
}catch(error){await client.query('ROLLBACK').catch(()=>{});console.error(error);res.status(error.status||500).json({error:error.message||'套用狀態失敗'});}finally{client.release();}});

app.delete('/api/admin/players/:id/statuses/:statusId',auth,adminAuth,async(req,res)=>{try{await run(`UPDATE character_statuses SET active=FALSE,updated_at=CURRENT_TIMESTAMP WHERE id=? AND user_id=?`,[req.params.statusId,req.params.id]);res.json({ok:true,character:await getCharacterByUser(req.params.id)});}catch(error){res.status(500).json({error:'移除狀態失敗'});}});

app.get('/api/item-templates',auth,async(req,res)=>{try{
  const rows=await all(`SELECT * FROM item_templates ORDER BY category,name`);const user=await getUser(req.user.id);if(user?.is_admin)return res.json({items:rows.map(parsedItemTemplate)});
  const knownRows=await all(`SELECT item_template_id FROM item_identifications WHERE user_id=?`,[req.user.id]);const known=new Set(knownRows.map(x=>Number(x.item_template_id)));
  const visible=rows.filter(row=>!row.requires_identification||known.has(Number(row.id))).map(parsedItemTemplate);
  res.json({items:visible});
}catch(error){res.status(500).json({error:'讀取道具模板失敗'});}});
app.get('/api/admin/item-templates',auth,adminAuth,async(req,res)=>{try{const rows=await all(`SELECT * FROM item_templates ORDER BY category,name`);res.json({items:rows.map(parsedItemTemplate)});}catch(error){res.status(500).json({error:'讀取道具模板失敗'});}});
app.post('/api/admin/item-templates',auth,adminAuth,async(req,res)=>{try{
  const name=String(req.body.name||'').trim().slice(0,120);if(!name)return res.status(400).json({error:'道具名稱不能為空'});
  const type=['normal','buff','revive','profession_book','skill_book'].includes(req.body.item_type)?req.body.item_type:'normal';
  const raw=req.body.effects&&typeof req.body.effects==='object'?req.body.effects:{};
  const effects=effectContainerWithList(raw,normalizeUnifiedEffects(raw));
  const grantsTrueTime=Boolean(req.body.grants_true_time);
  const grantsTimeView=Boolean(req.body.grants_time_view)||grantsTrueTime;
  const row=await get(`INSERT INTO item_templates(name,category,item_type,description,effects,consume_on_use,grants_time_view,grants_true_time,requires_identification,unknown_name,identification_attribute,identification_dc) VALUES(?,?,?,?,?::jsonb,?,?,?,?,?,?,?) ON CONFLICT(name) DO UPDATE SET category=EXCLUDED.category,item_type=EXCLUDED.item_type,description=EXCLUDED.description,effects=EXCLUDED.effects,consume_on_use=EXCLUDED.consume_on_use,grants_time_view=EXCLUDED.grants_time_view,grants_true_time=EXCLUDED.grants_true_time,requires_identification=EXCLUDED.requires_identification,unknown_name=EXCLUDED.unknown_name,identification_attribute=EXCLUDED.identification_attribute,identification_dc=EXCLUDED.identification_dc,updated_at=CURRENT_TIMESTAMP RETURNING *`,[name,String(req.body.category||'道具').slice(0,80),type,String(req.body.description||''),JSON.stringify(effects),req.body.consume_on_use===undefined?true:Boolean(req.body.consume_on_use),grantsTimeView,grantsTrueTime,Boolean(req.body.requires_identification),String(req.body.unknown_name||'未知物品').trim().slice(0,120)||'未知物品',['spirit','agility','luck','fifth'].includes(String(req.body.identification_attribute||''))?String(req.body.identification_attribute):'spirit',clampInt(req.body.identification_dc,1,100,50)]);
  res.json({item:parsedItemTemplate(row)});
}catch(error){console.error(error);res.status(500).json({error:'建立道具模板失敗'});}});
app.delete('/api/admin/item-templates/:id',auth,adminAuth,async(req,res)=>{try{await run(`DELETE FROM item_templates WHERE id=?`,[req.params.id]);res.json({ok:true});}catch(error){res.status(500).json({error:'刪除道具模板失敗'});}});

app.post('/api/character/items/use',auth,async(req,res)=>{const client=await pool.connect();try{
  await client.query('BEGIN');
  const roomId=Number(req.body.room_id)||null;
  const itemName=String(req.body.name||'').trim();
  if(!itemName){await client.query('ROLLBACK');return res.status(400).json({error:'請選擇道具'});}
  const cr=await client.query(`SELECT * FROM character_cards WHERE user_id=$1 FOR UPDATE`,[req.user.id]);
  const character=parseCharacter(cr.rows[0]);
  if(!character){await client.query('ROLLBACK');return res.status(404).json({error:'找不到角色'});}
  let inventory=normalizeInventory(character.inventory);
  if(inventoryQuantity(inventory,itemName)<=0){await client.query('ROLLBACK');return res.status(400).json({error:'你沒有這個道具'});}
  const templateResult=await client.query(`SELECT * FROM item_templates WHERE name=$1`,[itemName]);
  const template=parsedItemTemplate(templateResult.rows[0]);
  if(!template){await client.query('ROLLBACK');return res.status(400).json({error:'這個道具尚未建立使用效果'});}
  const effects=template.effects||{};
  const unified=normalizeUnifiedEffects(effects);
  const hasExplicitUnifiedEffects=Boolean(effects&&typeof effects==='object'&&Array.isArray(effects.list)&&effects.list.length);
  let message=`使用了「${itemName}」`;

  // v15 舊道具資料仍然向下相容；新道具則優先使用統一效果列。
  if(hasExplicitUnifiedEffects){
    const result=await applyUnifiedEffectsWithClient(client,{userId:req.user.id,roomId,effects:unified,source:`道具：${itemName}`,allowedTimings:new Set(['immediate'])});
    if(result.message_parts.length)message+=`：${result.message_parts.join('、')}`;
  }else if(template.item_type==='revive'){
    if(!roomId){await client.query('ROLLBACK');return res.status(400).json({error:'復活道具只能在房間中使用'});}
    const mr=await client.query(`SELECT * FROM room_members WHERE room_id=$1 AND user_id=$2 FOR UPDATE`,[roomId,req.user.id]);const member=mr.rows[0];
    if(!member){await client.query('ROLLBACK');return res.status(403).json({error:'你不在這個房間'});}
    if(Number(member.hp)>0){await client.query('ROLLBACK');return res.status(400).json({error:'只有生命值為 0 的角色才能使用復活道具'});}
    const pct=Math.max(1,Math.min(100,Number(effects.revive_hp_percent)||20));const nextHp=Math.max(1,Math.floor(Number(member.max_hp||character.max_hp||1)*pct/100));await client.query(`UPDATE room_members SET hp=$1 WHERE room_id=$2 AND user_id=$3`,[nextHp,roomId,req.user.id]);message+=`，生命恢復至 ${nextHp}`;
  }else if(template.item_type==='buff'){
    const duration=Math.max(0,Math.floor(Number(effects.duration_rounds)||0));await applyAdHocModifierStatusWithClient(client,req.user.id,legacyEffectsToUnified(effects),`道具：${itemName}`,duration);message+=duration?`，獲得 ${duration} 回合加成`:'，獲得加成狀態';
  }else{
    const heal=Math.max(0,Math.floor(Number(effects.heal_hp)||0));const spiritHeal=Math.max(0,Math.floor(Number(effects.restore_spirit)||0));if(roomId&&(heal||spiritHeal)){const mr=await client.query(`SELECT * FROM room_members WHERE room_id=$1 AND user_id=$2 FOR UPDATE`,[roomId,req.user.id]);if(mr.rows[0])await client.query(`UPDATE room_members SET hp=LEAST(max_hp,hp+$1),spirit=LEAST(max_spirit,spirit+$2) WHERE room_id=$3 AND user_id=$4`,[heal,spiritHeal,roomId,req.user.id]);}message+='。';
  }

  // 重新讀取，避免統一效果同時修改背包時覆蓋掉新值。
  const afterEffectResult=await client.query(`SELECT inventory FROM character_cards WHERE user_id=$1 FOR UPDATE`,[req.user.id]);
  inventory=normalizeInventory(afterEffectResult.rows[0]&&afterEffectResult.rows[0].inventory);
  if(template.consume_on_use){inventory=changeInventoryQuantity(inventory,itemName,-1);await client.query(`UPDATE character_cards SET inventory=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2`,[JSON.stringify(inventory),req.user.id]);}
  await client.query('COMMIT');
  if(roomId)await addEvent(roomId,req.user.id,'item',{text:`${character.name}${message}`,item_name:itemName});
  const updated=await getCharacterByUser(req.user.id);const snapshot=roomId?await roomSnapshot(roomId):null;if(snapshot)io.to('room:'+roomId).emit('room:snapshot',snapshot);res.json({ok:true,character:updated,room:snapshot,message});
}catch(error){await client.query('ROLLBACK').catch(()=>{});console.error('USE ITEM ERROR:',error);res.status(error.status||500).json({error:error.message||'使用道具失敗'});}finally{client.release();}});

app.get('/api/rooms/:id/map-nodes',auth,async(req,res)=>{try{const roomId=Number(req.params.id);const user=await getUser(req.user.id);const isAdmin=Boolean(user?.is_admin);if(!isAdmin)await requireRoomMember(req.user.id,roomId);const room=await get(`SELECT current_map_node_id FROM rooms WHERE id=?`,[roomId]);let rows;if(isAdmin){rows=await all(`SELECT * FROM room_map_nodes WHERE room_id=? ORDER BY id`,[roomId]);}else{const team=await getUserRoomTeam(req.user.id,roomId);let viewers=[Number(req.user.id)];if(team){const ms=await all(`SELECT user_id FROM room_team_members WHERE team_id=?`,[team.id]);viewers=ms.map(x=>Number(x.user_id)).filter(Boolean);}rows=await all(`SELECT DISTINCT n.* FROM room_map_nodes n LEFT JOIN map_node_discoveries d ON d.node_id=n.id AND d.user_id=ANY(?::bigint[]) WHERE n.room_id=? AND (n.hidden=FALSE OR d.node_id IS NOT NULL OR n.id=?) ORDER BY n.id`,[viewers,roomId,team?.current_map_node_id||room?.current_map_node_id||null]);}res.json({current_map_node_id:room?.current_map_node_id||null,nodes:rows.map(r=>({...r,connections:parseJsonField(r.connections,[])}))});}catch(error){res.status(error.status||500).json({error:error.message||'讀取地圖節點失敗'});}});
app.post('/api/admin/rooms/:id/map-nodes',auth,adminAuth,async(req,res)=>{try{await requireRoomAdmin(req.user.id,req.params.id);const connections=Array.isArray(req.body.connections)?req.body.connections.map(Number).filter(Boolean):[];const row=await get(`INSERT INTO room_map_nodes(room_id,name,description,image_url,connections,hidden,noise_threshold,time_offset_minutes,time_rate,investigation_dc,discovery_hint) VALUES(?,?,?,?,?::jsonb,?,?,?,?,?,?) RETURNING *`,[req.params.id,String(req.body.name||'未命名區域').slice(0,120),String(req.body.description||''),String(req.body.image_url||''),JSON.stringify(connections),Boolean(req.body.hidden),clampInt(req.body.noise_threshold,0,999,30),clampInt(req.body.time_offset_minutes,-10080,10080,0),Math.max(-4,Math.min(8,Number(req.body.time_rate)===0?0:(Number(req.body.time_rate)||1))),clampInt(req.body.investigation_dc,1,100,50),String(req.body.discovery_hint||'').slice(0,2000)]);await syncBidirectionalNodeConnections(Number(req.params.id),row.id,[],connections);res.json({node:{...row,connections:parseJsonField(row.connections,[])}});}catch(error){res.status(error.status||500).json({error:error.message||'建立地圖節點失敗'});}});
app.post('/api/admin/rooms/:id/map-nodes/:nodeId/enter',auth,adminAuth,async(req,res)=>{try{await requireRoomAdmin(req.user.id,req.params.id);const node=await get(`SELECT * FROM room_map_nodes WHERE id=? AND room_id=?`,[req.params.nodeId,req.params.id]);if(!node)return res.status(404).json({error:'找不到地圖節點'});await run(`UPDATE rooms SET current_map_node_id=?,map_name=?,map_description=?,map_image_url=?,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[node.id,node.name,node.description,node.image_url,req.params.id]);await addEvent(req.params.id,req.user.id,'map',{text:`隊伍進入地點「${node.name}」`,node_id:node.id});const snapshot=await roomSnapshot(req.params.id);io.to('room:'+req.params.id).emit('room:snapshot',snapshot);res.json({ok:true,room:snapshot});}catch(error){res.status(error.status||500).json({error:error.message||'切換地點失敗'});}});
app.delete('/api/admin/rooms/:id/map-nodes/:nodeId',auth,adminAuth,async(req,res)=>{try{await requireRoomAdmin(req.user.id,req.params.id);await run(`DELETE FROM room_map_nodes WHERE id=? AND room_id=?`,[req.params.nodeId,req.params.id]);res.json({ok:true});}catch(error){res.status(error.status||500).json({error:error.message||'刪除節點失敗'});}});

function hiddenCheckValue(character,check){const key=String(check||'精神');const map={力量:'final_strength',敏捷:'final_agility',體質:'final_constitution',精神:'final_spirit',幸運:'final_luck',大道:'final_great_way',信仰:'final_faith'};return Math.max(0,Math.floor(Number(character[map[key]||'final_spirit'])||0));}
function hiddenSuccessLevel(roll,target){if(roll===1)return '大成功';if(target<=0)return '失敗';if(roll<=Math.max(1,Math.floor(target/5)))return '極難成功';if(roll<=Math.max(1,Math.floor(target/2)))return '困難成功';if(roll<=target)return '普通成功';return '失敗';}
app.post('/api/rooms/:id/hidden-roll',auth,async(req,res)=>{try{await requireRoomMember(req.user.id,req.params.id);const character=await requireCharacter(req.user.id);const check=String(req.body.check||'精神').slice(0,80);const target=hiddenCheckValue(character,check);const max=Math.max(100,target);const roll=1+Math.floor(Math.random()*max);const level=hiddenSuccessLevel(roll,target);await run(`INSERT INTO hidden_rolls(room_id,user_id,character_name,check_name,target_value,roll_value,success_level,note) VALUES(?,?,?,?,?,?,?,?)`,[req.params.id,req.user.id,character.name,check,target,roll,level,String(req.body.note||'').slice(0,500)]);res.json({ok:true,message:'隱藏判定已送交 DM。'});}catch(error){res.status(error.status||500).json({error:error.message||'隱藏判定失敗'});}});
app.get('/api/admin/rooms/:id/hidden-rolls',auth,adminAuth,async(req,res)=>{try{await requireRoomAdmin(req.user.id,req.params.id);const rows=await all(`SELECT * FROM hidden_rolls WHERE room_id=? ORDER BY id DESC LIMIT 100`,[req.params.id]);res.json({rolls:rows});}catch(error){res.status(error.status||500).json({error:error.message||'讀取隱藏判定失敗'});}});


/* =========================================================
   V22 TEAM / PRIVATE VISION SYSTEM
========================================================= */

async function requireRoomAccessUser(userId, roomId) {
  const user=await getUser(userId);
  if(user&&user.is_admin)return user;
  await requireRoomMember(userId,roomId);
  return user;
}

function emitTeamUpdate(roomId,teamId){io.to('room:'+roomId).emit('team:update',{teamId:Number(teamId)||null});}

async function getUserRoomTeam(userId,roomId){
  return get(`SELECT rt.* FROM room_team_members rtm JOIN room_teams rt ON rt.id=rtm.team_id WHERE rtm.room_id=? AND rtm.user_id=?`,[roomId,userId]);
}

async function requireTeamMemberUser(userId,teamId){
  const row=await get(`SELECT rtm.*,rt.name,rt.room_id,rt.leader_user_id,rt.current_map_node_id FROM room_team_members rtm JOIN room_teams rt ON rt.id=rtm.team_id WHERE rtm.team_id=? AND rtm.user_id=?`,[teamId,userId]);
  if(!row)throw Object.assign(new Error('你不是這個隊伍的成員'),{status:403});
  return row;
}

async function ensureTeamLeaderAfterDeparture(teamId,departingUserId){
  const team=await get(`SELECT * FROM room_teams WHERE id=?`,[teamId]);
  if(!team)return;
  const remaining=await all(`SELECT user_id FROM room_team_members WHERE team_id=? ORDER BY joined_at ASC`,[teamId]);
  if(!remaining.length){await run(`DELETE FROM room_teams WHERE id=?`,[teamId]);return;}
  if(String(team.leader_user_id)===String(departingUserId)){
    await run(`UPDATE room_teams SET leader_user_id=?,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[remaining[0].user_id,teamId]);
  }
}

function teamRuleSharedText(row){
  const level=Math.max(1,Math.min(3,Number(row.knowledge_level)||1));
  if(level>=3)return String(row.rule_text||row.public_hint||'');
  if(level>=2)return String(row.public_hint||'這條規則的內容仍然殘缺。');
  return String(row.public_hint||'隊友只知道這條規則可能存在。');
}

app.get('/api/rooms/:id/teams',auth,async(req,res)=>{try{
  const roomId=Number(req.params.id);const user=await requireRoomAccessUser(req.user.id,roomId);const isAdmin=Boolean(user&&user.is_admin);
  const teams=await all(`SELECT rt.*,rmn.name AS map_node_name,COUNT(rtm.user_id)::INTEGER AS member_count FROM room_teams rt LEFT JOIN room_team_members rtm ON rtm.team_id=rt.id LEFT JOIN room_map_nodes rmn ON rmn.id=rt.current_map_node_id WHERE rt.room_id=? GROUP BY rt.id,rmn.name ORDER BY rt.id`,[roomId]);
  const teamIds=teams.map(t=>Number(t.id));
  const memberRows=teamIds.length?await all(`SELECT rtm.team_id,rtm.user_id,rtm.joined_at,u.username,c.name AS character_name FROM room_team_members rtm JOIN users u ON u.id=rtm.user_id LEFT JOIN character_cards c ON c.user_id=rtm.user_id WHERE rtm.team_id = ANY(?::bigint[]) ORDER BY rtm.joined_at`,[teamIds]):[];
  const teamsDecorated=teams.map(team=>({...team,members:memberRows.filter(m=>String(m.team_id)===String(team.id))}));
  const myTeam=await getUserRoomTeam(req.user.id,roomId);
  let details={shared_clues:[],shared_rules:[],shared_notes:[],messages:[],visions:[]};
  if(myTeam||isAdmin){
    const detailTeamId=myTeam?myTeam.id:null;
    if(detailTeamId){
      const sharedClues=await all(`SELECT tsc.*,ct.name,ct.category,ct.description,ct.image_url,u.username AS shared_by_username,c.name AS shared_by_character FROM team_shared_clues tsc JOIN clue_templates ct ON ct.id=tsc.clue_id LEFT JOIN users u ON u.id=tsc.shared_by_user_id LEFT JOIN character_cards c ON c.user_id=tsc.shared_by_user_id WHERE tsc.team_id=? ORDER BY tsc.shared_at DESC`,[detailTeamId]);
      const sharedRulesRaw=await all(`SELECT tsr.*,hr.name,hr.public_hint,hr.rule_text,u.username AS shared_by_username,c.name AS shared_by_character FROM team_shared_rules tsr JOIN horror_rules hr ON hr.id=tsr.rule_id LEFT JOIN users u ON u.id=tsr.shared_by_user_id LEFT JOIN character_cards c ON c.user_id=tsr.shared_by_user_id WHERE tsr.team_id=? ORDER BY tsr.updated_at DESC`,[detailTeamId]);
      const sharedRules=sharedRulesRaw.map(r=>({...r,shared_text:teamRuleSharedText(r),rule_text:Number(r.knowledge_level)>=3?r.rule_text:''}));
      const sharedNotes=await all(`SELECT tsn.*,u.username AS shared_by_username,c.name AS shared_by_character FROM team_shared_notes tsn LEFT JOIN users u ON u.id=tsn.shared_by_user_id LEFT JOIN character_cards c ON c.user_id=tsn.shared_by_user_id WHERE tsn.team_id=? ORDER BY tsn.id DESC LIMIT 100`,[detailTeamId]);
      const messages=await all(`SELECT tm.*,u.username,c.name AS character_name FROM team_messages tm LEFT JOIN users u ON u.id=tm.user_id LEFT JOIN character_cards c ON c.user_id=tm.user_id WHERE tm.team_id=? ORDER BY tm.id DESC LIMIT 100`,[detailTeamId]);
      details={shared_clues:sharedClues,shared_rules:sharedRules,shared_notes:sharedNotes,messages:messages.reverse(),visions:[]};
    }
  }
  const visions=isAdmin
    ? await all(`SELECT pv.*,u.username,c.name AS character_name FROM personal_visions pv JOIN users u ON u.id=pv.user_id LEFT JOIN character_cards c ON c.user_id=pv.user_id WHERE pv.room_id=? ORDER BY pv.id DESC LIMIT 200`,[roomId])
    : await all(`SELECT * FROM personal_visions WHERE room_id=? AND user_id=? ORDER BY id DESC LIMIT 100`,[roomId,req.user.id]);
  details.visions=visions;
  const mapNodes=await all(`SELECT id,name,description,image_url,hidden FROM room_map_nodes WHERE room_id=? AND (? OR hidden=FALSE) ORDER BY id`,[roomId,isAdmin]);
  res.json({teams:teamsDecorated,my_team:myTeam,details,map_nodes:mapNodes});
}catch(error){console.error('TEAM SNAPSHOT ERROR',error);res.status(error.status||500).json({error:error.message||'讀取隊伍資料失敗'});}});

app.post('/api/rooms/:id/teams',auth,async(req,res)=>{try{
  const roomId=Number(req.params.id);await requireRoomMember(req.user.id,roomId);
  const existing=await getUserRoomTeam(req.user.id,roomId);if(existing)return res.status(400).json({error:'你已經在一個隊伍中，請先離隊'});
  const name=String(req.body.name||'').trim().slice(0,120);if(!name)return res.status(400).json({error:'請輸入隊伍名稱'});
  const room=await get(`SELECT current_map_node_id FROM rooms WHERE id=?`,[roomId]);
  const team=await get(`INSERT INTO room_teams(room_id,name,leader_user_id,current_map_node_id) VALUES(?,?,?,?) RETURNING *`,[roomId,name,req.user.id,room&&room.current_map_node_id||null]);
  await run(`INSERT INTO room_team_members(team_id,room_id,user_id) VALUES(?,?,?)`,[team.id,roomId,req.user.id]);
  const ch=await getCharacterByUser(req.user.id);await addEvent(roomId,req.user.id,'team',{text:`${ch?.name||req.user.username} 建立隊伍「${name}」`,team_id:team.id});emitTeamUpdate(roomId,team.id);res.json({ok:true,team});
}catch(error){res.status(error.status||500).json({error:error.message||'建立隊伍失敗'});}});

app.post('/api/rooms/:id/teams/:teamId/join',auth,async(req,res)=>{try{
  const roomId=Number(req.params.id);const teamId=Number(req.params.teamId);await requireRoomMember(req.user.id,roomId);
  const team=await get(`SELECT * FROM room_teams WHERE id=? AND room_id=?`,[teamId,roomId]);if(!team)return res.status(404).json({error:'找不到隊伍'});
  const existing=await getUserRoomTeam(req.user.id,roomId);if(existing)return res.status(400).json({error:String(existing.id)===String(teamId)?'你已經在這個隊伍中':'你已經在其他隊伍，請先離隊'});
  await run(`INSERT INTO room_team_members(team_id,room_id,user_id) VALUES(?,?,?)`,[teamId,roomId,req.user.id]);
  const ch=await getCharacterByUser(req.user.id);await addEvent(roomId,req.user.id,'team',{text:`${ch?.name||req.user.username} 加入隊伍「${team.name}」`,team_id:teamId});emitTeamUpdate(roomId,teamId);res.json({ok:true});
}catch(error){res.status(error.status||500).json({error:error.message||'加入隊伍失敗'});}});

app.post('/api/rooms/:id/teams/:teamId/leave',auth,async(req,res)=>{try{
  const roomId=Number(req.params.id),teamId=Number(req.params.teamId);const membership=await requireTeamMemberUser(req.user.id,teamId);if(Number(membership.room_id)!==roomId)return res.status(404).json({error:'找不到隊伍'});
  await run(`DELETE FROM room_team_members WHERE team_id=? AND user_id=?`,[teamId,req.user.id]);await ensureTeamLeaderAfterDeparture(teamId,req.user.id);
  const ch=await getCharacterByUser(req.user.id);await addEvent(roomId,req.user.id,'team',{text:`${ch?.name||req.user.username} 離開了隊伍`,team_id:teamId});emitTeamUpdate(roomId,teamId);res.json({ok:true});
}catch(error){res.status(error.status||500).json({error:error.message||'離開隊伍失敗'});}});

app.post('/api/rooms/:id/teams/:teamId/move',auth,async(req,res)=>{try{
  const roomId=Number(req.params.id),teamId=Number(req.params.teamId);const user=await getUser(req.user.id);const team=await get(`SELECT * FROM room_teams WHERE id=? AND room_id=?`,[teamId,roomId]);if(!team)return res.status(404).json({error:'找不到隊伍'});
  if(!(user&&user.is_admin)&&String(team.leader_user_id)!==String(req.user.id))return res.status(403).json({error:'只有隊長或 DM 可以帶隊移動'});
  if(!(user&&user.is_admin))await requireTeamMemberUser(req.user.id,teamId);
  const node=await get(`SELECT * FROM room_map_nodes WHERE id=? AND room_id=?`,[req.body.node_id,roomId]);if(!node)return res.status(404).json({error:'找不到地圖區域'});if(node.hidden&&!(user&&user.is_admin))return res.status(403).json({error:'這個區域尚未被發現'});
  if(!(user&&user.is_admin)&&team.current_map_node_id&&String(team.current_map_node_id)!==String(node.id)){
    const currentNode=await get(`SELECT connections FROM room_map_nodes WHERE id=? AND room_id=?`,[team.current_map_node_id,roomId]);const connections=mapConnectionIds(currentNode?.connections);if(connections.length&&!connections.includes(Number(node.id)))return res.status(400).json({error:'這個地點沒有與目前位置直接連接'});
  }
  await run(`UPDATE room_teams SET current_map_node_id=?,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[node.id,teamId]);
  await syncTeamMonsterEncounters(roomId,teamId,node.id);
  const leader=await getCharacterByUser(team.leader_user_id);await addEvent(roomId,req.user.id,'team_move',{text:`隊伍「${team.name}」前往「${node.name}」`,team_id:teamId,node_id:node.id,leader:leader?.name||''});emitTeamUpdate(roomId,teamId);res.json({ok:true,node});
}catch(error){res.status(error.status||500).json({error:error.message||'隊伍移動失敗'});}});

app.post('/api/rooms/:id/teams/:teamId/share-clue',auth,async(req,res)=>{try{
  const roomId=Number(req.params.id),teamId=Number(req.params.teamId);const membership=await requireTeamMemberUser(req.user.id,teamId);if(Number(membership.room_id)!==roomId)return res.status(404).json({error:'找不到隊伍'});
  const clue=await get(`SELECT ct.* FROM character_clues cc JOIN clue_templates ct ON ct.id=cc.clue_id WHERE cc.user_id=? AND cc.clue_id=?`,[req.user.id,req.body.clue_id]);if(!clue)return res.status(403).json({error:'你沒有這條線索，不能分享'});
  await run(`INSERT INTO team_shared_clues(team_id,clue_id,shared_by_user_id,note) VALUES(?,?,?,?) ON CONFLICT(team_id,clue_id) DO UPDATE SET shared_by_user_id=EXCLUDED.shared_by_user_id,note=EXCLUDED.note,shared_at=CURRENT_TIMESTAMP`,[teamId,clue.id,req.user.id,String(req.body.note||'').slice(0,1000)]);emitTeamUpdate(roomId,teamId);res.json({ok:true});
}catch(error){res.status(error.status||500).json({error:error.message||'分享線索失敗'});}});

app.post('/api/rooms/:id/teams/:teamId/share-rule',auth,async(req,res)=>{try{
  const roomId=Number(req.params.id),teamId=Number(req.params.teamId);const membership=await requireTeamMemberUser(req.user.id,teamId);if(Number(membership.room_id)!==roomId)return res.status(404).json({error:'找不到隊伍'});
  const discovery=await get(`SELECT rd.*,hr.id AS actual_rule_id FROM rule_discoveries rd JOIN horror_rules hr ON hr.id=rd.rule_id WHERE rd.user_id=? AND rd.rule_id=? AND hr.room_id=?`,[req.user.id,req.body.rule_id,roomId]);if(!discovery)return res.status(403).json({error:'你不知道這條規則，不能分享'});
  const level=Math.max(1,Math.min(3,Number(discovery.knowledge_level)||1));
  await run(`INSERT INTO team_shared_rules(team_id,rule_id,knowledge_level,shared_by_user_id,note) VALUES(?,?,?,?,?) ON CONFLICT(team_id,rule_id) DO UPDATE SET knowledge_level=GREATEST(team_shared_rules.knowledge_level,EXCLUDED.knowledge_level),shared_by_user_id=EXCLUDED.shared_by_user_id,note=EXCLUDED.note,updated_at=CURRENT_TIMESTAMP`,[teamId,discovery.rule_id,level,req.user.id,String(req.body.note||'').slice(0,1000)]);emitTeamUpdate(roomId,teamId);res.json({ok:true});
}catch(error){res.status(error.status||500).json({error:error.message||'分享規則失敗'});}});

app.post('/api/rooms/:id/teams/:teamId/chat',auth,async(req,res)=>{try{
  const roomId=Number(req.params.id),teamId=Number(req.params.teamId);const membership=await requireTeamMemberUser(req.user.id,teamId);if(Number(membership.room_id)!==roomId)return res.status(404).json({error:'找不到隊伍'});
  const content=String(req.body.content||'').trim().slice(0,1500);if(!content)return res.status(400).json({error:'訊息不能為空'});
  await run(`INSERT INTO team_messages(team_id,user_id,content) VALUES(?,?,?)`,[teamId,req.user.id,content]);emitTeamUpdate(roomId,teamId);res.json({ok:true});
}catch(error){res.status(error.status||500).json({error:error.message||'發送隊伍訊息失敗'});}});

app.post('/api/admin/rooms/:id/visions',auth,adminAuth,async(req,res)=>{try{
  const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);const userId=Number(req.body.user_id);const member=await get(`SELECT rm.*,c.name AS character_name FROM room_members rm LEFT JOIN character_cards c ON c.user_id=rm.user_id WHERE rm.room_id=? AND rm.user_id=? AND rm.role='player'`,[roomId,userId]);if(!member)return res.status(404).json({error:'找不到這個房間的玩家'});
  const title=String(req.body.title||'私人視野').trim().slice(0,180)||'私人視野';const content=String(req.body.content||'').trim().slice(0,8000);if(!content)return res.status(400).json({error:'私人視野內容不能為空'});
  const kind=['perception','whisper','hallucination','memory','secret'].includes(String(req.body.vision_type||''))?String(req.body.vision_type):'perception';const row=await get(`INSERT INTO personal_visions(room_id,user_id,title,content,vision_type,shareable) VALUES(?,?,?,?,?,?) RETURNING *`,[roomId,userId,title,content,kind,req.body.shareable===undefined?true:Boolean(req.body.shareable)]);const targetTeam=await getUserRoomTeam(userId,roomId);emitTeamUpdate(roomId,targetTeam?.id||null);res.json({ok:true,vision:row});
}catch(error){res.status(error.status||500).json({error:error.message||'發送私人視野失敗'});}});

app.post('/api/rooms/:id/visions/:visionId/read',auth,async(req,res)=>{try{const roomId=Number(req.params.id);await requireRoomAccessUser(req.user.id,roomId);await run(`UPDATE personal_visions SET is_read=TRUE WHERE id=? AND room_id=? AND user_id=?`,[req.params.visionId,roomId,req.user.id]);res.json({ok:true});}catch(error){res.status(error.status||500).json({error:error.message||'更新私人視野失敗'});}});

app.post('/api/rooms/:id/visions/:visionId/share',auth,async(req,res)=>{try{
  const roomId=Number(req.params.id);const team=await getUserRoomTeam(req.user.id,roomId);if(!team)return res.status(400).json({error:'你目前沒有隊伍'});
  const vision=await get(`SELECT * FROM personal_visions WHERE id=? AND room_id=? AND user_id=?`,[req.params.visionId,roomId,req.user.id]);if(!vision)return res.status(404).json({error:'找不到私人視野'});if(!vision.shareable)return res.status(403).json({error:'這項資訊不能直接分享'});
  const character=await getCharacterByUser(req.user.id);const prefix=character?.name?`${character.name} 分享了自己的觀察`:'隊友分享了自己的觀察';await run(`INSERT INTO team_shared_notes(team_id,shared_by_user_id,title,content,note_type,source_vision_id) VALUES(?,?,?,?,?,?)`,[team.id,req.user.id,vision.title,`${prefix}：${vision.content}`,vision.vision_type,vision.id]);emitTeamUpdate(roomId,team.id);res.json({ok:true});
}catch(error){res.status(error.status||500).json({error:error.message||'分享私人視野失敗'});}});


/* =========================================================
   V24 WORLD ENGINE
   地圖實體 / 噪音警戒 / 追逐 / 污染 / 定時事件 / Boss 階段
========================================================= */

function clampInt(value,min,max,fallback=0){
  const n=Number(value);return Number.isFinite(n)?Math.max(min,Math.min(max,Math.floor(n))):fallback;
}
function roomGameTotal(room){return ((Math.max(1,Number(room?.game_day)||1)-1)*1440)+(clampInt(room?.game_hour,0,23,0)*60)+clampInt(room?.game_minute,0,59,0);}
function mapConnectionIds(value){
  const raw=parseJsonField(value,[]);if(!Array.isArray(raw))return [];
  return [...new Set(raw.map(x=>Number(x&&typeof x==='object'?(x.id??x.node_id):x)).filter(Number.isFinite).filter(x=>x>0))];
}
async function validRoomMapNodeId(roomId,value){
  const id=Number(value)||0;if(!id)return null;const row=await get(`SELECT id FROM room_map_nodes WHERE id=? AND room_id=?`,[id,roomId]);if(!row)throw Object.assign(new Error('選擇的地圖節點不存在於這個房間'),{status:400});return row.id;
}
async function getViewerMapNodeId(userId,roomId){
  const team=await getUserRoomTeam(userId,roomId);if(team?.current_map_node_id)return team.current_map_node_id;
  const room=await get(`SELECT current_map_node_id FROM rooms WHERE id=?`,[roomId]);return room?.current_map_node_id||null;
}
async function usersAtMapNode(roomId,nodeId){
  if(!nodeId){const rows=await all(`SELECT user_id FROM room_members WHERE room_id=? AND role='player'`,[roomId]);return rows.map(r=>r.user_id);}
  const rows=await all(`
    SELECT DISTINCT rm.user_id
    FROM room_members rm
    LEFT JOIN room_team_members rtm ON rtm.room_id=rm.room_id AND rtm.user_id=rm.user_id
    LEFT JOIN room_teams rt ON rt.id=rtm.team_id
    JOIN rooms r ON r.id=rm.room_id
    WHERE rm.room_id=? AND rm.role='player'
      AND ((rt.current_map_node_id=? ) OR (rt.id IS NULL AND r.current_map_node_id=?))
  `,[roomId,nodeId,nodeId]);return rows.map(r=>r.user_id);
}
async function syncTeamMonsterEncounters(roomId,teamId,nodeId){
  const members=await all(`SELECT user_id FROM room_team_members WHERE team_id=? AND room_id=?`,[teamId,roomId]);if(!members.length)return;
  const monsters=await all(`SELECT id FROM room_monsters WHERE room_id=? AND map_node_id=? AND status='encountered'`,[roomId,nodeId]);
  for(const monster of monsters)for(const member of members){await run(`INSERT INTO monster_discoveries(room_monster_id,user_id,encountered,investigated) VALUES(?,?,TRUE,FALSE) ON CONFLICT(room_monster_id,user_id) DO UPDATE SET encountered=TRUE,updated_at=CURRENT_TIMESTAMP`,[monster.id,member.user_id]);}
}
async function getRoomMapGraph(roomId){
  const rows=await all(`SELECT id,connections FROM room_map_nodes WHERE room_id=?`,[roomId]);const graph=new Map();for(const row of rows)graph.set(Number(row.id),mapConnectionIds(row.connections));return graph;
}
async function nextNodeToward(roomId,fromId,targetId){
  fromId=Number(fromId)||0;targetId=Number(targetId)||0;if(!fromId||!targetId||fromId===targetId)return targetId||fromId||null;
  const graph=await getRoomMapGraph(roomId);if(!graph.has(fromId)||!graph.has(targetId))return null;
  const queue=[fromId],prev=new Map([[fromId,null]]);
  while(queue.length){const cur=queue.shift();if(cur===targetId)break;for(const nxt of graph.get(cur)||[]){if(!graph.has(nxt)||prev.has(nxt))continue;prev.set(nxt,cur);queue.push(nxt);}}
  if(!prev.has(targetId))return null;let cur=targetId,last=targetId;while(prev.get(cur)!==null&&prev.get(cur)!==fromId){cur=prev.get(cur);last=cur;}return prev.get(cur)===fromId?cur:last;
}
function noiseLabel(level){const n=Number(level)||0;return n>=60?'劇烈':n>=30?'大聲':n>=15?'明顯':n>=5?'輕微':n>0?'極低':'無聲';}
function inferActionNoise(action,requested){
  const text=String(action||'');let inferred=0;
  if(/爆炸|爆破|手雷|炸彈/.test(text))inferred=80;
  else if(/開槍|射擊|槍|炮|砲/.test(text))inferred=60;
  else if(/尖叫|大喊|吶喊|砸|撞門|踹門|破門/.test(text))inferred=40;
  else if(/奔跑|衝刺|追趕|拖動|打鬥/.test(text))inferred=25;
  else if(/走|移動|搜索|翻找|開門/.test(text))inferred=10;
  else if(/潛行|躡手躡腳|輕聲|耳語/.test(text))inferred=3;
  const req=Number(requested);return Math.max(inferred,Number.isFinite(req)?clampInt(req,0,100,0):0);
}
async function applyActionNoise(roomId,nodeId,teamId,userId,action,requestedNoise){
  const amount=inferActionNoise(action,requestedNoise);if(!amount||!nodeId)return {amount,label:noiseLabel(amount),node_id:nodeId||null,alerts:0};
  const node=await get(`SELECT * FROM room_map_nodes WHERE id=? AND room_id=?`,[nodeId,roomId]);if(!node)return {amount,label:noiseLabel(amount),node_id:null,alerts:0};
  const nextNoise=Math.min(100,(Number(node.noise_level)||0)+amount);const nextAlert=Math.min(100,(Number(node.alert_level)||0)+Math.ceil(amount*0.6));
  await run(`UPDATE room_map_nodes SET noise_level=?,alert_level=?,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[nextNoise,nextAlert,node.id]);
  const graph=await getRoomMapGraph(roomId);const nearby=new Set([Number(node.id),...(graph.get(Number(node.id))||[])]);let alerts=0;
  const monsters=await all(`SELECT rm.id,rm.map_node_id,rm.alert_level,mt.name,mt.config FROM room_monsters rm JOIN monster_templates mt ON mt.id=rm.monster_template_id WHERE rm.room_id=? AND rm.status<>'defeated'`,[roomId]);
  for(const monster of monsters){if(monster.map_node_id&&!nearby.has(Number(monster.map_node_id)))continue;const cfg=parseWorldConfig(monster.config);const threshold=clampInt(cfg.hearing_threshold,0,100,30);if(amount<threshold)continue;alerts+=1;await run(`UPDATE room_monsters SET alert_level=LEAST(100,alert_level+?),chase_team_id=COALESCE(?,chase_team_id),updated_at=CURRENT_TIMESTAMP WHERE id=?`,[Math.max(5,Math.ceil(amount/2)),teamId||null,monster.id]);}
  return {amount,label:noiseLabel(amount),node_id:node.id,node_noise:nextNoise,node_alert:nextAlert,alerts};
}
async function decayWorldNoise(roomId,deltaMinutes){
  const steps=Math.floor(Math.max(0,Number(deltaMinutes)||0)/10);if(!steps)return;
  await run(`UPDATE room_map_nodes SET noise_level=GREATEST(0,noise_level-?),alert_level=GREATEST(0,alert_level-?),updated_at=CURRENT_TIMESTAMP WHERE room_id=?`,[steps*5,steps*3,roomId]);
  await run(`UPDATE room_monsters SET alert_level=GREATEST(0,alert_level-?),updated_at=CURRENT_TIMESTAMP WHERE room_id=?`,[steps*2,roomId]);
}
function pollutionStage(value){const n=clampInt(value,0,100,0);if(n>=80)return {key:'severe',label:'重度污染'};if(n>=50)return {key:'distorted',label:'感知扭曲'};if(n>=20)return {key:'trace',label:'輕度污染'};return {key:'clean',label:'正常'};}
async function maybeCreatePollutionVision(userId,roomId,location=''){
  const ch=await get(`SELECT pollution FROM character_cards WHERE user_id=?`,[userId]);const pollution=clampInt(ch?.pollution,0,100,0);if(pollution<20)return null;
  const chance=pollution>=80?0.45:pollution>=50?0.25:0.10;if(Math.random()>chance)return null;
  const mild=[`你在${location||'周圍'}的視野邊緣捕捉到一瞬間不自然的影子。`,`你短暫感覺空間的距離似乎和剛才不一樣。`,`某個聲音像是比實際位置更靠近你。`];
  const mid=[`你確信${location||'這裡'}有某個物件剛剛移動過，但其他人未必看見。`,`你聽見有人用熟悉的聲音叫你的名字，來源卻無法定位。`,`牆面上的細節短暫拼成了像規則文字一樣的形狀。`];
  const severe=[`你看見一個「應該存在」的人影站在隊伍之中；眨眼後位置又改變了。`,`你的一段記憶與眼前場景產生矛盾，你無法立刻判斷哪一個是真的。`,`你看見某條路似乎通往不可能存在的方向。`];
  const pool=pollution>=80?severe:pollution>=50?mid:mild;const content=pool[Math.floor(Math.random()*pool.length)];
  const row=await get(`INSERT INTO personal_visions(room_id,user_id,title,content,vision_type,shareable) VALUES(?,?,?,?,?,?) RETURNING *`,[roomId,userId,`污染感知 · ${pollution}%`,content,'hallucination',true]);return row;
}
function normalizeBossPhases(value){
  const raw=Array.isArray(value)?value:parseJsonField(value,[]);if(!Array.isArray(raw))return [];
  return raw.slice(0,12).map((p,i)=>({
    name:String(p?.name||`階段 ${i+1}`).slice(0,120),
    trigger_type:['hp_percent','time','manual'].includes(String(p?.trigger_type||''))?String(p.trigger_type):'hp_percent',
    threshold:clampInt(p?.threshold??p?.hp_percent,0,100,50),
    trigger_day:Math.max(1,clampInt(p?.trigger_day,1,9999,1)),
    trigger_minute:clampInt(p?.trigger_minute,0,1439,0),
    ai_behavior:['balanced','aggressive','defensive','random','story'].includes(String(p?.ai_behavior||''))?String(p.ai_behavior):'aggressive',
    damage_multiplier:Math.max(0,Math.min(10,Number(p?.damage_multiplier)||1)),
    skills:splitLines(p?.skills||p?.skill_refs||[],20),
    heal_percent:Math.max(0,Math.min(100,Number(p?.heal_percent)||0)),
    description:String(p?.description||'').slice(0,2000)
  }));
}
function bossPhaseFromEntity(entity){const cfg=parseWorldConfig(entity?.config);const phases=normalizeBossPhases(cfg.boss_phases);const idx=Math.max(0,Number(entity?.boss_phase_index)||0);return idx>0?phases[idx-1]||null:null;}
function bossDamageMultiplier(entity){const phase=bossPhaseFromEntity(entity);return phase?Math.max(0,Number(phase.damage_multiplier)||1):1;}
async function activateMonsterBossPhase(roomId,monsterId,nextIndex,phase,reason=''){
  const row=await get(`SELECT rm.*,mt.name,mt.config FROM room_monsters rm JOIN monster_templates mt ON mt.id=rm.monster_template_id WHERE rm.id=? AND rm.room_id=?`,[monsterId,roomId]);if(!row)return false;
  if(nextIndex<=Number(row.boss_phase_index)||0)return false;
  let healed=0;if(Number(phase.heal_percent)>0){const maxHp=Math.max(1,Number(row.max_hp)||100);const before=Math.max(0,Number(row.current_hp)||0);const after=Math.min(maxHp,before+Math.ceil(maxHp*Number(phase.heal_percent)/100));healed=Math.max(0,after-before);await run(`UPDATE room_monsters SET boss_phase_index=?,boss_phase_name=?,current_hp=?,status=CASE WHEN ?>0 THEN 'encountered' ELSE status END,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[nextIndex,phase.name,after,after,monsterId]);}else await run(`UPDATE room_monsters SET boss_phase_index=?,boss_phase_name=?,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[nextIndex,phase.name,monsterId]);
  await addEvent(roomId,null,'boss_phase',{text:`👑 ${row.name} 進入「${phase.name}」${healed?`，恢復 ${healed} HP`:''}${phase.skills?.length?`，解鎖階段技能：${phase.skills.join('、')}`:''}${phase.description?`：${phase.description}`:''}`,monster_id:monsterId,phase_index:nextIndex,phase,reason,healed});return true;
}
async function checkMonsterBossPhase(roomId,monsterId,reason='hp'){
  const row=await get(`SELECT rm.*,mt.name,mt.config FROM room_monsters rm JOIN monster_templates mt ON mt.id=rm.monster_template_id WHERE rm.id=? AND rm.room_id=?`,[monsterId,roomId]);if(!row)return null;const cfg=parseWorldConfig(row.config);if(!cfg.boss_enabled)return null;const phases=normalizeBossPhases(cfg.boss_phases);if(!phases.length)return null;
  const current=Math.max(0,Number(row.boss_phase_index)||0);const hpPct=Math.max(0,Math.min(100,(Number(row.current_hp)||0)/Math.max(1,Number(row.max_hp)||1)*100));const room=await get(`SELECT game_day,game_hour,game_minute FROM rooms WHERE id=?`,[roomId]);const total=roomGameTotal(room);
  for(let i=current;i<phases.length;i++){const phase=phases[i];let hit=false;if(phase.trigger_type==='hp_percent')hit=hpPct<=phase.threshold;else if(phase.trigger_type==='time')hit=total>=((phase.trigger_day-1)*1440+phase.trigger_minute);if(hit){await activateMonsterBossPhase(roomId,monsterId,i+1,phase,reason);return phase;}if(phase.trigger_type!=='manual')break;}
  return null;
}
async function checkAllBossTimePhases(roomId,total){const rows=await all(`SELECT id FROM room_monsters WHERE room_id=? AND status<>'defeated'`,[roomId]);for(const row of rows)await checkMonsterBossPhase(roomId,row.id,'time');}
async function v27ScheduledEventNodeId(ev,roomId){
  if(ev.map_node_id)return Number(ev.map_node_id);
  if(ev.audience==='team'&&ev.target_team_id){const t=await get(`SELECT current_map_node_id FROM room_teams WHERE id=? AND room_id=?`,[ev.target_team_id,roomId]);if(t?.current_map_node_id)return Number(t.current_map_node_id);}
  if(ev.audience==='user'&&ev.target_user_id){const id=await getViewerMapNodeId(ev.target_user_id,roomId);if(id)return Number(id);}
  const room=await get(`SELECT current_map_node_id FROM rooms WHERE id=?`,[roomId]);return Number(room?.current_map_node_id)||null;
}
async function v27ScheduledBasisInterval(ev,roomId,fromTotal,toTotal,flowAlreadyAdvanced=false){
  if(String(ev.time_basis||'world')!=='local')return {from:fromTotal,to:toTotal,node_id:null};
  const room=await get(`SELECT time_distortion_offset_minutes,time_rate,time_flow_offset_minutes FROM rooms WHERE id=?`,[roomId]);
  if(!room)return {from:fromTotal,to:toTotal,node_id:null};
  const rawDelta=toTotal-fromTotal;
  const roomRate=Number.isFinite(Number(room.time_rate))?Number(room.time_rate):1;
  const currentRoomFlow=clampInt(room.time_flow_offset_minutes,-525600,525600,0);
  const roomFlowStep=flowAlreadyAdvanced?Math.round(rawDelta*(roomRate-1)):0;
  const previousRoomFlow=currentRoomFlow-roomFlowStep;
  const nodeId=await v27ScheduledEventNodeId(ev,roomId);
  let nodeStatic=0,nodeRate=1,currentNodeFlow=0,nodeFlowStep=0;
  if(nodeId){const node=await get(`SELECT time_offset_minutes,time_rate,time_flow_offset_minutes FROM room_map_nodes WHERE id=? AND room_id=?`,[nodeId,roomId]);if(node){nodeStatic=clampInt(node.time_offset_minutes,-10080,10080,0);nodeRate=Number.isFinite(Number(node.time_rate))?Number(node.time_rate):1;currentNodeFlow=clampInt(node.time_flow_offset_minutes,-525600,525600,0);nodeFlowStep=flowAlreadyAdvanced?Math.round(rawDelta*(nodeRate-1)):0;}}
  const staticOffset=clampInt(room.time_distortion_offset_minutes,-10080,10080,0)+nodeStatic;
  return {from:fromTotal+staticOffset+previousRoomFlow+(currentNodeFlow-nodeFlowStep),to:toTotal+staticOffset+currentRoomFlow+currentNodeFlow,node_id:nodeId,room_rate:roomRate,node_rate:nodeRate};
}
function v27ScheduledOccurrences(ev,fromBasis,toBasis){
  if(!(toBasis>fromBasis))return [];
  const occurrences=[];
  if(ev.repeat_mode==='daily'){
    const startDay=Math.floor(fromBasis/1440)+1,endDay=Math.floor(toBasis/1440)+1;
    for(let day=startDay;day<=endDay&&day-startDay<370;day++){
      const total=((day-1)*1440)+clampInt(ev.trigger_minute,0,1439,0);
      if(total>fromBasis&&total<=toBasis&&(!ev.last_trigger_total||total>Number(ev.last_trigger_total)))occurrences.push(total);
    }
  }else{
    const total=((Math.max(1,Number(ev.trigger_day)||1)-1)*1440)+clampInt(ev.trigger_minute,0,1439,0);
    if(total>fromBasis&&total<=toBasis&&(!ev.last_trigger_total||Number(ev.last_trigger_total)<total))occurrences.push(total);
  }
  return occurrences;
}
async function processScheduledWorldEvents(roomId,fromTotal,toTotal,flowAlreadyAdvanced=false){
  if(!(toTotal>fromTotal))return [];
  const events=await all(`SELECT * FROM scheduled_world_events WHERE room_id=? AND active=TRUE ORDER BY id`,[roomId]);const fired=[];
  for(const ev of events){
    const basis=await v27ScheduledBasisInterval(ev,roomId,fromTotal,toTotal,flowAlreadyAdvanced);
    const occurrences=v27ScheduledOccurrences(ev,basis.from,basis.to);
    for(const total of occurrences){
      const day=Math.floor(total/1440)+1,minute=((total%1440)+1440)%1440,hour=Math.floor(minute/60),min=minute%60;
      const prefix=`⏰ ${ev.time_basis==='local'?'當地時間':'世界時間'} 第 ${day} 天 ${String(hour).padStart(2,'0')}:${String(min).padStart(2,'0')}`;const content=String(ev.content||ev.name||'事件發生');
      let delivered=false;
      if(ev.audience==='user'&&ev.target_user_id){const viewerNode=await getViewerMapNodeId(ev.target_user_id,roomId);if(!ev.map_node_id||String(viewerNode||'')===String(ev.map_node_id)){await run(`INSERT INTO personal_visions(room_id,user_id,title,content,vision_type,shareable) VALUES(?,?,?,?,?,?)`,[roomId,ev.target_user_id,ev.name,`${prefix}\n${content}`,'secret',true]);delivered=true;}}
      else if(ev.audience==='team'&&ev.target_team_id){const t=await get(`SELECT current_map_node_id FROM room_teams WHERE id=? AND room_id=?`,[ev.target_team_id,roomId]);if(!ev.map_node_id||String(t?.current_map_node_id||'')===String(ev.map_node_id)){await run(`INSERT INTO team_shared_notes(team_id,title,content,note_type) VALUES(?,?,?,?)`,[ev.target_team_id,ev.name,`${prefix}\n${content}`,'timed_event']);emitTeamUpdate(roomId,ev.target_team_id);delivered=true;}}
      else if(ev.map_node_id){const teamsHere=await all(`SELECT id FROM room_teams WHERE room_id=? AND current_map_node_id=?`,[roomId,ev.map_node_id]);for(const t of teamsHere){await run(`INSERT INTO team_shared_notes(team_id,title,content,note_type) VALUES(?,?,?,?)`,[t.id,ev.name,`${prefix}\n${content}`,'timed_event']);emitTeamUpdate(roomId,t.id);delivered=true;}const usersHere=await usersAtMapNode(roomId,ev.map_node_id);for(const uid of usersHere){const t=await getUserRoomTeam(uid,roomId);if(t)continue;await run(`INSERT INTO personal_visions(room_id,user_id,title,content,vision_type,shareable) VALUES(?,?,?,?,?,?)`,[roomId,uid,ev.name,`${prefix}\n${content}`,'event',true]);delivered=true;}}
      else{await addEvent(roomId,null,'timed_event',{text:`${prefix}｜${content}`,scheduled_event_id:ev.id,map_node_id:null});delivered=true;}
      await v27ExecuteWorldEventAction(ev,roomId);
      await run(`UPDATE scheduled_world_events SET trigger_count=trigger_count+1,last_trigger_total=?,active=CASE WHEN repeat_mode='once' THEN FALSE ELSE active END,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[total,ev.id]);
      fired.push({id:ev.id,total,time_basis:ev.time_basis||'world',delivered});
    }
  }
  return fired;
}
async function advanceWorldAi(roomId,dmUserId){
  const messages=[];const teams=await all(`SELECT * FROM room_teams WHERE room_id=?`,[roomId]);const teamById=new Map(teams.map(t=>[Number(t.id),t]));
  const monsters=await all(`SELECT rm.*,mt.name,mt.ai_level,mt.ai_behavior,mt.config FROM room_monsters rm JOIN monster_templates mt ON mt.id=rm.monster_template_id WHERE rm.room_id=? AND rm.status<>'defeated'`,[roomId]);
  for(const m of monsters){if(Number(m.ai_level)<=0)continue;const cfg=parseWorldConfig(m.config);const phase=bossPhaseFromEntity(m);const behavior=phase?.ai_behavior||m.ai_behavior;let moved=false;
    if(m.chase_team_id&&m.map_node_id){const team=teamById.get(Number(m.chase_team_id));if(team?.current_map_node_id&&String(team.current_map_node_id)!==String(m.map_node_id)){const next=await nextNodeToward(roomId,m.map_node_id,team.current_map_node_id);if(next){await run(`UPDATE room_monsters SET map_node_id=?,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[next,m.id]);const node=await get(`SELECT name FROM room_map_nodes WHERE id=?`,[next]);messages.push(`👹 ${m.name} 被聲響吸引，移動到「${node?.name||'鄰近區域'}」`);moved=true;if(String(next)===String(team.current_map_node_id)){await run(`UPDATE room_monsters SET status='encountered' WHERE id=?`,[m.id]);await syncTeamMonsterEncounters(roomId,team.id,next);messages.push(`⚠️ ${m.name} 追上了隊伍「${team.name}」。`);}}}}
    if(!moved&&m.status==='encountered'){const action=worldAiChoice(parseWorldList(cfg.ai_actions),behavior)||'觀察玩家的動向';messages.push(`👹 ${m.name}：${action}`);}
  }
  const npcs=await all(`SELECT rn.map_node_id,nt.* FROM room_npcs rn JOIN npc_templates nt ON nt.id=rn.npc_template_id WHERE rn.room_id=? AND rn.status='active'`,[roomId]);for(const n of npcs){if(Number(n.ai_level)<=0)continue;const config=parseWorldConfig(n.config);const line=worldAiChoice(parseWorldList(config.ai_lines),n.ai_behavior);const action=worldAiChoice(parseWorldList(config.ai_actions),n.ai_behavior);if(line&&n.speech_enabled)messages.push(`🧑 ${n.name}：「${line}」`);else if(action)messages.push(`🧑 ${n.name}：${action}`);}
  const dungeons=await all(`SELECT rd.map_node_id,dt.* FROM room_dungeons rd JOIN dungeon_templates dt ON dt.id=rd.dungeon_template_id WHERE rd.room_id=? AND rd.status='available'`,[roomId]);for(const d of dungeons){if(Number(d.ai_level)<=0)continue;const config=parseWorldConfig(d.config);const event=worldAiChoice(parseWorldList(config.ai_events),d.ai_behavior);if(event)messages.push(`🏚️ ${d.name}：${event}`);}
  return messages;
}

app.patch('/api/admin/rooms/:id/map-nodes/:nodeId',auth,adminAuth,async(req,res)=>{try{
  const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);const node=await get(`SELECT * FROM room_map_nodes WHERE id=? AND room_id=?`,[req.params.nodeId,roomId]);if(!node)return res.status(404).json({error:'找不到地圖節點'});
  const connections=req.body.connections===undefined?mapConnectionIds(node.connections):(Array.isArray(req.body.connections)?req.body.connections.map(Number).filter(Boolean):[]);for(const id of connections){if(!await get(`SELECT id FROM room_map_nodes WHERE id=? AND room_id=?`,[id,roomId]))return res.status(400).json({error:'連接節點包含不存在的地點'});}
  const oldConnections=mapConnectionIds(node.connections);const row=await get(`UPDATE room_map_nodes SET name=?,description=?,image_url=?,connections=?::jsonb,hidden=?,noise_threshold=?,time_offset_minutes=?,time_rate=?,investigation_dc=?,discovery_hint=?,updated_at=CURRENT_TIMESTAMP WHERE id=? RETURNING *`,[String(req.body.name??node.name).trim().slice(0,120)||node.name,String(req.body.description??node.description),String(req.body.image_url??node.image_url),JSON.stringify(connections),req.body.hidden===undefined?node.hidden:Boolean(req.body.hidden),clampInt(req.body.noise_threshold??node.noise_threshold,0,999,30),clampInt(req.body.time_offset_minutes??node.time_offset_minutes,-10080,10080,0),req.body.time_rate===undefined?(Number(node.time_rate)||1):Math.max(-4,Math.min(8,Number(req.body.time_rate)===0?0:(Number(req.body.time_rate)||1))),clampInt(req.body.investigation_dc??node.investigation_dc,1,100,50),String(req.body.discovery_hint??node.discovery_hint??'').slice(0,2000),node.id]);await syncBidirectionalNodeConnections(roomId,node.id,oldConnections,connections);res.json({node:{...row,connections:mapConnectionIds(row.connections)}});
}catch(error){res.status(error.status||500).json({error:error.message||'修改地圖節點失敗'});}});

app.patch('/api/admin/rooms/:id/world-location',auth,adminAuth,async(req,res)=>{try{
  const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);const type=String(req.body.type||req.body.kind||'');const entityId=Number(req.body.entity_id??req.body.id)||0;const nodeId=await validRoomMapNodeId(roomId,req.body.map_node_id);if(!entityId)return res.status(400).json({error:'缺少世界單位'});
  if(type==='monster')await run(`UPDATE room_monsters SET map_node_id=?,updated_at=CURRENT_TIMESTAMP WHERE room_id=? AND id=?`,[nodeId,roomId,entityId]);
  else if(type==='npc')await run(`UPDATE room_npcs SET map_node_id=?,updated_at=CURRENT_TIMESTAMP WHERE room_id=? AND npc_template_id=?`,[nodeId,roomId,entityId]);
  else if(type==='dungeon')await run(`UPDATE room_dungeons SET map_node_id=? WHERE room_id=? AND dungeon_template_id=?`,[nodeId,roomId,entityId]);
  else return res.status(400).json({error:'未知世界單位類型'});res.json({ok:true,map_node_id:nodeId});
}catch(error){res.status(error.status||500).json({error:error.message||'移動世界單位失敗'});}});

app.patch('/api/admin/rooms/:id/map-nodes/:nodeId/noise',auth,adminAuth,async(req,res)=>{try{
  const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);const row=await get(`UPDATE room_map_nodes SET noise_level=?,alert_level=?,updated_at=CURRENT_TIMESTAMP WHERE room_id=? AND id=? RETURNING *`,[clampInt(req.body.noise_level,0,100,0),clampInt(req.body.alert_level,0,100,0),roomId,req.params.nodeId]);if(!row)return res.status(404).json({error:'找不到地圖節點'});res.json({ok:true,node:row});
}catch(error){res.status(error.status||500).json({error:error.message||'調整警戒失敗'});}});

app.patch('/api/admin/rooms/:id/members/:userId/pollution',auth,adminAuth,async(req,res)=>{try{
  const roomId=Number(req.params.id),userId=Number(req.params.userId);await requireRoomAdmin(req.user.id,roomId);const member=await get(`SELECT user_id FROM room_members WHERE room_id=? AND user_id=?`,[roomId,userId]);if(!member)return res.status(404).json({error:'找不到玩家'});const before=await get(`SELECT pollution FROM character_cards WHERE user_id=?`,[userId]);const value=clampInt(req.body.value,0,100,0);await run(`UPDATE character_cards SET pollution=?,updated_at=CURRENT_TIMESTAMP WHERE user_id=?`,[value,userId]);const oldStage=pollutionStage(before?.pollution),newStage=pollutionStage(value);if(oldStage.key!==newStage.key&&value>Number(before?.pollution||0)){await run(`INSERT INTO personal_visions(room_id,user_id,title,content,vision_type,shareable) VALUES(?,?,?,?,?,?)`,[roomId,userId,`污染階段：${newStage.label}`,`你的感知開始出現新的異常。當前污染值：${value}%。`,'hallucination',true]);}const team=await getUserRoomTeam(userId,roomId);emitTeamUpdate(roomId,team?.id||null);res.json({ok:true,pollution:value,stage:newStage});
}catch(error){res.status(error.status||500).json({error:error.message||'調整污染失敗'});}});

app.get('/api/rooms/:id/world-systems',auth,async(req,res)=>{try{
  const roomId=Number(req.params.id);const user=await getUser(req.user.id);const isAdmin=Boolean(user?.is_admin);if(!isAdmin)await requireRoomMember(req.user.id,roomId);const team=isAdmin?null:await getUserRoomTeam(req.user.id,roomId);
  const chases=isAdmin?await all(`SELECT rc.*,rt.name AS team_name,mt.name AS monster_name FROM room_chases rc JOIN room_teams rt ON rt.id=rc.team_id JOIN room_monsters rm ON rm.id=rc.monster_id JOIN monster_templates mt ON mt.id=rm.monster_template_id WHERE rc.room_id=? ORDER BY rc.id DESC LIMIT 50`,[roomId]):team?await all(`SELECT rc.*,rt.name AS team_name,mt.name AS monster_name FROM room_chases rc JOIN room_teams rt ON rt.id=rc.team_id JOIN room_monsters rm ON rm.id=rc.monster_id JOIN monster_templates mt ON mt.id=rm.monster_template_id WHERE rc.room_id=? AND rc.team_id=? ORDER BY rc.id DESC LIMIT 20`,[roomId,team.id]):[];
  const nodes=await all(`SELECT id,name,noise_level,alert_level,noise_threshold,connections FROM room_map_nodes WHERE room_id=? ORDER BY id`,[roomId]);const character=isAdmin?null:await getCharacterByUser(req.user.id);const scheduled=isAdmin?await all(`SELECT swe.*,rt.name AS team_name,u.username AS user_name,rmn.name AS map_node_name FROM scheduled_world_events swe LEFT JOIN room_teams rt ON rt.id=swe.target_team_id LEFT JOIN users u ON u.id=swe.target_user_id LEFT JOIN room_map_nodes rmn ON rmn.id=swe.map_node_id WHERE swe.room_id=? ORDER BY swe.id DESC`,[roomId]):[];
  res.json({chases:chases.map(c=>({...c,log:parseJsonField(c.log,[])})),nodes:nodes.map(n=>({...n,connections:mapConnectionIds(n.connections)})),scheduled_events:scheduled,pollution:character?clampInt(character.pollution,0,100,0):null,pollution_stage:character?pollutionStage(character.pollution):null});
}catch(error){res.status(error.status||500).json({error:error.message||'讀取世界系統失敗'});}});

app.post('/api/admin/rooms/:id/scheduled-events',auth,adminAuth,async(req,res)=>{try{
  const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);const name=String(req.body.name||'').trim().slice(0,180);if(!name)return res.status(400).json({error:'請輸入事件名稱'});const audience=['room','team','user'].includes(String(req.body.audience||''))?String(req.body.audience):'room';const triggerMinute=clampInt(req.body.trigger_minute,0,1439,0);const timeBasis=req.body.time_basis==='local'?'local':'world';const actionType=['message','reveal_node','set_node_hidden','set_rule_active','spawn_monster','spawn_npc','boss_next','pollution_delta','sanity_delta','private_vision','set_time_rate','set_time_offset'].includes(String(req.body.action_type||''))?String(req.body.action_type):'message';const actionPayload=req.body.action_payload&&typeof req.body.action_payload==='object'?req.body.action_payload:{};const row=await get(`INSERT INTO scheduled_world_events(room_id,name,trigger_day,trigger_minute,repeat_mode,audience,target_team_id,target_user_id,map_node_id,content,active,time_basis,action_type,action_payload) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?::jsonb) RETURNING *`,[roomId,name,Math.max(1,clampInt(req.body.trigger_day,1,9999,1)),triggerMinute,req.body.repeat_mode==='daily'?'daily':'once',audience,Number(req.body.target_team_id)||null,Number(req.body.target_user_id)||null,await validRoomMapNodeId(roomId,req.body.map_node_id),String(req.body.content||'').slice(0,8000),req.body.active===undefined?true:Boolean(req.body.active),timeBasis,actionType,JSON.stringify(actionPayload)]);res.json({ok:true,event:row});
}catch(error){res.status(error.status||500).json({error:error.message||'建立定時事件失敗'});}});
app.delete('/api/admin/rooms/:id/scheduled-events/:eventId',auth,adminAuth,async(req,res)=>{try{const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);await run(`DELETE FROM scheduled_world_events WHERE id=? AND room_id=?`,[req.params.eventId,roomId]);res.json({ok:true});}catch(error){res.status(error.status||500).json({error:error.message||'刪除定時事件失敗'});}});

app.post('/api/admin/rooms/:id/chases',auth,adminAuth,async(req,res)=>{try{
  const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);const teamId=Number(req.body.team_id),monsterId=Number(req.body.monster_id);const team=await get(`SELECT * FROM room_teams WHERE id=? AND room_id=?`,[teamId,roomId]);const monster=await get(`SELECT rm.*,mt.name FROM room_monsters rm JOIN monster_templates mt ON mt.id=rm.monster_template_id WHERE rm.id=? AND rm.room_id=?`,[monsterId,roomId]);if(!team||!monster)return res.status(404).json({error:'找不到隊伍或怪物'});if(monster.map_node_id&&team.current_map_node_id&&String(monster.map_node_id)!==String(team.current_map_node_id))return res.status(400).json({error:'怪物與隊伍目前不在同一地點，請先用警戒 AI 追上或移動位置'});await run(`UPDATE room_chases SET status='ended',updated_at=CURRENT_TIMESTAMP WHERE room_id=? AND team_id=? AND status='active'`,[roomId,teamId]);const row=await get(`INSERT INTO room_chases(room_id,team_id,monster_id,distance,escape_distance,log) VALUES(?,?,?,?,?,?::jsonb) RETURNING *`,[roomId,teamId,monsterId,clampInt(req.body.distance,1,5,3),clampInt(req.body.escape_distance,4,12,6),JSON.stringify([`追逐開始：${monster.name}`])]);await addEvent(roomId,req.user.id,'chase',{text:`🏃 隊伍「${team.name}」遭到「${monster.name}」追逐！`,chase_id:row.id});emitTeamUpdate(roomId,teamId);res.json({ok:true,chase:row});
}catch(error){res.status(error.status||500).json({error:error.message||'開始追逐失敗'});}});

app.post('/api/rooms/:id/chases/:chaseId/action',auth,async(req,res)=>{try{
  const roomId=Number(req.params.id),chaseId=Number(req.params.chaseId);const chase=await get(`SELECT rc.*,rt.name AS team_name,rm.monster_template_id,mt.name AS monster_name,mt.attributes,mt.effects,mt.element_affinity FROM room_chases rc JOIN room_teams rt ON rt.id=rc.team_id JOIN room_monsters rm ON rm.id=rc.monster_id JOIN monster_templates mt ON mt.id=rm.monster_template_id WHERE rc.id=? AND rc.room_id=?`,[chaseId,roomId]);if(!chase||chase.status!=='active')return res.status(404).json({error:'目前沒有這場有效追逐'});const user=await getUser(req.user.id);if(!(user?.is_admin))await requireTeamMemberUser(req.user.id,chase.team_id);const ch=await getCharacterByUser(req.user.id);if(!ch&&!user?.is_admin)return res.status(404).json({error:'找不到角色卡'});
  const action=['sprint','obstacle','hide'].includes(String(req.body.action||''))?String(req.body.action):'sprint';let target=0,roll=0,success=false,delta=0,label='';if(user?.is_admin){success=true;delta=1;label='DM 推進';}else{const attr=action==='obstacle'?'strength':'agility';target=Math.max(5,Math.min(95,Math.floor(Math.sqrt(Math.max(0,Number(ch[`final_${attr}`]??ch[attr])||0))*10)+25));roll=1+Math.floor(Math.random()*100);success=roll<=target;if(action==='sprint'){delta=success?2:0;label='衝刺';}else if(action==='obstacle'){delta=success?2:-1;label='製造障礙';}else{delta=success&&Number(chase.distance)>=3?chase.escape_distance:0;label='躲藏';}}
  const monsterEntity=calculateEntityEffectStats(parseJsonField(chase.attributes,{}),parseJsonField(chase.element_affinity,{}),parseJsonField(chase.effects,{}));const mAgi=Math.max(0,Number(monsterEntity.final_attributes?.agility)||0);const mTarget=Math.max(5,Math.min(95,Math.floor(Math.sqrt(mAgi)*10)+20));const mRoll=1+Math.floor(Math.random()*100);const monsterSuccess=mRoll<=mTarget;let distance=clampInt(chase.distance,0,99,3)+delta-(monsterSuccess?2:1);distance=Math.max(0,distance);let status='active';if(distance>=Number(chase.escape_distance))status='escaped';if(distance<=0)status='caught';const actorName=user?.is_admin?'DM':ch?.name||user?.username||'玩家';const text=`${actorName} ${label}${user?.is_admin?'':`：${roll}/${target} ${success?'成功':'失敗'}`}；${chase.monster_name} 追擊 ${mRoll}/${mTarget} ${monsterSuccess?'成功':'失敗'} → 距離 ${distance}/${chase.escape_distance}`;const log=[...parseJsonField(chase.log,[]),text].slice(-50);await run(`UPDATE room_chases SET distance=?,status=?,round=round+1,last_actor_user_id=?,log=?::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[distance,status,req.user.id,JSON.stringify(log),chaseId]);await addEvent(roomId,req.user.id,'chase',{text:`🏃 ${text}${status==='escaped'?'｜✅ 成功脫離追逐':status==='caught'?'｜⚠️ 被追上了':''}`,chase_id:chaseId,status,distance});emitTeamUpdate(roomId,chase.team_id);const row=await get(`SELECT * FROM room_chases WHERE id=?`,[chaseId]);res.json({ok:true,chase:{...row,log:parseJsonField(row.log,[])}});
}catch(error){res.status(error.status||500).json({error:error.message||'追逐行動失敗'});}});
app.post('/api/admin/rooms/:id/chases/:chaseId/stop',auth,adminAuth,async(req,res)=>{try{const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);await run(`UPDATE room_chases SET status='ended',updated_at=CURRENT_TIMESTAMP WHERE id=? AND room_id=?`,[req.params.chaseId,roomId]);res.json({ok:true});}catch(error){res.status(error.status||500).json({error:error.message||'結束追逐失敗'});}});

app.post('/api/admin/rooms/:id/monsters/:monsterId/boss-next',auth,adminAuth,async(req,res)=>{try{
  const roomId=Number(req.params.id),monsterId=Number(req.params.monsterId);await requireRoomAdmin(req.user.id,roomId);const row=await get(`SELECT rm.*,mt.config FROM room_monsters rm JOIN monster_templates mt ON mt.id=rm.monster_template_id WHERE rm.id=? AND rm.room_id=?`,[monsterId,roomId]);if(!row)return res.status(404).json({error:'找不到怪物'});const phases=normalizeBossPhases(parseWorldConfig(row.config).boss_phases);const next=(Number(row.boss_phase_index)||0)+1;if(next>phases.length)return res.status(400).json({error:'已經沒有下一個 Boss 階段'});await activateMonsterBossPhase(roomId,monsterId,next,phases[next-1],'manual');res.json({ok:true,phase:phases[next-1]});
}catch(error){res.status(error.status||500).json({error:error.message||'切換 Boss 階段失敗'});}});


/* =========================================================
   V25: DM 控制台 / 搜索掉落 / NPC 記憶 / 存檔回溯
========================================================= */
function normalizeLootEntries(value){const raw=Array.isArray(value)?value:parseJsonField(value,[]);return raw.map(item=>({name:String(item?.name||'').trim().slice(0,120),category:String(item?.category||'').trim().slice(0,80),quantity:Math.max(1,Math.floor(Number(item?.quantity)||1)),chance:Math.max(0,Math.min(100,Number(item?.chance)===0?0:(Number(item?.chance)||100)))})).filter(item=>item.name).slice(0,100);}
function parsedSearchContainer(row){return row?{...row,loot:normalizeLootEntries(row.loot)}:null;}
async function v25ViewerNode(userId,roomId){const team=await getUserRoomTeam(userId,roomId);if(team?.current_map_node_id)return Number(team.current_map_node_id);const room=await get(`SELECT current_map_node_id FROM rooms WHERE id=?`,[roomId]);return room?.current_map_node_id?Number(room.current_map_node_id):null;}

app.get('/api/rooms/:id/containers',auth,async(req,res)=>{try{const roomId=Number(req.params.id);const user=await getUser(req.user.id);const isAdmin=Boolean(user?.is_admin);if(!isAdmin)await requireRoomMember(req.user.id,roomId);const nodeId=isAdmin?null:await v25ViewerNode(req.user.id,roomId);const rows=isAdmin?await all(`SELECT sc.*,mn.name AS map_node_name FROM search_containers sc LEFT JOIN room_map_nodes mn ON mn.id=sc.map_node_id WHERE sc.room_id=? AND sc.active=TRUE ORDER BY sc.id DESC`,[roomId]):await all(`SELECT sc.*,mn.name AS map_node_name,sr.claimed,sr.success,sr.last_roll,sr.last_target,sr.discovered FROM search_containers sc LEFT JOIN room_map_nodes mn ON mn.id=sc.map_node_id LEFT JOIN search_records sr ON sr.container_id=sc.id AND sr.user_id=? WHERE sc.room_id=? AND sc.active=TRUE AND (sc.hidden=FALSE OR COALESCE(sr.discovered,FALSE)=TRUE) AND (sc.map_node_id IS NULL OR sc.map_node_id=?) ORDER BY sc.id DESC`,[req.user.id,roomId,nodeId]);res.json({containers:rows.map(parsedSearchContainer)});}catch(error){res.status(error.status||500).json({error:error.message||'讀取可搜索容器失敗'});}});
app.post('/api/admin/rooms/:id/containers',auth,adminAuth,async(req,res)=>{try{const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);const name=String(req.body.name||'').trim().slice(0,160);if(!name)return res.status(400).json({error:'請輸入容器名稱'});const attr=['strength','agility','spirit','luck','fifth'].includes(String(req.body.search_attribute||''))?String(req.body.search_attribute):'spirit';const nodeId=await validRoomMapNodeId(roomId,req.body.map_node_id);const row=await get(`INSERT INTO search_containers(room_id,map_node_id,name,container_type,description,hidden,search_attribute,search_dc,required_item,one_time,loot,active) VALUES(?,?,?,?,?,?,?,?,?,?,?::jsonb,TRUE) RETURNING *`,[roomId,nodeId,name,String(req.body.container_type||'容器').slice(0,60),String(req.body.description||'').slice(0,6000),Boolean(req.body.hidden),attr,Math.max(1,Math.min(100,Number(req.body.search_dc)||50)),String(req.body.required_item||'').trim().slice(0,120),req.body.one_time===undefined?true:Boolean(req.body.one_time),JSON.stringify(normalizeLootEntries(req.body.loot))]);await addEvent(roomId,req.user.id,'dm_world',{text:`📦 DM 放置了可搜索物「${name}」。`,container_id:row.id});res.json({ok:true,container:parsedSearchContainer(row)});}catch(error){res.status(error.status||500).json({error:error.message||'建立搜索容器失敗'});}});
app.delete('/api/admin/rooms/:id/containers/:containerId',auth,adminAuth,async(req,res)=>{try{const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);await run(`DELETE FROM search_containers WHERE id=? AND room_id=?`,[req.params.containerId,roomId]);res.json({ok:true});}catch(error){res.status(error.status||500).json({error:error.message||'刪除容器失敗'});}});
app.post('/api/rooms/:id/containers/:containerId/search',auth,async(req,res)=>{try{const roomId=Number(req.params.id);await requireRoomMember(req.user.id,roomId);const row=await get(`SELECT * FROM search_containers WHERE id=? AND room_id=? AND active=TRUE`,[req.params.containerId,roomId]);if(!row)return res.status(404).json({error:'找不到可搜索物'});const nodeId=await v25ViewerNode(req.user.id,roomId);if(row.map_node_id&&String(row.map_node_id)!==String(nodeId||''))return res.status(403).json({error:'你不在這個可搜索物所在的地點'});if(row.hidden){const dr=await get(`SELECT discovered FROM search_records WHERE container_id=? AND user_id=?`,[row.id,req.user.id]);if(!dr?.discovered)return res.status(403).json({error:'你目前還沒有發現這個隱藏搜索點'});}const record=await get(`SELECT * FROM search_records WHERE container_id=? AND user_id=?`,[row.id,req.user.id]);if(row.one_time&&record?.claimed)return res.status(400).json({error:'你已經搜索並取走這裡的東西'});const character=await requireCharacter(req.user.id);const required=String(row.required_item||'').trim();if(required&&inventoryQuantity(character.inventory,required)<=0)return res.status(400).json({error:`需要持有「${required}」才能搜索`});const attr=String(row.search_attribute||'spirit');let value=0;if(attr==='strength')value=Number(character.final_strength??character.strength)||0;else if(attr==='agility')value=Number(character.final_agility??character.agility)||0;else if(attr==='luck')value=Number(character.final_luck??character.luck)||0;else if(attr==='fifth')value=Number(character.fifth_current)||0;else value=Number(character.final_spirit??character.spirit)||0;const target=Math.max(5,Math.min(95,Math.floor(Math.sqrt(Math.max(0,value))*10)+25-(Math.max(1,Number(row.search_dc)||50)-50)));const roll=1+Math.floor(Math.random()*100);const success=roll<=target;const found=[];if(success){let inventory=normalizeInventory(character.inventory);for(const loot of normalizeLootEntries(row.loot)){if(Math.random()*100<=loot.chance){ensureInventoryCanAdd(character,inventory,loot.name,loot.category);inventory=changeInventoryQuantity(inventory,loot.name,loot.quantity,loot.category);found.push(loot);}}await run(`UPDATE character_cards SET inventory=?::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=?`,[JSON.stringify(inventory),req.user.id]);}await run(`INSERT INTO search_records(container_id,user_id,attempts,success,claimed,last_roll,last_target,found) VALUES(?,?,1,?,?,?,?,?::jsonb) ON CONFLICT(container_id,user_id) DO UPDATE SET attempts=search_records.attempts+1,success=(search_records.success OR EXCLUDED.success),claimed=(search_records.claimed OR EXCLUDED.claimed),last_roll=EXCLUDED.last_roll,last_target=EXCLUDED.last_target,found=CASE WHEN EXCLUDED.claimed THEN EXCLUDED.found ELSE search_records.found END,updated_at=CURRENT_TIMESTAMP`,[row.id,req.user.id,success,Boolean(success&&row.one_time),roll,target,JSON.stringify(found)]);const ch=await getCharacterByUser(req.user.id);let resultText=`🔎 ${ch?.name||'玩家'} 搜索「${row.name}」：${success?'成功':'失敗'}`;if(success)resultText+=found.length?`，找到 ${found.map(x=>`${x.name}×${x.quantity}`).join('、')}`:'，沒有找到可帶走的東西';await addEvent(roomId,req.user.id,'search',{text:resultText,container_id:row.id,success,found});res.json({ok:true,success,found,character:ch});}catch(error){console.error('SEARCH CONTAINER ERROR',error);res.status(error.status||500).json({error:error.message||'搜索失敗'});}});

app.get('/api/rooms/:id/npc-relations',auth,async(req,res)=>{try{const roomId=Number(req.params.id);const user=await getUser(req.user.id);const isAdmin=Boolean(user?.is_admin);if(!isAdmin)await requireRoomMember(req.user.id,roomId);const rows=isAdmin?await all(`SELECT nr.*,nt.name AS npc_name,u.username,c.name AS character_name FROM npc_relationships nr JOIN npc_templates nt ON nt.id=nr.npc_template_id JOIN users u ON u.id=nr.user_id LEFT JOIN character_cards c ON c.user_id=nr.user_id WHERE nr.room_id=? ORDER BY nt.name,c.name`,[roomId]):await all(`SELECT nr.*,nt.name AS npc_name FROM npc_relationships nr JOIN npc_templates nt ON nt.id=nr.npc_template_id WHERE nr.room_id=? AND nr.user_id=? ORDER BY nt.name`,[roomId,req.user.id]);res.json({relations:rows.map(r=>({...r,memories:parseJsonField(r.memories,[])}))});}catch(error){res.status(error.status||500).json({error:error.message||'讀取 NPC 關係失敗'});}});
app.patch('/api/admin/rooms/:id/npc-relations',auth,adminAuth,async(req,res)=>{try{const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);const npcId=Number(req.body.npc_template_id),userId=Number(req.body.user_id);if(!npcId||!userId)return res.status(400).json({error:'請選擇 NPC 與玩家'});const npc=await get(`SELECT id,name FROM npc_templates WHERE id=?`,[npcId]);const player=await getUser(userId);if(!npc||!player)return res.status(404).json({error:'找不到 NPC 或玩家'});const old=await get(`SELECT * FROM npc_relationships WHERE room_id=? AND npc_template_id=? AND user_id=?`,[roomId,npcId,userId]);const memories=parseJsonField(old?.memories,[]);const memory=String(req.body.memory||'').trim();if(memory)memories.push({text:memory.slice(0,1000),at:new Date().toISOString()});const affinity=Math.max(-100,Math.min(100,Number(req.body.affinity??old?.affinity)||0)),trust=Math.max(-100,Math.min(100,Number(req.body.trust??old?.trust)||0)),fear=Math.max(0,Math.min(100,Number(req.body.fear??old?.fear)||0)),hostility=Math.max(0,Math.min(100,Number(req.body.hostility??old?.hostility)||0));const row=await get(`INSERT INTO npc_relationships(room_id,npc_template_id,user_id,affinity,trust,fear,hostility,memories) VALUES(?,?,?,?,?,?,?,?::jsonb) ON CONFLICT(room_id,npc_template_id,user_id) DO UPDATE SET affinity=EXCLUDED.affinity,trust=EXCLUDED.trust,fear=EXCLUDED.fear,hostility=EXCLUDED.hostility,memories=EXCLUDED.memories,updated_at=CURRENT_TIMESTAMP RETURNING *`,[roomId,npcId,userId,affinity,trust,fear,hostility,JSON.stringify(memories.slice(-100))]);await addEvent(roomId,req.user.id,'npc_memory',{text:`🧠 DM 更新了 NPC「${npc.name}」對 ${player.username} 的關係／記憶。`,npc_template_id:npcId,target_user_id:userId});res.json({ok:true,relation:{...row,memories:parseJsonField(row.memories,[])}});}catch(error){res.status(error.status||500).json({error:error.message||'更新 NPC 關係失敗'});}});

async function v25ControlTarget(roomId,userId){const member=await get(`SELECT * FROM room_members WHERE room_id=? AND user_id=?`,[roomId,userId]);if(!member)throw Object.assign(new Error('這個玩家不在指定房間'),{status:404});const character=await getCharacterByUser(userId);if(!character)throw Object.assign(new Error('找不到玩家角色卡'),{status:404});return {member,character};}
app.post('/api/admin/rooms/:id/control',auth,adminAuth,async(req,res)=>{try{const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);const userId=Number(req.body.user_id);const action=String(req.body.action||'');const {member,character}=await v25ControlTarget(roomId,userId);let text='';if(action==='hp'){const next=Math.max(0,Math.min(Number(member.max_hp)||0,(Number(member.hp)||0)+(Number(req.body.amount)||0)));await run(`UPDATE room_members SET hp=?,combat_state=CASE WHEN ?>0 THEN 'active' ELSE 'downed' END WHERE room_id=? AND user_id=?`,[next,next,roomId,userId]);text=`HP → ${next}/${Number(member.max_hp)||0}`;}else if(action==='sanity'){const sanity=await ensureRoomMemberSanity(roomId,userId);const max=Math.max(0,Number(sanity?.max_sanity)||0),before=Math.max(0,Number(sanity?.current_sanity)||0),next=Math.max(0,Math.min(max,before+(Number(req.body.amount)||0)));const oldStage=sanityStage(before,max),newStage=sanityStage(next,max);let sin=sanity?.seven_sin||null;if((before-next)>=max*0.25&&max>0&&!sin)sin=SEVEN_SINS[Math.floor(Math.random()*SEVEN_SINS.length)];await run(`UPDATE room_members SET current_sanity=?,sanity_stage=?,seven_sin=? WHERE room_id=? AND user_id=?`,[next,newStage.key,sin,roomId,userId]);if((SANITY_STAGE_ORDER[newStage.key]||0)>(SANITY_STAGE_ORDER[oldStage.key]||0))await createSanityStageVision(roomId,userId,newStage.key);text=`理智 → ${next}/${max}`;}else if(action==='fifth'){const field=character.faction==='西國'?'current_faith':'current_great_way',max=Math.max(0,Number(character.faction==='西國'?character.faith:character.great_way)||0),cur=Math.max(0,Number(character[field])||0),next=Math.max(0,Math.min(max,cur+(Number(req.body.amount)||0)));await run(`UPDATE character_cards SET ${field}=?,updated_at=CURRENT_TIMESTAMP WHERE user_id=?`,[next,userId]);text=`${character.faction==='西國'?'信仰':'大道'} → ${next}/${max}`;}else if(action==='pollution'){const next=Math.max(0,Math.min(100,(Number(character.pollution)||0)+(Number(req.body.amount)||0)));await run(`UPDATE character_cards SET pollution=?,updated_at=CURRENT_TIMESTAMP WHERE user_id=?`,[next,userId]);text=`污染 → ${next}%`;}else if(action==='weird_coin'||action==='game_coin'){const field=action==='weird_coin'?'weird_coins':'game_coins',next=Math.max(0,(Number(character[field])||0)+(Number(req.body.amount)||0));await run(`UPDATE character_cards SET ${field}=?,updated_at=CURRENT_TIMESTAMP WHERE user_id=?`,[next,userId]);text=`${action==='weird_coin'?'詭幣':'遊戲幣'} → ${next}`;}else if(action==='item'){const name=String(req.body.item_name||'').trim();if(!name)return res.status(400).json({error:'請輸入道具名稱'});const qty=Math.max(1,Math.floor(Number(req.body.quantity)||1));let inventory=normalizeInventory(character.inventory);ensureInventoryCanAdd(character,inventory,name,String(req.body.category||''));inventory=changeInventoryQuantity(inventory,name,qty,String(req.body.category||''));await run(`UPDATE character_cards SET inventory=?::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=?`,[JSON.stringify(inventory),userId]);text=`獲得「${name}」×${qty}`;}else if(action==='status'){const template=await get(`SELECT * FROM status_templates WHERE id=?`,[req.body.status_template_id]);if(!template)return res.status(404).json({error:'找不到狀態模板'});const client=await pool.connect();try{await client.query('BEGIN');await applyStatusTemplateWithClient(client,userId,template,'DM 控制台',req.body.duration_rounds===undefined?null:Number(req.body.duration_rounds));await client.query('COMMIT');}catch(e){await client.query('ROLLBACK');throw e;}finally{client.release();}text=`套用狀態「${template.name}」`;}else return res.status(400).json({error:'未知的 DM 控制動作'});const target=await getCharacterByUser(userId);await addEvent(roomId,req.user.id,'dm_control',{text:`🎛️ DM 對「${target?.name||character.name}」執行：${text}`,target_user_id:userId,action});const snapshot=await roomSnapshot(roomId);io.to('room:'+roomId).emit('room:snapshot',snapshot);res.json({ok:true,text,character:target,room:snapshot});}catch(error){console.error('DM CONTROL ERROR',error);res.status(error.status||500).json({error:error.message||'DM 控制操作失敗'});}});

async function buildRoomCheckpoint(roomId){const room=await get(`SELECT * FROM rooms WHERE id=?`,[roomId]);if(!room)throw Object.assign(new Error('找不到房間'),{status:404});const members=await all(`SELECT * FROM room_members WHERE room_id=? ORDER BY user_id`,[roomId]);const characters=await all(`SELECT c.* FROM character_cards c JOIN room_members rm ON rm.user_id=c.user_id WHERE rm.room_id=? ORDER BY c.user_id`,[roomId]);const map_nodes=await all(`SELECT * FROM room_map_nodes WHERE room_id=? ORDER BY id`,[roomId]);const teams=await all(`SELECT * FROM room_teams WHERE room_id=? ORDER BY id`,[roomId]);const monsters=await all(`SELECT * FROM room_monsters WHERE room_id=? ORDER BY id`,[roomId]);const npcs=await all(`SELECT * FROM room_npcs WHERE room_id=? ORDER BY npc_template_id`,[roomId]);const dungeons=await all(`SELECT * FROM room_dungeons WHERE room_id=? ORDER BY dungeon_template_id`,[roomId]);const summons=await all(`SELECT * FROM summon_instances WHERE room_id=? ORDER BY id`,[roomId]);const userIds=members.map(m=>Number(m.user_id)).filter(Boolean);const statuses=userIds.length?await all(`SELECT * FROM character_statuses WHERE user_id=ANY(?::bigint[]) ORDER BY id`,[userIds]):[];const combat_statuses=await all(`SELECT * FROM combat_entity_statuses WHERE room_id=? ORDER BY id`,[roomId]);const relations=await all(`SELECT * FROM npc_relationships WHERE room_id=? ORDER BY npc_template_id,user_id`,[roomId]);const containers=await all(`SELECT * FROM search_containers WHERE room_id=? ORDER BY id`,[roomId]);const scheduled=await all(`SELECT * FROM scheduled_world_events WHERE room_id=? ORDER BY id`,[roomId]);const chases=await all(`SELECT * FROM room_chases WHERE room_id=? ORDER BY id`,[roomId]);const map_discoveries=await all(`SELECT md.* FROM map_node_discoveries md JOIN room_map_nodes n ON n.id=md.node_id WHERE n.room_id=?`,[roomId]);const npc_discoveries=await all(`SELECT * FROM npc_discoveries WHERE room_id=?`,[roomId]);const clue_combinations=await all(`SELECT * FROM clue_combinations WHERE room_id=?`,[roomId]);const clue_unlocks=await all(`SELECT ccu.* FROM clue_combination_unlocks ccu JOIN clue_combinations cc ON cc.id=ccu.combination_id WHERE cc.room_id=?`,[roomId]);const great_way_assignments=userIds.length?await all(`SELECT * FROM character_great_ways WHERE user_id=ANY(?::bigint[]) ORDER BY user_id,great_way_id`,[userIds]):[];const faith_assignments=userIds.length?await all(`SELECT * FROM character_faiths WHERE user_id=ANY(?::bigint[]) ORDER BY user_id,deity_id`,[userIds]):[];return {version:29,created_at:new Date().toISOString(),room,members,characters,map_nodes,teams,monsters,npcs,dungeons,summons,statuses,combat_statuses,relations,containers,scheduled,chases,map_discoveries,npc_discoveries,clue_combinations,clue_unlocks,great_way_assignments,faith_assignments};}
async function restoreSnapshotRow(client,table,keys,row,fields,jsonFields=[]){if(!row)return;const vals=[];const sets=[];const jsonSet=new Set(jsonFields);for(const field of fields){if(!Object.prototype.hasOwnProperty.call(row,field))continue;vals.push(jsonSet.has(field)?JSON.stringify(row[field]??{}):row[field]);sets.push(`${field}=$${vals.length}${jsonSet.has(field)?'::jsonb':''}`);}if(!sets.length)return;const cond=[];for(const [field,value] of Object.entries(keys)){vals.push(value);cond.push(`${field}=$${vals.length}`);}await client.query(`UPDATE ${table} SET ${sets.join(',')} WHERE ${cond.join(' AND ')}`,vals);}
async function restoreRoomCheckpoint(roomId,snapshot){const client=await pool.connect();try{await client.query('BEGIN');await restoreSnapshotRow(client,'rooms',{id:roomId},snapshot.room,['status','round','current_actor_id','saved','closed_at','updated_at','map_name','map_image_url','map_description','game_day','game_hour','game_minute','time_hidden','time_distortion_offset_minutes','time_rate','time_flow_offset_minutes','battle_active','current_actor_type','current_actor_ref_id','combat_order','combat_index','current_map_node_id'],['combat_order']);for(const m of snapshot.members||[])await restoreSnapshotRow(client,'room_members',{room_id:roomId,user_id:m.user_id},m,['display_name','role','hp','max_hp','spirit','max_spirit','turn_actions_remaining','skill_charge_state','mounted_summon_id','combat_reaction','combat_reaction_skill_id','combat_reaction_round','combat_reaction_uses','combat_state','current_sanity','max_sanity','sanity_initialized','sanity_stage','seven_sin'],['skill_charge_state']);const charFields=['name','faction','tier','level','main_class','sub_classes','agility','strength','constitution','spirit','great_way','great_way_name','luck','attribute_points','experience','skill_points','skills','equipment','weapons','affinity_light','affinity_dark','affinity_metal','affinity_wood','affinity_water','affinity_fire','affinity_earth','class_resources','faiths','great_ways','faith','current_great_way','current_faith','profession_rank','innate_sub_class','weird_hatred','inventory','weird_coins','game_coins','game_coin_unlocked','profession_progress','inventory_slots','bloodline','bloodline_effects','profession_effects','great_way_effects','faith_effects','self_intro','game_room_unlocked','name_locked','backpack_bonus_applied','gear_loadout','pollution'];const charJson=['sub_classes','skills','equipment','weapons','class_resources','faiths','great_ways','inventory','profession_progress','bloodline_effects','profession_effects','great_way_effects','faith_effects','gear_loadout'];for(const c of snapshot.characters||[])await restoreSnapshotRow(client,'character_cards',{user_id:c.user_id},c,charFields,charJson);for(const n of snapshot.map_nodes||[])await restoreSnapshotRow(client,'room_map_nodes',{id:n.id,room_id:roomId},n,['name','description','image_url','connections','hidden','noise_level','alert_level','noise_threshold','time_offset_minutes','time_rate','time_flow_offset_minutes','investigation_dc','discovery_hint'],['connections']);for(const t of snapshot.teams||[])await restoreSnapshotRow(client,'room_teams',{id:t.id,room_id:roomId},t,['name','leader_user_id','current_map_node_id']);for(const m of snapshot.monsters||[])await restoreSnapshotRow(client,'room_monsters',{id:m.id,room_id:roomId},m,['status','current_hp','max_hp','state','turn_actions_remaining','skill_charge_state','current_spirit','max_spirit','current_fifth','max_fifth','map_node_id','alert_level','chase_team_id','boss_phase_index','boss_phase_name'],['state','skill_charge_state']);for(const n of snapshot.npcs||[])await restoreSnapshotRow(client,'room_npcs',{room_id:roomId,npc_template_id:n.npc_template_id},n,['status','state','combat_side','current_hp','max_hp','current_spirit','max_spirit','current_fifth','max_fifth','turn_actions_remaining','skill_charge_state','map_node_id'],['state','skill_charge_state']);for(const d of snapshot.dungeons||[])await restoreSnapshotRow(client,'room_dungeons',{room_id:roomId,dungeon_template_id:d.dungeon_template_id},d,['status','state','map_node_id'],['state']);for(const si of snapshot.summons||[])await restoreSnapshotRow(client,'summon_instances',{id:si.id},si,['room_id','status','overrides','current_state','name'],['overrides','current_state']);for(const st of snapshot.statuses||[])await restoreSnapshotRow(client,'character_statuses',{id:st.id},st,['remaining_rounds','effects','active','stacks','stack_mode','max_stacks','dispellable','expires_by_round','description','source'],['effects']);for(const st of snapshot.combat_statuses||[])await restoreSnapshotRow(client,'combat_entity_statuses',{id:st.id,room_id:roomId},st,['remaining_rounds','effects','active','stacks','stack_mode','max_stacks','dispellable','expires_by_round','description','source'],['effects']);for(const r of snapshot.relations||[])await restoreSnapshotRow(client,'npc_relationships',{room_id:roomId,npc_template_id:r.npc_template_id,user_id:r.user_id},r,['affinity','trust','fear','hostility','memories'],['memories']);for(const c of snapshot.containers||[])await restoreSnapshotRow(client,'search_containers',{id:c.id,room_id:roomId},c,['map_node_id','name','container_type','description','hidden','search_attribute','search_dc','required_item','one_time','loot','active'],['loot']);for(const e of snapshot.scheduled||[])await restoreSnapshotRow(client,'scheduled_world_events',{id:e.id,room_id:roomId},e,['name','trigger_day','trigger_minute','repeat_mode','audience','target_team_id','target_user_id','map_node_id','content','active','trigger_count','last_trigger_total','time_basis','action_type','action_payload'],['action_payload']);for(const c of snapshot.chases||[])await restoreSnapshotRow(client,'room_chases',{id:c.id,room_id:roomId},c,['team_id','monster_id','status','distance','escape_distance','round','last_actor_user_id','log'],['log']);
for(const cc of snapshot.clue_combinations||[])await restoreSnapshotRow(client,'clue_combinations',{id:cc.id,room_id:roomId},cc,['name','required_clue_ids','result_type','result_id','result_title','result_content','active'],['required_clue_ids']);
await client.query(`DELETE FROM map_node_discoveries WHERE node_id IN (SELECT id FROM room_map_nodes WHERE room_id=$1)`,[roomId]);
for(const d of snapshot.map_discoveries||[])await client.query(`INSERT INTO map_node_discoveries(node_id,user_id,discovered_at) VALUES($1,$2,COALESCE($3,CURRENT_TIMESTAMP)) ON CONFLICT(node_id,user_id) DO NOTHING`,[d.node_id,d.user_id,d.discovered_at]);
await client.query(`DELETE FROM npc_discoveries WHERE room_id=$1`,[roomId]);
for(const d of snapshot.npc_discoveries||[])await client.query(`INSERT INTO npc_discoveries(room_id,npc_template_id,user_id,investigated,last_roll,last_rate,updated_at) VALUES($1,$2,$3,$4,$5,$6,COALESCE($7,CURRENT_TIMESTAMP)) ON CONFLICT(room_id,npc_template_id,user_id) DO UPDATE SET investigated=EXCLUDED.investigated,last_roll=EXCLUDED.last_roll,last_rate=EXCLUDED.last_rate,updated_at=EXCLUDED.updated_at`,[roomId,d.npc_template_id,d.user_id,Boolean(d.investigated),d.last_roll,d.last_rate,d.updated_at]);
await client.query(`DELETE FROM clue_combination_unlocks WHERE combination_id IN (SELECT id FROM clue_combinations WHERE room_id=$1)`,[roomId]);
for(const d of snapshot.clue_unlocks||[])await client.query(`INSERT INTO clue_combination_unlocks(combination_id,user_id,unlocked_at) VALUES($1,$2,COALESCE($3,CURRENT_TIMESTAMP)) ON CONFLICT(combination_id,user_id) DO NOTHING`,[d.combination_id,d.user_id,d.unlocked_at]);
const identityUserIds=(snapshot.members||[]).map(m=>Number(m.user_id)).filter(Boolean);if(identityUserIds.length){await client.query(`DELETE FROM character_great_ways WHERE user_id=ANY($1::bigint[])`,[identityUserIds]);for(const x of snapshot.great_way_assignments||[])await client.query(`INSERT INTO character_great_ways(user_id,great_way_id,rank,progress,active,created_at,updated_at) VALUES($1,$2,$3,$4,$5,COALESCE($6,CURRENT_TIMESTAMP),COALESCE($7,CURRENT_TIMESTAMP)) ON CONFLICT(user_id,great_way_id) DO UPDATE SET rank=EXCLUDED.rank,progress=EXCLUDED.progress,active=EXCLUDED.active,updated_at=EXCLUDED.updated_at`,[x.user_id,x.great_way_id,x.rank,x.progress,Boolean(x.active),x.created_at,x.updated_at]);await client.query(`DELETE FROM character_faiths WHERE user_id=ANY($1::bigint[])`,[identityUserIds]);for(const x of snapshot.faith_assignments||[])await client.query(`INSERT INTO character_faiths(user_id,deity_id,rank,devotion,favor,active,created_at,updated_at) VALUES($1,$2,$3,$4,$5,$6,COALESCE($7,CURRENT_TIMESTAMP),COALESCE($8,CURRENT_TIMESTAMP)) ON CONFLICT(user_id,deity_id) DO UPDATE SET rank=EXCLUDED.rank,devotion=EXCLUDED.devotion,favor=EXCLUDED.favor,active=EXCLUDED.active,updated_at=EXCLUDED.updated_at`,[x.user_id,x.deity_id,x.rank,x.devotion,x.favor,Boolean(x.active),x.created_at,x.updated_at]);}
await client.query('COMMIT');}catch(error){await client.query('ROLLBACK');throw error;}finally{client.release();}}
app.get('/api/admin/rooms/:id/savepoints',auth,adminAuth,async(req,res)=>{try{const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);const rows=await all(`SELECT rs.id,rs.room_id,rs.label,rs.created_by,rs.created_at,u.username AS created_by_username FROM room_savepoints rs LEFT JOIN users u ON u.id=rs.created_by WHERE rs.room_id=? ORDER BY rs.id DESC LIMIT 30`,[roomId]);res.json({savepoints:rows});}catch(error){res.status(error.status||500).json({error:error.message||'讀取存檔失敗'});}});
app.post('/api/admin/rooms/:id/savepoints',auth,adminAuth,async(req,res)=>{try{const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);const snap=await buildRoomCheckpoint(roomId);const label=String(req.body.label||'手動存檔').trim().slice(0,180)||'手動存檔';const row=await get(`INSERT INTO room_savepoints(room_id,label,snapshot,created_by) VALUES(?,?,?::jsonb,?) RETURNING id,room_id,label,created_by,created_at`,[roomId,label,JSON.stringify(snap),req.user.id]);await addEvent(roomId,req.user.id,'savepoint',{text:`💾 DM 建立存檔「${label}」`,savepoint_id:row.id});res.json({ok:true,savepoint:row});}catch(error){console.error(error);res.status(error.status||500).json({error:error.message||'建立存檔失敗'});}});
app.post('/api/admin/rooms/:id/savepoints/:savepointId/restore',auth,adminAuth,async(req,res)=>{try{const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);const row=await get(`SELECT * FROM room_savepoints WHERE id=? AND room_id=?`,[req.params.savepointId,roomId]);if(!row)return res.status(404).json({error:'找不到存檔'});const safety=await buildRoomCheckpoint(roomId);await run(`INSERT INTO room_savepoints(room_id,label,snapshot,created_by) VALUES(?,?,?::jsonb,?)`,[roomId,`回溯前自動備份 ${new Date().toISOString().slice(0,16).replace('T',' ')}`,JSON.stringify(safety),req.user.id]);await restoreRoomCheckpoint(roomId,parseJsonField(row.snapshot,{}));await addEvent(roomId,req.user.id,'restore',{text:`⏪ DM 回溯到存檔「${row.label}」`,savepoint_id:row.id});const snapshot=await roomSnapshot(roomId);io.to('room:'+roomId).emit('room:snapshot',snapshot);emitTeamUpdate(roomId,null);res.json({ok:true,room:snapshot});}catch(error){console.error(error);res.status(error.status||500).json({error:error.message||'回溯存檔失敗'});}});
app.delete('/api/admin/rooms/:id/savepoints/:savepointId',auth,adminAuth,async(req,res)=>{try{const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);await run(`DELETE FROM room_savepoints WHERE id=? AND room_id=?`,[req.params.savepointId,roomId]);res.json({ok:true});}catch(error){res.status(error.status||500).json({error:error.message||'刪除存檔失敗'});}});
app.get('/api/admin/rooms/:id/timeline',auth,adminAuth,async(req,res)=>{try{const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);const limit=Math.max(20,Math.min(500,Number(req.query.limit)||200));const type=String(req.query.type||'').trim();const rows=type?await all(`SELECT e.*,u.username,c.name AS character_name FROM events e LEFT JOIN users u ON u.id=e.user_id LEFT JOIN character_cards c ON c.user_id=e.user_id WHERE e.room_id=? AND e.type=? ORDER BY e.id DESC LIMIT ?`,[roomId,type,limit]):await all(`SELECT e.*,u.username,c.name AS character_name FROM events e LEFT JOIN users u ON u.id=e.user_id LEFT JOIN character_cards c ON c.user_id=e.user_id WHERE e.room_id=? ORDER BY e.id DESC LIMIT ?`,[roomId,limit]);res.json({events:rows.map(e=>({...e,payload:parseJsonField(e.payload,{})}))});}catch(error){res.status(error.status||500).json({error:error.message||'讀取事件紀錄失敗'});}});



/* =========================================================
   V27 EXPLORATION / KNOWLEDGE / INFERENCE / TIME FLOW
========================================================= */
function v27InvestigationAttributeValue(character,attr){
  if(attr==='agility')return Number(character.final_agility??character.agility)||0;
  if(attr==='luck')return Number(character.final_luck??character.luck)||0;
  if(attr==='fifth')return Number(character.fifth_current??character.current_great_way??character.current_faith)||0;
  return Number(character.final_spirit??character.spirit)||0;
}
function v27InvestigationRate(character,skill,dc=50){
  const cfg=skillCombatConfig(skill);const attr=v27InvestigationAttributeValue(character,cfg.investigation_attribute||'spirit');
  const attrBonus=Math.min(40,Math.floor(Math.sqrt(Math.max(0,attr))*3));
  return Math.max(5,Math.min(95,(Number(cfg.investigation_base_rate)||40)+(Number(cfg.investigation_bonus)||0)+attrBonus-(Math.max(1,Number(dc)||50)-50)));
}
function v27InventoryDisplayEntry(item,template,identified){
  if(!template)return {...item,display_name:item.name,identified:true};
  const needs=Boolean(template.requires_identification);const known=!needs||identified;
  return {...item,display_name:known?item.name:String(template.unknown_name||'未知物品'),identified:known,item_type:known?template.item_type:'unknown',template_description:known?String(template.description||''):'尚未鑑定，效果未知。',requires_identification:needs};
}
async function decorateCharacterInventoryKnowledge(userId,character){
  if(!character)return character;const inventory=normalizeInventory(character.inventory);const names=[...new Set(inventory.map(x=>String(x.name||'').trim()).filter(Boolean))];if(!names.length)return {...character,inventory};
  const templates=await all(`SELECT * FROM item_templates WHERE name=ANY(?::text[])`,[names]);const ids=templates.map(x=>Number(x.id)).filter(Boolean);const known=ids.length?await all(`SELECT item_template_id FROM item_identifications WHERE user_id=? AND item_template_id=ANY(?::bigint[])`,[userId,ids]):[];const set=new Set(known.map(x=>Number(x.item_template_id)));const byName=new Map(templates.map(t=>[String(t.name).toLocaleLowerCase(),t]));
  return {...character,inventory:inventory.map(item=>{const t=byName.get(String(item.name||'').toLocaleLowerCase());return v27InventoryDisplayEntry(item,t,t?set.has(Number(t.id)):true);})};
}
async function v27GrantClue(userId,clue,note=''){if(!clue)return false;await run(`INSERT INTO character_clues(clue_id,user_id,note) VALUES(?,?,?) ON CONFLICT(clue_id,user_id) DO UPDATE SET note=CASE WHEN EXCLUDED.note<>'' THEN EXCLUDED.note ELSE character_clues.note END`,[clue.id,userId,String(note||'').slice(0,1000)]);if(clue.linked_rule_id)await run(`INSERT INTO rule_discoveries(rule_id,user_id,knowledge_level) VALUES(?,?,?) ON CONFLICT(rule_id,user_id) DO UPDATE SET knowledge_level=GREATEST(rule_discoveries.knowledge_level,EXCLUDED.knowledge_level),updated_at=CURRENT_TIMESTAMP`,[clue.linked_rule_id,userId,Math.max(1,Math.min(3,Number(clue.reveal_level)||1))]);return true;}
async function v27KnownClueIds(userId,roomId){const own=await all(`SELECT clue_id FROM character_clues WHERE user_id=?`,[userId]);const team=await getUserRoomTeam(userId,roomId);let shared=[];if(team)shared=await all(`SELECT clue_id FROM team_shared_clues WHERE team_id=?`,[team.id]);return new Set([...own,...shared].map(x=>Number(x.clue_id)));}

app.post('/api/rooms/:id/explore',auth,async(req,res)=>{try{
  const roomId=Number(req.params.id);await requireRoomMember(req.user.id,roomId);const character=await requireCharacter(req.user.id);const skill=(character.skills||[]).find(s=>String(s.id)===String(req.body.skill_id)||String(s.name)===String(req.body.skill_id));if(!skill||!skillCombatConfig(skill).investigation_skill)return res.status(400).json({error:'請選擇探查類技能'});const mode=['environment','npc','item'].includes(String(req.body.mode||''))?String(req.body.mode):'environment';const nodeId=await getViewerMapNodeId(req.user.id,roomId);const node=nodeId?await get(`SELECT * FROM room_map_nodes WHERE id=? AND room_id=?`,[nodeId,roomId]):null;let dc=50,targetName='環境';
  if(mode==='npc'){const npc=await get(`SELECT rn.map_node_id,nt.* FROM room_npcs rn JOIN npc_templates nt ON nt.id=rn.npc_template_id WHERE rn.room_id=? AND rn.npc_template_id=?`,[roomId,Number(req.body.target_id)]);if(!npc)return res.status(404).json({error:'找不到 NPC'});if(npc.map_node_id&&String(npc.map_node_id)!==String(nodeId||''))return res.status(403).json({error:'NPC 不在你目前的位置'});dc=Number(npc.investigation_dc)||50;targetName=npc.name;const rate=v27InvestigationRate(character,skill,dc),roll=1+Math.floor(Math.random()*100),success=roll<=rate;await run(`INSERT INTO npc_discoveries(room_id,npc_template_id,user_id,investigated,last_roll,last_rate) VALUES(?,?,?,?,?,?) ON CONFLICT(room_id,npc_template_id,user_id) DO UPDATE SET investigated=(npc_discoveries.investigated OR EXCLUDED.investigated),last_roll=EXCLUDED.last_roll,last_rate=EXCLUDED.last_rate,updated_at=CURRENT_TIMESTAMP`,[roomId,npc.id,req.user.id,success,roll,rate]);await addEvent(roomId,req.user.id,'investigation',{text:`🔎 ${character.name} 探查「${targetName}」：${success?'成功':'失敗'}（${roll}/${rate}）`,mode,roll,rate,success});return res.json({ok:true,mode,roll,rate,success,found:success?[`已掌握 ${targetName} 的完整情報`]:[]});}
  if(mode==='item'){const itemName=String(req.body.item_name||'').trim();if(!itemName||inventoryQuantity(character.inventory,itemName)<=0)return res.status(400).json({error:'你沒有這個道具'});const template=await get(`SELECT * FROM item_templates WHERE name=?`,[itemName]);if(!template)return res.status(404).json({error:'找不到對應道具模板'});if(!template.requires_identification)return res.json({ok:true,mode,success:true,roll:0,rate:100,found:[`${itemName} 不需要鑑定`]});dc=Number(template.identification_dc)||50;const fakeSkill={...skill,data:{...skillData(skill),investigation_attribute:template.identification_attribute||skillData(skill).investigation_attribute}};const rate=v27InvestigationRate(character,fakeSkill,dc),roll=1+Math.floor(Math.random()*100),success=roll<=rate;if(success)await run(`INSERT INTO item_identifications(item_template_id,user_id) VALUES(?,?) ON CONFLICT DO NOTHING`,[template.id,req.user.id]);await addEvent(roomId,req.user.id,'identification',{text:`🔎 ${character.name} 鑑定一件物品：${success?`成功，確認為「${itemName}」`:'失敗'}（${roll}/${rate}）`,roll,rate,success});return res.json({ok:true,mode,roll,rate,success,found:success?[`確認物品：${itemName}`]:[]});}
  dc=Number(node?.investigation_dc)||50;const rate=v27InvestigationRate(character,skill,dc),roll=1+Math.floor(Math.random()*100),success=roll<=rate;const found=[];
  if(success){const connections=mapConnectionIds(node?.connections);if(connections.length){const hiddenNodes=await all(`SELECT * FROM room_map_nodes WHERE room_id=? AND hidden=TRUE AND id=ANY(?::bigint[])`,[roomId,connections]);for(const n of hiddenNodes){await run(`INSERT INTO map_node_discoveries(node_id,user_id) VALUES(?,?) ON CONFLICT DO NOTHING`,[n.id,req.user.id]);found.push(`隱藏地點：${n.discovery_hint||n.name}`);}}
    const hiddenContainers=await all(`SELECT * FROM search_containers WHERE room_id=? AND active=TRUE AND hidden=TRUE AND (map_node_id IS NULL OR map_node_id=?)`,[roomId,nodeId]);for(const c of hiddenContainers){await run(`INSERT INTO search_records(container_id,user_id,discovered) VALUES(?,?,TRUE) ON CONFLICT(container_id,user_id) DO UPDATE SET discovered=TRUE,updated_at=CURRENT_TIMESTAMP`,[c.id,req.user.id]);found.push(`可搜索物：${c.name}`);}
    const clues=await all(`SELECT * FROM clue_templates WHERE auto_discover=TRUE AND (room_id IS NULL OR room_id=?) AND (map_node_id IS NULL OR map_node_id=?) AND investigation_dc<=?`,[roomId,nodeId,rate]);for(const clue of clues){const has=await get(`SELECT 1 FROM character_clues WHERE clue_id=? AND user_id=?`,[clue.id,req.user.id]);if(!has){await v27GrantClue(req.user.id,clue,'探索發現');found.push(`線索：${clue.name}`);}}
  }
  await addEvent(roomId,req.user.id,'investigation',{text:`🔎 ${character.name} 探查「${node?.name||'目前環境'}」：${success?'成功':'失敗'}（${roll}/${rate}）${found.length?`｜發現 ${found.join('、')}`:''}`,mode,roll,rate,success,found});emitTeamUpdate(roomId,(await getUserRoomTeam(req.user.id,roomId))?.id||null);res.json({ok:true,mode,roll,rate,success,found});
}catch(error){console.error('V27 EXPLORE ERROR',error);res.status(error.status||500).json({error:error.message||'探索失敗'});}});

app.get('/api/admin/clue-combinations',auth,adminAuth,async(req,res)=>{try{const roomId=Number(req.query.room_id)||0;const rows=roomId?await all(`SELECT * FROM clue_combinations WHERE room_id=? ORDER BY id DESC`,[roomId]):await all(`SELECT * FROM clue_combinations ORDER BY id DESC`);res.json({combinations:rows.map(r=>({...r,required_clue_ids:parseJsonField(r.required_clue_ids,[])}))});}catch(error){res.status(500).json({error:'讀取線索組合失敗'});}});
app.post('/api/admin/clue-combinations',auth,adminAuth,async(req,res)=>{try{const roomId=Number(req.body.room_id);await requireRoomAdmin(req.user.id,roomId);const ids=[...new Set((Array.isArray(req.body.required_clue_ids)?req.body.required_clue_ids:[]).map(Number).filter(Boolean))];if(ids.length<2)return res.status(400).json({error:'線索組合至少需要 2 條線索'});const type=['vision','rule','clue','map_node'].includes(String(req.body.result_type||''))?String(req.body.result_type):'vision';const row=await get(`INSERT INTO clue_combinations(room_id,name,required_clue_ids,result_type,result_id,result_title,result_content,active) VALUES(?,?,?::jsonb,?,?,?,?,TRUE) RETURNING *`,[roomId,String(req.body.name||'推理').trim().slice(0,180)||'推理',JSON.stringify(ids),type,Number(req.body.result_id)||null,String(req.body.result_title||'推理結果').slice(0,180),String(req.body.result_content||'').slice(0,8000)]);res.json({ok:true,combination:{...row,required_clue_ids:ids}});}catch(error){res.status(error.status||500).json({error:error.message||'建立線索組合失敗'});}});
app.delete('/api/admin/clue-combinations/:id',auth,adminAuth,async(req,res)=>{try{const row=await get(`SELECT room_id FROM clue_combinations WHERE id=?`,[req.params.id]);if(row)await requireRoomAdmin(req.user.id,row.room_id);await run(`DELETE FROM clue_combinations WHERE id=?`,[req.params.id]);res.json({ok:true});}catch(error){res.status(error.status||500).json({error:error.message||'刪除線索組合失敗'});}});
app.post('/api/rooms/:id/inference',auth,async(req,res)=>{try{const roomId=Number(req.params.id);await requireRoomMember(req.user.id,roomId);const known=await v27KnownClueIds(req.user.id,roomId);const combos=await all(`SELECT * FROM clue_combinations WHERE room_id=? AND active=TRUE ORDER BY id`,[roomId]);const unlocked=[];for(const combo of combos){const old=await get(`SELECT 1 FROM clue_combination_unlocks WHERE combination_id=? AND user_id=?`,[combo.id,req.user.id]);if(old)continue;const need=parseJsonField(combo.required_clue_ids,[]).map(Number).filter(Boolean);if(!need.length||!need.every(id=>known.has(id)))continue;await run(`INSERT INTO clue_combination_unlocks(combination_id,user_id) VALUES(?,?) ON CONFLICT DO NOTHING`,[combo.id,req.user.id]);let resultText=String(combo.result_content||combo.result_title||'你得到了一個新的推論。');if(combo.result_type==='rule'&&combo.result_id)await run(`INSERT INTO rule_discoveries(rule_id,user_id,knowledge_level) VALUES(?,?,3) ON CONFLICT(rule_id,user_id) DO UPDATE SET knowledge_level=3,updated_at=CURRENT_TIMESTAMP`,[combo.result_id,req.user.id]);else if(combo.result_type==='clue'&&combo.result_id){const clue=await get(`SELECT * FROM clue_templates WHERE id=?`,[combo.result_id]);if(clue)await v27GrantClue(req.user.id,clue,'推理結果');}else if(combo.result_type==='map_node'&&combo.result_id)await run(`INSERT INTO map_node_discoveries(node_id,user_id) VALUES(?,?) ON CONFLICT DO NOTHING`,[combo.result_id,req.user.id]);else await run(`INSERT INTO personal_visions(room_id,user_id,title,content,vision_type,shareable) VALUES(?,?,?,?,?,TRUE)`,[roomId,req.user.id,combo.result_title||combo.name,resultText,'memory']);unlocked.push({id:combo.id,name:combo.name,result_type:combo.result_type,title:combo.result_title,content:resultText});await addEvent(roomId,req.user.id,'inference',{text:`🧩 ${(await getCharacterByUser(req.user.id))?.name||'玩家'} 完成推理「${combo.name}」`,combination_id:combo.id});}res.json({ok:true,unlocked});}catch(error){res.status(error.status||500).json({error:error.message||'整理線索失敗'});}});

app.post('/api/admin/rooms/:id/time-flow/reset',auth,adminAuth,async(req,res)=>{try{const roomId=Number(req.params.id);await requireRoomAdmin(req.user.id,roomId);await run(`UPDATE rooms SET time_flow_offset_minutes=0,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[roomId]);await run(`UPDATE room_map_nodes SET time_flow_offset_minutes=0,updated_at=CURRENT_TIMESTAMP WHERE room_id=?`,[roomId]);res.json({ok:true});}catch(error){res.status(error.status||500).json({error:error.message||'重設時間流失敗'});}});

async function v27EventAudienceUsers(ev,roomId){if(ev.audience==='user'&&ev.target_user_id)return [Number(ev.target_user_id)];if(ev.audience==='team'&&ev.target_team_id){const rows=await all(`SELECT user_id FROM room_team_members WHERE team_id=?`,[ev.target_team_id]);return rows.map(x=>Number(x.user_id));}if(ev.map_node_id)return (await usersAtMapNode(roomId,ev.map_node_id)).map(Number);const rows=await all(`SELECT user_id FROM room_members WHERE room_id=? AND role='player'`,[roomId]);return rows.map(x=>Number(x.user_id));}
async function v27ExecuteWorldEventAction(ev,roomId){
  const type=String(ev.action_type||'message');const payload=parseJsonField(ev.action_payload,{});const boolValue=value=>!(value===false||value===0||['false','0','off','no'].includes(String(value??'').trim().toLowerCase()));
  if(type==='message')return;
  if(type==='reveal_node'||type==='set_node_hidden'){
    const id=Number(payload.node_id||ev.map_node_id);if(!id)return;
    const hidden=type==='reveal_node'?false:boolValue(payload.hidden);
    await run(`UPDATE room_map_nodes SET hidden=?,updated_at=CURRENT_TIMESTAMP WHERE room_id=? AND id=?`,[hidden,roomId,id]);return;
  }
  if(type==='set_rule_active'){
    const id=Number(payload.rule_id);if(id)await run(`UPDATE horror_rules SET active=?,updated_at=CURRENT_TIMESTAMP WHERE room_id=? AND id=?`,[boolValue(payload.active),roomId,id]);return;
  }
  if(type==='spawn_monster'){
    const t=await get(`SELECT * FROM monster_templates WHERE id=?`,[Number(payload.monster_template_id)]);if(!t)return;
    const entity=parseWorldEntityRow(t);const attrs=entity.final_attributes||{};const spirit=Math.max(0,Number(attrs.spirit)||0);const fifth=Math.max(0,Number(attrs.great_way??attrs.faith??attrs.fifth)||0);const hp=Math.max(1,Number(t.max_hp)||100);const nodeId=await validRoomMapNodeId(roomId,payload.map_node_id||ev.map_node_id);
    await run(`INSERT INTO room_monsters(room_id,monster_template_id,status,current_hp,max_hp,current_spirit,max_spirit,current_fifth,max_fifth,map_node_id) VALUES(?,?, 'hidden', ?, ?, ?, ?, ?, ?, ?)`,[roomId,t.id,hp,hp,spirit,spirit,fifth,fifth,nodeId]);return;
  }
  if(type==='spawn_npc'){
    const t=await get(`SELECT * FROM npc_templates WHERE id=?`,[Number(payload.npc_template_id)]);if(!t)return;
    const entity=parseWorldEntityRow(t);const attrs=entity.final_attributes||{};const spirit=Math.max(0,Number(attrs.spirit)||0);const fifth=Math.max(0,Number(attrs.great_way??attrs.faith??attrs.fifth)||0);const hp=Math.max(1,Number(t.max_hp)||Math.max(1,(Number(attrs.constitution)||0)*2)||100);const cfg=parseWorldConfig(t.config);const side=['ally','enemy','neutral'].includes(String(payload.combat_side||cfg.combat_side||''))?String(payload.combat_side||cfg.combat_side):'neutral';const nodeId=await validRoomMapNodeId(roomId,payload.map_node_id||ev.map_node_id);
    await run(`INSERT INTO room_npcs(room_id,npc_template_id,combat_side,current_hp,max_hp,current_spirit,max_spirit,current_fifth,max_fifth,map_node_id) VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(room_id,npc_template_id) DO UPDATE SET status='active',combat_side=EXCLUDED.combat_side,map_node_id=EXCLUDED.map_node_id,current_hp=EXCLUDED.current_hp,max_hp=EXCLUDED.max_hp,current_spirit=EXCLUDED.current_spirit,max_spirit=EXCLUDED.max_spirit,current_fifth=EXCLUDED.current_fifth,max_fifth=EXCLUDED.max_fifth,updated_at=CURRENT_TIMESTAMP`,[roomId,t.id,side,hp,hp,spirit,spirit,fifth,fifth,nodeId]);return;
  }
  if(type==='boss_next'){
    const monsterId=Number(payload.monster_id);if(!monsterId)return;const row=await get(`SELECT rm.*,mt.config FROM room_monsters rm JOIN monster_templates mt ON mt.id=rm.monster_template_id WHERE rm.id=? AND rm.room_id=?`,[monsterId,roomId]);if(!row)return;const phases=normalizeBossPhases(parseWorldConfig(row.config).boss_phases);const next=(Number(row.boss_phase_index)||0)+1;if(next<=phases.length)await activateMonsterBossPhase(roomId,monsterId,next,phases[next-1],'scheduled_event');return;
  }
  if(type==='set_time_rate'){
    const rate=Math.max(-4,Math.min(8,Number(payload.rate)||0));const nodeId=Number(payload.node_id||ev.map_node_id);if(nodeId)await run(`UPDATE room_map_nodes SET time_rate=?,updated_at=CURRENT_TIMESTAMP WHERE room_id=? AND id=?`,[rate,roomId,nodeId]);else await run(`UPDATE rooms SET time_rate=?,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[rate,roomId]);return;
  }
  if(type==='set_time_offset'){
    const offset=clampInt(payload.offset_minutes,-10080,10080,0);const nodeId=Number(payload.node_id||ev.map_node_id);if(nodeId)await run(`UPDATE room_map_nodes SET time_offset_minutes=?,updated_at=CURRENT_TIMESTAMP WHERE room_id=? AND id=?`,[offset,roomId,nodeId]);else await run(`UPDATE rooms SET time_distortion_offset_minutes=?,updated_at=CURRENT_TIMESTAMP WHERE id=?`,[offset,roomId]);return;
  }
  if(type==='private_vision'){
    const users=await v27EventAudienceUsers(ev,roomId);for(const uid of users)await run(`INSERT INTO personal_visions(room_id,user_id,title,content,vision_type,shareable) VALUES(?,?,?,?,?,?)`,[roomId,uid,String(payload.title||ev.name||'異常感知').slice(0,180),String(payload.content||ev.content||'').slice(0,8000),String(payload.vision_type||'hallucination').slice(0,30),boolValue(payload.shareable)]);return;
  }
  if(type==='pollution_delta'||type==='sanity_delta'){
    const users=await v27EventAudienceUsers(ev,roomId);for(const uid of users){
      if(type==='pollution_delta'){
        const ch=await get(`SELECT pollution FROM character_cards WHERE user_id=?`,[uid]);const next=clampInt((Number(ch?.pollution)||0)+(Number(payload.amount)||0),0,100,0);await run(`UPDATE character_cards SET pollution=?,updated_at=CURRENT_TIMESTAMP WHERE user_id=?`,[next,uid]);
      }else{
        const sanity=await ensureRoomMemberSanity(roomId,uid);if(!sanity)continue;const max=Math.max(0,Number(sanity.max_sanity)||0),before=Math.max(0,Number(sanity.current_sanity)||0),next=Math.max(0,Math.min(max,before+(Number(payload.amount)||0)));const oldStage=sanityStage(before,max),newStage=sanityStage(next,max);let sin=sanity.seven_sin||null;if((before-next)>=max*0.25&&max>0&&!sin)sin=SEVEN_SINS[Math.floor(Math.random()*SEVEN_SINS.length)];await run(`UPDATE room_members SET current_sanity=?,sanity_stage=?,seven_sin=? WHERE room_id=? AND user_id=?`,[next,newStage.key,sin,roomId,uid]);if((SANITY_STAGE_ORDER[newStage.key]||0)>(SANITY_STAGE_ORDER[oldStage.key]||0))await createSanityStageVision(roomId,uid,newStage.key);
      }
    }return;
  }
}




/* =========================================================
   V29: 大道 / 信仰 / 諸神殿 / 自訂輪盤
========================================================= */
function normalizeIdentityRank(value,fallback='G'){return normalizeProfessionRank(value,fallback);}
function validateIdentityRank(rank,maxRank,label='等階'){const r=normalizeIdentityRank(rank,'G'),m=normalizeIdentityRank(maxRank,'SSS');if(IDENTITY_RANKS_SERVER.indexOf(r)>IDENTITY_RANKS_SERVER.indexOf(m))throw Object.assign(new Error(`${label}不能高於 ${m}級`),{status:400});return r;}
const IDENTITY_RANKS_SERVER=['G','F','E','D','C','B','A','S','SS','SSS'];
function normalizeSkillTemplateIdList(value){return [...new Set((Array.isArray(value)?value:parseJsonField(value,[])).map(Number).filter(Boolean))].slice(0,100);}
function normalizeWheelOptions(value){
  const raw=Array.isArray(value)?value:parseJsonField(value,[]);
  return raw.map(item=>typeof item==='string'?{label:item,weight:1}:{label:String(item?.label||item?.name||'').trim().slice(0,300),weight:Math.max(1,Math.min(1000,Math.floor(Number(item?.weight)||1)))})
    .filter(item=>item.label).slice(0,60);
}
function parseGreatWayTemplate(row){return row?{...row,effects:effectContainerWithList(parseJsonField(row.effects,{}),normalizeUnifiedEffects(parseJsonField(row.effects,{}))),granted_skill_template_ids:normalizeSkillTemplateIdList(row.granted_skill_template_ids)}:null;}
function parseDeity(row){return row?{...row,domains:parseJsonField(row.domains,[]),effects:effectContainerWithList(parseJsonField(row.effects,{}),normalizeUnifiedEffects(parseJsonField(row.effects,{}))),granted_skill_template_ids:normalizeSkillTemplateIdList(row.granted_skill_template_ids)}:null;}
function parseWheel(row){return row?{...row,options:normalizeWheelOptions(row.options)}:null;}
async function grantIdentitySkills(userId,templateIds,acquisition){
  const client=await pool.connect();
  try{await client.query('BEGIN');const result=await client.query('SELECT * FROM character_cards WHERE user_id=$1 FOR UPDATE',[userId]);const character=parseCharacter(result.rows[0]);if(!character)throw Object.assign(new Error('找不到角色卡'),{status:404});const granted=await grantSkillTemplateIdsToCharacterWithClient(client,character,templateIds,acquisition);await persistCharacterProgressWithClient(client,character,userId);await client.query('COMMIT');return granted;}catch(error){await client.query('ROLLBACK').catch(()=>{});throw error;}finally{client.release();}
}
async function identityAssignmentsForUser(userId){
  const [greatWays,faiths]=await Promise.all([
    all(`SELECT cgw.*,gwt.name,gwt.principle,gwt.description,gwt.max_rank,gwt.effects,gwt.granted_skill_template_ids FROM character_great_ways cgw JOIN great_way_templates gwt ON gwt.id=cgw.great_way_id WHERE cgw.user_id=? ORDER BY cgw.created_at,gwt.id`,[userId]),
    all(`SELECT cf.*,fd.name,fd.title,fd.domains,fd.doctrine,fd.description,fd.max_rank,fd.effects,fd.granted_skill_template_ids FROM character_faiths cf JOIN faith_deities fd ON fd.id=cf.deity_id WHERE cf.user_id=? ORDER BY cf.created_at,fd.id`,[userId])
  ]);
  return {great_ways:greatWays.map(row=>({...parseGreatWayTemplate(row),great_way_id:row.great_way_id,rank:normalizeIdentityRank(row.rank),progress:Number(row.progress)||0,assignment_active:Boolean(row.active)})),faiths:faiths.map(row=>({...parseDeity(row),deity_id:row.deity_id,rank:normalizeIdentityRank(row.rank),devotion:Number(row.devotion)||0,favor:Number(row.favor)||0,assignment_active:Boolean(row.active)}))};
}

app.get('/api/great-ways',auth,async(req,res)=>{try{const user=await getUser(req.user.id);const rows=await all(`SELECT * FROM great_way_templates ${user?.is_admin?'':'WHERE active=TRUE'} ORDER BY id`);const mine=await identityAssignmentsForUser(req.user.id);res.json({great_ways:rows.map(parseGreatWayTemplate),my_great_ways:mine.great_ways});}catch(error){res.status(500).json({error:'讀取大道系統失敗'});}});
app.get('/api/pantheon',auth,async(req,res)=>{try{const user=await getUser(req.user.id);const rows=await all(`SELECT * FROM faith_deities ${user?.is_admin?'':'WHERE active=TRUE'} ORDER BY id`);const mine=await identityAssignmentsForUser(req.user.id);res.json({deities:rows.map(parseDeity),my_faiths:mine.faiths});}catch(error){res.status(500).json({error:'讀取諸神殿失敗'});}});

app.post('/api/admin/great-ways',auth,adminAuth,async(req,res)=>{try{const name=String(req.body.name||'').trim().slice(0,160);if(!name)return res.status(400).json({error:'請輸入大道名稱'});const row=await get(`INSERT INTO great_way_templates(name,principle,description,max_rank,effects,granted_skill_template_ids,active) VALUES(?,?,?,?,?::jsonb,?::jsonb,?) RETURNING *`,[name,String(req.body.principle||'').slice(0,6000),String(req.body.description||'').slice(0,10000),normalizeIdentityRank(req.body.max_rank,'SSS'),JSON.stringify(effectContainerWithList({},normalizeUnifiedEffects(req.body.effects||[]))),JSON.stringify(normalizeSkillTemplateIdList(req.body.granted_skill_template_ids)),req.body.active===undefined?true:Boolean(req.body.active)]);res.json({ok:true,great_way:parseGreatWayTemplate(row)});}catch(error){res.status(error.code==='23505'?409:500).json({error:error.code==='23505'?'大道名稱已存在':'建立大道失敗'});}});
app.patch('/api/admin/great-ways/:id',auth,adminAuth,async(req,res)=>{try{const row=await get(`UPDATE great_way_templates SET name=?,principle=?,description=?,max_rank=?,effects=?::jsonb,granted_skill_template_ids=?::jsonb,active=?,updated_at=CURRENT_TIMESTAMP WHERE id=? RETURNING *`,[String(req.body.name||'').trim().slice(0,160),String(req.body.principle||'').slice(0,6000),String(req.body.description||'').slice(0,10000),normalizeIdentityRank(req.body.max_rank,'SSS'),JSON.stringify(effectContainerWithList({},normalizeUnifiedEffects(req.body.effects||[]))),JSON.stringify(normalizeSkillTemplateIdList(req.body.granted_skill_template_ids)),req.body.active===undefined?true:Boolean(req.body.active),req.params.id]);if(!row)return res.status(404).json({error:'找不到大道'});await syncAllCharacterSourceEffects();res.json({ok:true,great_way:parseGreatWayTemplate(row)});}catch(error){res.status(error.code==='23505'?409:500).json({error:error.code==='23505'?'大道名稱已存在':'修改大道失敗'});}});
app.delete('/api/admin/great-ways/:id',auth,adminAuth,async(req,res)=>{try{await run(`DELETE FROM great_way_templates WHERE id=?`,[req.params.id]);await syncAllCharacterSourceEffects();res.json({ok:true});}catch(error){res.status(500).json({error:'刪除大道失敗'});}});

app.post('/api/admin/deities',auth,adminAuth,async(req,res)=>{try{const name=String(req.body.name||'').trim().slice(0,160);if(!name)return res.status(400).json({error:'請輸入神祇名稱'});const domains=(Array.isArray(req.body.domains)?req.body.domains:String(req.body.domains||'').split(/[,，\n]/)).map(x=>String(x).trim().slice(0,80)).filter(Boolean).slice(0,20);const row=await get(`INSERT INTO faith_deities(name,title,domains,doctrine,description,max_rank,effects,granted_skill_template_ids,active) VALUES(?,?,?::jsonb,?,?,?,?::jsonb,?::jsonb,?) RETURNING *`,[name,String(req.body.title||'').slice(0,180),JSON.stringify(domains),String(req.body.doctrine||'').slice(0,6000),String(req.body.description||'').slice(0,10000),normalizeIdentityRank(req.body.max_rank,'SSS'),JSON.stringify(effectContainerWithList({},normalizeUnifiedEffects(req.body.effects||[]))),JSON.stringify(normalizeSkillTemplateIdList(req.body.granted_skill_template_ids)),req.body.active===undefined?true:Boolean(req.body.active)]);res.json({ok:true,deity:parseDeity(row)});}catch(error){res.status(error.code==='23505'?409:500).json({error:error.code==='23505'?'神祇名稱已存在':'建立神祇失敗'});}});
app.patch('/api/admin/deities/:id',auth,adminAuth,async(req,res)=>{try{const domains=(Array.isArray(req.body.domains)?req.body.domains:String(req.body.domains||'').split(/[,，\n]/)).map(x=>String(x).trim().slice(0,80)).filter(Boolean).slice(0,20);const row=await get(`UPDATE faith_deities SET name=?,title=?,domains=?::jsonb,doctrine=?,description=?,max_rank=?,effects=?::jsonb,granted_skill_template_ids=?::jsonb,active=?,updated_at=CURRENT_TIMESTAMP WHERE id=? RETURNING *`,[String(req.body.name||'').trim().slice(0,160),String(req.body.title||'').slice(0,180),JSON.stringify(domains),String(req.body.doctrine||'').slice(0,6000),String(req.body.description||'').slice(0,10000),normalizeIdentityRank(req.body.max_rank,'SSS'),JSON.stringify(effectContainerWithList({},normalizeUnifiedEffects(req.body.effects||[]))),JSON.stringify(normalizeSkillTemplateIdList(req.body.granted_skill_template_ids)),req.body.active===undefined?true:Boolean(req.body.active),req.params.id]);if(!row)return res.status(404).json({error:'找不到神祇'});await syncAllCharacterSourceEffects();res.json({ok:true,deity:parseDeity(row)});}catch(error){res.status(error.code==='23505'?409:500).json({error:error.code==='23505'?'神祇名稱已存在':'修改神祇失敗'});}});
app.delete('/api/admin/deities/:id',auth,adminAuth,async(req,res)=>{try{await run(`DELETE FROM faith_deities WHERE id=?`,[req.params.id]);await syncAllCharacterSourceEffects();res.json({ok:true});}catch(error){res.status(500).json({error:'刪除神祇失敗'});}});

app.post('/api/admin/players/:userId/great-ways/:id',auth,adminAuth,async(req,res)=>{try{const userId=Number(req.params.userId),id=Number(req.params.id);const target=await getUser(userId);if(!target)return res.status(404).json({error:'找不到玩家'});const ch=await requireCharacter(userId);if(ch.faction==='西國')return res.status(400).json({error:'西國角色使用信仰系統；不能授予大道'});const template=await get(`SELECT * FROM great_way_templates WHERE id=?`,[id]);if(!template)return res.status(404).json({error:'找不到大道'});const rank=validateIdentityRank(req.body.rank,template.max_rank,'大道等階');await run(`INSERT INTO character_great_ways(user_id,great_way_id,rank,progress,active) VALUES(?,?,?,?,TRUE) ON CONFLICT(user_id,great_way_id) DO UPDATE SET rank=EXCLUDED.rank,progress=EXCLUDED.progress,active=TRUE,updated_at=CURRENT_TIMESTAMP`,[userId,id,rank,Math.max(0,Math.floor(Number(req.body.progress)||0))]);const granted=await grantIdentitySkills(userId,normalizeSkillTemplateIdList(template.granted_skill_template_ids),{type:'great_way',great_way_id:id,great_way_name:template.name,rank});await syncCharacterSourceEffects(userId);res.json({ok:true,granted});}catch(error){res.status(error.status||500).json({error:error.message||'授予大道失敗'});}});
app.delete('/api/admin/players/:userId/great-ways/:id',auth,adminAuth,async(req,res)=>{try{await run(`DELETE FROM character_great_ways WHERE user_id=? AND great_way_id=?`,[req.params.userId,req.params.id]);await syncCharacterSourceEffects(req.params.userId);res.json({ok:true});}catch(error){res.status(500).json({error:'移除大道失敗'});}});
app.post('/api/admin/players/:userId/faiths/:id',auth,adminAuth,async(req,res)=>{try{const userId=Number(req.params.userId),id=Number(req.params.id);const target=await getUser(userId);if(!target)return res.status(404).json({error:'找不到玩家'});const ch=await requireCharacter(userId);if(ch.faction!=='西國')return res.status(400).json({error:'東國角色使用大道系統；不能授予信仰'});const deity=await get(`SELECT * FROM faith_deities WHERE id=?`,[id]);if(!deity)return res.status(404).json({error:'找不到神祇'});const rank=validateIdentityRank(req.body.rank,deity.max_rank,'信仰等階');await run(`INSERT INTO character_faiths(user_id,deity_id,rank,devotion,favor,active) VALUES(?,?,?,?,?,TRUE) ON CONFLICT(user_id,deity_id) DO UPDATE SET rank=EXCLUDED.rank,devotion=EXCLUDED.devotion,favor=EXCLUDED.favor,active=TRUE,updated_at=CURRENT_TIMESTAMP`,[userId,id,rank,Math.max(0,Math.floor(Number(req.body.devotion)||0)),Math.max(-100,Math.min(100,Math.floor(Number(req.body.favor)||0)))]);const granted=await grantIdentitySkills(userId,normalizeSkillTemplateIdList(deity.granted_skill_template_ids),{type:'faith',deity_id:id,deity_name:deity.name,rank});await syncCharacterSourceEffects(userId);res.json({ok:true,granted});}catch(error){res.status(error.status||500).json({error:error.message||'授予信仰失敗'});}});
app.delete('/api/admin/players/:userId/faiths/:id',auth,adminAuth,async(req,res)=>{try{await run(`DELETE FROM character_faiths WHERE user_id=? AND deity_id=?`,[req.params.userId,req.params.id]);await syncCharacterSourceEffects(req.params.userId);res.json({ok:true});}catch(error){res.status(500).json({error:'移除信仰失敗'});}});

app.get('/api/wheels',auth,async(req,res)=>{try{const user=await getUser(req.user.id);const roomId=Math.max(0,Number(req.query.room_id)||0);let rows;if(user?.is_admin){rows=await all(`SELECT cw.*,u.username AS creator_username FROM custom_wheels cw LEFT JOIN users u ON u.id=cw.created_by ORDER BY cw.id DESC`);}else{rows=await all(`SELECT cw.*,u.username AS creator_username FROM custom_wheels cw LEFT JOIN users u ON u.id=cw.created_by WHERE (cw.room_id IS NULL OR cw.room_id=?) AND cw.active=TRUE AND cw.audience IN ('all','players') ORDER BY cw.id DESC`,[roomId||-1]);}res.json({wheels:rows.map(parseWheel)});}catch(error){res.status(500).json({error:'讀取輪盤失敗'});}});
app.post('/api/admin/wheels',auth,adminAuth,async(req,res)=>{try{const name=String(req.body.name||'').trim().slice(0,180);const options=normalizeWheelOptions(req.body.options);if(!name)return res.status(400).json({error:'請輸入輪盤名稱'});if(options.length<2)return res.status(400).json({error:'輪盤至少需要兩個選項'});const roomId=Math.max(0,Number(req.body.room_id)||0)||null;if(roomId){const room=await get(`SELECT id FROM rooms WHERE id=?`,[roomId]);if(!room)return res.status(404).json({error:'找不到房間'});}const audience=['all','players','dm'].includes(String(req.body.audience))?String(req.body.audience):'all';const row=await get(`INSERT INTO custom_wheels(room_id,name,description,audience,options,active,created_by) VALUES(?,?,?,?,?::jsonb,?,?) RETURNING *`,[roomId,name,String(req.body.description||'').slice(0,6000),audience,JSON.stringify(options),req.body.active===undefined?true:Boolean(req.body.active),req.user.id]);res.json({ok:true,wheel:parseWheel(row)});}catch(error){res.status(error.status||500).json({error:error.message||'建立輪盤失敗'});}});
app.patch('/api/admin/wheels/:id',auth,adminAuth,async(req,res)=>{try{const options=normalizeWheelOptions(req.body.options);if(options.length<2)return res.status(400).json({error:'輪盤至少需要兩個選項'});const roomId=Math.max(0,Number(req.body.room_id)||0)||null;const audience=['all','players','dm'].includes(String(req.body.audience))?String(req.body.audience):'all';const row=await get(`UPDATE custom_wheels SET room_id=?,name=?,description=?,audience=?,options=?::jsonb,active=?,updated_at=CURRENT_TIMESTAMP WHERE id=? RETURNING *`,[roomId,String(req.body.name||'').trim().slice(0,180),String(req.body.description||'').slice(0,6000),audience,JSON.stringify(options),req.body.active===undefined?true:Boolean(req.body.active),req.params.id]);if(!row)return res.status(404).json({error:'找不到輪盤'});res.json({ok:true,wheel:parseWheel(row)});}catch(error){res.status(error.status||500).json({error:error.message||'修改輪盤失敗'});}});
app.delete('/api/admin/wheels/:id',auth,adminAuth,async(req,res)=>{try{await run(`DELETE FROM custom_wheels WHERE id=?`,[req.params.id]);res.json({ok:true});}catch(error){res.status(500).json({error:'刪除輪盤失敗'});}});
app.post('/api/wheels/:id/spin',auth,async(req,res)=>{try{const wheel=await get(`SELECT * FROM custom_wheels WHERE id=? AND active=TRUE`,[req.params.id]);if(!wheel)return res.status(404).json({error:'找不到可用輪盤'});const user=await getUser(req.user.id);if(wheel.audience==='dm'&&!user?.is_admin)return res.status(403).json({error:'這個輪盤只有 DM 可以使用'});if(wheel.room_id&&!user?.is_admin)await requireRoomMember(req.user.id,wheel.room_id);const options=normalizeWheelOptions(wheel.options);if(options.length<2)return res.status(400).json({error:'輪盤選項不足'});const total=options.reduce((s,x)=>s+x.weight,0);let pick=Math.floor(Math.random()*total),index=0;for(let i=0;i<options.length;i++){if(pick<options[i].weight){index=i;break;}pick-=options[i].weight;}const result=options[index];await run(`INSERT INTO wheel_spins(wheel_id,room_id,user_id,result_index,result_label) VALUES(?,?,?,?,?)`,[wheel.id,wheel.room_id||null,req.user.id,index,result.label]);if(wheel.room_id)await addEvent(wheel.room_id,req.user.id,'wheel',{text:`🎡 ${(await getCharacterByUser(req.user.id))?.name||user?.username||'玩家'} 轉動「${wheel.name}」→ ${result.label}`,wheel_id:wheel.id,result:result.label});res.json({ok:true,result:{index,label:result.label},wheel:parseWheel(wheel)});}catch(error){res.status(error.status||500).json({error:error.message||'轉動輪盤失敗'});}});

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
    return syncAllCharacterProfessionProgress();
  })
  .then(function () {
    return syncAllCharacterSourceEffects();
  })
  .then(async function () {
    const rows=await all(`SELECT * FROM character_cards ORDER BY id`);
    for(const row of rows){await ensureBackpackBonusForUser(row.user_id,row);}
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
