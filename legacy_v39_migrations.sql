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

      gender VARCHAR(40) NOT NULL DEFAULT '',

      age INTEGER NOT NULL DEFAULT 0,

      height_cm INTEGER NOT NULL DEFAULT 0,

      birthday VARCHAR(40) NOT NULL DEFAULT '',

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
      ON character_room_bindings(character_id);;

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
    TIMESTAMPTZ;;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS combat_hatred INTEGER NOT NULL DEFAULT 100;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS faiths JSONB NOT NULL DEFAULT '[]'::jsonb;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS great_ways JSONB NOT NULL DEFAULT '[]'::jsonb;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS faith INTEGER NOT NULL DEFAULT 0;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS agility_trial_tier INTEGER NOT NULL DEFAULT 1;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS strength_trial_tier INTEGER NOT NULL DEFAULT 1;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS constitution_trial_tier INTEGER NOT NULL DEFAULT 1;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS spirit_trial_tier INTEGER NOT NULL DEFAULT 1;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS great_way_trial_tier INTEGER NOT NULL DEFAULT 1;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS faith_trial_tier INTEGER NOT NULL DEFAULT 1;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS current_great_way INTEGER;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS current_faith INTEGER;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS profession_rank VARCHAR(10) NOT NULL DEFAULT 'G';

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS innate_sub_class VARCHAR(100);

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS weird_hatred INTEGER NOT NULL DEFAULT 0;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS class_resources JSONB NOT NULL DEFAULT '{}'::jsonb;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS gender VARCHAR(40) NOT NULL DEFAULT '';

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS age INTEGER NOT NULL DEFAULT 0;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS height_cm INTEGER NOT NULL DEFAULT 0;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS birthday VARCHAR(40) NOT NULL DEFAULT '';

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS inventory JSONB NOT NULL DEFAULT '[]'::jsonb;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS weird_coins BIGINT NOT NULL DEFAULT 0;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS game_coins BIGINT NOT NULL DEFAULT 0;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS game_coin_unlocked BOOLEAN NOT NULL DEFAULT FALSE;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS profession_progress JSONB NOT NULL DEFAULT '{}'::jsonb;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS inventory_slots INTEGER NOT NULL DEFAULT 5;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS bloodline VARCHAR(120) NOT NULL DEFAULT '';

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS bloodline_effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS profession_effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS great_way_effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS faith_effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS self_intro TEXT NOT NULL DEFAULT '';

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS game_room_unlocked BOOLEAN NOT NULL DEFAULT FALSE;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS name_locked BOOLEAN NOT NULL DEFAULT FALSE;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS backpack_bonus_applied BOOLEAN NOT NULL DEFAULT FALSE;

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS gear_loadout JSONB NOT NULL DEFAULT '{"equipment":[],"weapons":[]}'::jsonb;

ALTER TABLE rooms ADD COLUMN IF NOT EXISTS map_name VARCHAR(120) NOT NULL DEFAULT '未設定地點';

ALTER TABLE rooms ADD COLUMN IF NOT EXISTS map_image_url TEXT NOT NULL DEFAULT '';

ALTER TABLE rooms ADD COLUMN IF NOT EXISTS map_description TEXT NOT NULL DEFAULT '';

ALTER TABLE rooms ADD COLUMN IF NOT EXISTS game_day INTEGER NOT NULL DEFAULT 1;

ALTER TABLE rooms ADD COLUMN IF NOT EXISTS game_hour INTEGER NOT NULL DEFAULT 8;

ALTER TABLE rooms ADD COLUMN IF NOT EXISTS game_minute INTEGER NOT NULL DEFAULT 0;

ALTER TABLE rooms ADD COLUMN IF NOT EXISTS battle_active BOOLEAN NOT NULL DEFAULT FALSE;

ALTER TABLE rooms ADD COLUMN IF NOT EXISTS current_actor_type VARCHAR(20) NOT NULL DEFAULT 'player';

ALTER TABLE rooms ADD COLUMN IF NOT EXISTS current_actor_ref_id BIGINT;

ALTER TABLE rooms ADD COLUMN IF NOT EXISTS combat_order JSONB NOT NULL DEFAULT '[]'::jsonb;

ALTER TABLE rooms ADD COLUMN IF NOT EXISTS combat_index INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_members ADD COLUMN IF NOT EXISTS turn_actions_remaining INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_members ADD COLUMN IF NOT EXISTS skill_charge_state JSONB NOT NULL DEFAULT '{}'::jsonb;

ALTER TABLE room_members ADD COLUMN IF NOT EXISTS combat_reaction VARCHAR(12) NOT NULL DEFAULT 'dodge';

ALTER TABLE room_members ADD COLUMN IF NOT EXISTS combat_reaction_skill_id TEXT NOT NULL DEFAULT '';

ALTER TABLE room_members ADD COLUMN IF NOT EXISTS combat_reaction_round INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_members ADD COLUMN IF NOT EXISTS combat_reaction_uses INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_members ADD COLUMN IF NOT EXISTS combat_state VARCHAR(20) NOT NULL DEFAULT 'active';

CREATE TABLE IF NOT EXISTS extra_skills (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(120) NOT NULL,
      tag VARCHAR(60) NOT NULL DEFAULT '其他特殊',
      description TEXT NOT NULL DEFAULT '',
      target_type VARCHAR(60) NOT NULL DEFAULT '',
      config JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

CREATE TABLE IF NOT EXISTS bloodline_templates (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(120) NOT NULL UNIQUE,
      description TEXT NOT NULL DEFAULT '',
      effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

CREATE TABLE IF NOT EXISTS summon_templates (
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
    );

CREATE TABLE IF NOT EXISTS summon_instances (
      id BIGSERIAL PRIMARY KEY,
      template_id BIGINT REFERENCES summon_templates(id) ON DELETE SET NULL,
      owner_user_id BIGINT REFERENCES users(id) ON DELETE CASCADE,
      name VARCHAR(120) NOT NULL,
      status VARCHAR(30) NOT NULL DEFAULT 'active',
      overrides JSONB NOT NULL DEFAULT '{}'::jsonb,
      current_state JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

CREATE TABLE IF NOT EXISTS recipes (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(120) NOT NULL,
      recipe_type VARCHAR(60) NOT NULL DEFAULT 'item',
      description TEXT NOT NULL DEFAULT '',
      materials JSONB NOT NULL DEFAULT '[]'::jsonb,
      output JSONB NOT NULL DEFAULT '{}'::jsonb,
      config JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

CREATE TABLE IF NOT EXISTS profession_templates (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(100) NOT NULL UNIQUE,
      category VARCHAR(20) NOT NULL DEFAULT 'both',
      rank VARCHAR(10) NOT NULL DEFAULT 'G',
      description TEXT NOT NULL DEFAULT '',
      config JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

CREATE TABLE IF NOT EXISTS skill_templates (
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
    );

CREATE TABLE IF NOT EXISTS shop_items (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(120) NOT NULL,
      item_type VARCHAR(30) NOT NULL DEFAULT 'item',
      description TEXT NOT NULL DEFAULT '',
      price_weird BIGINT NOT NULL DEFAULT 0,
      quantity INTEGER NOT NULL DEFAULT 1,
      active BOOLEAN NOT NULL DEFAULT TRUE,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );


-- [功能備註｜商店分類] 可由 DM 管理的商店分類。既有 item_category 文字欄位保留作 v39 相容。
CREATE TABLE IF NOT EXISTS shop_categories (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(80) NOT NULL UNIQUE,
      sort_order INTEGER NOT NULL DEFAULT 100,
      active BOOLEAN NOT NULL DEFAULT TRUE,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

INSERT INTO shop_categories(name, sort_order) VALUES
  ('武器',10),('防具',20),('消耗品',30),('材料',40),('任務道具',50),('特殊',60),('其他',90)
ON CONFLICT(name) DO NOTHING;

CREATE TABLE IF NOT EXISTS monster_templates (
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
    );

CREATE TABLE IF NOT EXISTS npc_templates (
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
    );

CREATE TABLE IF NOT EXISTS dungeon_templates (
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
    );

CREATE TABLE IF NOT EXISTS room_monsters (
      id BIGSERIAL PRIMARY KEY,
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      monster_template_id BIGINT NOT NULL REFERENCES monster_templates(id) ON DELETE CASCADE,
      status VARCHAR(30) NOT NULL DEFAULT 'hidden',
      current_hp INTEGER NOT NULL DEFAULT 100,
      max_hp INTEGER NOT NULL DEFAULT 100,
      state JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS turn_actions_remaining INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS skill_charge_state JSONB NOT NULL DEFAULT '{}'::jsonb;

ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS current_spirit INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS max_spirit INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS current_fifth INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS max_fifth INTEGER NOT NULL DEFAULT 0;

CREATE TABLE IF NOT EXISTS monster_discoveries (
      room_monster_id BIGINT NOT NULL REFERENCES room_monsters(id) ON DELETE CASCADE,
      user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      encountered BOOLEAN NOT NULL DEFAULT FALSE,
      investigated BOOLEAN NOT NULL DEFAULT FALSE,
      last_roll INTEGER,
      last_rate INTEGER,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      PRIMARY KEY(room_monster_id, user_id)
    );

CREATE TABLE IF NOT EXISTS room_npcs (
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      npc_template_id BIGINT NOT NULL REFERENCES npc_templates(id) ON DELETE CASCADE,
      status VARCHAR(30) NOT NULL DEFAULT 'active',
      state JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      PRIMARY KEY(room_id, npc_template_id)
    );

CREATE TABLE IF NOT EXISTS room_dungeons (
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      dungeon_template_id BIGINT NOT NULL REFERENCES dungeon_templates(id) ON DELETE CASCADE,
      status VARCHAR(30) NOT NULL DEFAULT 'available',
      state JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      PRIMARY KEY(room_id, dungeon_template_id)
    );

ALTER TABLE summon_templates ADD COLUMN IF NOT EXISTS control_mode VARCHAR(20) NOT NULL DEFAULT 'manual';

ALTER TABLE summon_templates ADD COLUMN IF NOT EXISTS behavior_mode VARCHAR(30) NOT NULL DEFAULT 'balanced';

ALTER TABLE summon_templates ADD COLUMN IF NOT EXISTS speech_enabled BOOLEAN NOT NULL DEFAULT FALSE;

ALTER TABLE summon_templates ADD COLUMN IF NOT EXISTS effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb;

ALTER TABLE monster_templates ADD COLUMN IF NOT EXISTS element_affinity JSONB NOT NULL DEFAULT '{}'::jsonb;

ALTER TABLE monster_templates ADD COLUMN IF NOT EXISTS effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb;

-- [功能備註｜60-cpp.21 怪物完整戰鬥實體] 怪物技能分為主動／被動，大道與 Boss 階段獨立保存；五維中的幸運存於 attributes.luck。
ALTER TABLE monster_templates ADD COLUMN IF NOT EXISTS passive_skills JSONB NOT NULL DEFAULT '[]'::jsonb;
ALTER TABLE monster_templates ADD COLUMN IF NOT EXISTS great_way JSONB NOT NULL DEFAULT '{}'::jsonb;
ALTER TABLE monster_templates ADD COLUMN IF NOT EXISTS boss_phases JSONB NOT NULL DEFAULT '[]'::jsonb;

ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS attributes JSONB NOT NULL DEFAULT '{}'::jsonb;

ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS element_affinity JSONB NOT NULL DEFAULT '{}'::jsonb;

ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb;

ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS skills JSONB NOT NULL DEFAULT '[]'::jsonb;

ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS max_hp INTEGER NOT NULL DEFAULT 100;

ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS combat_side VARCHAR(20) NOT NULL DEFAULT 'neutral';

ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP;

ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS current_hp INTEGER NOT NULL DEFAULT 100;

ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS max_hp INTEGER NOT NULL DEFAULT 100;

ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS current_spirit INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS max_spirit INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS current_fifth INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS max_fifth INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS turn_actions_remaining INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS skill_charge_state JSONB NOT NULL DEFAULT '{}'::jsonb;

ALTER TABLE summon_instances ADD COLUMN IF NOT EXISTS room_id BIGINT REFERENCES rooms(id) ON DELETE SET NULL;

ALTER TABLE summon_instances ADD COLUMN IF NOT EXISTS source_skill_id VARCHAR(100);

ALTER TABLE room_members ADD COLUMN IF NOT EXISTS mounted_summon_id BIGINT REFERENCES summon_instances(id) ON DELETE SET NULL;

ALTER TABLE shop_items ADD COLUMN IF NOT EXISTS item_category VARCHAR(80) NOT NULL DEFAULT '';

ALTER TABLE shop_items ADD COLUMN IF NOT EXISTS category_id BIGINT REFERENCES shop_categories(id) ON DELETE SET NULL;

-- 將舊版自由輸入的分類自動轉成正式分類，避免升級後原有商品變成未分類。
INSERT INTO shop_categories(name, sort_order)
SELECT DISTINCT BTRIM(item_category), 80
FROM shop_items
WHERE BTRIM(COALESCE(item_category,'')) <> ''
ON CONFLICT(name) DO NOTHING;

UPDATE shop_items s
SET category_id = c.id
FROM shop_categories c
WHERE s.category_id IS NULL
  AND BTRIM(COALESCE(s.item_category,'')) <> ''
  AND c.name = BTRIM(s.item_category);

CREATE INDEX IF NOT EXISTS idx_shop_items_category_active ON shop_items(category_id, active, id);
CREATE INDEX IF NOT EXISTS idx_shop_categories_active_sort ON shop_categories(active, sort_order, id);

CREATE INDEX IF NOT EXISTS idx_summon_instances_owner_status ON summon_instances(owner_user_id, status);

CREATE INDEX IF NOT EXISTS idx_summon_instances_room_status ON summon_instances(room_id, status);

CREATE INDEX IF NOT EXISTS idx_room_monsters_room_status ON room_monsters(room_id, status);

CREATE INDEX IF NOT EXISTS idx_room_npcs_room_side ON room_npcs(room_id, combat_side, status);

CREATE INDEX IF NOT EXISTS idx_monster_discoveries_user ON monster_discoveries(user_id, investigated);

CREATE TABLE IF NOT EXISTS combat_entity_statuses (
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
    );

CREATE INDEX IF NOT EXISTS idx_combat_entity_statuses_lookup ON combat_entity_statuses(room_id,entity_type,entity_id,active);

CREATE TABLE IF NOT EXISTS clue_templates (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(160) NOT NULL,
      category VARCHAR(80) NOT NULL DEFAULT '一般線索',
      description TEXT NOT NULL DEFAULT '',
      image_url TEXT NOT NULL DEFAULT '',
      linked_rule_id BIGINT,
      reveal_level INTEGER NOT NULL DEFAULT 1,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

CREATE TABLE IF NOT EXISTS character_clues (
      clue_id BIGINT NOT NULL REFERENCES clue_templates(id) ON DELETE CASCADE,
      user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      note TEXT NOT NULL DEFAULT '',
      acquired_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      PRIMARY KEY(clue_id,user_id)
    );

CREATE TABLE IF NOT EXISTS status_templates (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(120) NOT NULL,
      category VARCHAR(60) NOT NULL DEFAULT '狀態',
      description TEXT NOT NULL DEFAULT '',
      duration_rounds INTEGER NOT NULL DEFAULT 0,
      effects JSONB NOT NULL DEFAULT '{}'::jsonb,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

CREATE TABLE IF NOT EXISTS character_statuses (
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
    );

CREATE TABLE IF NOT EXISTS room_map_nodes (
      id BIGSERIAL PRIMARY KEY,
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      name VARCHAR(120) NOT NULL,
      description TEXT NOT NULL DEFAULT '',
      image_url TEXT NOT NULL DEFAULT '',
      connections JSONB NOT NULL DEFAULT '[]'::jsonb,
      hidden BOOLEAN NOT NULL DEFAULT FALSE,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

ALTER TABLE rooms ADD COLUMN IF NOT EXISTS current_map_node_id BIGINT REFERENCES room_map_nodes(id) ON DELETE SET NULL;

CREATE TABLE IF NOT EXISTS hidden_rolls (
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
    );

CREATE TABLE IF NOT EXISTS item_templates (
      id BIGSERIAL PRIMARY KEY,
      name VARCHAR(120) NOT NULL UNIQUE,
      category VARCHAR(80) NOT NULL DEFAULT '道具',
      item_type VARCHAR(30) NOT NULL DEFAULT 'normal',
      description TEXT NOT NULL DEFAULT '',
      effects JSONB NOT NULL DEFAULT '{}'::jsonb,
      consume_on_use BOOLEAN NOT NULL DEFAULT TRUE,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

ALTER TABLE status_templates ADD COLUMN IF NOT EXISTS stack_mode VARCHAR(20) NOT NULL DEFAULT 'refresh';

ALTER TABLE status_templates ADD COLUMN IF NOT EXISTS max_stacks INTEGER NOT NULL DEFAULT 1;

ALTER TABLE status_templates ADD COLUMN IF NOT EXISTS dispellable BOOLEAN NOT NULL DEFAULT TRUE;

ALTER TABLE status_templates ADD COLUMN IF NOT EXISTS tags JSONB NOT NULL DEFAULT '[]'::jsonb;

ALTER TABLE character_statuses ADD COLUMN IF NOT EXISTS stacks INTEGER NOT NULL DEFAULT 1;

ALTER TABLE character_statuses ADD COLUMN IF NOT EXISTS stack_mode VARCHAR(20) NOT NULL DEFAULT 'refresh';

ALTER TABLE character_statuses ADD COLUMN IF NOT EXISTS max_stacks INTEGER NOT NULL DEFAULT 1;

ALTER TABLE character_statuses ADD COLUMN IF NOT EXISTS dispellable BOOLEAN NOT NULL DEFAULT TRUE;

ALTER TABLE character_statuses ADD COLUMN IF NOT EXISTS expires_by_round BOOLEAN NOT NULL DEFAULT FALSE;

CREATE INDEX IF NOT EXISTS idx_character_statuses_user_active ON character_statuses(user_id,active);

CREATE INDEX IF NOT EXISTS idx_character_clues_user ON character_clues(user_id);

CREATE INDEX IF NOT EXISTS idx_room_map_nodes_room ON room_map_nodes(room_id);

CREATE INDEX IF NOT EXISTS idx_hidden_rolls_room_created ON hidden_rolls(room_id,created_at DESC);

CREATE TABLE IF NOT EXISTS horror_rules (
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
    );

CREATE TABLE IF NOT EXISTS rule_discoveries (
      rule_id BIGINT NOT NULL REFERENCES horror_rules(id) ON DELETE CASCADE,
      user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      knowledge_level INTEGER NOT NULL DEFAULT 1,
      note TEXT NOT NULL DEFAULT '',
      discovered_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      PRIMARY KEY(rule_id,user_id)
    );

CREATE INDEX IF NOT EXISTS idx_rule_discoveries_user ON rule_discoveries(user_id);

CREATE INDEX IF NOT EXISTS idx_horror_rules_room_active ON horror_rules(room_id, active);

CREATE TABLE IF NOT EXISTS room_teams (
      id BIGSERIAL PRIMARY KEY,
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      name VARCHAR(120) NOT NULL,
      leader_user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,
      current_map_node_id BIGINT REFERENCES room_map_nodes(id) ON DELETE SET NULL,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

CREATE TABLE IF NOT EXISTS room_team_members (
      team_id BIGINT NOT NULL REFERENCES room_teams(id) ON DELETE CASCADE,
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      joined_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      PRIMARY KEY(team_id,user_id),
      UNIQUE(room_id,user_id)
    );

CREATE TABLE IF NOT EXISTS team_shared_clues (
      team_id BIGINT NOT NULL REFERENCES room_teams(id) ON DELETE CASCADE,
      clue_id BIGINT NOT NULL REFERENCES clue_templates(id) ON DELETE CASCADE,
      shared_by_user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,
      note TEXT NOT NULL DEFAULT '',
      shared_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      PRIMARY KEY(team_id,clue_id)
    );

CREATE TABLE IF NOT EXISTS team_shared_rules (
      team_id BIGINT NOT NULL REFERENCES room_teams(id) ON DELETE CASCADE,
      rule_id BIGINT NOT NULL REFERENCES horror_rules(id) ON DELETE CASCADE,
      knowledge_level INTEGER NOT NULL DEFAULT 1,
      shared_by_user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,
      note TEXT NOT NULL DEFAULT '',
      shared_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      PRIMARY KEY(team_id,rule_id)
    );

CREATE TABLE IF NOT EXISTS personal_visions (
      id BIGSERIAL PRIMARY KEY,
      room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
      user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      title VARCHAR(180) NOT NULL DEFAULT '私人視野',
      content TEXT NOT NULL DEFAULT '',
      vision_type VARCHAR(40) NOT NULL DEFAULT 'perception',
      shareable BOOLEAN NOT NULL DEFAULT TRUE,
      is_read BOOLEAN NOT NULL DEFAULT FALSE,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

CREATE TABLE IF NOT EXISTS team_shared_notes (
      id BIGSERIAL PRIMARY KEY,
      team_id BIGINT NOT NULL REFERENCES room_teams(id) ON DELETE CASCADE,
      shared_by_user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,
      title VARCHAR(180) NOT NULL DEFAULT '共享情報',
      content TEXT NOT NULL DEFAULT '',
      note_type VARCHAR(40) NOT NULL DEFAULT 'intel',
      source_vision_id BIGINT REFERENCES personal_visions(id) ON DELETE SET NULL,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

CREATE TABLE IF NOT EXISTS team_messages (
      id BIGSERIAL PRIMARY KEY,
      team_id BIGINT NOT NULL REFERENCES room_teams(id) ON DELETE CASCADE,
      user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,
      content TEXT NOT NULL,
      created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

CREATE INDEX IF NOT EXISTS idx_room_teams_room ON room_teams(room_id);

CREATE INDEX IF NOT EXISTS idx_room_team_members_room ON room_team_members(room_id);

CREATE INDEX IF NOT EXISTS idx_room_team_members_user ON room_team_members(user_id);

CREATE INDEX IF NOT EXISTS idx_personal_visions_user_room ON personal_visions(user_id,room_id,created_at DESC);

CREATE INDEX IF NOT EXISTS idx_team_messages_team_created ON team_messages(team_id,created_at DESC);

ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS pollution INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_members ADD COLUMN IF NOT EXISTS current_sanity INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_members ADD COLUMN IF NOT EXISTS max_sanity INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_members ADD COLUMN IF NOT EXISTS sanity_initialized BOOLEAN NOT NULL DEFAULT FALSE;

ALTER TABLE room_members ADD COLUMN IF NOT EXISTS sanity_stage VARCHAR(40) NOT NULL DEFAULT 'stable';

ALTER TABLE room_members ADD COLUMN IF NOT EXISTS seven_sin VARCHAR(40);

ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS noise_level INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS alert_level INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS noise_threshold INTEGER NOT NULL DEFAULT 30;

ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS map_node_id BIGINT REFERENCES room_map_nodes(id) ON DELETE SET NULL;

ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS alert_level INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS chase_team_id BIGINT;

ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS boss_phase_index INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS boss_phase_name VARCHAR(160) NOT NULL DEFAULT '';

ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS map_node_id BIGINT REFERENCES room_map_nodes(id) ON DELETE SET NULL;

ALTER TABLE room_dungeons ADD COLUMN IF NOT EXISTS map_node_id BIGINT REFERENCES room_map_nodes(id) ON DELETE SET NULL;

CREATE TABLE IF NOT EXISTS scheduled_world_events (
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
    );

CREATE TABLE IF NOT EXISTS room_chases (
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
    );

CREATE INDEX IF NOT EXISTS idx_room_monsters_node ON room_monsters(room_id,map_node_id,status);

CREATE INDEX IF NOT EXISTS idx_room_npcs_node ON room_npcs(room_id,map_node_id,status);

CREATE INDEX IF NOT EXISTS idx_room_dungeons_node ON room_dungeons(room_id,map_node_id,status);

CREATE INDEX IF NOT EXISTS idx_scheduled_world_events_room ON scheduled_world_events(room_id,active);

CREATE INDEX IF NOT EXISTS idx_room_chases_room_team ON room_chases(room_id,team_id,status);

ALTER TABLE horror_rules ADD COLUMN IF NOT EXISTS truth_state VARCHAR(20) NOT NULL DEFAULT 'true';

ALTER TABLE horror_rules ADD COLUMN IF NOT EXISTS active_from_total BIGINT;

ALTER TABLE horror_rules ADD COLUMN IF NOT EXISTS active_until_total BIGINT;

ALTER TABLE horror_rules ADD COLUMN IF NOT EXISTS change_note TEXT NOT NULL DEFAULT '';

CREATE TABLE IF NOT EXISTS npc_relationships (
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
  );

CREATE INDEX IF NOT EXISTS idx_npc_relationships_room ON npc_relationships(room_id,npc_template_id);

CREATE INDEX IF NOT EXISTS idx_npc_relationships_user ON npc_relationships(user_id,room_id);

CREATE TABLE IF NOT EXISTS search_containers (
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
  );

CREATE TABLE IF NOT EXISTS search_records (
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
  );

CREATE INDEX IF NOT EXISTS idx_search_containers_room_node ON search_containers(room_id,map_node_id,active);

CREATE TABLE IF NOT EXISTS room_savepoints (
    id BIGSERIAL PRIMARY KEY,
    room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
    label VARCHAR(180) NOT NULL DEFAULT '手動存檔',
    snapshot JSONB NOT NULL,
    created_by BIGINT REFERENCES users(id) ON DELETE SET NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  );

CREATE INDEX IF NOT EXISTS idx_room_savepoints_room_created ON room_savepoints(room_id,created_at DESC);

CREATE TABLE IF NOT EXISTS character_savepoints (
    id BIGSERIAL PRIMARY KEY,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    label VARCHAR(180) NOT NULL DEFAULT '自動安全備份',
    snapshot JSONB NOT NULL,
    created_by BIGINT REFERENCES users(id) ON DELETE SET NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  );

CREATE INDEX IF NOT EXISTS idx_character_savepoints_user_created ON character_savepoints(user_id,created_at DESC);

CREATE TABLE IF NOT EXISTS audit_logs (
    id BIGSERIAL PRIMARY KEY,
    actor_user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,
    actor_username VARCHAR(80) NOT NULL DEFAULT '',
    method VARCHAR(10) NOT NULL DEFAULT '',
    action VARCHAR(120) NOT NULL DEFAULT '',
    path TEXT NOT NULL DEFAULT '',
    target_type VARCHAR(60) NOT NULL DEFAULT '',
    target_id VARCHAR(160) NOT NULL DEFAULT '',
    room_id BIGINT REFERENCES rooms(id) ON DELETE SET NULL,
    status_code INTEGER NOT NULL DEFAULT 0,
    summary TEXT NOT NULL DEFAULT '',
    metadata JSONB NOT NULL DEFAULT '{}'::jsonb,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  );

CREATE INDEX IF NOT EXISTS idx_audit_logs_created ON audit_logs(created_at DESC);

CREATE INDEX IF NOT EXISTS idx_audit_logs_actor ON audit_logs(actor_user_id,created_at DESC);

CREATE INDEX IF NOT EXISTS idx_audit_logs_action ON audit_logs(action,created_at DESC);

CREATE INDEX IF NOT EXISTS idx_audit_logs_room ON audit_logs(room_id,created_at DESC);

ALTER TABLE rooms ADD COLUMN IF NOT EXISTS time_hidden BOOLEAN NOT NULL DEFAULT TRUE;

ALTER TABLE rooms ADD COLUMN IF NOT EXISTS time_distortion_offset_minutes INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS time_offset_minutes INTEGER NOT NULL DEFAULT 0;

ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS grants_time_view BOOLEAN NOT NULL DEFAULT FALSE;

ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS grants_true_time BOOLEAN NOT NULL DEFAULT FALSE;

ALTER TABLE rooms ADD COLUMN IF NOT EXISTS time_rate NUMERIC(6,2) NOT NULL DEFAULT 1;

ALTER TABLE rooms ADD COLUMN IF NOT EXISTS time_flow_offset_minutes INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS time_rate NUMERIC(6,2) NOT NULL DEFAULT 1;

ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS time_flow_offset_minutes INTEGER NOT NULL DEFAULT 0;

ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS investigation_dc INTEGER NOT NULL DEFAULT 50;

ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS discovery_hint TEXT NOT NULL DEFAULT '';

ALTER TABLE search_records ADD COLUMN IF NOT EXISTS discovered BOOLEAN NOT NULL DEFAULT FALSE;

CREATE TABLE IF NOT EXISTS map_node_discoveries (
    node_id BIGINT NOT NULL REFERENCES room_map_nodes(id) ON DELETE CASCADE,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    discovered_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(node_id,user_id)
  );

CREATE INDEX IF NOT EXISTS idx_map_node_discoveries_user ON map_node_discoveries(user_id);

ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS public_hint TEXT NOT NULL DEFAULT '';

ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS investigation_dc INTEGER NOT NULL DEFAULT 50;

ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS requires_investigation BOOLEAN NOT NULL DEFAULT FALSE;

CREATE TABLE IF NOT EXISTS npc_discoveries (
    room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
    npc_template_id BIGINT NOT NULL REFERENCES npc_templates(id) ON DELETE CASCADE,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    investigated BOOLEAN NOT NULL DEFAULT FALSE,
    last_roll INTEGER,
    last_rate INTEGER,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(room_id,npc_template_id,user_id)
  );

ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS requires_identification BOOLEAN NOT NULL DEFAULT FALSE;

ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS unknown_name VARCHAR(120) NOT NULL DEFAULT '未知物品';

ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS identification_attribute VARCHAR(30) NOT NULL DEFAULT 'spirit';

ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS identification_dc INTEGER NOT NULL DEFAULT 50;

CREATE TABLE IF NOT EXISTS item_identifications (
    item_template_id BIGINT NOT NULL REFERENCES item_templates(id) ON DELETE CASCADE,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    identified_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(item_template_id,user_id)
  );

ALTER TABLE clue_templates ADD COLUMN IF NOT EXISTS room_id BIGINT REFERENCES rooms(id) ON DELETE CASCADE;

ALTER TABLE clue_templates ADD COLUMN IF NOT EXISTS map_node_id BIGINT REFERENCES room_map_nodes(id) ON DELETE SET NULL;

ALTER TABLE clue_templates ADD COLUMN IF NOT EXISTS auto_discover BOOLEAN NOT NULL DEFAULT FALSE;

ALTER TABLE clue_templates ADD COLUMN IF NOT EXISTS investigation_dc INTEGER NOT NULL DEFAULT 50;

CREATE TABLE IF NOT EXISTS clue_combinations (
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
  );

CREATE TABLE IF NOT EXISTS clue_combination_unlocks (
    combination_id BIGINT NOT NULL REFERENCES clue_combinations(id) ON DELETE CASCADE,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    unlocked_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(combination_id,user_id)
  );

ALTER TABLE horror_rules ADD COLUMN IF NOT EXISTS required_profession VARCHAR(100) NOT NULL DEFAULT '';

ALTER TABLE horror_rules ADD COLUMN IF NOT EXISTS required_bloodline VARCHAR(120) NOT NULL DEFAULT '';

ALTER TABLE horror_rules ADD COLUMN IF NOT EXISTS required_item VARCHAR(120) NOT NULL DEFAULT '';

ALTER TABLE horror_rules ADD COLUMN IF NOT EXISTS min_pollution INTEGER NOT NULL DEFAULT 0;

ALTER TABLE horror_rules ADD COLUMN IF NOT EXISTS max_pollution INTEGER NOT NULL DEFAULT 100;

ALTER TABLE scheduled_world_events ADD COLUMN IF NOT EXISTS time_basis VARCHAR(20) NOT NULL DEFAULT 'world';

ALTER TABLE scheduled_world_events ADD COLUMN IF NOT EXISTS action_type VARCHAR(30) NOT NULL DEFAULT 'message';

ALTER TABLE scheduled_world_events ADD COLUMN IF NOT EXISTS action_payload JSONB NOT NULL DEFAULT '{}'::jsonb;

CREATE TABLE IF NOT EXISTS great_way_templates (
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
  );

CREATE TABLE IF NOT EXISTS faith_deities (
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
  );

CREATE TABLE IF NOT EXISTS character_great_ways (
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    great_way_id BIGINT NOT NULL REFERENCES great_way_templates(id) ON DELETE CASCADE,
    rank VARCHAR(10) NOT NULL DEFAULT 'G',
    progress INTEGER NOT NULL DEFAULT 0,
    active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(user_id,great_way_id)
  );

CREATE TABLE IF NOT EXISTS character_faiths (
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    deity_id BIGINT NOT NULL REFERENCES faith_deities(id) ON DELETE CASCADE,
    rank VARCHAR(10) NOT NULL DEFAULT 'G',
    devotion INTEGER NOT NULL DEFAULT 0,
    favor INTEGER NOT NULL DEFAULT 0,
    active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(user_id,deity_id)
  );

CREATE TABLE IF NOT EXISTS custom_wheels (
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
  );

CREATE TABLE IF NOT EXISTS wheel_spins (
    id BIGSERIAL PRIMARY KEY,
    wheel_id BIGINT NOT NULL REFERENCES custom_wheels(id) ON DELETE CASCADE,
    room_id BIGINT REFERENCES rooms(id) ON DELETE CASCADE,
    user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,
    result_index INTEGER NOT NULL DEFAULT 0,
    result_label VARCHAR(300) NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  );

CREATE INDEX IF NOT EXISTS idx_character_great_ways_user ON character_great_ways(user_id,active);

CREATE INDEX IF NOT EXISTS idx_character_faiths_user ON character_faiths(user_id,active);

CREATE INDEX IF NOT EXISTS idx_custom_wheels_room ON custom_wheels(room_id,active);

CREATE INDEX IF NOT EXISTS idx_wheel_spins_wheel ON wheel_spins(wheel_id,created_at DESC);

CREATE TABLE IF NOT EXISTS faction_templates (
    id BIGSERIAL PRIMARY KEY,
    name VARCHAR(160) NOT NULL UNIQUE,
    description TEXT NOT NULL DEFAULT '',
    alignment VARCHAR(40) NOT NULL DEFAULT 'neutral',
    active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  );

CREATE TABLE IF NOT EXISTS character_reputations (
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    faction_id BIGINT NOT NULL REFERENCES faction_templates(id) ON DELETE CASCADE,
    reputation INTEGER NOT NULL DEFAULT 0,
    notes TEXT NOT NULL DEFAULT '',
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(user_id,faction_id)
  );

CREATE TABLE IF NOT EXISTS npc_dialogue_entries (
    id BIGSERIAL PRIMARY KEY,
    npc_template_id BIGINT NOT NULL REFERENCES npc_templates(id) ON DELETE CASCADE,
    choice_text VARCHAR(300) NOT NULL,
    npc_text TEXT NOT NULL DEFAULT '',
    required_faction_id BIGINT REFERENCES faction_templates(id) ON DELETE SET NULL,
    min_reputation INTEGER NOT NULL DEFAULT -9999,
    min_affinity INTEGER NOT NULL DEFAULT -100,
    min_trust INTEGER NOT NULL DEFAULT -100,
    max_hostility INTEGER NOT NULL DEFAULT 100,
    required_item VARCHAR(120) NOT NULL DEFAULT '',
    required_task_id BIGINT,
    grant_task_id BIGINT,
    relation_delta JSONB NOT NULL DEFAULT '{}'::jsonb,
    once_per_user BOOLEAN NOT NULL DEFAULT FALSE,
    sort_order INTEGER NOT NULL DEFAULT 0,
    active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  );

CREATE TABLE IF NOT EXISTS npc_dialogue_history (
    room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    npc_template_id BIGINT NOT NULL REFERENCES npc_templates(id) ON DELETE CASCADE,
    dialogue_entry_id BIGINT NOT NULL REFERENCES npc_dialogue_entries(id) ON DELETE CASCADE,
    used_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(room_id,user_id,dialogue_entry_id)
  );

CREATE TABLE IF NOT EXISTS npc_shop_items (
    id BIGSERIAL PRIMARY KEY,
    npc_template_id BIGINT NOT NULL REFERENCES npc_templates(id) ON DELETE CASCADE,
    name VARCHAR(160) NOT NULL,
    item_type VARCHAR(30) NOT NULL DEFAULT 'item',
    item_category VARCHAR(80) NOT NULL DEFAULT '',
    description TEXT NOT NULL DEFAULT '',
    currency VARCHAR(20) NOT NULL DEFAULT 'weird',
    price BIGINT NOT NULL DEFAULT 0,
    quantity INTEGER NOT NULL DEFAULT 1,
    stock INTEGER NOT NULL DEFAULT -1,
    required_faction_id BIGINT REFERENCES faction_templates(id) ON DELETE SET NULL,
    min_reputation INTEGER NOT NULL DEFAULT -9999,
    min_affinity INTEGER NOT NULL DEFAULT -100,
    active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  );

CREATE TABLE IF NOT EXISTS npc_shop_stock (
    room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
    shop_item_id BIGINT NOT NULL REFERENCES npc_shop_items(id) ON DELETE CASCADE,
    remaining INTEGER NOT NULL DEFAULT 0,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(room_id,shop_item_id)
  );

CREATE INDEX IF NOT EXISTS idx_character_reputations_user ON character_reputations(user_id,reputation DESC);

CREATE INDEX IF NOT EXISTS idx_npc_dialogue_npc ON npc_dialogue_entries(npc_template_id,active,sort_order,id);

CREATE INDEX IF NOT EXISTS idx_npc_shop_npc ON npc_shop_items(npc_template_id,active,id);

CREATE INDEX IF NOT EXISTS idx_npc_dialogue_history_user ON npc_dialogue_history(room_id,user_id,npc_template_id);

CREATE TABLE IF NOT EXISTS faction_relations (
    source_faction_id BIGINT NOT NULL REFERENCES faction_templates(id) ON DELETE CASCADE,
    target_faction_id BIGINT NOT NULL REFERENCES faction_templates(id) ON DELETE CASCADE,
    relation_score INTEGER NOT NULL DEFAULT 0,
    label VARCHAR(80) NOT NULL DEFAULT '',
    notes TEXT NOT NULL DEFAULT '',
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(source_faction_id,target_faction_id),
    CHECK(source_faction_id <> target_faction_id)
  );

CREATE INDEX IF NOT EXISTS idx_faction_relations_source ON faction_relations(source_faction_id,relation_score DESC);

ALTER TABLE npc_shop_items ADD COLUMN IF NOT EXISTS discount_faction_id BIGINT REFERENCES faction_templates(id) ON DELETE SET NULL;

ALTER TABLE npc_shop_items ADD COLUMN IF NOT EXISTS discount_per_100 NUMERIC(8,3) NOT NULL DEFAULT 0;

ALTER TABLE npc_shop_items ADD COLUMN IF NOT EXISTS max_discount_percent NUMERIC(8,3) NOT NULL DEFAULT 0;

ALTER TABLE npc_shop_items ADD COLUMN IF NOT EXISTS max_hostility INTEGER NOT NULL DEFAULT 100;

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
    );

CREATE TABLE IF NOT EXISTS task_participants (
      task_id BIGINT NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
      user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      status VARCHAR(20) NOT NULL DEFAULT 'accepted',
      accepted_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
      completed_at TIMESTAMPTZ,
      PRIMARY KEY(task_id, user_id)
    );

ALTER TABLE tasks ADD COLUMN IF NOT EXISTS profession_id BIGINT REFERENCES profession_templates(id) ON DELETE SET NULL;

ALTER TABLE tasks ADD COLUMN IF NOT EXISTS profession_rank VARCHAR(10);

ALTER TABLE tasks ADD COLUMN IF NOT EXISTS prerequisite_task_ids JSONB NOT NULL DEFAULT '[]'::jsonb;

ALTER TABLE tasks ADD COLUMN IF NOT EXISTS chain_name VARCHAR(160) NOT NULL DEFAULT '';

ALTER TABLE tasks ADD COLUMN IF NOT EXISTS chain_order INTEGER NOT NULL DEFAULT 0;

ALTER TABLE tasks ADD COLUMN IF NOT EXISTS required_faction_id BIGINT REFERENCES faction_templates(id) ON DELETE SET NULL;

ALTER TABLE tasks ADD COLUMN IF NOT EXISTS min_reputation INTEGER NOT NULL DEFAULT -9999;

ALTER TABLE tasks ADD COLUMN IF NOT EXISTS branch_group VARCHAR(160) NOT NULL DEFAULT '';

ALTER TABLE tasks ADD COLUMN IF NOT EXISTS branch_label VARCHAR(160) NOT NULL DEFAULT '';

ALTER TABLE tasks ADD COLUMN IF NOT EXISTS exclusive_group VARCHAR(160) NOT NULL DEFAULT '';

ALTER TABLE tasks ADD COLUMN IF NOT EXISTS ending_label VARCHAR(200) NOT NULL DEFAULT '';

CREATE INDEX IF NOT EXISTS idx_tasks_status_type ON tasks(status, task_type);

CREATE INDEX IF NOT EXISTS idx_tasks_target_user ON tasks(target_user_id);

CREATE INDEX IF NOT EXISTS idx_tasks_profession ON tasks(profession_id);

CREATE INDEX IF NOT EXISTS idx_tasks_chain ON tasks(chain_name,chain_order);

CREATE INDEX IF NOT EXISTS idx_tasks_required_faction ON tasks(required_faction_id,min_reputation);

CREATE INDEX IF NOT EXISTS idx_tasks_branch_group ON tasks(branch_group,chain_order);

CREATE INDEX IF NOT EXISTS idx_tasks_exclusive_group ON tasks(exclusive_group);

CREATE INDEX IF NOT EXISTS idx_task_participants_user ON task_participants(user_id);

-- =========================================================
-- 46-cpp.7：效率／分類／收藏／百科／通知／批量匯入擴充
-- =========================================================

-- [功能備註｜全站搜尋／標籤] 共用標籤，可掛在技能、道具、怪物、NPC、任務、商店商品等實體。
CREATE TABLE IF NOT EXISTS content_tags (
    id BIGSERIAL PRIMARY KEY,
    name VARCHAR(80) NOT NULL UNIQUE,
    color VARCHAR(32) NOT NULL DEFAULT '',
    active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  );

CREATE TABLE IF NOT EXISTS entity_tags (
    tag_id BIGINT NOT NULL REFERENCES content_tags(id) ON DELETE CASCADE,
    entity_type VARCHAR(40) NOT NULL,
    entity_key VARCHAR(120) NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(tag_id, entity_type, entity_key)
  );
CREATE INDEX IF NOT EXISTS idx_entity_tags_entity ON entity_tags(entity_type,entity_key);

-- [功能備註｜收藏] 玩家常用技能／道具／怪物／NPC／任務等收藏。
CREATE TABLE IF NOT EXISTS user_favorites (
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    entity_type VARCHAR(40) NOT NULL,
    entity_key VARCHAR(120) NOT NULL,
    label VARCHAR(160) NOT NULL DEFAULT '',
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(user_id, entity_type, entity_key)
  );
CREATE INDEX IF NOT EXISTS idx_user_favorites_user ON user_favorites(user_id,created_at DESC);

-- [功能備註｜通知中心] DM 或系統可推送任務、物品、輪到行動等通知。
CREATE TABLE IF NOT EXISTS user_notifications (
    id BIGSERIAL PRIMARY KEY,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    notification_type VARCHAR(40) NOT NULL DEFAULT 'info',
    title VARCHAR(160) NOT NULL,
    message TEXT NOT NULL DEFAULT '',
    data JSONB NOT NULL DEFAULT '{}'::jsonb,
    read_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  );
CREATE INDEX IF NOT EXISTS idx_notifications_user_read ON user_notifications(user_id,read_at,created_at DESC);

-- [功能備註｜世界百科] DM 建立百科條目，玩家可依公開設定或解鎖紀錄觀看。
CREATE TABLE IF NOT EXISTS codex_entries (
    id BIGSERIAL PRIMARY KEY,
    category VARCHAR(80) NOT NULL DEFAULT '世界',
    title VARCHAR(160) NOT NULL,
    summary TEXT NOT NULL DEFAULT '',
    content TEXT NOT NULL DEFAULT '',
    image_url TEXT NOT NULL DEFAULT '',
    public_by_default BOOLEAN NOT NULL DEFAULT FALSE,
    tags JSONB NOT NULL DEFAULT '[]'::jsonb,
    sort_order INTEGER NOT NULL DEFAULT 100,
    active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  );
CREATE INDEX IF NOT EXISTS idx_codex_entries_category ON codex_entries(category,sort_order,id);

CREATE TABLE IF NOT EXISTS codex_unlocks (
    entry_id BIGINT NOT NULL REFERENCES codex_entries(id) ON DELETE CASCADE,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    note TEXT NOT NULL DEFAULT '',
    unlocked_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(entry_id,user_id)
  );

-- [功能備註｜商店進階] 購買／出售紀錄與商品庫存、限購、折扣設定。
CREATE TABLE IF NOT EXISTS shop_transactions (
    id BIGSERIAL PRIMARY KEY,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    shop_item_id BIGINT REFERENCES shop_items(id) ON DELETE SET NULL,
    direction VARCHAR(20) NOT NULL DEFAULT 'buy',
    item_name VARCHAR(160) NOT NULL,
    quantity INTEGER NOT NULL DEFAULT 1,
    unit_price BIGINT NOT NULL DEFAULT 0,
    total_price BIGINT NOT NULL DEFAULT 0,
    meta JSONB NOT NULL DEFAULT '{}'::jsonb,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  );
CREATE INDEX IF NOT EXISTS idx_shop_transactions_user ON shop_transactions(user_id,created_at DESC);

ALTER TABLE shop_items ADD COLUMN IF NOT EXISTS stock INTEGER NOT NULL DEFAULT -1;
ALTER TABLE shop_items ADD COLUMN IF NOT EXISTS purchase_limit INTEGER NOT NULL DEFAULT -1;
ALTER TABLE shop_items ADD COLUMN IF NOT EXISTS discount_percent NUMERIC(8,3) NOT NULL DEFAULT 0;
ALTER TABLE shop_items ADD COLUMN IF NOT EXISTS sell_percent NUMERIC(8,3) NOT NULL DEFAULT 50;
ALTER TABLE shop_items ADD COLUMN IF NOT EXISTS tags JSONB NOT NULL DEFAULT '[]'::jsonb;

-- [功能備註｜技能樹／分類] 技能前置、技能樹分組與標籤。
ALTER TABLE skill_templates ADD COLUMN IF NOT EXISTS prerequisite_skill_ids JSONB NOT NULL DEFAULT '[]'::jsonb;
ALTER TABLE skill_templates ADD COLUMN IF NOT EXISTS tree_group VARCHAR(120) NOT NULL DEFAULT '';
ALTER TABLE skill_templates ADD COLUMN IF NOT EXISTS tree_order INTEGER NOT NULL DEFAULT 0;
ALTER TABLE skill_templates ADD COLUMN IF NOT EXISTS tags JSONB NOT NULL DEFAULT '[]'::jsonb;
ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS tags JSONB NOT NULL DEFAULT '[]'::jsonb;

-- [功能備註｜自訂資源條] 角色可建立魔力、怒氣、鍛造值、鑄造值、彈藥等自訂資源。
ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS custom_resources JSONB NOT NULL DEFAULT '[]'::jsonb;

-- [功能備註｜魔法學／儀式學／元素儲存｜47-cpp.8]
-- 角色初始七元素儲存上限全為 0；上限可由技能、道具、魔法／儀式效果或永久基礎加成提高。
ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS magic_points BIGINT NOT NULL DEFAULT 0;
ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS ritual_points BIGINT NOT NULL DEFAULT 0;
ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS element_storage JSONB NOT NULL DEFAULT '{"暗":0,"光":0,"金":0,"木":0,"水":0,"火":0,"土":0}'::jsonb;
ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS element_storage_base_caps JSONB NOT NULL DEFAULT '{"暗":0,"光":0,"金":0,"木":0,"水":0,"火":0,"土":0}'::jsonb;

CREATE TABLE IF NOT EXISTS magic_studies (
  id BIGSERIAL PRIMARY KEY,
  name VARCHAR(120) NOT NULL UNIQUE,
  category VARCHAR(80) NOT NULL DEFAULT '一般魔法',
  rank VARCHAR(10) NOT NULL DEFAULT 'G',
  description TEXT NOT NULL DEFAULT '',
  point_cost BIGINT NOT NULL DEFAULT 1,
  prerequisite_ids JSONB NOT NULL DEFAULT '[]'::jsonb,
  effects JSONB NOT NULL DEFAULT '{}'::jsonb,
  active BOOLEAN NOT NULL DEFAULT TRUE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS ritual_studies (
  id BIGSERIAL PRIMARY KEY,
  name VARCHAR(120) NOT NULL UNIQUE,
  category VARCHAR(80) NOT NULL DEFAULT '一般儀式',
  rank VARCHAR(10) NOT NULL DEFAULT 'G',
  description TEXT NOT NULL DEFAULT '',
  point_cost BIGINT NOT NULL DEFAULT 1,
  prerequisite_ids JSONB NOT NULL DEFAULT '[]'::jsonb,
  effects JSONB NOT NULL DEFAULT '{}'::jsonb,
  active BOOLEAN NOT NULL DEFAULT TRUE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS character_magics (
  user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  magic_id BIGINT NOT NULL REFERENCES magic_studies(id) ON DELETE CASCADE,
  learned_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY(user_id, magic_id)
);

CREATE TABLE IF NOT EXISTS character_rituals (
  user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  ritual_id BIGINT NOT NULL REFERENCES ritual_studies(id) ON DELETE CASCADE,
  learned_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY(user_id, ritual_id)
);

CREATE INDEX IF NOT EXISTS idx_character_magics_user ON character_magics(user_id);
CREATE INDEX IF NOT EXISTS idx_character_rituals_user ON character_rituals(user_id);
CREATE INDEX IF NOT EXISTS idx_magic_studies_active ON magic_studies(active, category, name);
CREATE INDEX IF NOT EXISTS idx_ritual_studies_active ON ritual_studies(active, category, name);

-- =========================================================
-- 48-cpp.9：規則書／頭像／場景通訊／元素自生／離線 AI
-- =========================================================

-- [功能備註｜規則書] 獨立於怪談隱藏規則的「公開規則書」，由 DM 編輯、玩家閱讀。
CREATE TABLE IF NOT EXISTS rulebook_entries (
  id BIGSERIAL PRIMARY KEY,
  title VARCHAR(180) NOT NULL,
  category VARCHAR(80) NOT NULL DEFAULT '一般',
  content TEXT NOT NULL DEFAULT '',
  sort_order INTEGER NOT NULL DEFAULT 100,
  public BOOLEAN NOT NULL DEFAULT TRUE,
  active BOOLEAN NOT NULL DEFAULT TRUE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX IF NOT EXISTS idx_rulebook_entries_order ON rulebook_entries(active,public,sort_order,id);

-- [功能備註｜玩家／NPC 頭像] NPC 原本已有 image_url；玩家新增 avatar_url。
ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS avatar_url TEXT NOT NULL DEFAULT '';
ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS merchant_enabled BOOLEAN NOT NULL DEFAULT FALSE;
ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS merchant_title VARCHAR(120) NOT NULL DEFAULT '';

-- [功能備註｜場景／通訊] 個人位置；有隊伍時仍以 room_teams.current_map_node_id 為優先。
ALTER TABLE room_members ADD COLUMN IF NOT EXISTS current_map_node_id BIGINT REFERENCES room_map_nodes(id) ON DELETE SET NULL;
ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS communication_blocked BOOLEAN NOT NULL DEFAULT FALSE;
ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS communication_note TEXT NOT NULL DEFAULT '';
ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS auto_generated BOOLEAN NOT NULL DEFAULT FALSE;

-- [功能備註｜離線 AI] 玩家斷線後依隊伍／探索規則接管；重新上線即停用 active。
ALTER TABLE room_members ADD COLUMN IF NOT EXISTS offline_ai_enabled BOOLEAN NOT NULL DEFAULT TRUE;
ALTER TABLE room_members ADD COLUMN IF NOT EXISTS offline_ai_active BOOLEAN NOT NULL DEFAULT FALSE;
ALTER TABLE room_members ADD COLUMN IF NOT EXISTS last_seen_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP;
ALTER TABLE room_members ADD COLUMN IF NOT EXISTS ai_last_action_at TIMESTAMPTZ;

CREATE TABLE IF NOT EXISTS room_team_invites (
  id BIGSERIAL PRIMARY KEY,
  room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
  team_id BIGINT NOT NULL REFERENCES room_teams(id) ON DELETE CASCADE,
  target_user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  invited_by_user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,
  status VARCHAR(20) NOT NULL DEFAULT 'pending',
  response_source VARCHAR(40) NOT NULL DEFAULT '',
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  responded_at TIMESTAMPTZ
);
ALTER TABLE room_team_invites DROP CONSTRAINT IF EXISTS room_team_invites_room_id_team_id_target_user_id_status_key;
CREATE UNIQUE INDEX IF NOT EXISTS uq_room_team_invites_pending ON room_team_invites(room_id,team_id,target_user_id) WHERE status='pending';
CREATE INDEX IF NOT EXISTS idx_room_team_invites_target ON room_team_invites(room_id,target_user_id,status);

-- [功能備註｜隊伍通訊] 每則訊息記錄當下可接收者，避免不同場景玩家讀到歷史內容。
ALTER TABLE team_messages ADD COLUMN IF NOT EXISTS audience_user_ids JSONB NOT NULL DEFAULT '[]'::jsonb;

-- [功能備註｜元素自生] 技能／道具 effects.element_storage_generation 以「每分鐘」為單位。
ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS element_generation_last_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP;

-- 49-cpp.10：條件式世界規則／規則怪談引擎
-- [功能備註｜規則引擎] 玩家看到的規則書與系統實際判定分離；可用 rulebook_entry_id 綁定。
CREATE TABLE IF NOT EXISTS rule_engine_rules (
  id BIGSERIAL PRIMARY KEY,
  room_id BIGINT REFERENCES rooms(id) ON DELETE CASCADE,
  rulebook_entry_id BIGINT REFERENCES rulebook_entries(id) ON DELETE SET NULL,
  code VARCHAR(100) NOT NULL DEFAULT '',
  name VARCHAR(180) NOT NULL,
  player_text TEXT NOT NULL DEFAULT '',
  dm_text TEXT NOT NULL DEFAULT '',
  trigger_event VARCHAR(80) NOT NULL DEFAULT 'PLAYER_ACTION',
  tags JSONB NOT NULL DEFAULT '[]'::jsonb,
  conditions JSONB NOT NULL DEFAULT '{"all":[]}'::jsonb,
  effects JSONB NOT NULL DEFAULT '[]'::jsonb,
  priority INTEGER NOT NULL DEFAULT 100,
  visibility VARCHAR(20) NOT NULL DEFAULT 'hidden',
  automatic BOOLEAN NOT NULL DEFAULT TRUE,
  notify_dm BOOLEAN NOT NULL DEFAULT TRUE,
  cooldown_seconds BIGINT NOT NULL DEFAULT 0,
  countdown_seconds BIGINT NOT NULL DEFAULT 0,
  max_triggers BIGINT NOT NULL DEFAULT 0,
  stop_on_match BOOLEAN NOT NULL DEFAULT FALSE,
  active BOOLEAN NOT NULL DEFAULT TRUE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX IF NOT EXISTS idx_rule_engine_rules_trigger
  ON rule_engine_rules(active,automatic,trigger_event,room_id,priority DESC,id);
CREATE INDEX IF NOT EXISTS idx_rule_engine_rules_rulebook
  ON rule_engine_rules(rulebook_entry_id);

-- [功能備註｜規則引擎狀態] 每條規則對每位玩家保存觸發/違規次數、冷卻與倒數。
CREATE TABLE IF NOT EXISTS rule_engine_rule_state (
  room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
  rule_id BIGINT NOT NULL REFERENCES rule_engine_rules(id) ON DELETE CASCADE,
  user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  trigger_count BIGINT NOT NULL DEFAULT 0,
  violation_count BIGINT NOT NULL DEFAULT 0,
  last_triggered_at TIMESTAMPTZ,
  countdown_due_at TIMESTAMPTZ,
  state JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY(room_id,rule_id,user_id)
);
CREATE INDEX IF NOT EXISTS idx_rule_engine_rule_state_user
  ON rule_engine_rule_state(room_id,user_id,updated_at DESC);

-- [功能備註｜規則引擎玩家狀態] 跨規則共用的旗標、計數器、值與倒數。
CREATE TABLE IF NOT EXISTS rule_engine_player_state (
  room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
  user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  state JSONB NOT NULL DEFAULT '{"flags":[],"counters":{},"values":{},"countdowns":{}}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY(room_id,user_id)
);

-- [功能備註｜規則引擎紀錄] DM 可回看每次結構化規則觸發的完整 context 與效果。
CREATE TABLE IF NOT EXISTS rule_engine_logs (
  id BIGSERIAL PRIMARY KEY,
  room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
  rule_id BIGINT REFERENCES rule_engine_rules(id) ON DELETE SET NULL,
  actor_user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,
  event_type VARCHAR(80) NOT NULL,
  matched BOOLEAN NOT NULL DEFAULT TRUE,
  dry_run BOOLEAN NOT NULL DEFAULT FALSE,
  context JSONB NOT NULL DEFAULT '{}'::jsonb,
  result JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX IF NOT EXISTS idx_rule_engine_logs_room
  ON rule_engine_logs(room_id,created_at DESC,id DESC);


-- 51-cpp.12：規則怪談 × 戰鬥橋接
-- [功能備註｜規則－戰鬥橋接] 房間保存特殊勝利條件與規則戰鬥旗標／數值。
ALTER TABLE rooms ADD COLUMN IF NOT EXISTS combat_victory_condition JSONB NOT NULL DEFAULT '{"type":"eliminate_enemies","auto_end":false}'::jsonb;
ALTER TABLE rooms ADD COLUMN IF NOT EXISTS rule_combat_state JSONB NOT NULL DEFAULT '{"flags":[],"values":{}}'::jsonb;
-- [功能備註｜Boss 規則階段] 怪物可被規則切換階段與保存規則專用修正。
ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS rule_boss_phase INTEGER NOT NULL DEFAULT 1;
ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS rule_modifiers JSONB NOT NULL DEFAULT '{}'::jsonb;
CREATE INDEX IF NOT EXISTS idx_room_monsters_rule_phase ON room_monsters(room_id,rule_boss_phase,status);


-- 53-cpp.14：玩家／DM 直接上傳頭像（圖片存 PostgreSQL，Render 重部署不遺失）
CREATE TABLE IF NOT EXISTS avatar_images (
  user_id BIGINT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
  mime_type VARCHAR(32) NOT NULL,
  image_base64 TEXT NOT NULL,
  size_bytes BIGINT NOT NULL DEFAULT 0,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX IF NOT EXISTS idx_avatar_images_updated_at ON avatar_images(updated_at);


-- =========================================================
-- 54-cpp.15：全站品階 + 遊戲間／書庫知識商店
-- =========================================================
-- [功能備註｜共用品階] 技能、材料／道具／裝備、配方與商店商品統一使用 G→SSS。
ALTER TABLE skill_templates ADD COLUMN IF NOT EXISTS rank VARCHAR(10) NOT NULL DEFAULT 'G';
ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS rank VARCHAR(10) NOT NULL DEFAULT 'G';
ALTER TABLE recipes ADD COLUMN IF NOT EXISTS rank VARCHAR(10) NOT NULL DEFAULT 'G';
ALTER TABLE shop_items ADD COLUMN IF NOT EXISTS rank VARCHAR(10) NOT NULL DEFAULT 'G';
ALTER TABLE npc_shop_items ADD COLUMN IF NOT EXISTS rank VARCHAR(10) NOT NULL DEFAULT 'G';

-- [功能備註｜遊戲間／書庫] 販售技能書、魔法學、儀式學解鎖；實際學習仍支付對應學習點數。
CREATE TABLE IF NOT EXISTS knowledge_shop_items (
  id BIGSERIAL PRIMARY KEY,
  venue VARCHAR(20) NOT NULL DEFAULT 'library',
  name VARCHAR(160) NOT NULL,
  content_type VARCHAR(20) NOT NULL,
  content_id BIGINT NOT NULL,
  rank VARCHAR(10) NOT NULL DEFAULT 'G',
  currency VARCHAR(20) NOT NULL DEFAULT 'weird',
  price BIGINT NOT NULL DEFAULT 0,
  stock INTEGER NOT NULL DEFAULT -1,
  purchase_limit INTEGER NOT NULL DEFAULT 1,
  description TEXT NOT NULL DEFAULT '',
  active BOOLEAN NOT NULL DEFAULT TRUE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS knowledge_shop_purchases (
  id BIGSERIAL PRIMARY KEY,
  user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  shop_item_id BIGINT NOT NULL REFERENCES knowledge_shop_items(id) ON DELETE CASCADE,
  content_type VARCHAR(20) NOT NULL,
  content_id BIGINT NOT NULL,
  currency VARCHAR(20) NOT NULL,
  unit_price BIGINT NOT NULL DEFAULT 0,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX IF NOT EXISTS idx_knowledge_shop_venue ON knowledge_shop_items(venue,active,rank,id);
CREATE INDEX IF NOT EXISTS idx_knowledge_shop_purchase_user ON knowledge_shop_purchases(user_id,content_type,content_id,created_at DESC);


-- =========================================================
-- 55-cpp.16：房間相容修復 + 七元素魔法技能樹 + NPC 任務/頭像 + 副本等級
-- =========================================================
-- [功能備註｜房間穩定性] 把 roomSnapshot 會使用的後期欄位再次做冪等遷移，舊資料庫升級時避免 /api/rooms 500。
ALTER TABLE room_members ADD COLUMN IF NOT EXISTS current_map_node_id BIGINT REFERENCES room_map_nodes(id) ON DELETE SET NULL;
ALTER TABLE room_members ADD COLUMN IF NOT EXISTS offline_ai_enabled BOOLEAN NOT NULL DEFAULT TRUE;
ALTER TABLE room_members ADD COLUMN IF NOT EXISTS offline_ai_active BOOLEAN NOT NULL DEFAULT FALSE;
ALTER TABLE room_members ADD COLUMN IF NOT EXISTS last_seen_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP;
ALTER TABLE room_members ADD COLUMN IF NOT EXISTS ai_last_action_at TIMESTAMPTZ;
ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS avatar_url TEXT NOT NULL DEFAULT '';
ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS merchant_enabled BOOLEAN NOT NULL DEFAULT FALSE;
ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS merchant_title VARCHAR(120) NOT NULL DEFAULT '';

-- [功能備註｜七元素魔法技能樹] 每個魔法節點屬於一棵元素樹；座標與圖示由 DM 自行設計。
ALTER TABLE magic_studies ADD COLUMN IF NOT EXISTS element VARCHAR(10) NOT NULL DEFAULT '無';
ALTER TABLE magic_studies ADD COLUMN IF NOT EXISTS tree_x INTEGER NOT NULL DEFAULT 0;
ALTER TABLE magic_studies ADD COLUMN IF NOT EXISTS tree_y INTEGER NOT NULL DEFAULT 0;
ALTER TABLE magic_studies ADD COLUMN IF NOT EXISTS icon VARCHAR(40) NOT NULL DEFAULT '✦';
ALTER TABLE magic_studies ADD COLUMN IF NOT EXISTS node_style JSONB NOT NULL DEFAULT '{}'::jsonb;
CREATE INDEX IF NOT EXISTS idx_magic_studies_tree ON magic_studies(element,tree_y,tree_x,id);

-- [功能備註｜NPC 任務發布] NPC 可直接掛多個既有任務；接受後仍走原任務系統。
CREATE TABLE IF NOT EXISTS npc_task_offers (
  id BIGSERIAL PRIMARY KEY,
  npc_template_id BIGINT NOT NULL REFERENCES npc_templates(id) ON DELETE CASCADE,
  task_id BIGINT NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
  title_override VARCHAR(160) NOT NULL DEFAULT '',
  description_override TEXT NOT NULL DEFAULT '',
  active BOOLEAN NOT NULL DEFAULT TRUE,
  sort_order INTEGER NOT NULL DEFAULT 0,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE(npc_template_id,task_id)
);
CREATE INDEX IF NOT EXISTS idx_npc_task_offers_npc ON npc_task_offers(npc_template_id,active,sort_order,id);

-- [功能備註｜NPC 頭像上傳] 與玩家頭像同樣保存 PostgreSQL，Render 重新部署不會遺失。
CREATE TABLE IF NOT EXISTS npc_avatar_images (
  npc_template_id BIGINT PRIMARY KEY REFERENCES npc_templates(id) ON DELETE CASCADE,
  mime_type VARCHAR(80) NOT NULL,
  image_base64 TEXT NOT NULL,
  size_bytes BIGINT NOT NULL DEFAULT 0,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- [功能備註｜副本等級] 副本使用獨立 1~10／神 級，不與 G~SSS 物品品階混用。
ALTER TABLE dungeon_templates ADD COLUMN IF NOT EXISTS dungeon_level VARCHAR(10) NOT NULL DEFAULT '1';

-- =========================================================
-- 56-cpp.17 通用詞條／洗煉／裝備槽
-- =========================================================
CREATE TABLE IF NOT EXISTS special_currency_types(
  code TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  description TEXT NOT NULL DEFAULT '',
  sort_order INTEGER NOT NULL DEFAULT 0,
  active BOOLEAN NOT NULL DEFAULT TRUE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS character_special_currencies(
  user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  currency_code TEXT NOT NULL REFERENCES special_currency_types(code) ON DELETE CASCADE,
  amount BIGINT NOT NULL DEFAULT 0 CHECK(amount>=0),
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY(user_id,currency_code)
);
CREATE TABLE IF NOT EXISTS equipment_slot_types(
  code TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  sort_order INTEGER NOT NULL DEFAULT 0,
  active BOOLEAN NOT NULL DEFAULT TRUE,
  config JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS affix_definitions(
  id BIGSERIAL PRIMARY KEY,
  name TEXT NOT NULL,
  rank TEXT NOT NULL DEFAULT 'G',
  category TEXT NOT NULL DEFAULT '特殊',
  description TEXT NOT NULL DEFAULT '',
  effects JSONB NOT NULL DEFAULT '{}'::jsonb,
  tags JSONB NOT NULL DEFAULT '[]'::jsonb,
  active BOOLEAN NOT NULL DEFAULT TRUE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS affix_pools(
  id BIGSERIAL PRIMARY KEY,
  name TEXT NOT NULL,
  target_type TEXT NOT NULL DEFAULT '*',
  rank TEXT NOT NULL DEFAULT 'G',
  description TEXT NOT NULL DEFAULT '',
  active BOOLEAN NOT NULL DEFAULT TRUE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS affix_pool_entries(
  pool_id BIGINT NOT NULL REFERENCES affix_pools(id) ON DELETE CASCADE,
  affix_id BIGINT NOT NULL REFERENCES affix_definitions(id) ON DELETE CASCADE,
  weight DOUBLE PRECISION NOT NULL DEFAULT 1 CHECK(weight>=0),
  active BOOLEAN NOT NULL DEFAULT TRUE,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY(pool_id,affix_id)
);
CREATE TABLE IF NOT EXISTS reforge_profiles(
  id BIGSERIAL PRIMARY KEY,
  target_type TEXT NOT NULL DEFAULT '*',
  rank TEXT NOT NULL DEFAULT 'G',
  pool_id BIGINT REFERENCES affix_pools(id) ON DELETE SET NULL,
  initial_affixes INTEGER NOT NULL DEFAULT 0,
  max_affixes INTEGER NOT NULL DEFAULT 0,
  random_currency_code TEXT NOT NULL DEFAULT 'trait_core',
  random_cost BIGINT NOT NULL DEFAULT 1,
  targeted_currency_code TEXT NOT NULL DEFAULT 'trait_core',
  targeted_cost BIGINT NOT NULL DEFAULT 5,
  direct_grant BOOLEAN NOT NULL DEFAULT FALSE,
  active BOOLEAN NOT NULL DEFAULT TRUE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE(target_type,rank)
);
CREATE TABLE IF NOT EXISTS affix_targets(
  id BIGSERIAL PRIMARY KEY,
  owner_user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  target_type TEXT NOT NULL,
  target_key TEXT NOT NULL,
  display_name TEXT NOT NULL,
  rank TEXT NOT NULL DEFAULT 'G',
  pool_id BIGINT REFERENCES affix_pools(id) ON DELETE SET NULL,
  max_affixes INTEGER NOT NULL DEFAULT 0,
  current_affixes JSONB NOT NULL DEFAULT '[]'::jsonb,
  data JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE(owner_user_id,target_type,target_key)
);
CREATE INDEX IF NOT EXISTS idx_affix_targets_owner ON affix_targets(owner_user_id,target_type);
CREATE INDEX IF NOT EXISTS idx_affix_pool_entries_pool ON affix_pool_entries(pool_id);
ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS equip_slot_code TEXT NOT NULL DEFAULT '';
ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS weapon_hands INTEGER NOT NULL DEFAULT 1;
ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS affix_pool_id BIGINT;
ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS affix_enabled BOOLEAN NOT NULL DEFAULT FALSE;
-- [功能備註｜59-cpp.20 詞條分層／混合品階／額外特殊詞條]
-- 一般詞條、額外特殊詞條與固有效果分開；特殊詞條不佔 current_affixes / max_affixes。
ALTER TABLE affix_definitions ADD COLUMN IF NOT EXISTS affix_kind TEXT NOT NULL DEFAULT 'normal';
ALTER TABLE affix_pools ADD COLUMN IF NOT EXISTS pool_kind TEXT NOT NULL DEFAULT 'normal';
ALTER TABLE affix_pools ADD COLUMN IF NOT EXISTS mixed_ranks BOOLEAN NOT NULL DEFAULT FALSE;
ALTER TABLE affix_pools ADD COLUMN IF NOT EXISTS rank_weights JSONB NOT NULL DEFAULT '{}'::jsonb;
ALTER TABLE affix_targets ADD COLUMN IF NOT EXISTS special_affixes JSONB NOT NULL DEFAULT '[]'::jsonb;
ALTER TABLE affix_targets ADD COLUMN IF NOT EXISTS intrinsic_effects JSONB NOT NULL DEFAULT '[]'::jsonb;
ALTER TABLE affix_targets ADD COLUMN IF NOT EXISTS max_special_affixes INTEGER NOT NULL DEFAULT 0;

CREATE TABLE IF NOT EXISTS special_affix_rules(
  id BIGSERIAL PRIMARY KEY,
  name TEXT NOT NULL,
  source_target_type TEXT NOT NULL DEFAULT 'equipment',
  source_name TEXT NOT NULL DEFAULT '',
  source_rank TEXT NOT NULL DEFAULT '*',
  recipient_target_type TEXT NOT NULL DEFAULT 'puppet',
  pool_id BIGINT NOT NULL REFERENCES affix_pools(id) ON DELETE CASCADE,
  chance_percent DOUBLE PRECISION NOT NULL DEFAULT 0,
  grant_count INTEGER NOT NULL DEFAULT 1,
  max_special_affixes INTEGER NOT NULL DEFAULT 1,
  once_per_pair BOOLEAN NOT NULL DEFAULT TRUE,
  active BOOLEAN NOT NULL DEFAULT TRUE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS special_affix_roll_history(
  id BIGSERIAL PRIMARY KEY,
  rule_id BIGINT NOT NULL REFERENCES special_affix_rules(id) ON DELETE CASCADE,
  source_target_id BIGINT NOT NULL REFERENCES affix_targets(id) ON DELETE CASCADE,
  recipient_target_id BIGINT NOT NULL REFERENCES affix_targets(id) ON DELETE CASCADE,
  success BOOLEAN NOT NULL DEFAULT FALSE,
  granted_affixes JSONB NOT NULL DEFAULT '[]'::jsonb,
  rolled_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX IF NOT EXISTS idx_special_affix_rules_source
  ON special_affix_rules(source_target_type,source_rank,active);
CREATE INDEX IF NOT EXISTS idx_special_affix_roll_history_pair
  ON special_affix_roll_history(rule_id,source_target_id,recipient_target_id);
INSERT INTO special_currency_types(code,name,description,sort_order,active)
VALUES('trait_core','詞條核心','通用洗煉特殊貨幣；可由合成配方產出。',10,TRUE)
ON CONFLICT(code) DO UPDATE SET name=EXCLUDED.name,description=EXCLUDED.description,active=TRUE;
INSERT INTO equipment_slot_types(code,name,sort_order,active) VALUES
('head','頭部',10,TRUE),('body','身體',20,TRUE),('hands','手部',30,TRUE),('legs','腿部',40,TRUE),
('feet','腳部',50,TRUE),('accessory_1','飾品 1',60,TRUE),('accessory_2','飾品 2',70,TRUE),('special','特殊裝備',80,TRUE)
ON CONFLICT(code) DO NOTHING;
INSERT INTO reforge_profiles(target_type,rank,initial_affixes,max_affixes,random_currency_code,random_cost,targeted_currency_code,targeted_cost,direct_grant,active)
VALUES('puppet','G',1,2,'trait_core',1,'trait_core',5,FALSE,TRUE)
ON CONFLICT(target_type,rank) DO NOTHING;
