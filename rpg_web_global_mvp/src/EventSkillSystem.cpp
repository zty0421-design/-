#include "trpg/EventSkillSystem.h"

#include "trpg/CoreService.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace trpg {
namespace {

std::string str(const Json::Value& value, const std::string& fallback = {}) {
  if (value.isString()) return value.asString();
  if (value.isBool()) return value.asBool() ? "true" : "false";
  if (value.isInt64() || value.isUInt64() || value.isInt() || value.isUInt()) {
    return std::to_string(value.asInt64());
  }
  return fallback;
}

std::int64_t integer(const Json::Value& value, std::int64_t fallback = 0) {
  if (value.isInt64() || value.isUInt64() || value.isInt() || value.isUInt()) return value.asInt64();
  if (value.isDouble()) return static_cast<std::int64_t>(value.asDouble());
  if (value.isBool()) return value.asBool() ? 1 : 0;
  if (value.isString()) {
    try { return std::stoll(value.asString()); } catch (...) { return fallback; }
  }
  return fallback;
}

double number(const Json::Value& value, double fallback = 0.0) {
  if (value.isNumeric()) return value.asDouble();
  if (value.isBool()) return value.asBool() ? 1.0 : 0.0;
  if (value.isString()) {
    try { return std::stod(value.asString()); } catch (...) { return fallback; }
  }
  return fallback;
}

Json::Value parseJson(const std::string& text, Json::Value fallback = Json::Value{Json::objectValue}) {
  if (text.empty()) return fallback;
  Json::CharReaderBuilder builder;
  Json::Value root;
  std::string errors;
  std::istringstream input{text};
  if (!Json::parseFromStream(builder, input, &root, &errors)) return fallback;
  return root;
}

std::string compactJson(const Json::Value& value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value);
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool looselyEqual(const Json::Value& left, const Json::Value& right) {
  if (left.isNumeric() || right.isNumeric()) return number(left) == number(right);
  if (left.isBool() || right.isBool()) return left.asBool() == right.asBool();
  return str(left) == str(right);
}

Json::Value atPath(const Json::Value& root, const std::string& rawPath) {
  auto path = rawPath;
  if (path.rfind("context.", 0) == 0) path = path.substr(8);
  if (path.empty()) return root;
  const Json::Value* current = &root;
  std::size_t start = 0;
  while (start <= path.size()) {
    const auto dot = path.find('.', start);
    const auto key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    if (key.empty() || !current->isObject() || !current->isMember(key)) return Json::Value{Json::nullValue};
    current = &((*current)[key]);
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  return *current;
}

std::int64_t randomPercent() {
  static thread_local std::mt19937_64 engine{std::random_device{}()};
  std::uniform_int_distribution<std::int64_t> dist(1, 100);
  return dist(engine);
}

struct EntityRef {
  std::string type;
  std::int64_t id{0};
  std::string name;
};

struct TriggerOwner {
  EntityRef entity;
  Json::Value skills{Json::arrayValue};
};

EntityRef refFrom(const Json::Value& value) {
  EntityRef ref;
  if (!value.isObject()) return ref;
  ref.type = str(value["type"]);
  ref.id = integer(value["id"]);
  ref.name = str(value["name"]);
  return ref;
}

EntityRef selectEntity(const TriggerOwner& owner, const Json::Value& context,
                       const std::string& rawSelector) {
  const auto selector = lower(rawSelector.empty() ? "target" : rawSelector);
  if (selector == "self" || selector == "owner") return owner.entity;
  if (selector == "actor" || selector == "attacker") {
    auto ref = refFrom(context["attacker"]);
    if (ref.id <= 0) ref = refFrom(context["actor"]);
    return ref;
  }
  if (selector == "defeated") {
    auto ref = refFrom(context["defeated"]);
    if (ref.id <= 0 && context["damage_change"].get("downed", false).asBool()) ref = refFrom(context["target"]);
    return ref;
  }
  auto ref = refFrom(context["target"]);
  if (ref.id <= 0) ref = refFrom(context["defeated"]);
  return ref;
}

std::optional<std::int64_t> mapNodeFor(const std::shared_ptr<CoreService>& service,
                                       std::int64_t roomId, const EntityRef& ref) {
  if (ref.id <= 0) return std::nullopt;
  try {
    if (ref.type == "player") {
      const auto rows = service->database()->execSqlSync(
          "SELECT current_map_node_id FROM room_members WHERE room_id=$1 AND user_id=$2", roomId, ref.id);
      if (!rows.empty() && !rows[0]["current_map_node_id"].isNull()) return rows[0]["current_map_node_id"].as<std::int64_t>();
    } else if (ref.type == "monster") {
      const auto rows = service->database()->execSqlSync(
          "SELECT map_node_id FROM room_monsters WHERE room_id=$1 AND id=$2", roomId, ref.id);
      if (!rows.empty() && !rows[0]["map_node_id"].isNull()) return rows[0]["map_node_id"].as<std::int64_t>();
    } else if (ref.type == "npc") {
      const auto rows = service->database()->execSqlSync(
          "SELECT map_node_id FROM room_npcs WHERE room_id=$1 AND npc_template_id=$2", roomId, ref.id);
      if (!rows.empty() && !rows[0]["map_node_id"].isNull()) return rows[0]["map_node_id"].as<std::int64_t>();
    }
  } catch (...) {}
  return std::nullopt;
}

std::int64_t hpPercentFor(const std::shared_ptr<CoreService>& service,
                          std::int64_t roomId, const EntityRef& ref) {
  if (ref.id <= 0) return 0;
  try {
    if (ref.type == "player") {
      const auto rows = service->database()->execSqlSync(
          "SELECT hp,max_hp FROM room_members WHERE room_id=$1 AND user_id=$2", roomId, ref.id);
      if (!rows.empty()) {
        const auto maxHp = std::max<std::int64_t>(1, rows[0]["max_hp"].as<std::int64_t>());
        return std::clamp<std::int64_t>(rows[0]["hp"].as<std::int64_t>() * 100 / maxHp, 0, 100);
      }
    } else if (ref.type == "monster") {
      const auto rows = service->database()->execSqlSync(
          "SELECT current_hp,max_hp FROM room_monsters WHERE room_id=$1 AND id=$2", roomId, ref.id);
      if (!rows.empty()) {
        const auto maxHp = std::max<std::int64_t>(1, rows[0]["max_hp"].as<std::int64_t>());
        return std::clamp<std::int64_t>(rows[0]["current_hp"].as<std::int64_t>() * 100 / maxHp, 0, 100);
      }
    } else if (ref.type == "npc") {
      const auto rows = service->database()->execSqlSync(
          "SELECT current_hp,max_hp FROM room_npcs WHERE room_id=$1 AND npc_template_id=$2", roomId, ref.id);
      if (!rows.empty()) {
        const auto maxHp = std::max<std::int64_t>(1, rows[0]["max_hp"].as<std::int64_t>());
        return std::clamp<std::int64_t>(rows[0]["current_hp"].as<std::int64_t>() * 100 / maxHp, 0, 100);
      }
    }
  } catch (...) {}
  return 0;
}

std::int64_t statusStacks(const std::shared_ptr<CoreService>& service,
                          std::int64_t roomId, const EntityRef& ref,
                          const std::string& statusName) {
  if (ref.id <= 0 || statusName.empty()) return 0;
  try {
    const auto rows = service->database()->execSqlSync(
        "SELECT COALESCE(MAX(stacks),0)::BIGINT AS stacks FROM combat_status_instances "
        "WHERE room_id=$1 AND target_type=$2 AND target_id=$3 AND active=TRUE AND name=$4",
        roomId, ref.type, ref.id, statusName);
    return rows.empty() ? 0 : rows[0]["stacks"].as<std::int64_t>();
  } catch (...) { return 0; }
}

struct ActiveStatus {
  std::int64_t id{0};
  std::string name;
  std::int64_t stacks{0};
  std::string sourceType;
  std::int64_t sourceId{0};
  Json::Value effects{Json::objectValue};
};

std::vector<ActiveStatus> activeStatusesFor(const std::shared_ptr<CoreService>& service,
                                            std::int64_t roomId,
                                            const std::string& targetType,
                                            std::int64_t targetId) {
  std::vector<ActiveStatus> out;
  if (!service || roomId <= 0 || targetType.empty() || targetId <= 0) return out;
  try {
    const auto rows = service->database()->execSqlSync(
        "SELECT id,name,stacks,source_type,source_id,effects::text AS effects_text "
        "FROM combat_status_instances WHERE room_id=$1 AND target_type=$2 AND target_id=$3 AND active=TRUE ORDER BY id",
        roomId, targetType, targetId);
    out.reserve(rows.size());
    for (const auto& row : rows) {
      ActiveStatus status;
      status.id = row["id"].as<std::int64_t>();
      status.name = row["name"].as<std::string>();
      status.stacks = std::max<std::int64_t>(0, row["stacks"].as<std::int64_t>());
      status.sourceType = row["source_type"].isNull() ? std::string{} : row["source_type"].as<std::string>();
      status.sourceId = row["source_id"].isNull() ? 0 : row["source_id"].as<std::int64_t>();
      status.effects = row["effects_text"].isNull()
                           ? Json::Value{Json::objectValue}
                           : parseJson(row["effects_text"].as<std::string>(), Json::Value{Json::objectValue});
      out.push_back(std::move(status));
    }
  } catch (const std::exception& error) {
    LOG_WARN << "event skill active status read degraded: " << error.what();
  }
  return out;
}

std::int64_t percentEffect(const Json::Value& effects,
                           const std::string& key,
                           std::int64_t stacks) {
  if (!effects.isObject() || stacks <= 0) return 0;
  return integer(effects[key]) * stacks;
}

std::int64_t flatEffect(const Json::Value& effects,
                        const std::string& key,
                        std::int64_t stacks) {
  if (!effects.isObject() || stacks <= 0) return 0;
  return integer(effects[key]) * stacks;
}

std::string equippedWeaponType(const std::shared_ptr<CoreService>& service,
                               const TriggerOwner& owner,
                               const Json::Value& context) {
  if (context.isMember("weapon_type") && !str(context["weapon_type"]).empty()) return lower(str(context["weapon_type"]));
  if (owner.entity.type != "player") return lower(str(context["skill"]["weapon_type"]));
  try {
    const auto character = service->characterByUser(owner.entity.id);
    if (character["equipped_weapons"].isArray() && !character["equipped_weapons"].empty()) {
      const auto& weapon = character["equipped_weapons"][0];
      for (const auto* key : {"weapon_type", "category", "type", "name"}) {
        const auto value = str(weapon[key]);
        if (!value.empty()) return lower(value);
      }
    }
  } catch (...) {}
  return {};
}

bool eventMatches(const std::string& requested, const std::string& actual,
                  const Json::Value& context) {
  const auto key = lower(requested);
  const auto actualKey = lower(actual);
  if (key == actualKey) return true;
  if (key == "on_skill_cast" || key == "on_skill_used") return actual == "COMBAT_SKILL_USED";
  if (key == "on_combat_start" || key == "combat_start" || key == "auto_combat_start") return actual == "COMBAT_STARTED";
  if (key == "on_combat_end" || key == "combat_end") return actual == "COMBAT_ENDED";
  if (key == "on_hit_sequence_start") return actual == "COMBAT_HIT_SEQUENCE_STARTED";
  if (key == "on_hit_sequence_end") return actual == "COMBAT_HIT_SEQUENCE_ENDED";
  if (key == "on_hit") return actual == "COMBAT_ATTACK_RESOLVED" && context.get("hit", false).asBool();
  if (key == "on_damage" || key == "on_damage_dealt") {
    return (actual == "COMBAT_ATTACK_RESOLVED" || actual == "COMBAT_DAMAGE_DEALT") &&
           context.get("hit", true).asBool() && integer(context["damage"]) > 0;
  }
  if (key == "on_damage_taken") {
    return (actual == "COMBAT_ATTACK_RESOLVED" || actual == "COMBAT_DAMAGE_DEALT") &&
           context.get("hit", true).asBool() && integer(context["damage"]) > 0;
  }
  if (key == "on_heal_done" || key == "on_heal_received") return actual == "COMBAT_HEAL_RESOLVED";
  if (key == "on_positive_effect_applied") return actual == "COMBAT_POSITIVE_EFFECT_APPLIED";
  if (key == "on_negative_effect_applied") return actual == "COMBAT_NEGATIVE_EFFECT_APPLIED";
  if (key == "on_status_added") return actual == "COMBAT_STATUS_ADDED";
  if (key == "on_status_removed") return actual == "COMBAT_STATUS_REMOVED";
  if (key == "on_status_stack_changed") return actual == "COMBAT_STATUS_STACK_CHANGED";
  if (key == "on_entity_defeated" || key == "on_kill") return actual == "COMBAT_ENTITY_DEFEATED";
  if (key == "on_round_start") return actual == "COMBAT_ROUND_STARTED";
  if (key == "on_turn_start") return actual == "COMBAT_TURN_STARTED";
  if (key == "on_skill_used") return actual == "COMBAT_SKILL_USED";
  return false;
}

bool conditionMatches(const std::shared_ptr<CoreService>& service,
                      std::int64_t roomId, const TriggerOwner& owner,
                      const Json::Value& condition, const Json::Value& context);

bool listAll(const std::shared_ptr<CoreService>& service, std::int64_t roomId,
             const TriggerOwner& owner, const Json::Value& list,
             const Json::Value& context) {
  if (!list.isArray()) return true;
  for (const auto& entry : list) if (!conditionMatches(service, roomId, owner, entry, context)) return false;
  return true;
}

bool listAny(const std::shared_ptr<CoreService>& service, std::int64_t roomId,
             const TriggerOwner& owner, const Json::Value& list,
             const Json::Value& context) {
  if (!list.isArray() || list.empty()) return false;
  for (const auto& entry : list) if (conditionMatches(service, roomId, owner, entry, context)) return true;
  return false;
}

bool conditionMatches(const std::shared_ptr<CoreService>& service,
                      std::int64_t roomId, const TriggerOwner& owner,
                      const Json::Value& condition, const Json::Value& context) {
  if (condition.isNull()) return true;
  if (condition.isBool()) return condition.asBool();
  if (condition.isArray()) return listAll(service, roomId, owner, condition, context);
  if (!condition.isObject()) return true;
  if (condition.isMember("all")) return listAll(service, roomId, owner, condition["all"], context);
  if (condition.isMember("any")) return listAny(service, roomId, owner, condition["any"], context);
  if (condition.isMember("not")) return !conditionMatches(service, roomId, owner, condition["not"], context);

  const auto op = lower(str(condition["op"], "eq"));
  if (op == "chance" || op == "chance_percent") {
    const auto chance = std::clamp<std::int64_t>(integer(condition["value"], integer(condition["percent"], 100)), 0, 100);
    return chance >= 100 || (chance > 0 && randomPercent() <= chance);
  }
  if (op == "owner_is_actor") {
    auto actor = refFrom(context["attacker"]);
    if (actor.id <= 0) actor = refFrom(context["actor"]);
    return actor.type == owner.entity.type && actor.id == owner.entity.id;
  }
  if (op == "owner_is_target") {
    const auto target = refFrom(context["target"]);
    return target.type == owner.entity.type && target.id == owner.entity.id;
  }
  if (op == "owner_is_defeated") {
    const auto defeated = refFrom(context["defeated"]);
    return defeated.type == owner.entity.type && defeated.id == owner.entity.id;
  }
  if (op == "weapon_type_is") {
    const auto expected = lower(str(condition["value"], str(condition["weapon_type"])));
    const auto actual = equippedWeaponType(service, owner, context);
    return !expected.empty() && (actual == expected || actual.find(expected) != std::string::npos);
  }
  if (op == "has_status" || op == "entity_has_status" || op == "target_has_status" || op == "defeated_has_status") {
    std::string selector = str(condition["target"], "target");
    if (op == "target_has_status") selector = "target";
    if (op == "defeated_has_status") selector = "defeated";
    const auto ref = selectEntity(owner, context, selector);
    const auto required = std::max<std::int64_t>(1, integer(condition["min_stacks"], 1));
    return statusStacks(service, roomId, ref, str(condition["status"], str(condition["name"]))) >= required;
  }
  if (op == "field_exists" || op == "field_active" || op == "field_not_exists") {
    const auto key = str(condition["field"], str(condition["key"], str(condition["name"])));
    bool exists = false;
    try {
      const auto rows = key.empty()
          ? service->database()->execSqlSync(
                "SELECT id FROM combat_field_effects WHERE room_id=$1 AND active=TRUE LIMIT 1", roomId)
          : service->database()->execSqlSync(
                "SELECT id FROM combat_field_effects WHERE room_id=$1 AND active=TRUE AND (definition_key=$2 OR name=$2) LIMIT 1",
                roomId, key);
      exists = !rows.empty();
    } catch (...) {}
    return op == "field_not_exists" ? !exists : exists;
  }
  if (op == "same_map_node" || op == "nearby") {
    const auto ref = selectEntity(owner, context, str(condition["target"], "target"));
    const auto selfNode = mapNodeFor(service, roomId, owner.entity);
    const auto otherNode = mapNodeFor(service, roomId, ref);
    // 戰鬥舊資料沒有節點時視為同一遭遇，避免舊房間被動技能全面失效。
    return !selfNode || !otherNode || *selfNode == *otherNode;
  }
  if (op == "hp_pct_lte" || op == "hp_pct_gte") {
    const auto ref = selectEntity(owner, context, str(condition["target"], "self"));
    const auto current = hpPercentFor(service, roomId, ref);
    const auto threshold = integer(condition["value"]);
    return op == "hp_pct_lte" ? current <= threshold : current >= threshold;
  }

  const auto left = atPath(context, str(condition["path"]));
  const auto right = condition.isMember("value") ? condition["value"] : Json::Value{Json::nullValue};
  if (op == "exists") return !left.isNull();
  if (op == "not_exists") return left.isNull();
  if (op == "truthy") return left.asBool();
  if (op == "falsy") return !left.asBool();
  if (op == "eq") return looselyEqual(left, right);
  if (op == "neq") return !looselyEqual(left, right);
  if (op == "gt") return number(left) > number(right);
  if (op == "gte") return number(left) >= number(right);
  if (op == "lt") return number(left) < number(right);
  if (op == "lte") return number(left) <= number(right);
  if (op == "contains") return str(left).find(str(right)) != std::string::npos;
  if (op == "not_contains") return str(left).find(str(right)) == std::string::npos;
  return true;
}

Json::Value loadStatusTemplate(const std::shared_ptr<CoreService>& service,
                               const Json::Value& effect) {
  try {
    const auto id = integer(effect["status_template_id"]);
    if (id > 0) {
      const auto rows = service->database()->execSqlSync(
          "SELECT row_to_json(s)::text AS json_text FROM status_templates s WHERE id=$1", id);
      if (!rows.empty()) return parseJson(rows[0]["json_text"].as<std::string>());
    } else {
      const auto name = str(effect["status"], str(effect["name"]));
      if (!name.empty()) {
        const auto rows = service->database()->execSqlSync(
            "SELECT row_to_json(s)::text AS json_text FROM status_templates s WHERE name=$1 ORDER BY id DESC LIMIT 1", name);
        if (!rows.empty()) return parseJson(rows[0]["json_text"].as<std::string>());
      }
    }
  } catch (...) {}
  Json::Value fallback{Json::objectValue};
  fallback["name"] = str(effect["status"], str(effect["name"], "未命名狀態"));
  fallback["description"] = str(effect["description"]);
  fallback["duration_rounds"] = Json::Int64(std::max<std::int64_t>(0, integer(effect["duration_rounds"])));
  fallback["stack_mode"] = str(effect["stack_mode"], "stack");
  fallback["max_stacks"] = Json::Int64(std::max<std::int64_t>(1, integer(effect["max_stacks"], 99)));
  fallback["dispellable"] = effect.get("dispellable", true).asBool();
  fallback["polarity"] = str(effect["polarity"], "neutral");
  fallback["tags"] = effect["tags"].isArray() ? effect["tags"] : Json::Value{Json::arrayValue};
  fallback["effects"] = effect["status_effects"].isObject() ? effect["status_effects"] : Json::Value{Json::objectValue};
  fallback["triggers"] = effect["status_triggers"].isArray() ? effect["status_triggers"] : Json::Value{Json::arrayValue};
  return fallback;
}

std::string statusPolarity(const Json::Value& templ) {
  auto polarity = lower(str(templ["polarity"], "neutral"));
  if (polarity == "positive" || polarity == "buff" || polarity == "beneficial") return "positive";
  if (polarity == "negative" || polarity == "debuff" || polarity == "harmful") return "negative";
  if (templ["tags"].isArray()) {
    for (const auto& raw : templ["tags"]) {
      const auto tag = lower(str(raw));
      if (tag == "positive" || tag == "buff" || tag == "beneficial" || tag == "正面" || tag == "增益") return "positive";
      if (tag == "negative" || tag == "debuff" || tag == "harmful" || tag == "負面" || tag == "減益") return "negative";
    }
  }
  return "neutral";
}

Json::Value addStatus(const std::shared_ptr<CoreService>& service,
                      std::int64_t roomId, const EntityRef& target,
                      const TriggerOwner& owner, const Json::Value& effect) {
  Json::Value result;
  result["type"] = "add_status";
  if (target.id <= 0 || target.type.empty()) { result["skipped"] = true; return result; }
  const auto templ = loadStatusTemplate(service, effect);
  const auto name = str(templ["name"], str(effect["status"], "未命名狀態"));
  const auto addStacks = std::max<std::int64_t>(1, integer(effect["stacks"], 1));
  const auto maxStacks = std::max<std::int64_t>(1, integer(templ["max_stacks"], 1));
  const auto mode = lower(str(templ["stack_mode"], "refresh"));
  const auto duration = std::max<std::int64_t>(0, integer(effect["duration_rounds"], integer(templ["duration_rounds"])));
  const auto templateId = integer(templ["id"]);
  const auto polarity = statusPolarity(templ);
  // [功能備註｜62-cpp.23 來源獨立標記]
  // 像「畫像標記•攻」這類效果只強化施加者本人對該目標的傷害，因此不同來源必須各自疊層。
  const bool sourceScoped = templ["effects"].isObject() && templ["effects"].get("source_scoped", false).asBool();
  const auto existing = sourceScoped
      ? service->database()->execSqlSync(
            "SELECT id,stacks,remaining_rounds FROM combat_status_instances WHERE room_id=$1 AND target_type=$2 AND target_id=$3 AND name=$4 AND source_type=$5 AND source_id=$6 AND active=TRUE ORDER BY id DESC LIMIT 1",
            roomId, target.type, target.id, name, owner.entity.type, owner.entity.id)
      : service->database()->execSqlSync(
            "SELECT id,stacks,remaining_rounds FROM combat_status_instances WHERE room_id=$1 AND target_type=$2 AND target_id=$3 AND name=$4 AND active=TRUE ORDER BY id DESC LIMIT 1",
            roomId, target.type, target.id, name);
  std::int64_t finalStacks = std::min(maxStacks, addStacks);
  std::int64_t beforeStacks = 0;
  std::int64_t statusId = 0;
  if (!existing.empty()) {
    statusId = existing[0]["id"].as<std::int64_t>();
    const auto oldStacks = existing[0]["stacks"].as<std::int64_t>();
    beforeStacks = oldStacks;
    if (mode == "stack" || mode == "add") finalStacks = std::min(maxStacks, oldStacks + addStacks);
    else finalStacks = std::min(maxStacks, std::max(oldStacks, addStacks));
    const auto nextDuration = duration <= 0 ? existing[0]["remaining_rounds"].as<std::int64_t>()
                                             : (mode == "extend" ? existing[0]["remaining_rounds"].as<std::int64_t>() + duration : duration);
    service->database()->execSqlSync(
        "UPDATE combat_status_instances SET stacks=$1,remaining_rounds=$2,expires_by_round=($2>0),status_template_id=NULLIF($3,0),description=$4,polarity=$5,tags=$6::jsonb,effects=$7::jsonb,triggers=$8::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=$9",
        finalStacks, nextDuration, templateId, str(templ["description"]), polarity, compactJson(templ["tags"]), compactJson(templ["effects"]), compactJson(templ["triggers"]), statusId);
  } else {
    const auto rows = service->database()->execSqlSync(
        "INSERT INTO combat_status_instances(room_id,target_type,target_id,status_template_id,name,description,polarity,stacks,max_stacks,remaining_rounds,expires_by_round,dispellable,tags,effects,triggers,source_type,source_id,active) "
        "VALUES($1,$2,$3,NULLIF($4,0),$5,$6,$7,$8,$9,$10,($10>0),$11,$12::jsonb,$13::jsonb,$14::jsonb,$15,$16,TRUE) RETURNING id",
        roomId, target.type, target.id, templateId, name, str(templ["description"]), polarity, finalStacks, maxStacks, duration,
        templ.get("dispellable", true).asBool(), compactJson(templ["tags"]), compactJson(templ["effects"]), compactJson(templ["triggers"]), owner.entity.type, owner.entity.id);
    if (!rows.empty()) statusId = rows[0]["id"].as<std::int64_t>();
  }
  result["status_id"] = Json::Int64(statusId);
  result["name"] = name;
  result["target_type"] = target.type;
  result["target_id"] = Json::Int64(target.id);
  result["before_stacks"] = Json::Int64(beforeStacks);
  result["stacks"] = Json::Int64(finalStacks);
  result["stack_delta"] = Json::Int64(finalStacks - beforeStacks);
  result["polarity"] = polarity;
  result["new_status"] = existing.empty();
  result["remaining_rounds"] = Json::Int64(duration);
  return result;
}

Json::Value damageEntity(const std::shared_ptr<CoreService>& service,
                         std::int64_t roomId, const EntityRef& target,
                         const TriggerOwner& owner,
                         std::int64_t amount, const std::string& rawKind) {
  Json::Value out;
  out["type"] = "bonus_damage";
  out["target_type"] = target.type;
  out["target_id"] = Json::Int64(target.id);
  Json::Value modifierBreakdown;
  const auto damage = applyEventSkillDamageModifiers(
      service, roomId, owner.entity.type, owner.entity.id, target.type, target.id,
      std::max<std::int64_t>(0, amount), &modifierBreakdown);
  out["base_amount"] = Json::Int64(std::max<std::int64_t>(0, amount));
  out["amount"] = Json::Int64(damage);
  out["modifier_breakdown"] = modifierBreakdown;
  if (target.id <= 0 || damage <= 0) { out["skipped"] = true; return out; }
  const auto kind = rawKind == "spirit" || rawKind == "sanity" || rawKind == "fifth" ? rawKind : std::string{"hp"};
  try {
    if (target.type == "player") {
      const auto field = kind == "hp" ? "hp" : kind == "spirit" ? "spirit" : kind == "sanity" ? "current_sanity" : "hp";
      const auto rows = service->database()->execSqlSync(
          std::string{"SELECT "} + field + " AS current FROM room_members WHERE room_id=$1 AND user_id=$2", roomId, target.id);
      if (rows.empty()) { out["skipped"] = true; return out; }
      const auto before = rows[0]["current"].as<std::int64_t>();
      const auto after = std::max<std::int64_t>(0, before - damage);
      service->database()->execSqlSync(
          std::string{"UPDATE room_members SET "} + field + "=$1,combat_state=CASE WHEN $2='hp' AND $1<=0 THEN 'downed' ELSE combat_state END WHERE room_id=$3 AND user_id=$4",
          after, kind, roomId, target.id);
      out["before"] = Json::Int64(before); out["after"] = Json::Int64(after); out["downed"] = kind == "hp" && after <= 0;
      return out;
    }
    const auto table = target.type == "monster" ? "room_monsters" : "room_npcs";
    const auto idField = target.type == "monster" ? "id" : "npc_template_id";
    const auto field = kind == "hp" ? "current_hp" : kind == "fifth" ? "current_fifth" : "current_spirit";
    const auto rows = service->database()->execSqlSync(
        std::string{"SELECT "} + field + " AS current FROM " + table + " WHERE room_id=$1 AND " + idField + "=$2", roomId, target.id);
    if (rows.empty()) { out["skipped"] = true; return out; }
    const auto before = rows[0]["current"].as<std::int64_t>();
    const auto after = std::max<std::int64_t>(0, before - damage);
    if (kind == "hp") {
      const auto defeatedStatus = target.type == "monster" ? "defeated" : "downed";
      service->database()->execSqlSync(
          std::string{"UPDATE "} + table + " SET " + field + "=$1,status=CASE WHEN $1<=0 THEN $2 ELSE status END,updated_at=CURRENT_TIMESTAMP WHERE room_id=$3 AND " + idField + "=$4",
          after, defeatedStatus, roomId, target.id);
    } else {
      service->database()->execSqlSync(
          std::string{"UPDATE "} + table + " SET " + field + "=$1,updated_at=CURRENT_TIMESTAMP WHERE room_id=$2 AND " + idField + "=$3",
          after, roomId, target.id);
    }
    out["before"] = Json::Int64(before); out["after"] = Json::Int64(after); out["downed"] = kind == "hp" && after <= 0;
  } catch (const std::exception& error) {
    out["error"] = error.what();
  }
  return out;
}

Json::Value healEntity(const std::shared_ptr<CoreService>& service,
                       std::int64_t roomId, const EntityRef& target,
                       const TriggerOwner& owner,
                       std::int64_t amount, const std::string& rawKind) {
  Json::Value out;
  out["type"] = "heal";
  out["target_type"] = target.type;
  out["target_id"] = Json::Int64(target.id);
  Json::Value modifierBreakdown;
  const auto healing = applyEventSkillHealingModifiers(
      service, roomId, owner.entity.type, owner.entity.id, target.type, target.id,
      std::max<std::int64_t>(0, amount), &modifierBreakdown);
  out["base_amount"] = Json::Int64(std::max<std::int64_t>(0, amount));
  out["amount"] = Json::Int64(healing);
  out["modifier_breakdown"] = modifierBreakdown;
  if (target.id <= 0 || healing <= 0) { out["skipped"] = true; return out; }
  const auto kind = rawKind == "spirit" || rawKind == "sanity" || rawKind == "fifth" ? rawKind : std::string{"hp"};
  try {
    if (target.type == "player") {
      if (kind == "hp" || kind == "spirit" || kind == "sanity") {
        const auto field = kind == "hp" ? "hp" : kind == "spirit" ? "spirit" : "current_sanity";
        const auto maxField = kind == "hp" ? "max_hp" : kind == "spirit" ? "max_spirit" : "max_sanity";
        const auto rows = service->database()->execSqlSync(
            std::string{"SELECT "} + field + " AS current," + maxField + " AS maximum FROM room_members WHERE room_id=$1 AND user_id=$2",
            roomId, target.id);
        if (rows.empty()) { out["skipped"] = true; return out; }
        const auto before = rows[0]["current"].as<std::int64_t>();
        const auto maximum = std::max<std::int64_t>(before, rows[0]["maximum"].as<std::int64_t>());
        const auto after = std::min<std::int64_t>(maximum, before + healing);
        service->database()->execSqlSync(
            std::string{"UPDATE room_members SET "} + field + "=$1,combat_state=CASE WHEN $2='hp' AND $1>0 THEN 'active' ELSE combat_state END WHERE room_id=$3 AND user_id=$4",
            after, kind, roomId, target.id);
        out["before"] = Json::Int64(before); out["after"] = Json::Int64(after); out["actual_healing"] = Json::Int64(after - before);
        return out;
      }
      const auto character = service->characterByUser(target.id);
      const auto west = str(character["faction"], "東國") == "西國";
      const auto field = west ? "current_faith" : "current_great_way";
      const auto maxField = west ? "faith" : "great_way";
      const auto before = integer(character["fifth_current"]);
      const auto maximum = std::max<std::int64_t>(before, integer(character["fifth_max"]));
      const auto after = std::min<std::int64_t>(maximum, before + healing);
      service->database()->execSqlSync(
          std::string{"UPDATE character_cards SET "} + field + "=$1,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2", after, target.id);
      out["before"] = Json::Int64(before); out["after"] = Json::Int64(after); out["actual_healing"] = Json::Int64(after - before);
      return out;
    }
    const auto table = target.type == "monster" ? "room_monsters" : "room_npcs";
    const auto idField = target.type == "monster" ? "id" : "npc_template_id";
    if (kind != "hp") { out["skipped"] = true; out["reason"] = "非玩家目前只支援 HP 治療"; return out; }
    const auto rows = service->database()->execSqlSync(
        std::string{"SELECT current_hp AS current,max_hp AS maximum FROM "} + table + " WHERE room_id=$1 AND " + idField + "=$2",
        roomId, target.id);
    if (rows.empty()) { out["skipped"] = true; return out; }
    const auto before = rows[0]["current"].as<std::int64_t>();
    const auto maximum = std::max<std::int64_t>(before, rows[0]["maximum"].as<std::int64_t>());
    const auto after = std::min<std::int64_t>(maximum, before + healing);
    service->database()->execSqlSync(
        std::string{"UPDATE "} + table + " SET current_hp=$1,updated_at=CURRENT_TIMESTAMP WHERE room_id=$2 AND " + idField + "=$3",
        after, roomId, target.id);
    out["before"] = Json::Int64(before); out["after"] = Json::Int64(after); out["actual_healing"] = Json::Int64(after - before);
  } catch (const std::exception& error) {
    out["error"] = error.what();
  }
  return out;
}

Json::Value createFieldEffect(const std::shared_ptr<CoreService>& service,
                                  std::int64_t roomId,
                                  const TriggerOwner& owner,
                                  const Json::Value& effect) {
  // [功能備註｜62-cpp.23.1 Combat Field Effect]
  // 場地效果掛在整個戰鬥，而不是任何單一角色；0 回合代表持續到戰鬥結束。
  Json::Value out;
  out["type"] = "create_field";
  const auto field = effect["field"].isObject() ? effect["field"] : effect;
  const auto name = str(field["name"], str(effect["name"], "未命名場地"));
  auto key = str(field["key"], str(field["definition_key"], str(effect["field_key"])));
  if (key.empty()) key = name;
  const auto policy = lower(str(field["stack_policy"], str(effect["stack_policy"], "replace_same")));
  const auto priority = std::clamp<std::int64_t>(integer(field["priority"], integer(effect["priority"])), -100000, 100000);
  const auto duration = std::max<std::int64_t>(0, integer(field["duration_rounds"], integer(effect["duration_rounds"])));
  const auto effects = field["effects"].isObject() ? field["effects"] : Json::Value{Json::objectValue};
  const auto triggers = field["triggers"].isArray() ? field["triggers"] : Json::Value{Json::arrayValue};
  const auto tags = field["tags"].isArray() ? field["tags"] : Json::Value{Json::arrayValue};

  try {
    if (policy == "exclusive") {
      service->database()->execSqlSync(
          "UPDATE combat_field_effects SET active=FALSE,updated_at=CURRENT_TIMESTAMP WHERE room_id=$1 AND active=TRUE",
          roomId);
    } else if (policy == "replace_same" || policy == "refresh") {
      service->database()->execSqlSync(
          "UPDATE combat_field_effects SET active=FALSE,updated_at=CURRENT_TIMESTAMP WHERE room_id=$1 AND active=TRUE AND definition_key=$2",
          roomId, key);
    } else if (policy == "replace_lower_priority") {
      const auto blockers = service->database()->execSqlSync(
          "SELECT id,priority,name FROM combat_field_effects WHERE room_id=$1 AND active=TRUE AND priority>$2 ORDER BY priority DESC LIMIT 1",
          roomId, priority);
      if (!blockers.empty()) {
        out["skipped"] = true;
        out["suppressed_by"] = blockers[0]["name"].as<std::string>();
        return out;
      }
      service->database()->execSqlSync(
          "UPDATE combat_field_effects SET active=FALSE,updated_at=CURRENT_TIMESTAMP WHERE room_id=$1 AND active=TRUE AND priority<=$2",
          roomId, priority);
    }

    const auto rows = service->database()->execSqlSync(
        "INSERT INTO combat_field_effects(room_id,definition_key,name,description,owner_type,owner_id,owner_name,priority,stack_policy,remaining_rounds,expires_by_round,effects,triggers,tags,active) "
        "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,($10>0),$11::jsonb,$12::jsonb,$13::jsonb,TRUE) RETURNING id",
        roomId, key, name, str(field["description"]), owner.entity.type, owner.entity.id, owner.entity.name,
        priority, policy, duration, compactJson(effects), compactJson(triggers), compactJson(tags));
    if (!rows.empty()) out["field_id"] = Json::Int64(rows[0]["id"].as<std::int64_t>());
    out["definition_key"] = key;
    out["name"] = name;
    out["priority"] = Json::Int64(priority);
    out["stack_policy"] = policy;
    out["remaining_rounds"] = Json::Int64(duration);
  } catch (const std::exception& error) {
    out["error"] = error.what();
  }
  return out;
}

Json::Value removeFieldEffect(const std::shared_ptr<CoreService>& service,
                              std::int64_t roomId,
                              const Json::Value& effect) {
  Json::Value out;
  out["type"] = "remove_field";
  const auto key = str(effect["field"], str(effect["key"], str(effect["name"])));
  try {
    if (key.empty()) {
      service->database()->execSqlSync(
          "UPDATE combat_field_effects SET active=FALSE,updated_at=CURRENT_TIMESTAMP WHERE room_id=$1 AND active=TRUE",
          roomId);
    } else {
      service->database()->execSqlSync(
          "UPDATE combat_field_effects SET active=FALSE,updated_at=CURRENT_TIMESTAMP WHERE room_id=$1 AND active=TRUE AND (definition_key=$2 OR name=$2)",
          roomId, key);
    }
    out["definition_key"] = key;
  } catch (const std::exception& error) {
    out["error"] = error.what();
  }
  return out;
}

Json::Value applyEffect(const std::shared_ptr<CoreService>& service,
                        std::int64_t roomId, const TriggerOwner& owner,
                        const Json::Value& effect, const Json::Value& context) {
  const auto type = lower(str(effect["type"]));
  if (type == "create_field" || type == "add_field" || type == "field") return createFieldEffect(service, roomId, owner, effect);
  if (type == "remove_field" || type == "clear_field") return removeFieldEffect(service, roomId, effect);
  const auto target = selectEntity(owner, context, str(effect["target"], type == "add_status" ? "target" : "target"));
  if (type == "add_status") return addStatus(service, roomId, target, owner, effect);
  if (type == "remove_status") {
    Json::Value out; out["type"] = type; out["target_type"] = target.type; out["target_id"] = Json::Int64(target.id);
    const auto name = str(effect["status"], str(effect["name"]));
    if (target.id > 0 && !name.empty()) {
      service->database()->execSqlSync(
          "UPDATE combat_status_instances SET active=FALSE,updated_at=CURRENT_TIMESTAMP WHERE room_id=$1 AND target_type=$2 AND target_id=$3 AND name=$4 AND active=TRUE",
          roomId, target.type, target.id, name);
    }
    return out;
  }
  if (type == "bonus_damage" || type == "deal_damage") {
    auto amount = integer(effect["amount"], integer(effect["base"]));
    const auto perStack = integer(effect["amount_per_stack"], integer(effect["per_stack"]));
    const auto statusName = str(effect["status"], str(effect["status_name"]));
    if (perStack != 0 && !statusName.empty()) amount += perStack * statusStacks(service, roomId, target, statusName);
    return damageEntity(service, roomId, target, owner, amount, str(effect["damage_target"], "hp"));
  }
  if (type == "heal" || type == "restore") {
    auto amount = integer(effect["amount"], integer(effect["base"]));
    const auto perStack = integer(effect["amount_per_stack"], integer(effect["per_stack"]));
    const auto statusName = str(effect["status"], str(effect["status_name"]));
    if (perStack != 0 && !statusName.empty()) amount += perStack * statusStacks(service, roomId, target, statusName);
    return healEntity(service, roomId, target, owner, amount, str(effect["heal_target"], "hp"));
  }
  if (type == "consume_status" || type == "remove_status_stacks") {
    Json::Value out; out["type"] = type; out["target_type"] = target.type; out["target_id"] = Json::Int64(target.id);
    const auto name = str(effect["status"], str(effect["name"]));
    const auto amount = std::max<std::int64_t>(1, integer(effect["stacks"], integer(effect["amount"], 1)));
    if (target.id > 0 && !name.empty()) {
      const auto rows = service->database()->execSqlSync(
          "SELECT id,stacks FROM combat_status_instances WHERE room_id=$1 AND target_type=$2 AND target_id=$3 AND name=$4 AND active=TRUE ORDER BY id DESC LIMIT 1",
          roomId, target.type, target.id, name);
      if (!rows.empty()) {
        const auto before = rows[0]["stacks"].as<std::int64_t>();
        const auto after = std::max<std::int64_t>(0, before - amount);
        service->database()->execSqlSync(
            "UPDATE combat_status_instances SET stacks=$1,active=($1>0),updated_at=CURRENT_TIMESTAMP WHERE id=$2",
            after, rows[0]["id"].as<std::int64_t>());
        out["name"] = name; out["before_stacks"] = Json::Int64(before); out["stacks"] = Json::Int64(after); out["stack_delta"] = Json::Int64(after - before);
      }
    }
    return out;
  }
  if (type == "add_action") {
    Json::Value out; out["type"] = type;
    const auto amount = std::clamp<std::int64_t>(integer(effect["amount"], 1), -10, 10);
    if (target.type == "player") service->database()->execSqlSync(
        "UPDATE room_members SET turn_actions_remaining=GREATEST(0,turn_actions_remaining+$1) WHERE room_id=$2 AND user_id=$3", amount, roomId, target.id);
    else if (target.type == "monster") service->database()->execSqlSync(
        "UPDATE room_monsters SET turn_actions_remaining=GREATEST(0,turn_actions_remaining+$1) WHERE room_id=$2 AND id=$3", amount, roomId, target.id);
    else if (target.type == "npc") service->database()->execSqlSync(
        "UPDATE room_npcs SET turn_actions_remaining=GREATEST(0,turn_actions_remaining+$1) WHERE room_id=$2 AND npc_template_id=$3", amount, roomId, target.id);
    out["amount"] = Json::Int64(amount); return out;
  }
  if (type == "event" || type == "emit_event") {
    Json::Value payload = effect["payload"].isObject() ? effect["payload"] : Json::Value{Json::objectValue};
    payload["text"] = str(effect["text"], str(payload["text"], "事件型技能觸發"));
    payload["source_type"] = owner.entity.type; payload["source_id"] = Json::Int64(owner.entity.id);
    service->addEvent(roomId, owner.entity.type == "player" ? owner.entity.id : 0, "event_skill", payload);
    Json::Value out; out["type"] = type; out["payload"] = payload; return out;
  }
  Json::Value out; out["type"] = type; out["skipped"] = true; return out;
}

Json::Value triggerListFromSkill(const Json::Value& skill) {
  Json::Value triggers{Json::arrayValue};
  if (skill["event_triggers"].isArray()) triggers = skill["event_triggers"];
  else if (skill["triggers"].isArray()) triggers = skill["triggers"];
  else if (skill["data"].isObject() && skill["data"]["event_triggers"].isArray()) triggers = skill["data"]["event_triggers"];
  else if (skill["data"].isObject() && skill["data"]["triggers"].isArray()) triggers = skill["data"]["triggers"];

  // [功能備註｜62-cpp.23.1 戰鬥開始自動觸發]
  // DM 選 AUTO_COMBAT_START 後不用手寫 COMBAT_STARTED trigger；field_definition 會自動轉成 create_field effect。
  const auto data = skill["data"].isObject() ? skill["data"] : Json::Value{Json::objectValue};
  const auto activation = lower(str(data["activation_mode"], str(skill["activation_mode"])));
  const auto fieldDefinition = data["field_definition"].isObject() ? data["field_definition"] : Json::Value{Json::objectValue};
  const bool hasFieldDefinition = !str(fieldDefinition["name"]).empty() || !str(fieldDefinition["key"], str(fieldDefinition["definition_key"])).empty();
  if ((activation == "auto_combat_start" || activation == "combat_start") && hasFieldDefinition) {
    Json::Value trigger;
    trigger["event"] = "ON_COMBAT_START";
    trigger["proc_mode"] = "once_per_event";
    Json::Value effect;
    effect["type"] = "create_field";
    effect["field"] = fieldDefinition;
    trigger["effects"].append(std::move(effect));
    triggers.append(std::move(trigger));
  }
  return triggers;
}

std::vector<TriggerOwner> loadOwners(const std::shared_ptr<CoreService>& service,
                                     std::int64_t roomId) {
  std::vector<TriggerOwner> owners;
  std::unordered_map<std::int64_t, Json::Value> templateCache;
  const auto hydrateSkill = [&](const Json::Value& raw) {
    if (!raw.isObject()) return raw;
    auto skill = raw;
    const auto templateId = integer(raw["template_id"]);
    if (templateId <= 0) return skill;
    try {
      if (!templateCache.contains(templateId)) {
        const auto rows = service->database()->execSqlSync(
            "SELECT row_to_json(s)::text AS json_text FROM skill_templates s WHERE id=$1", templateId);
        templateCache[templateId] = rows.empty() ? Json::Value{Json::nullValue}
                                                  : parseJson(rows[0]["json_text"].as<std::string>());
      }
      const auto& templ = templateCache[templateId];
      if (templ.isObject()) {
        for (const auto* key : {"name", "skill_type", "damage_formula", "resources", "data", "rank", "category"}) {
          if (templ.isMember(key)) skill[key] = templ[key];
        }
      }
    } catch (const std::exception& error) {
      LOG_WARN << "event skill template hydrate degraded: " << error.what();
    }
    return skill;
  };
  try {
    const auto players = service->database()->execSqlSync(
        "SELECT rm.user_id,rm.display_name,cc.skills::text AS skills_text FROM room_members rm LEFT JOIN character_cards cc ON cc.user_id=rm.user_id WHERE rm.room_id=$1 AND rm.role='player'",
        roomId);
    for (const auto& row : players) {
      TriggerOwner owner; owner.entity.type = "player"; owner.entity.id = row["user_id"].as<std::int64_t>(); owner.entity.name = row["display_name"].as<std::string>();
      const auto rawSkills = row["skills_text"].isNull() ? Json::Value{Json::arrayValue} : parseJson(row["skills_text"].as<std::string>(), Json::Value{Json::arrayValue});
      owner.skills = Json::Value{Json::arrayValue};
      if (rawSkills.isArray()) for (const auto& skill : rawSkills) owner.skills.append(hydrateSkill(skill));
      owners.push_back(std::move(owner));
    }
  } catch (const std::exception& error) { LOG_WARN << "event skill players degraded: " << error.what(); }
  try {
    const auto monsters = service->database()->execSqlSync(
        "SELECT rm.id,mt.name,mt.skills::text AS skills_text,mt.passive_skills::text AS passive_text,mt.great_way::text AS great_way_text FROM room_monsters rm JOIN monster_templates mt ON mt.id=rm.monster_template_id WHERE rm.room_id=$1 AND rm.status='encountered' AND rm.current_hp>0",
        roomId);
    for (const auto& row : monsters) {
      TriggerOwner owner; owner.entity.type = "monster"; owner.entity.id = row["id"].as<std::int64_t>(); owner.entity.name = row["name"].as<std::string>();
      owner.skills = Json::Value{Json::arrayValue};
      for (const auto* col : {"skills_text", "passive_text"}) {
        if (row[col].isNull()) continue; const auto list = parseJson(row[col].as<std::string>(), Json::Value{Json::arrayValue}); if (list.isArray()) for (const auto& skill : list) owner.skills.append(skill);
      }
      if (!row["great_way_text"].isNull()) {
        const auto way = parseJson(row["great_way_text"].as<std::string>(), Json::Value{Json::objectValue});
        if (way.get("enabled", false).asBool() && way["skills"].isArray()) for (const auto& skill : way["skills"]) owner.skills.append(skill);
      }
      owners.push_back(std::move(owner));
    }
  } catch (const std::exception& error) { LOG_WARN << "event skill monsters degraded: " << error.what(); }
  try {
    const auto npcs = service->database()->execSqlSync(
        "SELECT rn.npc_template_id,nt.name,nt.skills::text AS skills_text FROM room_npcs rn JOIN npc_templates nt ON nt.id=rn.npc_template_id WHERE rn.room_id=$1 AND rn.status='active' AND rn.current_hp>0",
        roomId);
    for (const auto& row : npcs) {
      TriggerOwner owner; owner.entity.type = "npc"; owner.entity.id = row["npc_template_id"].as<std::int64_t>(); owner.entity.name = row["name"].as<std::string>();
      owner.skills = row["skills_text"].isNull() ? Json::Value{Json::arrayValue} : parseJson(row["skills_text"].as<std::string>(), Json::Value{Json::arrayValue});
      owners.push_back(std::move(owner));
    }
  } catch (const std::exception& error) { LOG_WARN << "event skill npcs degraded: " << error.what(); }
  return owners;
}

void appendStatusTriggerOwners(const std::shared_ptr<CoreService>& service,
                               std::int64_t roomId,
                               std::vector<TriggerOwner>& owners) {
  try {
    const auto rows = service->database()->execSqlSync(
        "SELECT target_type,target_id,name,triggers::text AS triggers_text FROM combat_status_instances WHERE room_id=$1 AND active=TRUE",
        roomId);
    for (const auto& row : rows) {
      if (row["triggers_text"].isNull()) continue;
      const auto triggers = parseJson(row["triggers_text"].as<std::string>(), Json::Value{Json::arrayValue});
      if (!triggers.isArray() || triggers.empty()) continue;
      TriggerOwner owner; owner.entity.type = row["target_type"].as<std::string>(); owner.entity.id = row["target_id"].as<std::int64_t>(); owner.entity.name = row["name"].as<std::string>();
      Json::Value pseudo; pseudo["name"] = row["name"].as<std::string>(); pseudo["event_triggers"] = triggers; pseudo["source_kind"] = "status"; owner.skills.append(std::move(pseudo));
      owners.push_back(std::move(owner));
    }
  } catch (const std::exception& error) { LOG_WARN << "event status triggers degraded: " << error.what(); }
}

void appendFieldTriggerOwners(const std::shared_ptr<CoreService>& service,
                              std::int64_t roomId,
                              std::vector<TriggerOwner>& owners) {
  // [功能備註｜62-cpp.23.1 場地事件觸發]
  // 場地自身可監聽回合、傷害、治療、狀態等 COMBAT_* 事件；owner 仍保留原施放者。
  try {
    const auto rows = service->database()->execSqlSync(
        "SELECT id,name,owner_type,owner_id,owner_name,triggers::text AS triggers_text FROM combat_field_effects WHERE room_id=$1 AND active=TRUE ORDER BY priority DESC,id",
        roomId);
    for (const auto& row : rows) {
      if (row["triggers_text"].isNull()) continue;
      const auto triggers = parseJson(row["triggers_text"].as<std::string>(), Json::Value{Json::arrayValue});
      if (!triggers.isArray() || triggers.empty()) continue;
      TriggerOwner owner;
      owner.entity.type = row["owner_type"].as<std::string>();
      owner.entity.id = row["owner_id"].as<std::int64_t>();
      owner.entity.name = row["owner_name"].isNull() ? row["name"].as<std::string>() : row["owner_name"].as<std::string>();
      Json::Value pseudo;
      pseudo["id"] = "field:" + std::to_string(row["id"].as<std::int64_t>());
      pseudo["name"] = row["name"].as<std::string>();
      pseudo["event_triggers"] = triggers;
      pseudo["source_kind"] = "field";
      owner.skills.append(std::move(pseudo));
      owners.push_back(std::move(owner));
    }
  } catch (const std::exception& error) {
    LOG_WARN << "event field triggers degraded: " << error.what();
  }
}

std::string triggerIdentity(const TriggerOwner& owner,
                            const Json::Value& skill,
                            Json::ArrayIndex index) {
  return owner.entity.type + ":" + std::to_string(owner.entity.id) + ":" +
         str(skill["id"], str(skill["template_id"], str(skill["name"], "skill"))) + ":" +
         std::to_string(index);
}

std::string procKeyFor(const TriggerOwner& owner,
                       const Json::Value& skill,
                       Json::ArrayIndex index,
                       const Json::Value& trigger,
                       const Json::Value& context) {
  const auto mode = lower(str(trigger["proc_mode"], "per_hit"));
  const auto base = triggerIdentity(owner, skill, index);
  const auto sequence = str(context["sequence_id"], str(context["event_chain_id"], "single"));
  const auto target = refFrom(context["target"]);
  if (mode == "once_per_skill" || mode == "per_skill") return base + "|skill|" + sequence;
  if (mode == "once_per_target" || mode == "per_target") {
    return base + "|target|" + sequence + "|" + target.type + ":" + std::to_string(target.id);
  }
  if (mode == "per_event" || mode == "once_per_event") {
    return base + "|event|" + str(context["event_instance_id"], sequence + ":" + str(context["hit_index"], "0"));
  }
  return {};
}

bool finalHitAllowed(const Json::Value& trigger, const Json::Value& context) {
  const auto mode = lower(str(trigger["proc_mode"], "per_hit"));
  if (mode != "final_hit_only" && mode != "final_hit") return true;
  if (context.isMember("is_final_hit")) return context["is_final_hit"].asBool();
  const auto index = integer(context["hit_index"]);
  const auto count = integer(context["hit_count"]);
  return count > 0 && index >= count;
}

bool triggerIsActive(Json::Value* state, const std::string& identity) {
  return state && (*state)["active"].isObject() && (*state)["active"].get(identity, false).asBool();
}

bool procAlreadyFired(Json::Value* state, const std::string& key) {
  return !key.empty() && state && (*state)["fired"].isObject() && (*state)["fired"].get(key, false).asBool();
}

void markProcFired(Json::Value* state, const std::string& key) {
  if (!state || key.empty()) return;
  if (!(*state)["fired"].isObject()) (*state)["fired"] = Json::Value{Json::objectValue};
  (*state)["fired"][key] = true;
}

void setTriggerActive(Json::Value* state, const std::string& identity, bool active) {
  if (!state) return;
  if (!(*state)["active"].isObject()) (*state)["active"] = Json::Value{Json::objectValue};
  (*state)["active"][identity] = active;
}

EntityRef resultTargetRef(const Json::Value& applied) {
  EntityRef ref;
  ref.type = str(applied["target_type"]);
  ref.id = integer(applied["target_id"]);
  return ref;
}

}  // namespace

std::int64_t applyEventSkillDamageModifiers(const std::shared_ptr<CoreService>& service,
                                            std::int64_t roomId,
                                            const std::string& attackerType,
                                            std::int64_t attackerId,
                                            const std::string& targetType,
                                            std::int64_t targetId,
                                            std::int64_t baseDamage,
                                            Json::Value* breakdown) {
  const auto base = std::max<std::int64_t>(0, baseDamage);
  std::int64_t percent = 0;
  std::int64_t flat = 0;
  Json::Value details{Json::arrayValue};
  for (const auto& status : activeStatusesFor(service, roomId, attackerType, attackerId)) {
    const auto p = percentEffect(status.effects, "damage_dealt_percent_per_stack", status.stacks);
    const auto f = flatEffect(status.effects, "damage_dealt_flat_per_stack", status.stacks);
    if (p == 0 && f == 0) continue;
    percent += p; flat += f;
    Json::Value row; row["status"] = status.name; row["side"] = "attacker"; row["stacks"] = Json::Int64(status.stacks); row["percent"] = Json::Int64(p); row["flat"] = Json::Int64(f); details.append(std::move(row));
  }
  for (const auto& status : activeStatusesFor(service, roomId, targetType, targetId)) {
    auto p = percentEffect(status.effects, "damage_taken_percent_per_stack", status.stacks);
    auto f = flatEffect(status.effects, "damage_taken_flat_per_stack", status.stacks);
    if (status.sourceType == attackerType && status.sourceId == attackerId) {
      p += percentEffect(status.effects, "damage_taken_from_source_percent_per_stack", status.stacks);
      f += flatEffect(status.effects, "damage_taken_from_source_flat_per_stack", status.stacks);
    }
    if (p == 0 && f == 0) continue;
    percent += p; flat += f;
    Json::Value row; row["status"] = status.name; row["side"] = "target"; row["stacks"] = Json::Int64(status.stacks); row["percent"] = Json::Int64(p); row["flat"] = Json::Int64(f); details.append(std::move(row));
  }
  // [功能備註｜62-cpp.23.1 場地傷害修正]
  try {
    const auto fields = service->database()->execSqlSync(
        "SELECT name,effects::text AS effects_text FROM combat_field_effects WHERE room_id=$1 AND active=TRUE ORDER BY priority,id", roomId);
    for (const auto& row : fields) {
      const auto effects = row["effects_text"].isNull() ? Json::Value{Json::objectValue}
                                                         : parseJson(row["effects_text"].as<std::string>(), Json::Value{Json::objectValue});
      const auto p = integer(effects["damage_percent"], integer(effects["damage_dealt_percent"]));
      const auto f = integer(effects["damage_flat"], integer(effects["damage_dealt_flat"]));
      if (p == 0 && f == 0) continue;
      percent += p; flat += f;
      Json::Value rowOut; rowOut["status"] = row["name"].as<std::string>(); rowOut["side"] = "field"; rowOut["stacks"] = 1; rowOut["percent"] = Json::Int64(p); rowOut["flat"] = Json::Int64(f); details.append(std::move(rowOut));
    }
  } catch (...) {}
  percent = std::clamp<std::int64_t>(percent, -100, 100000);
  long double scaled = static_cast<long double>(base) * static_cast<long double>(100 + percent) / 100.0L;
  scaled += static_cast<long double>(flat);
  const auto maximum = static_cast<long double>(std::numeric_limits<std::int64_t>::max() / 4);
  const auto result = static_cast<std::int64_t>(std::clamp<long double>(std::floor(scaled + 0.000001L), 0.0L, maximum));
  if (breakdown) {
    (*breakdown)["base"] = Json::Int64(base);
    (*breakdown)["percent"] = Json::Int64(percent);
    (*breakdown)["flat"] = Json::Int64(flat);
    (*breakdown)["result"] = Json::Int64(result);
    (*breakdown)["statuses"] = std::move(details);
  }
  return result;
}

std::int64_t applyEventSkillHealingModifiers(const std::shared_ptr<CoreService>& service,
                                             std::int64_t roomId,
                                             const std::string& healerType,
                                             std::int64_t healerId,
                                             const std::string& targetType,
                                             std::int64_t targetId,
                                             std::int64_t baseHealing,
                                             Json::Value* breakdown) {
  const auto base = std::max<std::int64_t>(0, baseHealing);
  std::int64_t percent = 0;
  std::int64_t flat = 0;
  Json::Value details{Json::arrayValue};
  for (const auto& status : activeStatusesFor(service, roomId, healerType, healerId)) {
    const auto p = percentEffect(status.effects, "healing_done_percent_per_stack", status.stacks);
    const auto f = flatEffect(status.effects, "healing_done_flat_per_stack", status.stacks);
    if (p == 0 && f == 0) continue;
    percent += p; flat += f;
    Json::Value row; row["status"] = status.name; row["side"] = "healer"; row["stacks"] = Json::Int64(status.stacks); row["percent"] = Json::Int64(p); row["flat"] = Json::Int64(f); details.append(std::move(row));
  }
  for (const auto& status : activeStatusesFor(service, roomId, targetType, targetId)) {
    auto p = percentEffect(status.effects, "healing_received_percent_per_stack", status.stacks);
    p += percentEffect(status.effects, "healing_efficiency_percent_per_stack", status.stacks);
    auto f = flatEffect(status.effects, "healing_received_flat_per_stack", status.stacks);
    f += flatEffect(status.effects, "healing_efficiency_flat_per_stack", status.stacks);
    if (p == 0 && f == 0) continue;
    percent += p; flat += f;
    Json::Value row; row["status"] = status.name; row["side"] = "target"; row["stacks"] = Json::Int64(status.stacks); row["percent"] = Json::Int64(p); row["flat"] = Json::Int64(f); details.append(std::move(row));
  }
  // [功能備註｜62-cpp.23.1 場地治療修正]
  try {
    const auto fields = service->database()->execSqlSync(
        "SELECT name,effects::text AS effects_text FROM combat_field_effects WHERE room_id=$1 AND active=TRUE ORDER BY priority,id", roomId);
    for (const auto& row : fields) {
      const auto effects = row["effects_text"].isNull() ? Json::Value{Json::objectValue}
                                                         : parseJson(row["effects_text"].as<std::string>(), Json::Value{Json::objectValue});
      const auto p = integer(effects["healing_percent"], integer(effects["healing_received_percent"]));
      const auto f = integer(effects["healing_flat"], integer(effects["healing_received_flat"]));
      if (p == 0 && f == 0) continue;
      percent += p; flat += f;
      Json::Value rowOut; rowOut["status"] = row["name"].as<std::string>(); rowOut["side"] = "field"; rowOut["stacks"] = 1; rowOut["percent"] = Json::Int64(p); rowOut["flat"] = Json::Int64(f); details.append(std::move(rowOut));
    }
  } catch (...) {}
  percent = std::clamp<std::int64_t>(percent, -100, 100000);
  long double scaled = static_cast<long double>(base) * static_cast<long double>(100 + percent) / 100.0L;
  scaled += static_cast<long double>(flat);
  const auto maximum = static_cast<long double>(std::numeric_limits<std::int64_t>::max() / 4);
  const auto result = static_cast<std::int64_t>(std::clamp<long double>(std::floor(scaled + 0.000001L), 0.0L, maximum));
  if (breakdown) {
    (*breakdown)["base"] = Json::Int64(base);
    (*breakdown)["percent"] = Json::Int64(percent);
    (*breakdown)["flat"] = Json::Int64(flat);
    (*breakdown)["result"] = Json::Int64(result);
    (*breakdown)["statuses"] = std::move(details);
  }
  return result;
}

void applyEventSkillMigrations(const std::shared_ptr<CoreService>& service) {
  if (!service) return;
  auto db = service->database();
  // [功能備註｜62-cpp.23 事件型技能 migration]
  // status template 的 triggers 與技能 data.event_triggers 採同一 JSON 結構；戰鬥狀態使用泛型 target_type/target_id。
  db->execSqlSync("ALTER TABLE status_templates ADD COLUMN IF NOT EXISTS stack_mode VARCHAR(20) NOT NULL DEFAULT 'refresh'");
  db->execSqlSync("ALTER TABLE status_templates ADD COLUMN IF NOT EXISTS max_stacks BIGINT NOT NULL DEFAULT 1");
  db->execSqlSync("ALTER TABLE status_templates ADD COLUMN IF NOT EXISTS dispellable BOOLEAN NOT NULL DEFAULT TRUE");
  db->execSqlSync("ALTER TABLE status_templates ADD COLUMN IF NOT EXISTS tags JSONB NOT NULL DEFAULT '[]'::jsonb");
  db->execSqlSync("ALTER TABLE status_templates ADD COLUMN IF NOT EXISTS triggers JSONB NOT NULL DEFAULT '[]'::jsonb");
  db->execSqlSync("ALTER TABLE status_templates ADD COLUMN IF NOT EXISTS polarity VARCHAR(20) NOT NULL DEFAULT 'neutral'");
  db->execSqlSync(R"SQL(CREATE TABLE IF NOT EXISTS combat_status_instances (
    id BIGSERIAL PRIMARY KEY,
    room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
    target_type VARCHAR(20) NOT NULL,
    target_id BIGINT NOT NULL,
    status_template_id BIGINT,
    name VARCHAR(120) NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    polarity VARCHAR(20) NOT NULL DEFAULT 'neutral',
    stacks BIGINT NOT NULL DEFAULT 1,
    max_stacks BIGINT NOT NULL DEFAULT 1,
    remaining_rounds BIGINT NOT NULL DEFAULT 0,
    expires_by_round BOOLEAN NOT NULL DEFAULT FALSE,
    dispellable BOOLEAN NOT NULL DEFAULT TRUE,
    tags JSONB NOT NULL DEFAULT '[]'::jsonb,
    effects JSONB NOT NULL DEFAULT '{}'::jsonb,
    triggers JSONB NOT NULL DEFAULT '[]'::jsonb,
    source_type VARCHAR(20) NOT NULL DEFAULT '',
    source_id BIGINT NOT NULL DEFAULT 0,
    active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  ))SQL");
  db->execSqlSync("ALTER TABLE combat_status_instances ADD COLUMN IF NOT EXISTS polarity VARCHAR(20) NOT NULL DEFAULT 'neutral'");
  db->execSqlSync("CREATE INDEX IF NOT EXISTS idx_combat_status_instances_target ON combat_status_instances(room_id,target_type,target_id,active)");
  db->execSqlSync("CREATE INDEX IF NOT EXISTS idx_combat_status_instances_name ON combat_status_instances(room_id,name,active)");

  // [功能備註｜62-cpp.23.1 場地技能 migration]
  db->execSqlSync(R"SQL(CREATE TABLE IF NOT EXISTS combat_field_effects (
    id BIGSERIAL PRIMARY KEY,
    room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
    definition_key VARCHAR(160) NOT NULL,
    name VARCHAR(160) NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    owner_type VARCHAR(20) NOT NULL DEFAULT '',
    owner_id BIGINT NOT NULL DEFAULT 0,
    owner_name VARCHAR(160) NOT NULL DEFAULT '',
    priority BIGINT NOT NULL DEFAULT 0,
    stack_policy VARCHAR(32) NOT NULL DEFAULT 'replace_same',
    remaining_rounds BIGINT NOT NULL DEFAULT 0,
    expires_by_round BOOLEAN NOT NULL DEFAULT FALSE,
    effects JSONB NOT NULL DEFAULT '{}'::jsonb,
    triggers JSONB NOT NULL DEFAULT '[]'::jsonb,
    tags JSONB NOT NULL DEFAULT '[]'::jsonb,
    active BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
  ))SQL");
  db->execSqlSync("CREATE INDEX IF NOT EXISTS idx_combat_field_effects_room ON combat_field_effects(room_id,active,priority DESC)");
  db->execSqlSync("CREATE INDEX IF NOT EXISTS idx_combat_field_effects_key ON combat_field_effects(room_id,definition_key,active)");

  // [功能備註｜62-cpp.23 畫像標記範例]
  // 這兩個狀態是「速寫標記」的正式可執行資料；若 DM 已建立同名模板，不會覆蓋既有描述／ID，
  // 但會補上本系統需要的層數、極性與效果欄位。
  db->execSqlSync(R"SQL(
    INSERT INTO status_templates(name,category,description,duration_rounds,effects,stack_mode,max_stacks,dispellable,tags,triggers,polarity)
    SELECT '畫像標記•攻','標記','每層使標記施加者對此目標造成的傷害 +10%。',0,
           '{"damage_taken_from_source_percent_per_stack":10,"source_scoped":true}'::jsonb,'stack',20,TRUE,
           '["portrait","mark","negative"]'::jsonb,'[]'::jsonb,'negative'
    WHERE NOT EXISTS (SELECT 1 FROM status_templates WHERE name='畫像標記•攻')
  )SQL");
  db->execSqlSync(R"SQL(
    UPDATE status_templates SET stack_mode='stack',max_stacks=20,polarity='negative',
      effects=COALESCE(effects,'{}'::jsonb) || '{"damage_taken_from_source_percent_per_stack":10,"source_scoped":true}'::jsonb,
      tags='["portrait","mark","negative"]'::jsonb,
      updated_at=CURRENT_TIMESTAMP
    WHERE name='畫像標記•攻'
  )SQL");
  db->execSqlSync(R"SQL(
    INSERT INTO status_templates(name,category,description,duration_rounds,effects,stack_mode,max_stacks,dispellable,tags,triggers,polarity)
    SELECT '畫像標記•療','標記','每層治療效率 +10%。',0,
           '{"healing_efficiency_percent_per_stack":10}'::jsonb,'stack',20,TRUE,
           '["portrait","mark","positive"]'::jsonb,'[]'::jsonb,'positive'
    WHERE NOT EXISTS (SELECT 1 FROM status_templates WHERE name='畫像標記•療')
  )SQL");
  db->execSqlSync(R"SQL(
    UPDATE status_templates SET stack_mode='stack',max_stacks=20,polarity='positive',
      effects=COALESCE(effects,'{}'::jsonb) || '{"healing_efficiency_percent_per_stack":10}'::jsonb,
      tags='["portrait","mark","positive"]'::jsonb,
      updated_at=CURRENT_TIMESTAMP
    WHERE name='畫像標記•療'
  )SQL");

  // [功能備註｜62-cpp.23 畫兵成真／速寫標記範例]
  // 同名技能存在時只補事件技能核心欄位；其他 DM 自訂資料保留。角色卡持有模板技能時會在戰鬥事件讀取時
  // 以 template_id 即時 hydrate，因此舊角色不需要重新學習技能。
  db->execSqlSync(R"SQL(
    INSERT INTO skill_templates(name,category,skill_type,level,description,damage_formula,data)
    SELECT '畫兵成真','繪師','active',1,'具現一把墨之武器攻擊敵人，造成三次的高額精神傷害。','1+精神*2',
           '{"combat_attack":true,"combat_check_attribute":"spirit","combat_damage_target":"hp","combat_target_mode":"enemy_single","hit_count":3,"event_triggers":[]}'::jsonb
    WHERE NOT EXISTS (SELECT 1 FROM skill_templates WHERE name='畫兵成真')
  )SQL");
  db->execSqlSync(R"SQL(
    UPDATE skill_templates SET damage_formula=CASE WHEN COALESCE(damage_formula,'')='' THEN '1+精神*2' ELSE damage_formula END,
      data=COALESCE(data,'{}'::jsonb) || '{"combat_attack":true,"combat_check_attribute":"spirit","combat_damage_target":"hp","combat_target_mode":"enemy_single","hit_count":3}'::jsonb,
      updated_at=CURRENT_TIMESTAMP
    WHERE name='畫兵成真'
  )SQL");
  db->execSqlSync(R"SQL(
    INSERT INTO skill_templates(name,category,skill_type,level,description,data)
    SELECT '速寫標記','繪師','passive',1,
           '快速描繪一名敵人或自身，為其繪製【畫像標記•攻】或【畫像標記•療】。造成傷害／施加負面效果時對目標疊加攻標記；附加正面效果／受到傷害／受到治療時為自身疊加療標記。',
           '{"event_triggers":[]}'::jsonb
    WHERE NOT EXISTS (SELECT 1 FROM skill_templates WHERE name='速寫標記')
  )SQL");
  db->execSqlSync(R"SQL(
    UPDATE skill_templates SET skill_type='passive', data=COALESCE(data,'{}'::jsonb) || jsonb_build_object(
      'event_triggers', '[
        {"event":"ON_DAMAGE_DEALT","proc_mode":"per_hit","conditions":{"op":"owner_is_actor"},"effects":[{"type":"add_status","target":"target","status":"畫像標記•攻","stacks":1}]},
        {"event":"ON_NEGATIVE_EFFECT_APPLIED","proc_mode":"per_event","conditions":{"all":[{"op":"owner_is_actor"},{"path":"status.name","op":"neq","value":"畫像標記•攻"}]},"effects":[{"type":"add_status","target":"target","status":"畫像標記•攻","stacks":1}]},
        {"event":"ON_POSITIVE_EFFECT_APPLIED","proc_mode":"per_event","conditions":{"all":[{"op":"owner_is_actor"},{"path":"status.name","op":"neq","value":"畫像標記•療"}]},"effects":[{"type":"add_status","target":"self","status":"畫像標記•療","stacks":2}]},
        {"event":"ON_DAMAGE_TAKEN","proc_mode":"per_hit","conditions":{"op":"owner_is_target"},"effects":[{"type":"add_status","target":"self","status":"畫像標記•療","stacks":2}]},
        {"event":"ON_HEAL_RECEIVED","proc_mode":"per_event","conditions":{"op":"owner_is_target"},"effects":[{"type":"add_status","target":"self","status":"畫像標記•療","stacks":2}]}
      ]'::jsonb
    ), updated_at=CURRENT_TIMESTAMP
    WHERE name='速寫標記'
  )SQL");
}

// [功能備註｜63-cpp.24 儀式效果共用技能核心]
Json::Value applyConfiguredEventSkillEffects(const std::shared_ptr<CoreService>& service,
                                             std::int64_t roomId,
                                             const std::string& actorType,
                                             std::int64_t actorId,
                                             const Json::Value& effects,
                                             const Json::Value& context) {
  Json::Value out{Json::arrayValue};
  TriggerOwner owner; owner.entity.type=actorType; owner.entity.id=actorId; owner.entity.name="ritual";
  Json::Value ctx=context.isObject()?context:Json::Value{Json::objectValue};
  if(!ctx.isMember("actor")){ctx["actor"]["type"]=actorType;ctx["actor"]["id"]=Json::Int64(actorId);}
  if(!ctx.isMember("target"))ctx["target"]=ctx["actor"];
  if(effects.isArray())for(const auto& effect:effects)out.append(applyEffect(service,roomId,owner,effect,ctx));
  else if(effects.isObject())out.append(applyEffect(service,roomId,owner,effects,ctx));
  return out;
}

Json::Value dispatchEventSkillEvent(const std::shared_ptr<CoreService>& service,
                                    std::int64_t roomId,
                                    const std::string& actorType,
                                    std::int64_t actorId,
                                    const std::string& eventType,
                                    const Json::Value& rawContext,
                                    Json::Value* externalProcState) {
  Json::Value out;
  out["event"] = eventType;
  out["triggered"] = Json::arrayValue;
  out["effects"] = Json::arrayValue;
  out["defeated"] = Json::arrayValue;
  out["nested_events"] = Json::arrayValue;
  if (!service || roomId <= 0) return out;

  Json::Value localProcState{Json::objectValue};
  auto* procState = externalProcState ? externalProcState : &localProcState;
  Json::Value context = rawContext.isObject() ? rawContext : Json::Value{Json::objectValue};
  if (!context["actor"].isObject() && !actorType.empty() && actorId > 0) {
    context["actor"]["type"] = actorType;
    context["actor"]["id"] = Json::Int64(actorId);
  }
  if (!context["attacker"].isObject() &&
      (eventType == "COMBAT_ATTACK_RESOLVED" || eventType == "COMBAT_DAMAGE_DEALT") &&
      !actorType.empty() && actorId > 0) {
    context["attacker"] = context["actor"];
  }
  if (!context.isMember("event_instance_id")) {
    static thread_local std::uint64_t eventCounter = 0;
    context["event_instance_id"] = str(context["sequence_id"], "evt") + ":" + std::to_string(++eventCounter);
  }

  auto owners = loadOwners(service, roomId);
  appendStatusTriggerOwners(service, roomId, owners);
  appendFieldTriggerOwners(service, roomId, owners);
  static thread_local int nestedDepth = 0;

  for (const auto& owner : owners) {
    if (!owner.skills.isArray()) continue;
    for (const auto& skill : owner.skills) {
      if (!skill.isObject()) continue;
      const auto triggers = triggerListFromSkill(skill);
      for (Json::ArrayIndex index = 0; index < triggers.size(); ++index) {
        const auto& trigger = triggers[index];
        if (!trigger.isObject() || !eventMatches(str(trigger["event"]), eventType, context)) continue;
        if (!finalHitAllowed(trigger, context)) continue;
        const auto identity = triggerIdentity(owner, skill, index);
        if (triggerIsActive(procState, identity)) continue;  // 阻止同一 trigger 直接遞迴觸發自己。
        if (!conditionMatches(service, roomId, owner, trigger["conditions"], context)) continue;
        const auto procKey = procKeyFor(owner, skill, index, trigger, context);
        if (procAlreadyFired(procState, procKey)) continue;
        const auto chance = std::clamp<std::int64_t>(integer(trigger["chance_percent"], 100), 0, 100);
        if (chance <= 0 || (chance < 100 && randomPercent() > chance)) continue;

        markProcFired(procState, procKey);
        setTriggerActive(procState, identity, true);
        try {
          Json::Value fired;
          fired["owner_type"] = owner.entity.type;
          fired["owner_id"] = Json::Int64(owner.entity.id);
          fired["owner_name"] = owner.entity.name;
          fired["skill_name"] = str(skill["name"], "事件效果");
          fired["trigger_index"] = index;
          fired["proc_mode"] = str(trigger["proc_mode"], "per_hit");
          out["triggered"].append(fired);

          const auto effects = trigger["effects"].isArray() ? trigger["effects"] : Json::Value{Json::arrayValue};
          for (const auto& effect : effects) {
            if (!effect.isObject()) continue;
            auto applied = applyEffect(service, roomId, owner, effect, context);
            applied["source_skill"] = str(skill["name"], "事件效果");
            applied["source_owner_type"] = owner.entity.type;
            applied["source_owner_id"] = Json::Int64(owner.entity.id);

            if (applied.get("downed", false).asBool()) {
              Json::Value defeated;
              defeated["type"] = applied["target_type"];
              defeated["id"] = applied["target_id"];
              out["defeated"].append(defeated);
            }

            // [功能備註｜62-cpp.23 狀態／治療事件順序]
            // 先完成本次效果，再送衍生事件；因此「受到治療後 +2 療標記」不會回頭放大同一次治療。
            if (nestedDepth < 8) {
              const auto appliedType = lower(str(applied["type"]));
              const auto targetRef = resultTargetRef(applied);
              Json::Value nestedContext = context;
              nestedContext["actor"]["type"] = owner.entity.type;
              nestedContext["actor"]["id"] = Json::Int64(owner.entity.id);
              nestedContext["actor"]["name"] = owner.entity.name;
              nestedContext["attacker"] = nestedContext["actor"];
              nestedContext["target"]["type"] = targetRef.type;
              nestedContext["target"]["id"] = Json::Int64(targetRef.id);
              nestedContext["source_skill"] = str(skill["name"], "事件效果");
              nestedContext["source_event"] = eventType;
              nestedContext["event_chain_id"] = str(context["event_chain_id"], str(context["sequence_id"], str(context["event_instance_id"]))) ;

              auto dispatchNested = [&](const std::string& nestedEvent, Json::Value ctx) {
                // [功能備註｜62-cpp.23 每個衍生事件獨立判定]
                // 同一技能若先後施加兩個不同負面效果，兩次都必須算「一次施加負面效果」，
                // 因此衍生事件不能沿用父事件的 event_instance_id。
                ctx.removeMember("event_instance_id");
                ++nestedDepth;
                auto nested = dispatchEventSkillEvent(service, roomId, owner.entity.type, owner.entity.id,
                                                      nestedEvent, ctx, procState);
                --nestedDepth;
                out["nested_events"].append(std::move(nested));
              };

              if (appliedType == "add_status" && targetRef.id > 0) {
                nestedContext["status"]["name"] = applied["name"];
                nestedContext["status"]["polarity"] = applied["polarity"];
                nestedContext["status"]["before_stacks"] = applied["before_stacks"];
                nestedContext["status"]["stacks"] = applied["stacks"];
                nestedContext["status"]["stack_delta"] = applied["stack_delta"];
                dispatchNested(applied.get("new_status", false).asBool() ? "COMBAT_STATUS_ADDED"
                                                                         : "COMBAT_STATUS_STACK_CHANGED",
                               nestedContext);
                const auto polarity = lower(str(applied["polarity"]));
                if (polarity == "positive") dispatchNested("COMBAT_POSITIVE_EFFECT_APPLIED", nestedContext);
                else if (polarity == "negative") dispatchNested("COMBAT_NEGATIVE_EFFECT_APPLIED", nestedContext);
              } else if ((appliedType == "consume_status" || appliedType == "remove_status_stacks") && targetRef.id > 0) {
                nestedContext["status"]["name"] = applied["name"];
                nestedContext["status"]["before_stacks"] = applied["before_stacks"];
                nestedContext["status"]["stacks"] = applied["stacks"];
                nestedContext["status"]["stack_delta"] = applied["stack_delta"];
                dispatchNested(integer(applied["stacks"]) <= 0 ? "COMBAT_STATUS_REMOVED"
                                                                  : "COMBAT_STATUS_STACK_CHANGED",
                               nestedContext);
              } else if (appliedType == "heal" && integer(applied["actual_healing"]) > 0) {
                nestedContext["healing"] = applied["actual_healing"];
                nestedContext["base_healing"] = applied["base_amount"];
                dispatchNested("COMBAT_HEAL_RESOLVED", nestedContext);
              } else if (appliedType == "bonus_damage" && integer(applied["amount"]) > 0 && !applied.get("skipped", false).asBool()) {
                nestedContext["damage"] = applied["amount"];
                nestedContext["hit"] = true;
                dispatchNested("COMBAT_DAMAGE_DEALT", nestedContext);
              }
            }

            out["effects"].append(std::move(applied));
          }
        } catch (...) {
          setTriggerActive(procState, identity, false);
          throw;
        }
        setTriggerActive(procState, identity, false);
      }
    }
  }

  // [功能備註｜62-cpp.23 連鎖擊敗]
  // 額外傷害若真正擊敗目標，也會補送一次 COMBAT_ENTITY_DEFEATED；限制遞迴深度避免死亡連鎖無限循環。
  if (eventType != "COMBAT_ENTITY_DEFEATED" && !out["defeated"].empty() && nestedDepth < 8) {
    for (const auto& defeated : out["defeated"]) {
      Json::Value deathContext = context;
      deathContext["defeated"] = defeated;
      deathContext["target"] = defeated;
      deathContext["source_event"] = eventType;
      ++nestedDepth;
      auto chained = dispatchEventSkillEvent(service, roomId, actorType, actorId,
                                             "COMBAT_ENTITY_DEFEATED", deathContext, procState);
      --nestedDepth;
      out["nested_events"].append(std::move(chained));
    }
  }
  return out;
}

Json::Value activeCombatFieldEffects(const CoreService& service,
                                     std::int64_t roomId) {
  Json::Value out{Json::arrayValue};
  if (roomId <= 0) return out;
  try {
    const auto rows = service.database()->execSqlSync(
        "SELECT row_to_json(f)::text AS json_text FROM combat_field_effects f WHERE room_id=$1 AND active=TRUE ORDER BY priority DESC,id",
        roomId);
    for (const auto& row : rows) out.append(parseJson(row["json_text"].as<std::string>()));
  } catch (const std::exception& error) {
    LOG_WARN << "active combat fields degraded for room " << roomId << ": " << error.what();
  }
  return out;
}

void clearCombatFieldEffects(const std::shared_ptr<CoreService>& service,
                             std::int64_t roomId) {
  if (!service || roomId <= 0) return;
  try {
    service->database()->execSqlSync(
        "UPDATE combat_field_effects SET active=FALSE,updated_at=CURRENT_TIMESTAMP WHERE room_id=$1 AND active=TRUE",
        roomId);
  } catch (const std::exception& error) {
    LOG_WARN << "clear combat fields degraded for room " << roomId << ": " << error.what();
  }
}

void tickEventSkillStatuses(const std::shared_ptr<CoreService>& service,
                            std::int64_t roomId,
                            std::int64_t round) {
  if (!service || roomId <= 0) return;
  try {
    service->database()->execSqlSync(
        "UPDATE combat_status_instances SET remaining_rounds=GREATEST(0,remaining_rounds-1),updated_at=CURRENT_TIMESTAMP "
        "WHERE room_id=$1 AND active=TRUE AND remaining_rounds>0",
        roomId);
    service->database()->execSqlSync(
        "UPDATE combat_status_instances SET active=FALSE,updated_at=CURRENT_TIMESTAMP "
        "WHERE room_id=$1 AND active=TRUE AND remaining_rounds=0 AND expires_by_round=TRUE",
        roomId);
    service->database()->execSqlSync(
        "UPDATE combat_field_effects SET remaining_rounds=GREATEST(0,remaining_rounds-1),updated_at=CURRENT_TIMESTAMP "
        "WHERE room_id=$1 AND active=TRUE AND remaining_rounds>0", roomId);
    service->database()->execSqlSync(
        "UPDATE combat_field_effects SET active=FALSE,updated_at=CURRENT_TIMESTAMP "
        "WHERE room_id=$1 AND active=TRUE AND remaining_rounds=0 AND expires_by_round=TRUE", roomId);
    Json::Value context; context["round"] = Json::Int64(round);
    dispatchEventSkillEvent(service, roomId, "system", 0, "COMBAT_ROUND_STARTED", context);
  } catch (const std::exception& error) {
    LOG_WARN << "tick event skill statuses degraded for room " << roomId << ": " << error.what();
  }
}

}  // namespace trpg
