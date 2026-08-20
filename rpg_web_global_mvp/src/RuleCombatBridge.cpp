#include "trpg/RuleCombatBridge.h"

#include "trpg/CoreService.h"
#include "trpg/RoomSocket.h"
#include "trpg/RuleEngine.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace trpg {
namespace {

using Request = drogon::HttpRequestPtr;
using Callback = std::function<void(const drogon::HttpResponsePtr&)>;
using Db = drogon::orm::DbClientPtr;

struct ApiError final : std::runtime_error {
  drogon::HttpStatusCode status;
  ApiError(drogon::HttpStatusCode value, std::string message)
      : std::runtime_error(std::move(message)), status(value) {}
};

Json::Value parseJson(const std::string& source,
                      Json::Value fallback = Json::Value{Json::objectValue}) {
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value value;
  std::string errors;
  if (!reader->parse(source.data(), source.data() + source.size(), &value, &errors)) {
    return fallback;
  }
  return value;
}

std::string compactJson(const Json::Value& value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value);
}

std::string text(const Json::Value& value, std::string fallback = {}) {
  if (value.isString()) return value.asString();
  if (value.isInt64()) return std::to_string(value.asInt64());
  if (value.isUInt64()) return std::to_string(value.asUInt64());
  return fallback;
}

std::int64_t integer(const Json::Value& value, std::int64_t fallback = 0) {
  try {
    if (value.isInt64()) return value.asInt64();
    if (value.isUInt64()) return static_cast<std::int64_t>(value.asUInt64());
    if (value.isDouble()) return static_cast<std::int64_t>(value.asDouble());
    if (value.isString()) return std::stoll(value.asString());
  } catch (...) {
  }
  return fallback;
}

void reply(Callback& callback, Json::Value body,
           drogon::HttpStatusCode status = drogon::k200OK) {
  auto response = drogon::HttpResponse::newHttpJsonResponse(body);
  response->setStatusCode(status);
  response->addHeader("Cache-Control", "no-store");
  callback(response);
}

void fail(Callback& callback, drogon::HttpStatusCode status,
          const std::string& message) {
  Json::Value body;
  body["error"] = message;
  reply(callback, std::move(body), status);
}

template <typename Fn>
void runHandler(Callback& cb, Fn&& fn, const char* fallback) {
  try {
    fn();
  } catch (const ApiError& e) {
    fail(cb, e.status, e.what());
  } catch (const std::exception& e) {
    LOG_ERROR << "rule combat bridge error: " << e.what();
    fail(cb, drogon::k500InternalServerError, fallback);
  }
}

void requireAdmin(const std::shared_ptr<CoreService>& service,
                  const Request& request) {
  const auto claims = service->authenticateRequest(request);
  if (!claims) throw ApiError(drogon::k401Unauthorized, "請先登入");
  const auto rows = service->database()->execSqlSync(
      "SELECT is_admin FROM users WHERE id=$1", claims->id);
  if (rows.empty() || !rows[0]["is_admin"].as<bool>()) {
    throw ApiError(drogon::k403Forbidden, "需要 DM 權限");
  }
}

std::int64_t positiveId(const std::string& raw, const char* message = "ID 無效") {
  try {
    const auto value = std::stoll(raw);
    if (value <= 0) throw std::invalid_argument{"id"};
    return value;
  } catch (...) {
    throw ApiError(drogon::k400BadRequest, message);
  }
}

Json::Value loadBridgeState(const std::shared_ptr<CoreService>& service,
                            std::int64_t roomId) {
  const auto rows = service->database()->execSqlSync(R"SQL(
    SELECT battle_active,round,
           combat_victory_condition::text AS victory_text,
           rule_combat_state::text AS state_text
    FROM rooms WHERE id=$1
  )SQL", roomId);
  if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到房間");
  Json::Value out;
  out["room_id"] = Json::Int64(roomId);
  out["battle_active"] = rows[0]["battle_active"].as<bool>();
  out["round"] = Json::Int64(rows[0]["round"].as<std::int64_t>());
  out["victory_condition"] = parseJson(rows[0]["victory_text"].as<std::string>());
  out["state"] = parseJson(rows[0]["state_text"].as<std::string>());
  const auto bosses = service->database()->execSqlSync(R"SQL(
    SELECT rm.id,rm.monster_template_id,mt.name,rm.status,rm.current_hp,rm.max_hp,
           rm.rule_boss_phase,rm.rule_modifiers::text AS modifiers_text
    FROM room_monsters rm JOIN monster_templates mt ON mt.id=rm.monster_template_id
    WHERE rm.room_id=$1 AND (rm.rule_boss_phase>1 OR rm.rule_modifiers<>'{}'::jsonb)
    ORDER BY rm.id
  )SQL", roomId);
  out["rule_monsters"] = Json::arrayValue;
  for (const auto& row : bosses) {
    Json::Value item;
    item["id"] = Json::Int64(row["id"].as<std::int64_t>());
    item["monster_template_id"] = Json::Int64(row["monster_template_id"].as<std::int64_t>());
    item["name"] = row["name"].as<std::string>();
    item["status"] = row["status"].as<std::string>();
    item["current_hp"] = Json::Int64(row["current_hp"].as<std::int64_t>());
    item["max_hp"] = Json::Int64(row["max_hp"].as<std::int64_t>());
    item["boss_phase"] = Json::Int64(row["rule_boss_phase"].as<std::int64_t>());
    item["modifiers"] = parseJson(row["modifiers_text"].as<std::string>());
    out["rule_monsters"].append(item);
  }
  return out;
}

Json::Value spawnMonster(const std::shared_ptr<CoreService>& service,
                         std::int64_t roomId, const Json::Value& effect,
                         bool dryRun) {
  Json::Value out;
  const auto templateId = integer(effect["template_id"]);
  const auto count = std::clamp<std::int64_t>(integer(effect["count"], 1), 1, 20);
  if (templateId <= 0) {
    out["error"] = "spawn_monster 需要 template_id";
    out["applied"] = false;
    return out;
  }
  const auto templates = service->database()->execSqlSync(
      "SELECT id,name,max_hp,attributes::text AS attributes_text FROM monster_templates WHERE id=$1",
      templateId);
  if (templates.empty()) {
    out["error"] = "找不到怪物模板";
    out["applied"] = false;
    return out;
  }
  out["template_id"] = Json::Int64(templateId);
  out["count"] = Json::Int64(count);
  out["name"] = templates[0]["name"].as<std::string>();
  out["instance_ids"] = Json::arrayValue;
  if (dryRun) {
    out["applied"] = false;
    return out;
  }
  const auto attributes = parseJson(templates[0]["attributes_text"].as<std::string>());
  const auto hpBase = std::max<std::int64_t>(1, templates[0]["max_hp"].as<std::int64_t>());
  const auto hpMultiplier = std::clamp<double>(effect.get("hp_multiplier", 1.0).asDouble(), 0.01, 100.0);
  const auto hp = std::max<std::int64_t>(1, static_cast<std::int64_t>(hpBase * hpMultiplier));
  const auto spirit = std::max<std::int64_t>(0, integer(attributes["spirit"]));
  const auto fifth = std::max({std::int64_t{0}, integer(attributes["great_way"]), integer(attributes["faith"]), integer(attributes["fifth"])});
  const auto status = text(effect["status"], "encountered") == "hidden" ? "hidden" : "encountered";
  const auto phase = std::max<std::int64_t>(1, integer(effect["boss_phase"], 1));
  const auto modifiers = effect["modifiers"].isObject() ? effect["modifiers"] : Json::Value{Json::objectValue};
  for (std::int64_t i = 0; i < count; ++i) {
    const auto rows = service->database()->execSqlSync(R"SQL(
      INSERT INTO room_monsters(room_id,monster_template_id,status,current_hp,max_hp,
        current_spirit,max_spirit,current_fifth,max_fifth,rule_boss_phase,rule_modifiers)
      VALUES($1,$2,$3,$4,$4,$5,$5,$6,$6,$7,$8::jsonb) RETURNING id
    )SQL", roomId, templateId, status, hp, spirit, fifth, phase, compactJson(modifiers));
    out["instance_ids"].append(Json::Int64(rows[0]["id"].as<std::int64_t>()));
  }
  out["applied"] = true;
  return out;
}

void setRuleActive(const std::shared_ptr<CoreService>& service,
                   std::int64_t roomId, const Json::Value& effect, bool active) {
  const auto ruleId = integer(effect["rule_id"]);
  const auto code = text(effect["rule_code"]);
  if (ruleId > 0) {
    service->database()->execSqlSync(
        "UPDATE rule_engine_rules SET active=$1,updated_at=CURRENT_TIMESTAMP WHERE id=$2",
        active, ruleId);
  } else if (!code.empty()) {
    service->database()->execSqlSync(
        "UPDATE rule_engine_rules SET active=$1,updated_at=CURRENT_TIMESTAMP WHERE code=$2 AND (room_id IS NULL OR room_id=$3)",
        active, code, roomId);
  }
}

Json::Value updateMonster(const std::shared_ptr<CoreService>& service,
                          std::int64_t roomId, const Json::Value& effect,
                          bool dryRun) {
  Json::Value out;
  const auto monsterId = integer(effect["room_monster_id"]);
  if (monsterId <= 0) {
    out["error"] = "modify_monster/set_boss_phase 需要 room_monster_id";
    out["applied"] = false;
    return out;
  }
  const auto rows = service->database()->execSqlSync(
      "SELECT current_hp,max_hp,rule_boss_phase,rule_modifiers::text AS modifiers_text FROM room_monsters WHERE room_id=$1 AND id=$2",
      roomId, monsterId);
  if (rows.empty()) {
    out["error"] = "找不到房間怪物";
    out["applied"] = false;
    return out;
  }
  auto modifiers = parseJson(rows[0]["modifiers_text"].as<std::string>());
  if (!modifiers.isObject()) modifiers = Json::Value{Json::objectValue};
  if (effect["modifiers"].isObject()) {
    for (const auto& key : effect["modifiers"].getMemberNames()) modifiers[key] = effect["modifiers"][key];
  }
  auto maxHp = rows[0]["max_hp"].as<std::int64_t>();
  auto currentHp = rows[0]["current_hp"].as<std::int64_t>();
  if (effect.isMember("max_hp_multiplier")) {
    const auto mult = std::clamp<double>(effect["max_hp_multiplier"].asDouble(), 0.01, 100.0);
    maxHp = std::max<std::int64_t>(1, static_cast<std::int64_t>(maxHp * mult));
    currentHp = std::min(currentHp, maxHp);
  }
  if (effect.isMember("heal")) currentHp = std::min(maxHp, currentHp + std::max<std::int64_t>(0, integer(effect["heal"])));
  if (effect.isMember("damage")) currentHp = std::max<std::int64_t>(0, currentHp - std::max<std::int64_t>(0, integer(effect["damage"])));
  const auto phase = std::max<std::int64_t>(1, integer(effect["phase"], rows[0]["rule_boss_phase"].as<std::int64_t>()));
  out["room_monster_id"] = Json::Int64(monsterId);
  out["max_hp"] = Json::Int64(maxHp);
  out["current_hp"] = Json::Int64(currentHp);
  out["boss_phase"] = Json::Int64(phase);
  out["modifiers"] = modifiers;
  if (!dryRun) {
    service->database()->execSqlSync(
        "UPDATE room_monsters SET current_hp=$1,max_hp=$2,rule_boss_phase=$3,rule_modifiers=$4::jsonb,status=CASE WHEN $1<=0 THEN 'defeated' ELSE status END,updated_at=CURRENT_TIMESTAMP WHERE room_id=$5 AND id=$6",
        currentHp, maxHp, phase, compactJson(modifiers), roomId, monsterId);
  }
  out["applied"] = !dryRun;
  return out;
}

Json::Value addCombatStatus(const std::shared_ptr<CoreService>& service,
                            std::int64_t roomId, std::int64_t actorUserId,
                            const Json::Value& effect, bool dryRun) {
  Json::Value out;
  auto entityType = text(effect["entity_type"], "player");
  auto entityId = integer(effect["entity_id"], actorUserId);
  if (entityId <= 0 && entityType == "player") entityId = actorUserId;
  if (entityType != "player" && entityType != "monster" && entityType != "npc") entityType = "player";
  if (entityId <= 0) {
    out["error"] = "add_combat_status 缺少有效 entity_id";
    out["applied"] = false;
    return out;
  }
  const auto name = text(effect["name"], "規則效果").substr(0, 120);
  const auto rounds = std::max<std::int64_t>(0, integer(effect["duration_rounds"], 1));
  const auto effects = effect["effects"].isObject() ? effect["effects"] : Json::Value{Json::objectValue};
  if (!dryRun) {
    service->database()->execSqlSync(R"SQL(
      INSERT INTO combat_entity_statuses(room_id,entity_type,entity_id,name,description,remaining_rounds,effects,source,active,expires_by_round)
      VALUES($1,$2,$3,$4,$5,$6,$7::jsonb,$8,TRUE,$9)
    )SQL", roomId, entityType, entityId, name, text(effect["description"]), rounds,
        compactJson(effects), "規則戰鬥橋接", rounds > 0);
  }
  out["entity_type"] = entityType;
  out["entity_id"] = Json::Int64(entityId);
  out["name"] = name;
  out["applied"] = !dryRun;
  return out;
}

Json::Value victoryConditionResult(const std::shared_ptr<CoreService>& service,
                                   std::int64_t roomId, const Json::Value& condition) {
  Json::Value result;
  const auto type = text(condition["type"], "eliminate_enemies");
  result["type"] = type;
  result["met"] = false;
  if (type == "eliminate_enemies") {
    const auto rows = service->database()->execSqlSync(R"SQL(
      SELECT
        (SELECT COUNT(*) FROM room_monsters WHERE room_id=$1 AND status='encountered' AND current_hp>0) +
        (SELECT COUNT(*) FROM room_npcs WHERE room_id=$1 AND status='active' AND combat_side='enemy' AND current_hp>0) AS remaining
    )SQL", roomId);
    const auto remaining = rows.empty() ? 0 : rows[0]["remaining"].as<std::int64_t>();
    result["remaining_enemies"] = Json::Int64(remaining);
    result["met"] = remaining <= 0;
  } else if (type == "survive_rounds") {
    const auto rows = service->database()->execSqlSync("SELECT round FROM rooms WHERE id=$1", roomId);
    const auto current = rows.empty() ? 0 : rows[0]["round"].as<std::int64_t>();
    const auto required = std::max<std::int64_t>(1, integer(condition["rounds"], 1));
    result["current_round"] = Json::Int64(current);
    result["required_rounds"] = Json::Int64(required);
    result["met"] = current >= required;
  } else if (type == "monster_defeated") {
    const auto monsterId = integer(condition["room_monster_id"]);
    const auto rows = service->database()->execSqlSync(
        "SELECT status,current_hp FROM room_monsters WHERE room_id=$1 AND id=$2", roomId, monsterId);
    result["met"] = !rows.empty() && (rows[0]["status"].as<std::string>() == "defeated" || rows[0]["current_hp"].as<std::int64_t>() <= 0);
  } else if (type == "combat_flag") {
    const auto key = text(condition["key"]);
    const auto rows = service->database()->execSqlSync("SELECT rule_combat_state::text AS state_text FROM rooms WHERE id=$1", roomId);
    auto state = rows.empty() ? Json::Value{Json::objectValue} : parseJson(rows[0]["state_text"].as<std::string>());
    result["met"] = !key.empty() && state["flags"].isArray() && [&] {
      for (const auto& item : state["flags"]) if (text(item) == key) return true;
      return false;
    }();
  }
  return result;
}

}  // namespace

Json::Value applyRuleCombatEffect(const std::shared_ptr<CoreService>& service,
                                  const Json::Value& rule,
                                  std::int64_t roomId,
                                  std::int64_t actorUserId,
                                  const Json::Value& context,
                                  const Json::Value& effect,
                                  bool dryRun) {
  Json::Value out;
  out["supported"] = true;
  const auto type = text(effect["type"]);
  if (type == "spawn_monster") {
    out = spawnMonster(service, roomId, effect, dryRun);
  } else if (type == "start_battle") {
    out["applied"] = !dryRun;
    if (!dryRun) out["room"] = startBattleFromSystem(service, roomId, actorUserId,
        text(effect["reason"], "規則觸發戰鬥"), context);
  } else if (type == "end_battle") {
    out["applied"] = !dryRun;
    if (!dryRun) out["room"] = endBattleFromSystem(service, roomId, actorUserId,
        text(effect["reason"], "規則結束戰鬥"), context);
  } else if (type == "force_encounter") {
    const auto monsterId = integer(effect["room_monster_id"]);
    out["room_monster_id"] = Json::Int64(monsterId);
    out["applied"] = !dryRun;
    if (!dryRun && monsterId > 0) {
      service->database()->execSqlSync(
          "UPDATE room_monsters SET status='encountered',updated_at=CURRENT_TIMESTAMP WHERE room_id=$1 AND id=$2",
          roomId, monsterId);
    }
  } else if (type == "modify_monster" || type == "set_boss_phase") {
    out = updateMonster(service, roomId, effect, dryRun);
  } else if (type == "add_combat_status") {
    out = addCombatStatus(service, roomId, actorUserId, effect, dryRun);
  } else if (type == "set_victory_condition") {
    auto condition = effect["condition"].isObject() ? effect["condition"] : Json::Value{Json::objectValue};
    if (!condition.isMember("type")) condition["type"] = "eliminate_enemies";
    condition["auto_end"] = effect.get("auto_end", condition.get("auto_end", true));
    out["condition"] = condition;
    out["applied"] = !dryRun;
    if (!dryRun) {
      service->database()->execSqlSync(
          "UPDATE rooms SET combat_victory_condition=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=$2",
          compactJson(condition), roomId);
    }
  } else if (type == "enable_rule" || type == "disable_rule") {
    out["applied"] = !dryRun;
    out["active"] = type == "enable_rule";
    if (!dryRun) setRuleActive(service, roomId, effect, type == "enable_rule");
  } else if (type == "set_combat_flag" || type == "clear_combat_flag" ||
             type == "set_combat_value" || type == "modify_combat_value") {
    const auto rows = service->database()->execSqlSync(
        "SELECT rule_combat_state::text AS state_text FROM rooms WHERE id=$1", roomId);
    auto state = rows.empty() ? Json::Value{Json::objectValue} : parseJson(rows[0]["state_text"].as<std::string>());
    if (!state["flags"].isArray()) state["flags"] = Json::arrayValue;
    if (!state["values"].isObject()) state["values"] = Json::objectValue;
    const auto key = text(effect["key"]);
    if (type == "set_combat_flag" && !key.empty()) {
      bool exists = false; for (const auto& item : state["flags"]) if (text(item) == key) exists = true;
      if (!exists) state["flags"].append(key);
    } else if (type == "clear_combat_flag" && !key.empty()) {
      Json::Value next{Json::arrayValue}; for (const auto& item : state["flags"]) if (text(item) != key) next.append(item); state["flags"] = next;
    } else if (type == "set_combat_value" && !key.empty()) {
      state["values"][key] = effect["value"];
    } else if (type == "modify_combat_value" && !key.empty()) {
      state["values"][key] = Json::Int64(integer(state["values"][key]) + integer(effect["amount"], 1));
    }
    out["state"] = state;
    out["applied"] = !dryRun;
    if (!dryRun) service->database()->execSqlSync(
        "UPDATE rooms SET rule_combat_state=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=$2",
        compactJson(state), roomId);
  } else {
    out["supported"] = false;
    out["applied"] = false;
    return out;
  }
  if (!dryRun) RoomSocket::broadcastSnapshotForRoom(roomId);
  out["type"] = type;
  out["rule_id"] = rule["id"];
  return out;
}

Json::Value evaluateRuleCombatVictory(const std::shared_ptr<CoreService>& service,
                                      std::int64_t roomId,
                                      std::int64_t actorUserId,
                                      const std::string& sourceEvent) {
  Json::Value out;
  out["checked"] = false;
  const auto rows = service->database()->execSqlSync(
      "SELECT battle_active,combat_victory_condition::text AS victory_text FROM rooms WHERE id=$1",
      roomId);
  if (rows.empty()) return out;
  auto condition = parseJson(rows[0]["victory_text"].as<std::string>());
  if (!condition.isObject() || condition.empty()) return out;
  out["checked"] = true;
  out["condition"] = condition;
  out["result"] = victoryConditionResult(service, roomId, condition);
  if (!out["result"]["met"].asBool()) return out;
  Json::Value event;
  event["source_event"] = sourceEvent;
  event["condition"] = condition;
  event["result"] = out["result"];
  service->addEvent(roomId, actorUserId, "combat_victory_condition", event);
  evaluateRuleEvent(service, roomId, actorUserId, "COMBAT_VICTORY_CONDITION_MET", event, false);
  if (condition.get("auto_end", false).asBool() && rows[0]["battle_active"].as<bool>()) {
    endBattleFromSystem(service, roomId, actorUserId, "特殊勝利條件已達成", event);
    out["battle_ended"] = true;
  }
  return out;
}

void registerRuleCombatBridgeRoutes(const std::shared_ptr<CoreService>& service) {
  drogon::app().registerHandler(
      "/api/admin/rooms/{1}/rule-combat",
      [service](const Request& req, Callback&& raw, const std::string& roomText) {
        Callback cb = std::move(raw);
        runHandler(cb, [&] {
          requireAdmin(service, req);
          reply(cb, loadBridgeState(service, positiveId(roomText, "房間 ID 無效")));
        }, "讀取規則戰鬥狀態失敗");
      }, {drogon::Get});

  drogon::app().registerHandler(
      "/api/admin/rooms/{1}/rule-combat",
      [service](const Request& req, Callback&& raw, const std::string& roomText) {
        Callback cb = std::move(raw);
        runHandler(cb, [&] {
          requireAdmin(service, req);
          const auto roomId = positiveId(roomText, "房間 ID 無效");
          const auto json = req->getJsonObject();
          const auto body = json && json->isObject() ? *json : Json::Value{Json::objectValue};
          if (body["victory_condition"].isObject()) {
            service->database()->execSqlSync(
                "UPDATE rooms SET combat_victory_condition=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=$2",
                compactJson(body["victory_condition"]), roomId);
          }
          if (body["state"].isObject()) {
            service->database()->execSqlSync(
                "UPDATE rooms SET rule_combat_state=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=$2",
                compactJson(body["state"]), roomId);
          }
          RoomSocket::broadcastSnapshotForRoom(roomId);
          reply(cb, loadBridgeState(service, roomId));
        }, "更新規則戰鬥狀態失敗");
      }, {drogon::Patch});
}

}  // namespace trpg
