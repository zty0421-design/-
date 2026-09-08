#include "trpg/RuleEngine.h"

#include "trpg/CoreService.h"
#include "trpg/LegacyCompat.h"
#include "trpg/RuleCombatBridge.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

struct UserInfo {
  std::int64_t id{};
  bool isAdmin{};
  std::string username;
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
  if (value.isBool()) return value.asBool() ? "true" : "false";
  if (value.isInt64()) return std::to_string(value.asInt64());
  if (value.isUInt64()) return std::to_string(value.asUInt64());
  if (value.isDouble()) return std::to_string(value.asDouble());
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

double number(const Json::Value& value, double fallback = 0.0) {
  try {
    if (value.isNumeric()) return value.asDouble();
    if (value.isString()) return std::stod(value.asString());
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
  Json::Value out;
  out["error"] = message;
  reply(callback, std::move(out), status);
}

template <typename Function>
void runHandler(Callback& callback, Function&& function,
                const char* genericMessage = "規則引擎操作失敗") {
  try {
    function();
  } catch (const ApiError& error) {
    fail(callback, error.status, error.what());
  } catch (const std::exception& error) {
    LOG_ERROR << "rule engine error: " << error.what();
    fail(callback, drogon::k500InternalServerError, genericMessage);
  }
}

Json::Value bodyOrEmpty(const Request& request) {
  const auto json = request->getJsonObject();
  return json && json->isObject() ? *json : Json::Value{Json::objectValue};
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

UserInfo requireUser(const std::shared_ptr<CoreService>& service,
                     const Request& request, bool admin = false) {
  const auto claims = service->authenticateRequest(request);
  if (!claims) throw ApiError(drogon::k401Unauthorized, "請先登入");
  const auto rows = service->database()->execSqlSync(
      "SELECT id,username,is_admin FROM users WHERE id=$1", claims->id);
  if (rows.empty()) throw ApiError(drogon::k401Unauthorized, "帳號不存在");
  UserInfo user{rows[0]["id"].as<std::int64_t>(), rows[0]["is_admin"].as<bool>(),
                rows[0]["username"].as<std::string>()};
  if (admin && !user.isAdmin) throw ApiError(drogon::k403Forbidden, "需要 DM 權限");
  return user;
}

Json::Value rowJson(const drogon::orm::Result& rows, std::size_t index = 0) {
  if (rows.empty() || index >= rows.size()) return Json::Value{};
  return parseJson(rows[index]["json_text"].as<std::string>());
}

Json::Value listRows(const drogon::orm::Result& rows) {
  Json::Value out{Json::arrayValue};
  for (std::size_t i = 0; i < rows.size(); ++i) out.append(rowJson(rows, i));
  return out;
}

Json::Value defaultPlayerState() {
  Json::Value state{Json::objectValue};
  state["flags"] = Json::arrayValue;
  state["counters"] = Json::objectValue;
  state["values"] = Json::objectValue;
  state["countdowns"] = Json::objectValue;
  return state;
}

Json::Value loadPlayerState(const Db& db, std::int64_t roomId,
                            std::int64_t userId) {
  if (userId <= 0) return defaultPlayerState();
  const auto rows = db->execSqlSync(
      "SELECT state::text AS state_text FROM rule_engine_player_state WHERE room_id=$1 AND user_id=$2",
      roomId, userId);
  if (rows.empty() || rows[0]["state_text"].isNull()) return defaultPlayerState();
  auto state = parseJson(rows[0]["state_text"].as<std::string>(), defaultPlayerState());
  if (!state["flags"].isArray()) state["flags"] = Json::arrayValue;
  if (!state["counters"].isObject()) state["counters"] = Json::objectValue;
  if (!state["values"].isObject()) state["values"] = Json::objectValue;
  if (!state["countdowns"].isObject()) state["countdowns"] = Json::objectValue;
  return state;
}

void savePlayerState(const Db& db, std::int64_t roomId, std::int64_t userId,
                     const Json::Value& state) {
  if (userId <= 0) return;
  db->execSqlSync(
      "INSERT INTO rule_engine_player_state(room_id,user_id,state) VALUES($1,$2,$3::jsonb) "
      "ON CONFLICT(room_id,user_id) DO UPDATE SET state=EXCLUDED.state,updated_at=CURRENT_TIMESTAMP",
      roomId, userId, compactJson(state));
}

bool arrayContains(const Json::Value& array, const Json::Value& expected) {
  if (!array.isArray()) return false;
  for (const auto& item : array) {
    if (item == expected) return true;
    if (item.isString() && expected.isString() && item.asString() == expected.asString()) return true;
  }
  return false;
}

void addFlag(Json::Value& state, const std::string& flag) {
  if (flag.empty()) return;
  if (!state["flags"].isArray()) state["flags"] = Json::arrayValue;
  for (const auto& item : state["flags"]) if (text(item) == flag) return;
  state["flags"].append(flag);
}

void removeFlag(Json::Value& state, const std::string& flag) {
  Json::Value next{Json::arrayValue};
  if (state["flags"].isArray()) {
    for (const auto& item : state["flags"]) if (text(item) != flag) next.append(item);
  }
  state["flags"] = std::move(next);
}

Json::Value entityTags(const Db& db, const std::string& type,
                       const std::string& key) {
  Json::Value tags{Json::arrayValue};
  if (type.empty() || key.empty()) return tags;
  const auto rows = db->execSqlSync(
      "SELECT t.name FROM entity_tags e JOIN content_tags t ON t.id=e.tag_id "
      "WHERE e.entity_type=$1 AND e.entity_key=$2 AND t.active=TRUE ORDER BY t.name",
      type, key);
  for (const auto& row : rows) tags.append(row["name"].as<std::string>());
  return tags;
}

std::optional<std::int64_t> effectiveNodeId(const Db& db, std::int64_t roomId,
                                            std::int64_t userId) {
  if (userId <= 0) return std::nullopt;
  const auto rows = db->execSqlSync(R"SQL(
    SELECT COALESCE(t.current_map_node_id,rm.current_map_node_id,r.current_map_node_id) AS node_id
    FROM room_members rm
    JOIN rooms r ON r.id=rm.room_id
    LEFT JOIN room_team_members tm ON tm.room_id=rm.room_id AND tm.user_id=rm.user_id
    LEFT JOIN room_teams t ON t.id=tm.team_id
    WHERE rm.room_id=$1 AND rm.user_id=$2
  )SQL", roomId, userId);
  if (rows.empty() || rows[0]["node_id"].isNull()) return std::nullopt;
  const auto id = rows[0]["node_id"].as<std::int64_t>();
  return id > 0 ? std::optional<std::int64_t>{id} : std::nullopt;
}

Json::Value buildContext(const std::shared_ptr<CoreService>& service,
                         std::int64_t roomId, std::int64_t actorUserId,
                         const std::string& eventType,
                         const Json::Value& payload) {
  const auto db = service->database();
  Json::Value root{Json::objectValue};
  root["event"]["type"] = eventType;
  root["event"]["payload"] = payload;
  root["actor"]["user_id"] = Json::Int64(actorUserId);
  if (actorUserId > 0) {
    const auto users = db->execSqlSync(
        "SELECT row_to_json(u)::text AS json_text FROM (SELECT id,username,is_admin FROM users WHERE id=$1) u",
        actorUserId);
    root["actor"]["user"] = rowJson(users);
    root["actor"]["character"] = service->characterByUser(actorUserId);
    root["actor"]["tags"] = entityTags(db, "character", std::to_string(actorUserId));
  }
  const auto rooms = db->execSqlSync(
      "SELECT row_to_json(r)::text AS json_text FROM rooms r WHERE id=$1", roomId);
  root["room"] = rowJson(rooms);
  // [功能備註｜規則－戰鬥橋接] 規則條件可直接讀 combat.active/round/victory_condition/state/enemy_count。
  root["combat"]["active"] = root["room"].get("battle_active", false);
  root["combat"]["round"] = root["room"].get("round", 1);
  root["combat"]["victory_condition"] = root["room"].get("combat_victory_condition", Json::Value{Json::objectValue});
  root["combat"]["state"] = root["room"].get("rule_combat_state", Json::Value{Json::objectValue});
  const auto combatCounts = db->execSqlSync(R"SQL(
    SELECT
      (SELECT COUNT(*) FROM room_monsters WHERE room_id=$1 AND status='encountered' AND current_hp>0)::BIGINT AS living_monsters,
      (SELECT COUNT(*) FROM room_npcs WHERE room_id=$1 AND status='active' AND combat_side='enemy' AND current_hp>0)::BIGINT AS enemy_npcs,
      (SELECT COUNT(*) FROM room_members WHERE room_id=$1 AND role='player' AND hp>0)::BIGINT AS living_players
  )SQL", roomId);
  if (!combatCounts.empty()) {
    root["combat"]["living_monsters"] = Json::Int64(combatCounts[0]["living_monsters"].as<std::int64_t>());
    root["combat"]["enemy_npcs"] = Json::Int64(combatCounts[0]["enemy_npcs"].as<std::int64_t>());
    root["combat"]["living_players"] = Json::Int64(combatCounts[0]["living_players"].as<std::int64_t>());
    root["combat"]["enemy_count"] = Json::Int64(combatCounts[0]["living_monsters"].as<std::int64_t>() + combatCounts[0]["enemy_npcs"].as<std::int64_t>());
  }
  if (const auto nodeId = effectiveNodeId(db, roomId, actorUserId)) {
    const auto nodes = db->execSqlSync(
        "SELECT row_to_json(n)::text AS json_text FROM room_map_nodes n WHERE id=$1", *nodeId);
    root["location"] = rowJson(nodes);
    root["location"]["tags"] = entityTags(db, "map_node", std::to_string(*nodeId));
  } else {
    root["location"] = Json::Value{Json::objectValue};
  }
  if (actorUserId > 0) {
    const auto team = db->execSqlSync(R"SQL(
      SELECT row_to_json(x)::text AS json_text FROM (
        SELECT t.*,m.user_id AS member_user_id,
          (SELECT COUNT(*) FROM room_team_members tm2 WHERE tm2.team_id=t.id)::BIGINT AS member_count
        FROM room_team_members m JOIN room_teams t ON t.id=m.team_id
        WHERE m.room_id=$1 AND m.user_id=$2 LIMIT 1
      ) x
    )SQL", roomId, actorUserId);
    root["team"] = rowJson(team);
  }
  root["state"] = loadPlayerState(db, roomId, actorUserId);
  if (actorUserId > 0) {
    const auto allies = db->execSqlSync(
        "SELECT COUNT(*)::BIGINT AS count FROM room_members rm2 WHERE rm2.room_id=$1 AND rm2.user_id<>$2",
        roomId, actorUserId);
    if (!allies.empty()) root["context"]["other_room_members"] = Json::Int64(allies[0]["count"].as<std::int64_t>());
    if (const auto actorNode = effectiveNodeId(db, roomId, actorUserId)) {
      const auto nearby = db->execSqlSync(R"SQL(
        SELECT COUNT(*)::BIGINT AS count FROM room_members rm2
        JOIN rooms r2 ON r2.id=rm2.room_id
        LEFT JOIN room_team_members tm2 ON tm2.room_id=rm2.room_id AND tm2.user_id=rm2.user_id
        LEFT JOIN room_teams t2 ON t2.id=tm2.team_id
        WHERE rm2.room_id=$1 AND rm2.user_id<>$2
          AND COALESCE(t2.current_map_node_id,rm2.current_map_node_id,r2.current_map_node_id)=$3
      )SQL", roomId, actorUserId, *actorNode);
      if (!nearby.empty()) root["context"]["nearby_ally_count"] = Json::Int64(nearby[0]["count"].as<std::int64_t>());
    }
  }
  return root;
}

const Json::Value* pathValue(const Json::Value& root, const std::string& path) {
  if (path.empty()) return &root;
  const Json::Value* current = &root;
  std::size_t start = 0;
  while (start <= path.size()) {
    const auto dot = path.find('.', start);
    const auto key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    if (key.empty() || !current->isObject() || !current->isMember(key)) return nullptr;
    current = &((*current)[key]);
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  return current;
}

bool valuesEqual(const Json::Value& actual, const Json::Value& expected) {
  if (actual.isNumeric() && expected.isNumeric()) return std::fabs(actual.asDouble() - expected.asDouble()) < 1e-9;
  if (actual.isBool() || expected.isBool()) return actual.asBool() == expected.asBool();
  if (actual.isString() || expected.isString()) return text(actual) == text(expected);
  return actual == expected;
}

bool conditionLeaf(const Json::Value& condition, const Json::Value& context,
                   Json::Value* trace) {
  const auto path = text(condition["path"]);
  const auto op = text(condition["op"], "eq");
  const auto* actualPtr = pathValue(context, path);
  const Json::Value missing;
  const auto& actual = actualPtr ? *actualPtr : missing;
  const auto& expected = condition["value"];
  bool matched = false;
  if (op == "exists") matched = actualPtr != nullptr && !actual.isNull();
  else if (op == "not_exists") matched = actualPtr == nullptr || actual.isNull();
  else if (op == "truthy") matched = actual.asBool();
  else if (op == "falsy") matched = !actual.asBool();
  else if (op == "eq") matched = actualPtr && valuesEqual(actual, expected);
  else if (op == "neq") matched = !actualPtr || !valuesEqual(actual, expected);
  else if (op == "gt") matched = actualPtr && number(actual) > number(expected);
  else if (op == "gte") matched = actualPtr && number(actual) >= number(expected);
  else if (op == "lt") matched = actualPtr && number(actual) < number(expected);
  else if (op == "lte") matched = actualPtr && number(actual) <= number(expected);
  else if (op == "contains") {
    if (actual.isArray()) matched = arrayContains(actual, expected);
    else matched = text(actual).find(text(expected)) != std::string::npos;
  } else if (op == "not_contains") {
    if (actual.isArray()) matched = !arrayContains(actual, expected);
    else matched = text(actual).find(text(expected)) == std::string::npos;
  } else if (op == "in") matched = expected.isArray() && arrayContains(expected, actual);
  else if (op == "not_in") matched = !expected.isArray() || !arrayContains(expected, actual);
  else if (op == "regex") {
    try { matched = std::regex_search(text(actual), std::regex(text(expected))); }
    catch (...) { matched = false; }
  }
  if (trace) {
    (*trace)["path"] = path;
    (*trace)["op"] = op;
    (*trace)["expected"] = expected;
    (*trace)["actual"] = actualPtr ? actual : Json::Value{Json::nullValue};
    (*trace)["matched"] = matched;
  }
  return matched;
}

bool evaluateCondition(const Json::Value& condition, const Json::Value& context,
                       Json::Value* trace = nullptr) {
  if (condition.isNull()) return true;
  if (!condition.isObject()) return false;
  if (condition.isMember("all")) {
    const auto& list = condition["all"];
    bool matched = list.isArray();
    Json::Value children{Json::arrayValue};
    if (list.isArray()) {
      for (const auto& child : list) {
        Json::Value childTrace;
        if (!evaluateCondition(child, context, &childTrace)) matched = false;
        children.append(childTrace);
      }
    }
    if (trace) { (*trace)["type"]="all"; (*trace)["children"]=children; (*trace)["matched"]=matched; }
    return matched;
  }
  if (condition.isMember("any")) {
    const auto& list = condition["any"];
    bool matched = false;
    Json::Value children{Json::arrayValue};
    if (list.isArray()) {
      for (const auto& child : list) {
        Json::Value childTrace;
        if (evaluateCondition(child, context, &childTrace)) matched = true;
        children.append(childTrace);
      }
    }
    if (trace) { (*trace)["type"]="any"; (*trace)["children"]=children; (*trace)["matched"]=matched; }
    return matched;
  }
  if (condition.isMember("not")) {
    Json::Value childTrace;
    const bool matched = !evaluateCondition(condition["not"], context, &childTrace);
    if (trace) { (*trace)["type"]="not"; (*trace)["child"]=childTrace; (*trace)["matched"]=matched; }
    return matched;
  }
  return conditionLeaf(condition, context, trace);
}

void notifyDm(const Db& db, std::int64_t roomId, std::int64_t ruleId,
              const std::string& title, const std::string& message,
              const Json::Value& data) {
  const auto admins = db->execSqlSync("SELECT id FROM users WHERE is_admin=TRUE");
  Json::Value payload = data;
  payload["room_id"] = Json::Int64(roomId);
  payload["rule_id"] = Json::Int64(ruleId);
  for (const auto& row : admins) {
    db->execSqlSync(
        "INSERT INTO user_notifications(user_id,notification_type,title,message,data) VALUES($1,'rule_engine',$2,$3,$4::jsonb)",
        row["id"].as<std::int64_t>(), title, message, compactJson(payload));
  }
}

void upsertRuleState(const Db& db, std::int64_t roomId, std::int64_t ruleId,
                     std::int64_t userId, bool violation,
                     std::int64_t countdownSeconds) {
  // [功能備註｜規則引擎狀態] 系統級/DM 測試可能沒有實際玩家；避免寫入 user_id=0 造成外鍵錯誤。
  if (userId <= 0) return;
  db->execSqlSync(R"SQL(
    INSERT INTO rule_engine_rule_state(room_id,rule_id,user_id,trigger_count,violation_count,last_triggered_at,countdown_due_at)
    VALUES($1,$2,$3,1,$4,CURRENT_TIMESTAMP,
      CASE WHEN $5>0 THEN CURRENT_TIMESTAMP + make_interval(secs => $5::int) ELSE NULL END)
    ON CONFLICT(room_id,rule_id,user_id) DO UPDATE SET
      trigger_count=rule_engine_rule_state.trigger_count+1,
      violation_count=rule_engine_rule_state.violation_count+EXCLUDED.violation_count,
      last_triggered_at=CURRENT_TIMESTAMP,
      countdown_due_at=CASE WHEN $5>0 THEN CURRENT_TIMESTAMP + make_interval(secs => $5::int) ELSE rule_engine_rule_state.countdown_due_at END,
      updated_at=CURRENT_TIMESTAMP
  )SQL", roomId, ruleId, userId, violation ? 1 : 0, countdownSeconds);
}

Json::Value applyEffects(const std::shared_ptr<CoreService>& service,
                         const Json::Value& rule, std::int64_t roomId,
                         std::int64_t actorUserId, const Json::Value& context,
                         bool dryRun) {
  Json::Value result{Json::objectValue};
  result["effects"] = Json::arrayValue;
  result["blocked"] = false;
  result["violation"] = false;
  auto state = loadPlayerState(service->database(), roomId, actorUserId);
  bool stateChanged = false;
  bool explicitDmNotification = false;
  const auto& effects = rule["effects"];
  if (effects.isArray()) {
    for (const auto& effect : effects) {
      if (!effect.isObject()) continue;
      const auto type = text(effect["type"]);
      Json::Value applied = effect;
      applied["applied"] = !dryRun;
      if (type == "add_flag") {
        if (!dryRun) { addFlag(state, text(effect["flag"])); stateChanged = true; }
      } else if (type == "remove_flag") {
        if (!dryRun) { removeFlag(state, text(effect["flag"])); stateChanged = true; }
      } else if (type == "increment_counter") {
        const auto key = text(effect["key"]);
        const auto amount = integer(effect["amount"], 1);
        if (!dryRun && !key.empty()) {
          state["counters"][key] = Json::Int64(integer(state["counters"][key]) + amount);
          stateChanged = true;
        }
      } else if (type == "set_value") {
        const auto key = text(effect["key"]);
        if (!dryRun && !key.empty()) { state["values"][key] = effect["value"]; stateChanged = true; }
      } else if (type == "start_countdown") {
        const auto key = text(effect["key"], "rule_" + std::to_string(integer(rule["id"])));
        const auto seconds = std::max<std::int64_t>(0, integer(effect["seconds"], integer(rule["countdown_seconds"])));
        if (!dryRun && !key.empty() && seconds > 0) {
          state["countdowns"][key]["started_unix"] = Json::Int64(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
          state["countdowns"][key]["seconds"] = Json::Int64(seconds);
          stateChanged = true;
        }
      } else if (type == "clear_countdown") {
        const auto key = text(effect["key"]);
        if (!dryRun && !key.empty()) { state["countdowns"].removeMember(key); stateChanged = true; }
      } else if (type == "notify_dm") {
        explicitDmNotification = true;
        if (!dryRun) notifyDm(service->database(), roomId, integer(rule["id"]),
                              text(effect["title"], "規則引擎觸發：" + text(rule["name"])),
                              text(effect["message"], text(rule["dm_text"], text(rule["name"]))), context);
      } else if (type == "add_event") {
        if (!dryRun) {
          Json::Value payload = effect["payload"].isObject() ? effect["payload"] : Json::Value{Json::objectValue};
          payload["text"] = text(effect["text"], "規則觸發：" + text(rule["name"]));
          payload["rule_id"] = rule["id"];
          service->addEvent(roomId, actorUserId, text(effect["event_type"], "rule_engine"), payload);
        }
      } else if (type == "log_violation") {
        result["violation"] = true;
      } else if (type == "block_action") {
        result["blocked"] = true;
      } else if (type == "reveal_rule") {
        const auto horrorRuleId = integer(effect["horror_rule_id"]);
        if (!dryRun && actorUserId > 0 && horrorRuleId > 0) {
          service->database()->execSqlSync(
              "INSERT INTO rule_discoveries(rule_id,user_id,knowledge_level,note) VALUES($1,$2,$3,$4) "
              "ON CONFLICT(rule_id,user_id) DO UPDATE SET knowledge_level=GREATEST(rule_discoveries.knowledge_level,EXCLUDED.knowledge_level),note=EXCLUDED.note,updated_at=CURRENT_TIMESTAMP",
              horrorRuleId, actorUserId, std::clamp<std::int64_t>(integer(effect["level"],1),1,3),
              text(effect["note"], "規則引擎自動揭露"));
        }
      } else if (type == "add_status") {
        if (!dryRun && actorUserId > 0) {
          const auto statusName = text(effect["name"]);
          if (!statusName.empty()) {
            service->database()->execSqlSync(
                "INSERT INTO character_statuses(user_id,name,description,remaining_rounds,effects,source,active) VALUES($1,$2,$3,$4,$5::jsonb,$6,TRUE)",
                actorUserId, statusName, text(effect["description"]), std::max<std::int64_t>(0, integer(effect["duration_rounds"])),
                compactJson(effect["effects"].isObject()?effect["effects"]:Json::Value{Json::objectValue}),
                "規則引擎:" + text(rule["name"]));
          }
        }
      } else if (type == "set_location") {
        const auto nodeId = integer(effect["node_id"]);
        if (!dryRun && actorUserId > 0 && nodeId > 0) {
          // [功能備註｜規則引擎傳送] 只能傳送到同一房間的地圖節點，避免跨房間引用造成資料污染。
          const auto validNode = service->database()->execSqlSync(
              "SELECT 1 FROM room_map_nodes WHERE id=$1 AND room_id=$2", nodeId, roomId);
          if (!validNode.empty()) {
            service->database()->execSqlSync(
                "UPDATE room_members SET current_map_node_id=$1 WHERE room_id=$2 AND user_id=$3",
                nodeId, roomId, actorUserId);
          } else {
            applied["applied"] = false;
            applied["error"] = "目標地圖節點不屬於此房間";
          }
        }
      } else {
        // [功能備註｜規則－戰鬥橋接] 世界規則未知效果交給 C++ 戰鬥橋接層處理。
        const auto combatApplied = applyRuleCombatEffect(service, rule, roomId, actorUserId, context, effect, dryRun);
        if (combatApplied.get("supported", false).asBool()) applied = combatApplied;
        else applied["supported"] = false;
      }
      result["effects"].append(applied);
    }
  }
  if (rule.get("notify_dm", false).asBool() && !explicitDmNotification) {
    Json::Value notification;
    notification["rule_name"] = rule["name"];
    notification["event_type"] = context["event"]["type"];
    if (!dryRun) notifyDm(service->database(), roomId, integer(rule["id"]),
                          "規則引擎觸發：" + text(rule["name"]),
                          text(rule["dm_text"], "條件已符合"), notification);
  }
  if (stateChanged && !dryRun) savePlayerState(service->database(), roomId, actorUserId, state);
  result["state"] = state;
  return result;
}

Json::Value normalizeRuleBody(Json::Value body) {
  if (!body.isObject()) body = Json::Value{Json::objectValue};
  if (!body["tags"].isArray()) body["tags"] = Json::arrayValue;
  if (!body["conditions"].isObject()) { body["conditions"] = Json::Value{Json::objectValue}; body["conditions"]["all"] = Json::arrayValue; }
  if (!body["effects"].isArray()) body["effects"] = Json::arrayValue;
  body["name"] = text(body["name"]).substr(0, 180);
  body["code"] = text(body["code"]).substr(0, 100);
  body["trigger_event"] = text(body["trigger_event"], "PLAYER_ACTION").substr(0, 80);
  body["priority"] = Json::Int64(std::clamp<std::int64_t>(integer(body["priority"],100),-100000,100000));
  body["cooldown_seconds"] = Json::Int64(std::max<std::int64_t>(0, integer(body["cooldown_seconds"])));
  body["countdown_seconds"] = Json::Int64(std::max<std::int64_t>(0, integer(body["countdown_seconds"])));
  body["max_triggers"] = Json::Int64(std::max<std::int64_t>(0, integer(body["max_triggers"])));
  if (!body.isMember("automatic")) body["automatic"] = true;
  if (!body.isMember("notify_dm")) body["notify_dm"] = true;
  if (!body.isMember("stop_on_match")) body["stop_on_match"] = false;
  if (!body.isMember("active")) body["active"] = true;
  if (body["name"].asString().empty()) throw ApiError(drogon::k400BadRequest, "規則名稱不能空白");
  if (body["trigger_event"].asString().empty()) throw ApiError(drogon::k400BadRequest, "觸發事件不能空白");
  return body;
}

Json::Value insertRule(const Db& db, const Json::Value& body) {
  const auto rows = db->execSqlSync(R"SQL(
    INSERT INTO rule_engine_rules(room_id,rulebook_entry_id,code,name,player_text,dm_text,trigger_event,tags,conditions,effects,priority,visibility,automatic,notify_dm,cooldown_seconds,countdown_seconds,max_triggers,stop_on_match,active)
    VALUES(NULLIF($1,0),NULLIF($2,0),$3,$4,$5,$6,$7,$8::jsonb,$9::jsonb,$10::jsonb,$11,$12,$13,$14,$15,$16,$17,$18,$19)
    RETURNING row_to_json(rule_engine_rules)::text AS json_text
  )SQL", integer(body["room_id"]), integer(body["rulebook_entry_id"]), text(body["code"]), text(body["name"]),
      text(body["player_text"]), text(body["dm_text"]), text(body["trigger_event"]), compactJson(body["tags"]),
      compactJson(body["conditions"]), compactJson(body["effects"]), integer(body["priority"]), text(body["visibility"],"hidden"),
      body["automatic"].asBool(), body["notify_dm"].asBool(), integer(body["cooldown_seconds"]), integer(body["countdown_seconds"]),
      integer(body["max_triggers"]), body["stop_on_match"].asBool(), body["active"].asBool());
  return rowJson(rows);
}

Json::Value updateRule(const Db& db, std::int64_t id, const Json::Value& body) {
  const auto rows = db->execSqlSync(R"SQL(
    UPDATE rule_engine_rules SET room_id=NULLIF($1,0),rulebook_entry_id=NULLIF($2,0),code=$3,name=$4,player_text=$5,dm_text=$6,
      trigger_event=$7,tags=$8::jsonb,conditions=$9::jsonb,effects=$10::jsonb,priority=$11,visibility=$12,automatic=$13,notify_dm=$14,
      cooldown_seconds=$15,countdown_seconds=$16,max_triggers=$17,stop_on_match=$18,active=$19,updated_at=CURRENT_TIMESTAMP
    WHERE id=$20 RETURNING row_to_json(rule_engine_rules)::text AS json_text
  )SQL", integer(body["room_id"]), integer(body["rulebook_entry_id"]), text(body["code"]), text(body["name"]), text(body["player_text"]),
      text(body["dm_text"]), text(body["trigger_event"]), compactJson(body["tags"]), compactJson(body["conditions"]), compactJson(body["effects"]),
      integer(body["priority"]), text(body["visibility"],"hidden"), body["automatic"].asBool(), body["notify_dm"].asBool(),
      integer(body["cooldown_seconds"]), integer(body["countdown_seconds"]), integer(body["max_triggers"]), body["stop_on_match"].asBool(),
      body["active"].asBool(), id);
  if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到規則");
  return rowJson(rows);
}

}  // namespace

Json::Value ruleMapNodePayload(const std::shared_ptr<CoreService>& service,
                               std::int64_t roomId,
                               std::int64_t nodeId) {
  Json::Value payload{Json::objectValue};
  payload["node_id"] = Json::Int64(nodeId);
  payload["target_node_tags"] = Json::arrayValue;
  if (!service || roomId <= 0 || nodeId <= 0) return payload;
  const auto nodeRows = service->database()->execSqlSync(
      "SELECT row_to_json(n)::text AS json_text FROM room_map_nodes n WHERE id=$1 AND room_id=$2", nodeId, roomId);
  if (!nodeRows.empty()) payload["target_node"] = rowJson(nodeRows);
  const auto tagRows = service->database()->execSqlSync(
      "SELECT ct.name FROM entity_tags et JOIN content_tags ct ON ct.id=et.tag_id "
      "WHERE et.entity_type='map_node' AND et.entity_key=$1 ORDER BY ct.name", std::to_string(nodeId));
  for (const auto& row : tagRows) payload["target_node_tags"].append(row["name"].as<std::string>());
  return payload;
}

Json::Value evaluateRuleEvent(const std::shared_ptr<CoreService>& service,
                              std::int64_t roomId,
                              std::int64_t actorUserId,
                              const std::string& eventType,
                              const Json::Value& eventContext,
                              bool dryRun) {
  Json::Value out{Json::objectValue};
  out["event_type"] = eventType;
  out["dry_run"] = dryRun;
  out["matched"] = Json::arrayValue;
  out["blocked"] = false;
  if (roomId <= 0 || eventType.empty()) return out;
  // [73-cpp.34.25] 所有真正發生的規則事件都可驅動結構化任務；dry-run 不改任務進度。
  if (!dryRun && actorUserId > 0) advanceStructuredTaskStepsForEvent(service, roomId, actorUserId, eventType, eventContext);
  const auto context = buildContext(service, roomId, actorUserId, eventType, eventContext);
  const auto rows = service->database()->execSqlSync(R"SQL(
    SELECT row_to_json(r)::text AS json_text FROM rule_engine_rules r
    WHERE r.active=TRUE AND r.automatic=TRUE AND r.trigger_event=$1 AND (r.room_id IS NULL OR r.room_id=$2)
    ORDER BY r.priority DESC,r.id ASC
  )SQL", eventType, roomId);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto rule = rowJson(rows, i);
    const auto ruleId = integer(rule["id"]);
    const auto stateRows = service->database()->execSqlSync(R"SQL(
      SELECT trigger_count,violation_count,last_triggered_at,
        CASE WHEN last_triggered_at IS NULL THEN TRUE
             ELSE EXTRACT(EPOCH FROM (CURRENT_TIMESTAMP-last_triggered_at)) >= $4 END AS cooldown_ready
      FROM rule_engine_rule_state WHERE room_id=$1 AND rule_id=$2 AND user_id=$3
    )SQL", roomId, ruleId, actorUserId, integer(rule["cooldown_seconds"]));
    if (!stateRows.empty()) {
      if (!stateRows[0]["cooldown_ready"].as<bool>()) continue;
      const auto maxTriggers = integer(rule["max_triggers"]);
      if (maxTriggers > 0 && stateRows[0]["trigger_count"].as<std::int64_t>() >= maxTriggers) continue;
    }
    Json::Value conditionTrace;
    const bool matched = evaluateCondition(rule["conditions"], context, &conditionTrace);
    if (!matched) continue;
    auto effectResult = applyEffects(service, rule, roomId, actorUserId, context, dryRun);
    Json::Value item;
    item["rule"] = rule;
    item["condition_trace"] = conditionTrace;
    item["result"] = effectResult;
    out["matched"].append(item);
    out["blocked"] = out["blocked"].asBool() || effectResult["blocked"].asBool();
    if (!dryRun) {
      const bool violation = effectResult["violation"].asBool();
      upsertRuleState(service->database(), roomId, ruleId, actorUserId, violation,
                      integer(rule["countdown_seconds"]));
      service->database()->execSqlSync(
          "INSERT INTO rule_engine_logs(room_id,rule_id,actor_user_id,event_type,matched,dry_run,context,result) VALUES($1,$2,NULLIF($3,0),$4,TRUE,FALSE,$5::jsonb,$6::jsonb)",
          roomId, ruleId, actorUserId, eventType, compactJson(context), compactJson(effectResult));
      Json::Value event;
      event["text"] = "規則引擎觸發：" + text(rule["name"]);
      event["rule_id"] = Json::Int64(ruleId);
      event["code"] = rule["code"];
      service->addEvent(roomId, actorUserId, "rule_engine_trigger", event);
    }
    if (rule.get("stop_on_match", false).asBool()) break;
  }
  out["context"] = context;
  if (!dryRun) out["task_progress"] = advanceStructuredTaskStepsForEvent(service, roomId, actorUserId, eventType, eventContext);
  return out;
}

Json::Value runRuleEngineTick(const std::shared_ptr<CoreService>& service,
                              std::int64_t roomId) {
  Json::Value out; out["ok"] = true; out["expired"] = Json::arrayValue;
  std::string sql =
      "SELECT room_id,rule_id,user_id FROM rule_engine_rule_state WHERE countdown_due_at IS NOT NULL AND countdown_due_at<=CURRENT_TIMESTAMP";
  const auto process = [&](const drogon::orm::Result& rows) {
    for (const auto& row : rows) {
      const auto rid = row["room_id"].as<std::int64_t>();
      const auto ruleId = row["rule_id"].as<std::int64_t>();
      const auto uid = row["user_id"].as<std::int64_t>();
      service->database()->execSqlSync(
          "UPDATE rule_engine_rule_state SET countdown_due_at=NULL,updated_at=CURRENT_TIMESTAMP WHERE room_id=$1 AND rule_id=$2 AND user_id=$3",
          rid, ruleId, uid);
      Json::Value payload; payload["source_rule_id"] = Json::Int64(ruleId);
      payload["text"] = "規則倒數到期";
      service->addEvent(rid, uid, "rule_countdown_expired", payload);
      auto triggered = evaluateRuleEvent(service, rid, uid, "RULE_COUNTDOWN_EXPIRED", payload, false);
      Json::Value item; item["room_id"] = Json::Int64(rid); item["rule_id"] = Json::Int64(ruleId); item["user_id"] = Json::Int64(uid); item["triggered"] = triggered["matched"];
      out["expired"].append(item);
    }
  };
  if (roomId > 0) process(service->database()->execSqlSync(sql + " AND room_id=$1", roomId));
  else process(service->database()->execSqlSync(sql));

  // [功能備註｜規則引擎場景輪詢] 每 30 秒檢查房間玩家目前所處場景，支援「不可獨處／不可久留」等持續條件規則。
  const auto scenePlayers = roomId > 0
      ? service->database()->execSqlSync(
            "SELECT rm.room_id,rm.user_id FROM room_members rm JOIN rooms r ON r.id=rm.room_id "
            "WHERE rm.room_id=$1 AND rm.role='player' AND r.status='active' ORDER BY rm.user_id", roomId)
      : service->database()->execSqlSync(
            "SELECT rm.room_id,rm.user_id FROM room_members rm JOIN rooms r ON r.id=rm.room_id "
            "WHERE rm.role='player' AND r.status='active' ORDER BY rm.room_id,rm.user_id");
  for (const auto& player : scenePlayers) {
    const auto rid = player["room_id"].as<std::int64_t>();
    const auto uid = player["user_id"].as<std::int64_t>();
    Json::Value payload; payload["source"] = "background_30s";
    const auto triggered = evaluateRuleEvent(service, rid, uid, "PLAYER_SCENE_TICK", payload, false);
    if (!triggered["matched"].empty()) {
      Json::Value item; item["room_id"] = Json::Int64(rid); item["user_id"] = Json::Int64(uid); item["triggered"] = triggered["matched"];
      out["scene_ticks"].append(item);
    }
  }
  return out;
}

void registerRuleEngineRoutes(const std::shared_ptr<CoreService>& service) {
  auto reg0 = [](const std::string& path, drogon::HttpMethod method, auto handler) {
    drogon::app().registerHandler(path, std::move(handler), {method});
  };
  auto reg1 = [](const std::string& path, drogon::HttpMethod method, auto handler) {
    drogon::app().registerHandler(path, std::move(handler), {method});
  };

  // [功能備註｜規則引擎｜DM] 讀取全部結構化規則。
  reg0("/api/admin/rule-engine/rules", drogon::Get,
       [service](const Request& req, Callback&& raw) {
    Callback cb = std::move(raw);
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto roomText = req->getParameter("room_id");
      Json::Value out;
      if (!roomText.empty()) {
        const auto roomId = positiveId(roomText, "房間 ID 無效");
        out["rules"] = listRows(service->database()->execSqlSync(
            "SELECT row_to_json(r)::text AS json_text FROM rule_engine_rules r WHERE r.room_id IS NULL OR r.room_id=$1 ORDER BY priority DESC,id",
            roomId));
      } else {
        out["rules"] = listRows(service->database()->execSqlSync(
            "SELECT row_to_json(r)::text AS json_text FROM rule_engine_rules r ORDER BY room_id NULLS FIRST,priority DESC,id"));
      }
      reply(cb, std::move(out));
    }, "讀取規則引擎失敗");
  });

  // [功能備註｜規則引擎｜DM] 建立規則。
  reg0("/api/admin/rule-engine/rules", drogon::Post,
       [service](const Request& req, Callback&& raw) {
    Callback cb = std::move(raw);
    runHandler(cb, [&] {
      requireUser(service, req, true);
      auto body = normalizeRuleBody(bodyOrEmpty(req));
      Json::Value out; out["ok"] = true; out["rule"] = insertRule(service->database(), body); reply(cb, std::move(out));
    }, "建立規則失敗");
  });

  // [功能備註｜規則引擎｜DM] 修改規則。
  reg1("/api/admin/rule-engine/rules/{1}", drogon::Patch,
       [service](const Request& req, Callback&& raw, const std::string& idText) {
    Callback cb = std::move(raw);
    runHandler(cb, [&] {
      requireUser(service, req, true);
      auto body = normalizeRuleBody(bodyOrEmpty(req));
      Json::Value out; out["ok"] = true; out["rule"] = updateRule(service->database(), positiveId(idText), body); reply(cb, std::move(out));
    }, "修改規則失敗");
  });

  // [功能備註｜規則引擎｜DM] 刪除規則及其執行狀態。
  reg1("/api/admin/rule-engine/rules/{1}", drogon::Delete,
       [service](const Request& req, Callback&& raw, const std::string& idText) {
    Callback cb = std::move(raw);
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto id = positiveId(idText);
      const auto rows = service->database()->execSqlSync("DELETE FROM rule_engine_rules WHERE id=$1 RETURNING id", id);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到規則");
      Json::Value out; out["ok"] = true; reply(cb, std::move(out));
    }, "刪除規則失敗");
  });

  // [功能備註｜規則引擎｜DM] 用測試事件預覽條件；dry_run=false 可真正套用效果。
  reg0("/api/admin/rule-engine/test", drogon::Post,
       [service](const Request& req, Callback&& raw) {
    Callback cb = std::move(raw);
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto body = bodyOrEmpty(req);
      const auto roomId = integer(body["room_id"]);
      const auto actorUserId = integer(body["actor_user_id"]);
      const auto eventType = text(body["event_type"]);
      if (roomId <= 0 || eventType.empty()) throw ApiError(drogon::k400BadRequest, "需要 room_id 與 event_type");
      auto out = evaluateRuleEvent(service, roomId, actorUserId, eventType,
                                   body["context"].isObject() ? body["context"] : Json::Value{Json::objectValue},
                                   body.get("dry_run", true).asBool());
      reply(cb, std::move(out));
    }, "測試規則失敗");
  });

  // [功能備註｜規則引擎｜DM] 查看最近觸發紀錄。
  reg0("/api/admin/rule-engine/logs", drogon::Get,
       [service](const Request& req, Callback&& raw) {
    Callback cb = std::move(raw);
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto rows = service->database()->execSqlSync(
          "SELECT row_to_json(x)::text AS json_text FROM (SELECT l.*,r.name AS rule_name,u.username AS actor_username FROM rule_engine_logs l LEFT JOIN rule_engine_rules r ON r.id=l.rule_id LEFT JOIN users u ON u.id=l.actor_user_id ORDER BY l.id DESC LIMIT 300) x");
      Json::Value out; out["logs"] = listRows(rows); reply(cb, std::move(out));
    }, "讀取規則紀錄失敗");
  });

  // [功能備註｜規則引擎｜DM] 查看指定玩家的全域旗標／計數器與各規則觸發狀態。
  reg0("/api/admin/rule-engine/state", drogon::Get,
       [service](const Request& req, Callback&& raw) {
    Callback cb = std::move(raw);
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto roomId = positiveId(req->getParameter("room_id"), "房間 ID 無效");
      const auto userId = positiveId(req->getParameter("user_id"), "玩家 ID 無效");
      Json::Value out; out["state"] = loadPlayerState(service->database(), roomId, userId);
      out["rules"] = listRows(service->database()->execSqlSync(
          "SELECT row_to_json(s)::text AS json_text FROM rule_engine_rule_state s WHERE room_id=$1 AND user_id=$2 ORDER BY rule_id", roomId, userId));
      reply(cb, std::move(out));
    }, "讀取規則狀態失敗");
  });

  // [功能備註｜規則引擎｜DM] 清除指定玩家的規則旗標與觸發狀態，方便重跑副本。
  reg0("/api/admin/rule-engine/state/reset", drogon::Post,
       [service](const Request& req, Callback&& raw) {
    Callback cb = std::move(raw);
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto body = bodyOrEmpty(req);
      const auto roomId = integer(body["room_id"]), userId = integer(body["user_id"]), ruleId = integer(body["rule_id"]);
      if (roomId <= 0 || userId <= 0) throw ApiError(drogon::k400BadRequest, "需要 room_id 與 user_id");
      if (ruleId > 0) service->database()->execSqlSync("DELETE FROM rule_engine_rule_state WHERE room_id=$1 AND user_id=$2 AND rule_id=$3", roomId, userId, ruleId);
      else {
        service->database()->execSqlSync("DELETE FROM rule_engine_rule_state WHERE room_id=$1 AND user_id=$2", roomId, userId);
        service->database()->execSqlSync("DELETE FROM rule_engine_player_state WHERE room_id=$1 AND user_id=$2", roomId, userId);
      }
      Json::Value out; out["ok"] = true; reply(cb, std::move(out));
    }, "重設規則狀態失敗");
  });
}

}  // namespace trpg
