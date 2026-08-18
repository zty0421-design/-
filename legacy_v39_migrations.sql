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
