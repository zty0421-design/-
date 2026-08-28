#include "trpg/CoreService.h"
#include "trpg/EventSkillSystem.h"
#include "trpg/LegacyCompat.h"
#include "trpg/WorldSystems.h"
#include "trpg/LegacyRouteManifest.h"
#include "trpg/RoomSocket.h"
#include "trpg/RuleEngine.h"
#include "trpg/RuleCombatBridge.h"
#include "trpg/GearAffixSystem.h"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trpg {
namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr&)>;

struct ApiError final : std::runtime_error {
  drogon::HttpStatusCode status;
  ApiError(drogon::HttpStatusCode statusCode, std::string message)
      : std::runtime_error(std::move(message)), status(statusCode) {}
};

std::mutex instanceMutex;
std::weak_ptr<CoreService> serviceInstance;

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

Json::Value requireBody(const drogon::HttpRequestPtr& request) {
  const auto json = request->getJsonObject();
  if (!json || !json->isObject()) {
    throw ApiError(drogon::k400BadRequest, "請提供正確的 JSON 資料");
  }
  return *json;
}

std::string trimmed(std::string value) {
  const auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
  return value;
}

std::string clipped(std::string value, std::size_t bytes) {
  if (value.size() <= bytes) return value;
  value.resize(bytes);
  while (!value.empty() &&
         (static_cast<unsigned char>(value.back()) & 0xc0U) == 0x80U) {
    value.pop_back();
  }
  return value;
}

bool validUsername(std::string_view value) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < value.size();) {
    const auto first = static_cast<unsigned char>(value[i]);
    std::uint32_t codepoint = 0;
    std::size_t width = 0;
    if (first < 0x80) {
      codepoint = first;
      width = 1;
    } else if ((first & 0xe0U) == 0xc0U) {
      codepoint = first & 0x1fU;
      width = 2;
    } else if ((first & 0xf0U) == 0xe0U) {
      codepoint = first & 0x0fU;
      width = 3;
    } else if ((first & 0xf8U) == 0xf0U) {
      codepoint = first & 0x07U;
      width = 4;
    } else {
      return false;
    }
    if (i + width > value.size()) return false;
    for (std::size_t offset = 1; offset < width; ++offset) {
      const auto continuation = static_cast<unsigned char>(value[i + offset]);
      if ((continuation & 0xc0U) != 0x80U) return false;
      codepoint = (codepoint << 6U) | (continuation & 0x3fU);
    }
    const bool asciiAllowed =
        width == 1 && (std::isalnum(first) || first == '_' || first == '-');
    const bool cjkAllowed = codepoint >= 0x4e00 && codepoint <= 0x9fff;
    if (!asciiAllowed && !cjkAllowed) return false;
    ++count;
    i += width;
  }
  return count >= 2 && count <= 20;
}

std::int64_t integerValue(const Json::Value& value, std::int64_t fallback = 0) {
  try {
    if (value.isInt64()) return value.asInt64();
    if (value.isUInt64()) {
      const auto raw = value.asUInt64();
      if (raw <= static_cast<Json::UInt64>(std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(raw);
      }
      return fallback;
    }
    if (value.isDouble()) {
      const auto raw = value.asDouble();
      if (raw >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
          raw <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(raw);
      }
      return fallback;
    }
    if (value.isString()) {
      std::size_t consumed = 0;
      const auto raw = std::stoll(value.asString(), &consumed, 10);
      return consumed == value.asString().size() ? raw : fallback;
    }
  } catch (const std::exception&) {
  }
  return fallback;
}

std::int64_t nonNegative(const Json::Value& value, std::int64_t fallback = 0) {
  return std::max<std::int64_t>(0, integerValue(value, fallback));
}

// [功能備註｜64-cpp.25 角色正式等階] 角色使用 0～10 階、每階 1～4 級；屬性總額依大階固定，不使用一般 EXP 等級制。
std::int64_t characterTierBudget(std::int64_t tier) {
  static constexpr std::array<std::int64_t, 11> budgets = {
      0, 50, 100, 180, 320, 600, 1100, 2000, 3600, 6500, 10000};
  tier = std::clamp<std::int64_t>(tier, 0, 10);
  return budgets[static_cast<std::size_t>(tier)];
}

std::string characterTierTitle(std::int64_t tier, std::int64_t level) {
  tier = std::clamp<std::int64_t>(tier, 0, 10);
  level = std::clamp<std::int64_t>(level, 1, 4);
  if (tier >= 10) return "真神";
  if (tier == 9 && level >= 4) return "半神";
  if (tier == 9) return "偽神";
  if (tier == 0) return "未登階";
  return "凡階";
}

std::string stringValue(const Json::Value& value, std::string fallback = {}) {
  return value.isString() ? value.asString() : std::move(fallback);
}

std::string randomRoomCode() {
  static constexpr std::string_view alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  std::array<unsigned char, 6> random{};
  if (RAND_bytes(random.data(), random.size()) != 1) {
    throw std::runtime_error("secure random generation failed");
  }
  std::string code;
  code.reserve(random.size());
  for (const auto byte : random) code.push_back(alphabet[byte % alphabet.size()]);
  return code;
}

std::string randomRealtimeTicket() {
  static constexpr char hex[] = "0123456789abcdef";
  std::array<unsigned char, 24> random{};
  if (RAND_bytes(random.data(), random.size()) != 1) {
    throw std::runtime_error("secure random generation failed");
  }
  std::string ticket;
  ticket.reserve(random.size() * 2);
  for (const auto byte : random) {
    ticket.push_back(hex[(byte >> 4) & 0x0f]);
    ticket.push_back(hex[byte & 0x0f]);
  }
  return ticket;
}

std::int64_t parseRoomId(const std::string& value) {
  try {
    std::size_t consumed = 0;
    const auto id = std::stoll(value, &consumed, 10);
    if (consumed != value.size() || id <= 0) throw std::invalid_argument{"id"};
    return id;
  } catch (const std::exception&) {
    throw ApiError(drogon::k400BadRequest, "房間 ID 無效");
  }
}

std::int64_t parsePositiveId(const std::string& value, const std::string& message) {
  try {
    std::size_t consumed = 0;
    const auto id = std::stoll(value, &consumed, 10);
    if (consumed != value.size() || id <= 0) throw std::invalid_argument{"id"};
    return id;
  } catch (const std::exception&) {
    throw ApiError(drogon::k400BadRequest, message);
  }
}

void ensureArray(Json::Value& object, const char* name) {
  if (!object.isMember(name) || !object[name].isArray()) object[name] = Json::arrayValue;
}

void ensureObject(Json::Value& object, const char* name) {
  if (!object.isMember(name) || !object[name].isObject()) object[name] = Json::objectValue;
}

template <typename Function>
void runHandler(Callback& callback, Function&& function,
                const std::string& genericMessage = "操作失敗") {
  try {
    function();
  } catch (const ApiError& error) {
    fail(callback, error.status, error.what());
  } catch (const drogon::orm::DrogonDbException& error) {
    LOG_ERROR << "database request failed: " << error.base().what();
    fail(callback, drogon::k500InternalServerError, genericMessage);
  } catch (const std::exception& error) {
    LOG_ERROR << "request failed: " << error.what();
    fail(callback, drogon::k500InternalServerError, genericMessage);
  }
}

JwtClaims requireAuth(const std::shared_ptr<CoreService>& service,
                      const drogon::HttpRequestPtr& request) {
  const auto claims = service->authenticateRequest(request);
  if (!claims) throw ApiError(drogon::k401Unauthorized, "請先登入");
  return *claims;
}

JwtClaims requireCurrentUser(const std::shared_ptr<CoreService>& service,
                             const drogon::HttpRequestPtr& request,
                             bool adminOnly = false) {
  const auto tokenClaims = requireAuth(service, request);
  const auto rows = service->database()->execSqlSync(
      "SELECT id,username,is_admin FROM users WHERE id=$1", tokenClaims.id);
  if (rows.empty()) throw ApiError(drogon::k401Unauthorized, "帳號不存在");
  JwtClaims claims{rows[0]["id"].as<std::int64_t>(),
                   rows[0]["username"].as<std::string>(),
                   rows[0]["is_admin"].as<bool>(), 0, 0};
  if (adminOnly && !claims.isAdmin) {
    throw ApiError(drogon::k403Forbidden, "需要全站 DM 權限");
  }
  return claims;
}


std::int64_t combatActionsForAgility(std::int64_t agility) {
  return std::max<std::int64_t>(1, 1 + std::max<std::int64_t>(0, agility) / 50);
}

std::string combatActorSide(const Json::Value& actor) {
  const auto type = stringValue(actor["type"]);
  if (type == "monster") return "enemy";
  if (type == "npc") {
    const auto side = stringValue(actor["side"], "neutral");
    return side == "enemy" ? "enemy" : side == "ally" ? "ally" : "neutral";
  }
  return "ally";
}

bool combatActorAlive(const std::shared_ptr<CoreService>& service,
                     std::int64_t roomId, const Json::Value& actor) {
  const auto type = stringValue(actor["type"]);
  const auto id = integerValue(actor["id"]);
  if (id <= 0) return false;
  if (type == "player") {
    const auto rows = service->database()->execSqlSync(
        "SELECT hp,combat_state FROM room_members WHERE room_id=$1 AND user_id=$2",
        roomId, id);
    if (rows.empty()) return false;
    const auto state = rows[0]["combat_state"].isNull()
                           ? std::string{"active"}
                           : rows[0]["combat_state"].as<std::string>();
    return rows[0]["hp"].as<std::int64_t>() > 0 && state != "dead" &&
           state != "escaped";
  }
  if (type == "monster") {
    const auto rows = service->database()->execSqlSync(
        "SELECT current_hp,status FROM room_monsters WHERE room_id=$1 AND id=$2",
        roomId, id);
    return !rows.empty() && rows[0]["current_hp"].as<std::int64_t>() > 0 &&
           rows[0]["status"].as<std::string>() == "encountered";
  }
  if (type == "npc") {
    const auto rows = service->database()->execSqlSync(
        "SELECT current_hp,status,combat_side FROM room_npcs "
        "WHERE room_id=$1 AND npc_template_id=$2",
        roomId, id);
    if (rows.empty() || rows[0]["current_hp"].as<std::int64_t>() <= 0 ||
        rows[0]["status"].as<std::string>() != "active") {
      return false;
    }
    const auto side = rows[0]["combat_side"].as<std::string>();
    return side == "ally" || side == "enemy";
  }
  return false;
}

Json::Value buildCombatOrder(const std::shared_ptr<CoreService>& service,
                             std::int64_t roomId) {
  Json::Value order{Json::arrayValue};
  const auto players = service->database()->execSqlSync(R"SQL(
    SELECT rm.user_id,rm.display_name,rm.hp,rm.combat_state,
           COALESCE(cc.agility,0) AS agility
    FROM room_members rm
    LEFT JOIN character_cards cc ON cc.user_id=rm.user_id
    WHERE rm.room_id=$1 AND rm.role='player' AND rm.hp>0
      AND COALESCE(rm.combat_state,'active') NOT IN ('dead','escaped')
  )SQL", roomId);
  for (const auto& row : players) {
    Json::Value actor;
    const auto agility = row["agility"].as<std::int64_t>();
    actor["type"] = "player";
    actor["id"] = Json::Int64(row["user_id"].as<std::int64_t>());
    actor["name"] = row["display_name"].as<std::string>();
    actor["initiative"] = Json::Int64(agility);
    actor["actions_max"] = Json::Int64(combatActionsForAgility(agility));
    order.append(std::move(actor));
  }

  const auto monsters = service->database()->execSqlSync(R"SQL(
    SELECT rm.id,mt.name,mt.attributes::text AS attributes_text
    FROM room_monsters rm
    JOIN monster_templates mt ON mt.id=rm.monster_template_id
    WHERE rm.room_id=$1 AND rm.status='encountered' AND rm.current_hp>0
  )SQL", roomId);
  for (const auto& row : monsters) {
    const auto attributes = parseJson(row["attributes_text"].as<std::string>());
    const auto agility = nonNegative(attributes["agility"]);
    Json::Value actor;
    actor["type"] = "monster";
    actor["id"] = Json::Int64(row["id"].as<std::int64_t>());
    actor["name"] = row["name"].as<std::string>();
    actor["initiative"] = Json::Int64(agility);
    actor["actions_max"] = Json::Int64(combatActionsForAgility(agility));
    order.append(std::move(actor));
  }

  const auto npcs = service->database()->execSqlSync(R"SQL(
    SELECT rn.npc_template_id,nt.name,nt.attributes::text AS attributes_text,
           rn.combat_side
    FROM room_npcs rn
    JOIN npc_templates nt ON nt.id=rn.npc_template_id
    WHERE rn.room_id=$1 AND rn.status='active' AND rn.current_hp>0
      AND rn.combat_side IN ('ally','enemy')
  )SQL", roomId);
  for (const auto& row : npcs) {
    const auto attributes = parseJson(row["attributes_text"].as<std::string>());
    const auto agility = nonNegative(attributes["agility"]);
    Json::Value actor;
    actor["type"] = "npc";
    actor["id"] = Json::Int64(row["npc_template_id"].as<std::int64_t>());
    actor["name"] = row["name"].as<std::string>();
    actor["side"] = row["combat_side"].as<std::string>();
    actor["initiative"] = Json::Int64(agility);
    actor["actions_max"] = Json::Int64(combatActionsForAgility(agility));
    order.append(std::move(actor));
  }

  std::vector<Json::Value> sorted;
  sorted.reserve(order.size());
  for (const auto& actor : order) sorted.push_back(actor);
  std::sort(sorted.begin(), sorted.end(), [](const Json::Value& a, const Json::Value& b) {
    const auto ai = integerValue(a["initiative"]);
    const auto bi = integerValue(b["initiative"]);
    if (ai != bi) return ai > bi;
    const auto at = stringValue(a["type"]);
    const auto bt = stringValue(b["type"]);
    if (at != bt && (at == "player" || bt == "player")) return at == "player";
    return integerValue(a["id"]) < integerValue(b["id"]);
  });
  Json::Value result{Json::arrayValue};
  for (auto& actor : sorted) result.append(std::move(actor));
  return result;
}

void resetCombatActions(const std::shared_ptr<CoreService>& service,
                        std::int64_t roomId, const Json::Value& order) {
  for (const auto& actor : order) {
    if (!combatActorAlive(service, roomId, actor)) continue;
    const auto type = stringValue(actor["type"]);
    const auto id = integerValue(actor["id"]);
    const auto actions = std::max<std::int64_t>(1, integerValue(actor["actions_max"], 1));
    if (type == "player") {
      service->database()->execSqlSync(
          "UPDATE room_members SET turn_actions_remaining=$1 WHERE room_id=$2 AND user_id=$3",
          actions, roomId, id);
    } else if (type == "monster") {
      service->database()->execSqlSync(
          "UPDATE room_monsters SET turn_actions_remaining=$1 WHERE room_id=$2 AND id=$3",
          actions, roomId, id);
    } else if (type == "npc") {
      service->database()->execSqlSync(
          "UPDATE room_npcs SET turn_actions_remaining=$1 WHERE room_id=$2 AND npc_template_id=$3",
          actions, roomId, id);
    }
  }
}

// [功能備註｜狀態自動化] 每當戰鬥跨入新回合時，統一結算「每回合效果」並倒數有期限的狀態。
// effects.per_round 支援 hp / spirit / sanity 三種增減值；正數恢復、負數傷害。
void processRoundStatuses(const std::shared_ptr<CoreService>& service,
                          std::int64_t roomId, std::int64_t eventUserId,
                          std::int64_t round) {
  auto applyDelta = [&](const std::string& type, std::int64_t id,
                        const Json::Value& effects) -> bool {
    if (!effects.isObject() || !effects["per_round"].isObject()) return false;
    const auto& tick = effects["per_round"];
    const auto hp = integerValue(tick["hp"]);
    const auto spirit = integerValue(tick["spirit"]);
    const auto sanity = integerValue(tick["sanity"]);
    if (hp == 0 && spirit == 0 && sanity == 0) return false;

    if (type == "player") {
      service->database()->execSqlSync(
          "UPDATE room_members SET "
          "hp=GREATEST(0,LEAST(max_hp,hp+$1)),"
          "spirit=GREATEST(0,LEAST(max_spirit,spirit+$2)),"
          "current_sanity=GREATEST(0,LEAST(max_sanity,current_sanity+$3)),"
          "combat_state=CASE WHEN GREATEST(0,LEAST(max_hp,hp+$1))<=0 THEN 'downed' ELSE combat_state END "
          "WHERE room_id=$4 AND user_id=$5",
          hp, spirit, sanity, roomId, id);
      return true;
    }
    if (type == "monster") {
      if (hp == 0) return false;
      service->database()->execSqlSync(
          "UPDATE room_monsters SET current_hp=GREATEST(0,LEAST(max_hp,current_hp+$1)),"
          "status=CASE WHEN GREATEST(0,LEAST(max_hp,current_hp+$1))<=0 THEN 'defeated' ELSE status END "
          "WHERE room_id=$2 AND id=$3",
          hp, roomId, id);
      return true;
    }
    if (type == "npc") {
      if (hp == 0) return false;
      service->database()->execSqlSync(
          "UPDATE room_npcs SET current_hp=GREATEST(0,LEAST(max_hp,current_hp+$1)),"
          "status=CASE WHEN GREATEST(0,LEAST(max_hp,current_hp+$1))<=0 THEN 'downed' ELSE status END "
          "WHERE room_id=$2 AND npc_template_id=$3",
          hp, roomId, id);
      return true;
    }
    return false;
  };

  std::int64_t ticked = 0;
  std::int64_t expired = 0;
  const auto combatRows = service->database()->execSqlSync(
      "SELECT id,entity_type,entity_id,name,remaining_rounds,expires_by_round,effects::text AS effects_text "
      "FROM combat_entity_statuses WHERE room_id=$1 AND active=TRUE ORDER BY id",
      roomId);
  for (const auto& row : combatRows) {
    const auto effects = parseJson(row["effects_text"].as<std::string>(), Json::Value{Json::objectValue});
    if (applyDelta(row["entity_type"].as<std::string>(), row["entity_id"].as<std::int64_t>(), effects)) ++ticked;
    const auto remaining = row["remaining_rounds"].as<std::int64_t>();
    if (row["expires_by_round"].as<bool>() && remaining > 0) {
      const auto next = std::max<std::int64_t>(0, remaining - 1);
      service->database()->execSqlSync(
          "UPDATE combat_entity_statuses SET remaining_rounds=$1,active=($1>0),updated_at=CURRENT_TIMESTAMP WHERE id=$2",
          next, row["id"].as<std::int64_t>());
      if (next == 0) ++expired;
    }
  }

  const auto characterRows = service->database()->execSqlSync(
      "SELECT cs.id,cs.user_id,cs.name,cs.remaining_rounds,cs.expires_by_round,cs.effects::text AS effects_text "
      "FROM character_statuses cs JOIN room_members rm ON rm.user_id=cs.user_id "
      "WHERE rm.room_id=$1 AND rm.role='player' AND cs.active=TRUE ORDER BY cs.id",
      roomId);
  for (const auto& row : characterRows) {
    const auto effects = parseJson(row["effects_text"].as<std::string>(), Json::Value{Json::objectValue});
    if (applyDelta("player", row["user_id"].as<std::int64_t>(), effects)) ++ticked;
    const auto remaining = row["remaining_rounds"].as<std::int64_t>();
    if (row["expires_by_round"].as<bool>() && remaining > 0) {
      const auto next = std::max<std::int64_t>(0, remaining - 1);
      service->database()->execSqlSync(
          "UPDATE character_statuses SET remaining_rounds=$1,active=($1>0),updated_at=CURRENT_TIMESTAMP WHERE id=$2",
          next, row["id"].as<std::int64_t>());
      if (next == 0) ++expired;
    }
  }

  if (ticked > 0 || expired > 0) {
    Json::Value event;
    event["round"] = Json::Int64(round);
    event["ticked"] = Json::Int64(ticked);
    event["expired"] = Json::Int64(expired);
    event["text"] = "第 " + std::to_string(round) + " 回合狀態自動結算：" +
                    std::to_string(ticked) + " 個持續效果、" +
                    std::to_string(expired) + " 個狀態到期";
    service->addEvent(roomId, eventUserId, "status_tick", event);
  }
}

void setCombatActor(const std::shared_ptr<CoreService>& service,
                    std::int64_t roomId, const Json::Value& actor,
                    std::int64_t index, std::int64_t round) {
  const auto type = stringValue(actor["type"]);
  const auto id = integerValue(actor["id"]);
  const auto playerId = type == "player" ? id : std::int64_t{0};
  if (type == "player") {
    service->database()->execSqlSync(
        "UPDATE rooms SET round=$1,current_actor_type=$2,current_actor_ref_id=$3,"
        "current_actor_id=$4,combat_index=$5,updated_at=CURRENT_TIMESTAMP WHERE id=$6",
        std::max<std::int64_t>(1, round), type, id, playerId,
        std::max<std::int64_t>(0, index), roomId);
  } else {
    service->database()->execSqlSync(
        "UPDATE rooms SET round=$1,current_actor_type=$2,current_actor_ref_id=$3,"
        "current_actor_id=NULL,combat_index=$4,updated_at=CURRENT_TIMESTAMP WHERE id=$5",
        std::max<std::int64_t>(1, round), type, id,
        std::max<std::int64_t>(0, index), roomId);
  }
}

struct CombatState {
  bool exists{false};
  bool active{false};
  std::int64_t round{1};
  std::int64_t index{0};
  Json::Value order{Json::arrayValue};
  Json::Value actor{Json::nullValue};
};

CombatState currentCombatState(const std::shared_ptr<CoreService>& service,
                               std::int64_t roomId) {
  CombatState state;
  const auto rows = service->database()->execSqlSync(
      "SELECT battle_active,round,combat_index,combat_order::text AS combat_order_text "
      "FROM rooms WHERE id=$1", roomId);
  if (rows.empty()) return state;
  state.exists = true;
  state.active = rows[0]["battle_active"].as<bool>();
  state.round = std::max<std::int64_t>(1, rows[0]["round"].as<std::int64_t>());
  state.index = std::max<std::int64_t>(0, rows[0]["combat_index"].as<std::int64_t>());
  state.order = parseJson(rows[0]["combat_order_text"].as<std::string>(), Json::arrayValue);
  if (!state.order.isArray() || state.order.empty()) {
    state.order = buildCombatOrder(service, roomId);
    service->database()->execSqlSync(
        "UPDATE rooms SET combat_order=$1::jsonb,combat_index=0 WHERE id=$2",
        compactJson(state.order), roomId);
    state.index = 0;
  }
  if (state.order.empty()) return state;
  state.index = std::min<std::int64_t>(state.index,
                                      static_cast<std::int64_t>(state.order.size()) - 1);
  state.actor = state.order[static_cast<Json::ArrayIndex>(state.index)];
  if (!combatActorAlive(service, roomId, state.actor)) {
    for (Json::ArrayIndex step = 1; step <= state.order.size(); ++step) {
      const auto index = static_cast<Json::ArrayIndex>(
          (state.index + static_cast<std::int64_t>(step)) % state.order.size());
      if (combatActorAlive(service, roomId, state.order[index])) {
        state.index = index;
        state.actor = state.order[index];
        setCombatActor(service, roomId, state.actor, state.index, state.round);
        return state;
      }
    }
    state.actor = Json::nullValue;
  }
  return state;
}

void finishBattle(const std::shared_ptr<CoreService>& service,
                  std::int64_t roomId, std::int64_t eventUserId,
                  const std::string& text) {
  service->database()->execSqlSync(
      "UPDATE rooms SET battle_active=FALSE,current_actor_id=NULL,current_actor_type=NULL,"
      "current_actor_ref_id=NULL,combat_order='[]'::jsonb,combat_index=0,"
      "updated_at=CURRENT_TIMESTAMP WHERE id=$1", roomId);
  service->database()->execSqlSync(
      "UPDATE room_members SET combat_state=CASE WHEN hp>0 THEN 'active' ELSE 'downed' END,"
      "turn_actions_remaining=0,skill_charge_state='{}'::jsonb "
      "WHERE room_id=$1 AND role='player'", roomId);
  Json::Value event;
  event["text"] = text;
  service->addEvent(roomId, eventUserId, "system", event);
  // [功能備註｜62-cpp.23.1 戰鬥結束場地清理]
  // 先讓 ON_COMBAT_END / 場地 trigger 有最後一次結算機會，再清除整場 FieldEffect。
  try { dispatchEventSkillEvent(service, roomId, "system", 0, "COMBAT_ENDED", event); }
  catch (const std::exception& error) { LOG_WARN << "combat end event skill degraded: " << error.what(); }
  clearCombatFieldEffects(service, roomId);
}

bool endBattleIfResolved(const std::shared_ptr<CoreService>& service,
                         std::int64_t roomId, std::int64_t eventUserId) {
  const auto rows = service->database()->execSqlSync(
      "SELECT battle_active FROM rooms WHERE id=$1", roomId);
  if (rows.empty() || !rows[0]["battle_active"].as<bool>()) return false;
  const auto order = buildCombatOrder(service, roomId);
  bool ally = false;
  bool enemy = false;
  for (const auto& actor : order) {
    if (!combatActorAlive(service, roomId, actor)) continue;
    const auto side = combatActorSide(actor);
    ally = ally || side == "ally";
    enemy = enemy || side == "enemy";
  }
  if (ally && enemy) return false;
  const auto endText = enemy ? "⚠️ 戰鬥結束：玩家／友方已無可戰鬥單位。"
                             : "🏁 戰鬥結束：敵方已無可戰鬥單位。";
  finishBattle(service, roomId, eventUserId, endText);
  Json::Value ruleEvent;
  ruleEvent["winner"] = enemy ? "enemy" : "ally";
  ruleEvent["text"] = endText;
  evaluateRuleEvent(service, roomId, eventUserId, "COMBAT_ENDED", ruleEvent, false);
  return true;
}

std::int64_t consumeCombatAction(const std::shared_ptr<CoreService>& service,
                                 std::int64_t roomId, const Json::Value& actor) {
  const auto type = stringValue(actor["type"]);
  const auto id = integerValue(actor["id"]);
  const auto fallback = std::max<std::int64_t>(1, integerValue(actor["actions_max"], 1));
  std::int64_t remaining = 0;
  if (type == "player") {
    const auto rows = service->database()->execSqlSync(
        "SELECT turn_actions_remaining FROM room_members WHERE room_id=$1 AND user_id=$2",
        roomId, id);
    if (rows.empty()) return 0;
    remaining = rows[0]["turn_actions_remaining"].as<std::int64_t>();
    if (remaining <= 0) remaining = fallback;
    remaining = std::max<std::int64_t>(0, remaining - 1);
    service->database()->execSqlSync(
        "UPDATE room_members SET turn_actions_remaining=$1 WHERE room_id=$2 AND user_id=$3",
        remaining, roomId, id);
  } else if (type == "monster") {
    const auto rows = service->database()->execSqlSync(
        "SELECT turn_actions_remaining FROM room_monsters WHERE room_id=$1 AND id=$2",
        roomId, id);
    if (rows.empty()) return 0;
    remaining = rows[0]["turn_actions_remaining"].as<std::int64_t>();
    if (remaining <= 0) remaining = fallback;
    remaining = std::max<std::int64_t>(0, remaining - 1);
    service->database()->execSqlSync(
        "UPDATE room_monsters SET turn_actions_remaining=$1 WHERE room_id=$2 AND id=$3",
        remaining, roomId, id);
  } else if (type == "npc") {
    const auto rows = service->database()->execSqlSync(
        "SELECT turn_actions_remaining FROM room_npcs WHERE room_id=$1 AND npc_template_id=$2",
        roomId, id);
    if (rows.empty()) return 0;
    remaining = rows[0]["turn_actions_remaining"].as<std::int64_t>();
    if (remaining <= 0) remaining = fallback;
    remaining = std::max<std::int64_t>(0, remaining - 1);
    service->database()->execSqlSync(
        "UPDATE room_npcs SET turn_actions_remaining=$1 WHERE room_id=$2 AND npc_template_id=$3",
        remaining, roomId, id);
  }
  return remaining;
}

void advanceCombatAction(const std::shared_ptr<CoreService>& service,
                         std::int64_t roomId, const std::string& expectedType,
                         std::int64_t expectedId, std::int64_t eventUserId,
                         const std::string& reason) {
  auto state = currentCombatState(service, roomId);
  if (!state.exists) throw ApiError(drogon::k404NotFound, "找不到房間");
  if (!state.active) throw ApiError(drogon::k400BadRequest, "目前不是戰鬥階段");
  if (state.actor.isNull()) throw ApiError(drogon::k400BadRequest, "目前沒有可行動單位");
  if (stringValue(state.actor["type"]) != expectedType ||
      integerValue(state.actor["id"]) != expectedId) {
    throw ApiError(drogon::k403Forbidden, "現在不是這個單位的行動");
  }

  const auto remaining = consumeCombatAction(service, roomId, state.actor);
  if (endBattleIfResolved(service, roomId, eventUserId)) return;
  if (remaining > 0) {
    Json::Value event;
    event["text"] = stringValue(state.actor["name"], "單位") + " 消耗 1 回合數（" +
                    reason + "），本輪剩餘 " + std::to_string(remaining);
    event["remaining"] = Json::Int64(remaining);
    event["actor_type"] = expectedType;
    event["actor_id"] = Json::Int64(expectedId);
    service->addEvent(roomId, eventUserId, "turn_action", event);
    return;
  }

  if (state.order.empty()) return;
  std::int64_t nextIndex = -1;
  bool wrapped = false;
  for (Json::ArrayIndex step = 1; step <= state.order.size(); ++step) {
    const auto index = static_cast<Json::ArrayIndex>(
        (state.index + static_cast<std::int64_t>(step)) % state.order.size());
    if (combatActorAlive(service, roomId, state.order[index])) {
      nextIndex = index;
      wrapped = static_cast<std::int64_t>(index) <= state.index;
      break;
    }
  }
  if (nextIndex < 0) return;
  auto nextRound = state.round;
  if (wrapped) {
    ++nextRound;
    resetCombatActions(service, roomId, state.order);
    processRoundStatuses(service, roomId, eventUserId, nextRound);
    // [功能備註｜62-cpp.23 狀態回合] 新事件狀態在回合切換時遞減，並觸發 ON_ROUND_START。
    tickEventSkillStatuses(service, roomId, nextRound);
    if (endBattleIfResolved(service, roomId, eventUserId)) return;
  }
  const auto next = state.order[static_cast<Json::ArrayIndex>(nextIndex)];
  setCombatActor(service, roomId, next, nextIndex, nextRound);
  if (wrapped) {
    Json::Value roundEvent; roundEvent["round"] = Json::Int64(nextRound);
    roundEvent["next_actor"] = next;
    evaluateRuleEvent(service, roomId, eventUserId, "COMBAT_ROUND_STARTED", roundEvent, false);
    evaluateRuleCombatVictory(service, roomId, eventUserId, "COMBAT_ROUND_STARTED");
  }
  Json::Value turnRuleEvent; turnRuleEvent["round"] = Json::Int64(nextRound); turnRuleEvent["actor"] = next;
  evaluateRuleEvent(service, roomId, eventUserId, "COMBAT_TURN_STARTED", turnRuleEvent, false);
  try {
    dispatchEventSkillEvent(service, roomId, stringValue(next["type"]), integerValue(next["id"]),
                            "COMBAT_TURN_STARTED", turnRuleEvent);
  } catch (const std::exception& error) {
    LOG_WARN << "event skill turn trigger degraded: " << error.what();
  }
  // [功能備註｜通知中心] 自動通知下一位玩家，不需要 DM 另外提醒。
  if (stringValue(next["type"]) == "player") {
    Json::Value noticeData; noticeData["room_id"] = Json::Int64(roomId); noticeData["round"] = Json::Int64(nextRound);
    service->database()->execSqlSync(
        "INSERT INTO user_notifications(user_id,notification_type,title,message,data) "
        "VALUES($1,'turn','輪到你行動',$2,$3::jsonb)",
        integerValue(next["id"]),
        "第 " + std::to_string(nextRound) + " 回合輪到你行動。",
        compactJson(noticeData));
  }
  Json::Value event;
  event["round"] = Json::Int64(nextRound);
  event["actor_type"] = next["type"];
  event["actor_id"] = next["id"];
  event["text"] = "輪到 " + stringValue(next["name"], "單位") + " 行動";
  service->addEvent(roomId, eventUserId, "turn", event);
}


std::int64_t secureCombatRoll(std::int64_t minimum, std::int64_t maximum) {
  if (maximum <= minimum) return minimum;
  std::uint64_t raw = 0;
  if (RAND_bytes(reinterpret_cast<unsigned char*>(&raw), sizeof(raw)) != 1) {
    throw std::runtime_error("secure combat random generation failed");
  }
  const auto span = static_cast<std::uint64_t>(maximum - minimum + 1);
  return minimum + static_cast<std::int64_t>(raw % span);
}

std::string normalizeCombatAttribute(std::string value,
                                     std::string fallback = "strength") {
  value = trimmed(std::move(value));
  std::string lower = value;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  if (lower == "strength" || value == "力量") return "strength";
  if (lower == "agility" || value == "敏捷") return "agility";
  if (lower == "constitution" || value == "體質") return "constitution";
  if (lower == "spirit" || value == "精神") return "spirit";
  if (lower == "luck" || value == "幸運") return "luck";
  if (lower == "fifth" || value == "大道" || value == "信仰") return "fifth";
  if (lower == "endurance" || value == "耐力") return "endurance";
  if (lower == "will" || value == "意志") return "will";
  if (lower == "sanity" || value == "理智") return "sanity";
  return fallback;
}

[[maybe_unused]] std::string combatAttributeLabel(const std::string& key) {
  if (key == "strength") return "力量";
  if (key == "agility") return "敏捷";
  if (key == "constitution") return "體質";
  if (key == "spirit") return "精神";
  if (key == "luck") return "幸運";
  if (key == "fifth") return "大道／信仰";
  if (key == "endurance") return "耐力";
  if (key == "will") return "意志";
  if (key == "sanity") return "理智";
  return key;
}

std::int64_t combatPlayerStat(const Json::Value& character,
                              const std::string& rawKey) {
  const auto key = normalizeCombatAttribute(rawKey);
  if (key == "strength") return nonNegative(character["final_strength"]);
  if (key == "agility") return nonNegative(character["final_agility"]);
  if (key == "constitution") return nonNegative(character["final_constitution"]);
  if (key == "spirit") return nonNegative(character["final_spirit"]);
  if (key == "luck") return nonNegative(character["luck"]);
  if (key == "fifth") return nonNegative(character["fifth_current"]);
  if (key == "endurance") return nonNegative(character["endurance"]);
  if (key == "will") return nonNegative(character["will"]);
  if (key == "sanity") return nonNegative(character["sanity"]);
  return 0;
}

std::int64_t combatEntityStat(const Json::Value& entity,
                              const std::string& rawKey) {
  const auto key = normalizeCombatAttribute(rawKey);
  const auto attrs = entity["attributes"].isObject()
                         ? entity["attributes"]
                         : Json::Value{Json::objectValue};
  if (key == "fifth") {
    return std::max(nonNegative(attrs["great_way"]),
                    std::max(nonNegative(attrs["faith"]),
                             nonNegative(attrs["fifth"])));
  }
  if (key == "endurance") {
    return (nonNegative(attrs["constitution"]) + nonNegative(attrs["spirit"])) / 2;
  }
  if (key == "will") return nonNegative(attrs["spirit"]) * 8 / 10;
  if (key == "sanity") return nonNegative(attrs["spirit"]) * 12 / 10;
  return nonNegative(attrs[key]);
}

std::string combatSuccessLevel(std::int64_t roll, std::int64_t target) {
  target = std::max<std::int64_t>(0, target);
  if (roll == 1) return "大成功";
  if (target <= 0) return "失敗";
  if (roll <= std::max<std::int64_t>(1, target / 5)) return "極難成功";
  if (roll <= std::max<std::int64_t>(1, target / 2)) return "困難成功";
  if (roll <= target) return "普通成功";
  return "失敗";
}

int combatLevelRank(const std::string& level) {
  if (level == "大成功") return 4;
  if (level == "極難成功") return 3;
  if (level == "困難成功") return 2;
  if (level == "普通成功") return 1;
  return 0;
}

Json::Value rollCombatCheck(std::int64_t targetValue, std::int64_t bonus = 0) {
  Json::Value result;
  const auto target = std::max<std::int64_t>(0, targetValue + bonus);
  const auto roll = secureCombatRoll(1, 100);
  result["roll"] = Json::Int64(roll);
  result["target"] = Json::Int64(target);
  result["level"] = combatSuccessLevel(roll, target);
  return result;
}

bool combatAttackHits(const Json::Value& attack, const Json::Value& reaction,
                      const std::string& reactionType) {
  const auto attackLevel = stringValue(attack["level"]);
  const auto reactionLevel = stringValue(reaction["level"]);
  const auto a = combatLevelRank(attackLevel);
  const auto d = combatLevelRank(reactionLevel);
  if (a <= 0) return false;
  if (d <= 0) return true;
  if (attackLevel == "大成功" && reactionLevel == "大成功" &&
      reactionType == "defense") {
    return true;
  }
  return a > d;
}

struct CombatTarget {
  std::string type;
  std::int64_t id{0};
  std::string name;
  std::string side{"neutral"};
  Json::Value member{Json::objectValue};
  Json::Value character{Json::objectValue};
  Json::Value entity{Json::objectValue};
};

CombatTarget loadCombatTarget(const std::shared_ptr<CoreService>& service,
                              std::int64_t roomId, const std::string& type,
                              std::int64_t id) {
  if (id <= 0) throw ApiError(drogon::k400BadRequest, "戰鬥目標 ID 無效");
  if (type == "player") {
    const auto rows = service->database()->execSqlSync(
        "SELECT row_to_json(rm)::text AS member_text FROM room_members rm "
        "WHERE rm.room_id=$1 AND rm.user_id=$2 AND rm.role='player'",
        roomId, id);
    if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到玩家戰鬥單位");
    CombatTarget target;
    target.type = "player";
    target.id = id;
    target.side = "ally";
    target.member = parseJson(rows[0]["member_text"].as<std::string>());
    target.character = service->characterByUser(id);
    if (target.character.isNull()) throw ApiError(drogon::k404NotFound, "找不到玩家角色卡");
    target.name = stringValue(target.character["name"],
                              stringValue(target.member["display_name"], "玩家"));
    if (nonNegative(target.member["hp"]) <= 0 ||
        stringValue(target.member["combat_state"], "active") == "escaped") {
      throw ApiError(drogon::k400BadRequest, "這名玩家目前無法戰鬥");
    }
    return target;
  }
  if (type == "monster") {
    const auto rows = service->database()->execSqlSync(R"SQL(
      SELECT jsonb_build_object(
        'id',rm.id,'name',mt.name,'status',rm.status,'current_hp',rm.current_hp,
        'max_hp',rm.max_hp,'current_spirit',rm.current_spirit,
        'current_fifth',rm.current_fifth,'attributes',mt.attributes,
        'skills',mt.skills,'passive_skills',mt.passive_skills,'great_way',mt.great_way,
        'boss_phases',mt.boss_phases,'config',mt.config,'ai_behavior',mt.ai_behavior,'element_affinity',mt.element_affinity,'element_resistance',mt.element_resistance,'element_weakness',mt.element_weakness,
        'boss_phase_index',COALESCE((to_jsonb(rm)->>'boss_phase_index')::bigint,0),
        'boss_phase_name',COALESCE(to_jsonb(rm)->>'boss_phase_name',''),
        'rule_boss_phase',rm.rule_boss_phase,'rule_modifiers',rm.rule_modifiers
      )::text AS json_text
      FROM room_monsters rm JOIN monster_templates mt ON mt.id=rm.monster_template_id
      WHERE rm.room_id=$1 AND rm.id=$2
    )SQL", roomId, id);
    if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到怪物戰鬥單位");
    CombatTarget target;
    target.type = "monster";
    target.id = id;
    target.side = "enemy";
    target.entity = parseJson(rows[0]["json_text"].as<std::string>());
    // [功能備註｜規則－戰鬥橋接] Boss/規則修正會在真正戰鬥判定前合併到怪物 attributes/config。
    if (target.entity["rule_modifiers"].isObject()) {
      const auto& mods = target.entity["rule_modifiers"];
      if (!target.entity["attributes"].isObject()) target.entity["attributes"] = Json::Value{Json::objectValue};
      if (!target.entity["config"].isObject()) target.entity["config"] = Json::Value{Json::objectValue};
      if (mods["attributes"].isObject()) {
        for (const auto& key : mods["attributes"].getMemberNames())
          target.entity["attributes"][key] = Json::Int64(integerValue(target.entity["attributes"][key]) + integerValue(mods["attributes"][key]));
      }
      if (mods["config"].isObject())
        for (const auto& key : mods["config"].getMemberNames()) target.entity["config"][key] = mods["config"][key];
      if (mods.isMember("damage_percent")) target.entity["config"]["rule_damage_percent"] = mods["damage_percent"];
      if (mods.isMember("accuracy_bonus"))
        target.entity["config"]["combat_accuracy_bonus"] = Json::Int64(integerValue(target.entity["config"]["combat_accuracy_bonus"]) + integerValue(mods["accuracy_bonus"]));
    }
    target.name = stringValue(target.entity["name"], "怪物");
    if (stringValue(target.entity["status"]) != "encountered" ||
        nonNegative(target.entity["current_hp"]) <= 0) {
      throw ApiError(drogon::k400BadRequest, "這個怪物目前不是可攻擊目標");
    }
    return target;
  }
  if (type == "npc") {
    const auto rows = service->database()->execSqlSync(R"SQL(
      SELECT jsonb_build_object(
        'id',rn.npc_template_id,'name',nt.name,'status',rn.status,
        'combat_side',rn.combat_side,'current_hp',rn.current_hp,'max_hp',rn.max_hp,
        'current_spirit',rn.current_spirit,'current_fifth',rn.current_fifth,
        'attributes',nt.attributes,'config',nt.config,'ai_behavior',nt.ai_behavior,'element_affinity',nt.element_affinity,'element_resistance',nt.element_resistance,'element_weakness',nt.element_weakness
      )::text AS json_text
      FROM room_npcs rn JOIN npc_templates nt ON nt.id=rn.npc_template_id
      WHERE rn.room_id=$1 AND rn.npc_template_id=$2
    )SQL", roomId, id);
    if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到 NPC 戰鬥單位");
    CombatTarget target;
    target.type = "npc";
    target.id = id;
    target.entity = parseJson(rows[0]["json_text"].as<std::string>());
    target.name = stringValue(target.entity["name"], "NPC");
    target.side = stringValue(target.entity["combat_side"], "neutral");
    if (stringValue(target.entity["status"]) != "active" ||
        nonNegative(target.entity["current_hp"]) <= 0 ||
        (target.side != "ally" && target.side != "enemy")) {
      throw ApiError(drogon::k400BadRequest, "這個 NPC 目前無法戰鬥");
    }
    return target;
  }
  throw ApiError(drogon::k400BadRequest, "不支援的戰鬥目標類型");
}

std::string combatTargetReactionType(const CombatTarget& target) {
  if (target.type == "player") {
    return stringValue(target.member["combat_reaction"], "dodge") == "defense"
               ? "defense"
               : "dodge";
  }
  const auto behavior = stringValue(target.entity["ai_behavior"], "balanced");
  if (behavior == "defensive") return "defense";
  if (behavior == "aggressive") return "dodge";
  const auto config = target.entity["config"].isObject()
                          ? target.entity["config"]
                          : Json::Value{Json::objectValue};
  const auto defenseKey = normalizeCombatAttribute(
      stringValue(config["combat_defense_attribute"], "constitution"),
      "constitution");
  const auto dodgeKey = normalizeCombatAttribute(
      stringValue(config["combat_dodge_attribute"], "agility"), "agility");
  return combatEntityStat(target.entity, defenseKey) >=
                 combatEntityStat(target.entity, dodgeKey)
             ? "defense"
             : "dodge";
}

Json::Value combatTargetReaction(const CombatTarget& target) {
  const auto type = combatTargetReactionType(target);
  std::string key = type == "defense" ? "constitution" : "agility";
  if (target.type != "player") {
    const auto config = target.entity["config"].isObject()
                            ? target.entity["config"]
                            : Json::Value{Json::objectValue};
    key = normalizeCombatAttribute(
        stringValue(config[type == "defense" ? "combat_defense_attribute"
                                               : "combat_dodge_attribute"],
                    key),
        key);
  }
  auto check = target.type == "player"
                   ? rollCombatCheck(combatPlayerStat(target.character, key))
                   : rollCombatCheck(combatEntityStat(target.entity, key));
  check["type"] = type;
  check["attribute"] = key;
  return check;
}

// [功能備註｜62-cpp.23 屬性運算傷害式]
// 除既有 1D力量 / 2D6+3 外，支援安全的四則運算式，例如「1+精神*2」。
// 僅允許數字、括號、+ - * / 與已知戰鬥屬性，不使用 eval、不執行任意程式碼。
class CombatExpressionParser {
 public:
  CombatExpressionParser(std::string text, const Json::Value& source,
                         std::string sourceType)
      : text_(std::move(text)), source_(source), sourceType_(std::move(sourceType)) {}

  long double parse() {
    position_ = 0;
    const auto value = parseExpression();
    skipSpaces();
    if (position_ != text_.size()) throw ApiError(drogon::k400BadRequest, "傷害公式包含不支援的內容");
    return value;
  }

 private:
  std::string text_;
  const Json::Value& source_;
  std::string sourceType_;
  std::size_t position_{0};

  void skipSpaces() {
    while (position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_]))) ++position_;
  }

  bool consume(char ch) {
    skipSpaces();
    if (position_ < text_.size() && text_[position_] == ch) { ++position_; return true; }
    return false;
  }

  long double parseExpression() {
    auto value = parseTerm();
    while (true) {
      if (consume('+')) value += parseTerm();
      else if (consume('-')) value -= parseTerm();
      else break;
    }
    return value;
  }

  long double parseTerm() {
    auto value = parseUnary();
    while (true) {
      if (consume('*')) value *= parseUnary();
      else if (consume('/')) {
        const auto divisor = parseUnary();
        if (divisor == 0.0L) throw ApiError(drogon::k400BadRequest, "傷害公式不能除以 0");
        value /= divisor;
      } else break;
    }
    return value;
  }

  long double parseUnary() {
    if (consume('+')) return parseUnary();
    if (consume('-')) return -parseUnary();
    return parsePrimary();
  }

  long double parsePrimary() {
    skipSpaces();
    if (consume('(')) {
      const auto value = parseExpression();
      if (!consume(')')) throw ApiError(drogon::k400BadRequest, "傷害公式括號不完整");
      return value;
    }
    if (position_ >= text_.size()) throw ApiError(drogon::k400BadRequest, "傷害公式不完整");
    if (std::isdigit(static_cast<unsigned char>(text_[position_])) || text_[position_] == '.') {
      const auto begin = position_;
      bool dot = false;
      while (position_ < text_.size()) {
        const auto ch = text_[position_];
        if (std::isdigit(static_cast<unsigned char>(ch))) { ++position_; continue; }
        if (ch == '.' && !dot) { dot = true; ++position_; continue; }
        break;
      }
      try { return std::stold(text_.substr(begin, position_ - begin)); }
      catch (...) { throw ApiError(drogon::k400BadRequest, "傷害公式數字格式錯誤"); }
    }

    static const std::array<std::pair<std::string_view, std::string_view>, 18> names = {{
        {"constitution", "constitution"}, {"endurance", "endurance"}, {"strength", "strength"},
        {"agility", "agility"}, {"spirit", "spirit"}, {"sanity", "sanity"},
        {"fifth", "fifth"}, {"luck", "luck"}, {"will", "will"},
        {"力量", "strength"}, {"敏捷", "agility"}, {"體質", "constitution"},
        {"精神", "spirit"}, {"幸運", "luck"}, {"大道", "fifth"}, {"信仰", "fifth"},
        {"耐力", "endurance"}, {"意志", "will"}
    }};
    for (const auto& [token, key] : names) {
      if (text_.compare(position_, token.size(), token) != 0) continue;
      position_ += token.size();
      return static_cast<long double>(sourceType_ == "player"
                                          ? combatPlayerStat(source_, std::string{key})
                                          : combatEntityStat(source_, std::string{key}));
    }
    if (text_.compare(position_, std::string_view{"理智"}.size(), "理智") == 0) {
      position_ += std::string_view{"理智"}.size();
      return static_cast<long double>(sourceType_ == "player"
                                          ? combatPlayerStat(source_, "sanity")
                                          : combatEntityStat(source_, "sanity"));
    }
    throw ApiError(drogon::k400BadRequest, "傷害公式只能使用數字、四則運算與角色屬性");
  }
};

Json::Value rollSimpleDamageFormula(const std::string& rawFormula,
                                    const Json::Value& source,
                                    const std::string& sourceType) {
  const auto formula = trimmed(rawFormula.empty() ? std::string{"1D力量"} : rawFormula);
  static const std::regex pattern(
      R"(^\s*(\d{1,2})\s*[dD]\s*(力量|敏捷|體質|精神|幸運|大道|信仰|耐力|意志|理智|\d{1,6})\s*([+-]\s*\d{1,9})?\s*$)");
  std::smatch match;
  if (!std::regex_match(formula, match, pattern)) {
    const auto value = CombatExpressionParser(formula, source, sourceType).parse();
    const auto maximum = static_cast<long double>(std::numeric_limits<std::int64_t>::max() / 4);
    const auto total = static_cast<std::int64_t>(std::clamp<long double>(value, 0.0L, maximum));
    Json::Value result;
    result["formula"] = formula;
    result["rolls"] = Json::arrayValue;
    result["modifier"] = 0;
    result["total"] = Json::Int64(total);
    result["calculation_mode"] = "expression";
    return result;
  }
  const auto count = std::clamp<std::int64_t>(std::stoll(match[1].str()), 1, 20);
  const auto sidesText = match[2].str();
  std::int64_t sides = 1;
  if (!sidesText.empty() && std::isdigit(static_cast<unsigned char>(sidesText[0]))) {
    sides = std::clamp<std::int64_t>(std::stoll(sidesText), 1, 100000);
  } else {
    const auto key = normalizeCombatAttribute(sidesText, "strength");
    sides = std::max<std::int64_t>(1, sourceType == "player"
                                         ? combatPlayerStat(source, key)
                                         : combatEntityStat(source, key));
  }
  std::int64_t modifier = 0;
  if (match[3].matched) {
    auto text = match[3].str();
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char ch) {
                 return std::isspace(ch);
               }),
               text.end());
    modifier = std::stoll(text);
  }
  Json::Value rolls{Json::arrayValue};
  std::int64_t sum = 0;
  for (std::int64_t index = 0; index < count; ++index) {
    const auto value = secureCombatRoll(1, sides);
    sum += value;
    rolls.append(Json::Int64(value));
  }
  const auto total = std::max<std::int64_t>(0, sum + modifier);
  Json::Value result;
  result["formula"] = formula;
  result["rolls"] = std::move(rolls);
  result["modifier"] = Json::Int64(modifier);
  result["total"] = Json::Int64(total);
  result["calculation_mode"] = "dice";
  return result;
}

Json::Value applyCombatDamage(const std::shared_ptr<CoreService>& service,
                              std::int64_t roomId, const CombatTarget& target,
                              const std::string& rawKind, std::int64_t amount) {
  const auto damage = std::max<std::int64_t>(0, amount);
  const auto kind = rawKind == "spirit" || rawKind == "sanity" || rawKind == "fifth"
                        ? rawKind
                        : std::string{"hp"};
  Json::Value change;
  change["kind"] = kind;
  if (target.type == "player") {
    if (kind == "hp") {
      const auto before = nonNegative(target.member["hp"]);
      const auto after = std::max<std::int64_t>(0, before - damage);
      service->database()->execSqlSync(
          "UPDATE room_members SET hp=$1,combat_state=CASE WHEN $1>0 THEN 'active' "
          "ELSE 'downed' END WHERE room_id=$2 AND user_id=$3",
          after, roomId, target.id);
      change["before"] = Json::Int64(before);
      change["after"] = Json::Int64(after);
      change["downed"] = after <= 0;
      return change;
    }
    if (kind == "spirit" || kind == "sanity") {
      const auto field = kind == "spirit" ? "spirit" : "current_sanity";
      const auto before = nonNegative(target.member[field]);
      const auto after = std::max<std::int64_t>(0, before - damage);
      service->database()->execSqlSync(
          std::string{"UPDATE room_members SET "} + field + "=$1 WHERE room_id=$2 AND user_id=$3",
          after, roomId, target.id);
      change["before"] = Json::Int64(before);
      change["after"] = Json::Int64(after);
      return change;
    }
    const auto west = stringValue(target.character["faction"], "東國") == "西國";
    const auto field = west ? "current_faith" : "current_great_way";
    const auto before = nonNegative(target.character["fifth_current"]);
    const auto after = std::max<std::int64_t>(0, before - damage);
    service->database()->execSqlSync(
        std::string{"UPDATE character_cards SET "} + field +
            "=$1,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",
        after, target.id);
    change["before"] = Json::Int64(before);
    change["after"] = Json::Int64(after);
    return change;
  }

  const auto field = kind == "hp" ? "current_hp"
                    : (kind == "spirit" || kind == "sanity") ? "current_spirit"
                                                                : "current_fifth";
  const auto before = nonNegative(target.entity[field]);
  const auto after = std::max<std::int64_t>(0, before - damage);
  if (target.type == "monster") {
    if (field == std::string{"current_hp"}) {
      service->database()->execSqlSync(
          "UPDATE room_monsters SET current_hp=$1,status=CASE WHEN $1<=0 THEN "
          "'defeated' ELSE status END,updated_at=CURRENT_TIMESTAMP WHERE room_id=$2 AND id=$3",
          after, roomId, target.id);
    } else {
      service->database()->execSqlSync(
          std::string{"UPDATE room_monsters SET "} + field +
              "=$1,updated_at=CURRENT_TIMESTAMP WHERE room_id=$2 AND id=$3",
          after, roomId, target.id);
    }
  } else {
    if (field == std::string{"current_hp"}) {
      service->database()->execSqlSync(
          "UPDATE room_npcs SET current_hp=$1,status=CASE WHEN $1<=0 THEN 'downed' "
          "ELSE 'active' END,updated_at=CURRENT_TIMESTAMP WHERE room_id=$2 AND npc_template_id=$3",
          after, roomId, target.id);
    } else {
      service->database()->execSqlSync(
          std::string{"UPDATE room_npcs SET "} + field +
              "=$1,updated_at=CURRENT_TIMESTAMP WHERE room_id=$2 AND npc_template_id=$3",
          after, roomId, target.id);
    }
  }
  change["before"] = Json::Int64(before);
  change["after"] = Json::Int64(after);
  change["downed"] = kind == "hp" && after <= 0;
  return change;
}

void adjustCombatActions(const std::shared_ptr<CoreService>& service,
                         std::int64_t roomId, const std::string& type,
                         std::int64_t id, std::int64_t delta) {
  if (id <= 0 || delta == 0) return;
  if (type == "player") {
    service->database()->execSqlSync(
        "UPDATE room_members SET turn_actions_remaining=GREATEST(0,turn_actions_remaining+$1) "
        "WHERE room_id=$2 AND user_id=$3",
        delta, roomId, id);
  } else if (type == "monster") {
    service->database()->execSqlSync(
        "UPDATE room_monsters SET turn_actions_remaining=GREATEST(0,turn_actions_remaining+$1) "
        "WHERE room_id=$2 AND id=$3",
        delta, roomId, id);
  } else if (type == "npc") {
    service->database()->execSqlSync(
        "UPDATE room_npcs SET turn_actions_remaining=GREATEST(0,turn_actions_remaining+$1) "
        "WHERE room_id=$2 AND npc_template_id=$3",
        delta, roomId, id);
  }
}

// [功能備註｜65-cpp.26 元素親和／抗性／弱點]
// 元素傷害倍率 = 100% + 攻擊者親和 - 目標抗性 + 目標弱點；最終限制在 0%~1000%。
Json::Value elementalDamageModifier(const Json::Value& attackerSource,
                                    const std::string& attackerType,
                                    const CombatTarget& target,
                                    const std::string& rawElement,
                                    std::int64_t damage) {
  static const std::set<std::string> elements={"暗","光","金","木","水","火","土"};
  const auto element=trimmed(rawElement);
  Json::Value out{Json::objectValue};
  out["element"]=element;
  out["base_damage"]=Json::Int64(std::max<std::int64_t>(0,damage));
  if(!elements.contains(element)) { out["applied"]=false; out["damage"]=out["base_damage"]; return out; }
  const auto affinityMap=attackerSource["element_affinity"].isObject()?attackerSource["element_affinity"]:Json::Value{Json::objectValue};
  const auto targetSource=target.type=="player"?target.character:target.entity;
  const auto resistanceMap=targetSource["element_resistance"].isObject()?targetSource["element_resistance"]:Json::Value{Json::objectValue};
  const auto weaknessMap=targetSource["element_weakness"].isObject()?targetSource["element_weakness"]:Json::Value{Json::objectValue};
  const auto affinity=std::clamp<std::int64_t>(integerValue(affinityMap[element]),0,900);
  const auto resistance=std::clamp<std::int64_t>(integerValue(resistanceMap[element]),0,900);
  const auto weakness=std::clamp<std::int64_t>(integerValue(weaknessMap[element]),0,900);
  const auto percent=std::clamp<std::int64_t>(100+affinity-resistance+weakness,0,1000);
  const auto finalDamage=std::max<std::int64_t>(0,damage*percent/100);
  out["applied"]=true;
  out["attacker_type"]=attackerType;
  out["affinity_percent"]=Json::Int64(affinity);
  out["resistance_percent"]=Json::Int64(resistance);
  out["weakness_percent"]=Json::Int64(weakness);
  out["multiplier_percent"]=Json::Int64(percent);
  out["damage"]=Json::Int64(finalDamage);
  return out;
}

Json::Value resolveBasicCombatAttack(const std::shared_ptr<CoreService>& service,
                                     std::int64_t roomId,
                                     const std::string& attackerType,
                                     std::int64_t attackerId,
                                     const CombatTarget& target,
                                     const Json::Value& actionConfig = Json::Value{Json::nullValue}) {
  Json::Value attackerSource;
  std::string attackerName;
  std::string attackerSide;
  Json::Value config{Json::objectValue};
  if (attackerType == "player") {
    attackerSource = service->characterByUser(attackerId);
    if (attackerSource.isNull()) throw ApiError(drogon::k404NotFound, "找不到攻擊者角色卡");
    attackerName = stringValue(attackerSource["name"], "玩家");
    attackerSide = "ally";
  } else {
    const auto attacker = loadCombatTarget(service, roomId, attackerType, attackerId);
    attackerSource = attacker.entity;
    attackerName = attacker.name;
    attackerSide = attacker.side;
    config = attacker.entity["config"].isObject()
                 ? attacker.entity["config"]
                 : Json::Value{Json::objectValue};
  }
  if (attackerSide == target.side) {
    throw ApiError(drogon::k400BadRequest, "不能攻擊同一陣營的目標");
  }
  // [功能備註｜60-cpp.21 怪物技能] AI 主動技能／大道技可以覆蓋基本攻擊的判定屬性、傷害式、傷害資源與命中。
  if (actionConfig.isObject()) {
    if (actionConfig.isMember("attack_attribute")) config["combat_attack_attribute"] = actionConfig["attack_attribute"];
    if (actionConfig.isMember("damage_formula")) config["combat_damage_formula"] = actionConfig["damage_formula"];
    if (actionConfig.isMember("damage_target")) config["combat_damage_target"] = actionConfig["damage_target"];
    if (actionConfig.isMember("accuracy_bonus")) config["combat_accuracy_bonus"] = actionConfig["accuracy_bonus"];
    if (actionConfig.isMember("element")) config["combat_element"] = actionConfig["element"];
  }
  const auto attackKey = normalizeCombatAttribute(
      stringValue(config["combat_attack_attribute"], "strength"), "strength");
  const auto accuracy = std::clamp<std::int64_t>(integerValue(config["combat_accuracy_bonus"]),
                                                 -10000, 10000);
  const auto attackStat = attackerType == "player"
                              ? combatPlayerStat(attackerSource, attackKey)
                              : combatEntityStat(attackerSource, attackKey);
  auto attack = rollCombatCheck(attackStat, accuracy);
  attack["attribute"] = attackKey;
  const auto reaction = combatTargetReaction(target);
  const auto reactionType = stringValue(reaction["type"], "dodge");
  const bool hit = combatAttackHits(attack, reaction, reactionType);

  const auto formula = stringValue(config["combat_damage_formula"], "1D力量");
  const auto damageTarget = [&] {
    const auto value = stringValue(config["combat_damage_target"], "hp");
    return value == "spirit" || value == "sanity" || value == "fifth" ? value
                                                                        : std::string{"hp"};
  }();
  Json::Value formulaResult;
  formulaResult["formula"] = formula;
  formulaResult["rolls"] = Json::arrayValue;
  formulaResult["modifier"] = 0;
  formulaResult["total"] = 0;
  std::int64_t baseDamage = 0;
  std::int64_t finalDamage = 0;
  Json::Value damageChange{Json::nullValue};
  Json::Value eventSkillDamageModifier{Json::objectValue};
  Json::Value elementalModifier{Json::objectValue};
  if (hit) {
    formulaResult = rollSimpleDamageFormula(
        formula, attackerSource, attackerType == "player" ? "player" : "entity");
    baseDamage = integerValue(formulaResult["total"]);
    finalDamage = reactionType == "defense" ? baseDamage * 8 / 10 : baseDamage;
    if (actionConfig.isObject()) {
      const auto multiplier = std::clamp(actionConfig.get("damage_multiplier", 1.0).asDouble(), 0.0, 1000.0);
      const auto percent = std::clamp<std::int64_t>(integerValue(actionConfig["damage_percent"], 100), 0, 100000);
      finalDamage = static_cast<std::int64_t>(static_cast<long double>(finalDamage) * multiplier * static_cast<long double>(percent) / 100.0L);
    }
    const auto ruleDamagePercent = std::clamp<std::int64_t>(integerValue(config["rule_damage_percent"]), -95, 1000);
    finalDamage = std::max<std::int64_t>(0, finalDamage * (100 + ruleDamagePercent) / 100);
    const auto combatElement=stringValue(config["combat_element"]);
    elementalModifier=elementalDamageModifier(attackerSource,attackerType,target,combatElement,finalDamage);
    finalDamage=integerValue(elementalModifier["damage"],finalDamage);
    // [功能備註｜62-cpp.23 狀態層數增傷] 在真正扣血前讀取目前狀態；本 Hit 造成的新標記只會影響下一 Hit。
    finalDamage = applyEventSkillDamageModifiers(service, roomId, attackerType, attackerId,
                                                  target.type, target.id, finalDamage,
                                                  &eventSkillDamageModifier);
    damageChange = applyCombatDamage(service, roomId, target, damageTarget, finalDamage);
  }

  const bool attackerCritical = stringValue(attack["level"]) == "大成功";
  const bool reactionCritical = stringValue(reaction["level"]) == "大成功";
  if (attackerCritical) adjustCombatActions(service, roomId, attackerType, attackerId, 1);
  if (reactionCritical) adjustCombatActions(service, roomId, target.type, target.id, 1);

  Json::Value event;
  const auto reactionLabel = reactionType == "defense" ? "防禦" : "閃避";
  const auto actionName = actionConfig.isObject() ? stringValue(actionConfig["name"]) : std::string{};
  event["text"] = (attackerType == "monster" ? "👹 " : attackerType == "npc" ? "🧑 " : "") +
                  attackerName + (actionName.empty() ? " 攻擊「" : " 使用「" + actionName + "」攻擊「") + target.name + "」：攻擊 " +
                  std::to_string(integerValue(attack["roll"])) + "/" +
                  std::to_string(integerValue(attack["target"])) + " " +
                  stringValue(attack["level"]) + " VS " + reactionLabel + " " +
                  std::to_string(integerValue(reaction["roll"])) + "/" +
                  std::to_string(integerValue(reaction["target"])) + " " +
                  stringValue(reaction["level"]) + " → " +
                  (hit ? "命中，造成 " + std::to_string(finalDamage) + " 傷害" : "未命中") +
                  ((!damageChange.isNull() && damageChange.get("downed", false).asBool())
                       ? "，目標倒地／失去戰鬥能力"
                       : "");
  event["attacker_type"] = attackerType;
  event["attacker_id"] = Json::Int64(attackerId);
  event["attacker_name"] = attackerName;
  event["target_type"] = target.type;
  event["target_id"] = Json::Int64(target.id);
  event["target_name"] = target.name;
  event["attack"] = attack;
  event["reaction"] = reaction;
  event["hit"] = hit;
  event["base_damage"] = Json::Int64(baseDamage);
  event["damage"] = Json::Int64(finalDamage);
  event["damage_target"] = damageTarget;
  event["damage_change"] = damageChange;
  event["formula"] = formulaResult;
  event["event_skill_damage_modifier"] = eventSkillDamageModifier;
  event["elemental_damage_modifier"] = elementalModifier;
  event["attacker_critical_action_bonus"] = attackerCritical;
  event["reaction_critical_action_bonus"] = reactionCritical;
  if (actionConfig.isObject()) event["skill"] = actionConfig;

  Json::Value result;
  result["attacker"]["type"] = attackerType;
  result["attacker"]["id"] = Json::Int64(attackerId);
  result["attacker"]["name"] = attackerName;
  result["target"]["type"] = target.type;
  result["target"]["id"] = Json::Int64(target.id);
  result["target"]["name"] = target.name;
  result["attack"] = attack;
  result["reaction"] = reaction;
  result["hit"] = hit;
  result["base_damage"] = Json::Int64(baseDamage);
  result["damage"] = Json::Int64(finalDamage);
  result["damage_target"] = damageTarget;
  result["damage_change"] = damageChange;
  result["formula"] = formulaResult;
  result["event_skill_damage_modifier"] = eventSkillDamageModifier;
  result["elemental_damage_modifier"] = elementalModifier;
  result["attacker_critical_action_bonus"] = attackerCritical;
  result["reaction_critical_action_bonus"] = reactionCritical;
  if (actionConfig.isObject()) result["skill"] = actionConfig;
  result["event_payload"] = event;
  return result;
}


// [功能備註｜60-cpp.21 怪物戰鬥 AI]
// 主動技能與大道技都能帶 ai_condition；被動技能不進主動行動選擇，因此不會誤消耗回合。
std::int64_t combatTargetHpPercent(const CombatTarget& target) {
  const auto current = target.type == "player" ? nonNegative(target.member["hp"])
                                                 : nonNegative(target.entity["current_hp"]);
  const auto maximum = target.type == "player" ? std::max<std::int64_t>(1, nonNegative(target.member["max_hp"], 1))
                                                 : std::max<std::int64_t>(1, nonNegative(target.entity["max_hp"], 1));
  return std::clamp<std::int64_t>(current * 100 / maximum, 0, 100);
}

bool monsterSkillConditionMatches(const Json::Value& skill,
                                  const CombatTarget& attacker,
                                  const CombatTarget& target,
                                  std::int64_t round) {
  if (!skill.isObject()) return false;
  const auto type = stringValue(skill["type"], "active");
  if (type == "passive") return false;
  const auto formula = stringValue(skill["damage_formula"]);
  if (formula.empty()) return false; // 60-cpp.21 先只自動執行有戰鬥傷害式的技能；敘事技能保留給 DM。
  const auto condition = skill["ai_condition"].isObject()
                             ? skill["ai_condition"]
                             : Json::Value{Json::objectValue};
  const auto selfHp = combatTargetHpPercent(attacker);
  const auto targetHp = combatTargetHpPercent(target);
  if (condition.isMember("self_hp_lte") && selfHp > integerValue(condition["self_hp_lte"])) return false;
  if (condition.isMember("self_hp_gte") && selfHp < integerValue(condition["self_hp_gte"])) return false;
  if (condition.isMember("target_hp_lte") && targetHp > integerValue(condition["target_hp_lte"])) return false;
  if (condition.isMember("target_hp_gte") && targetHp < integerValue(condition["target_hp_gte"])) return false;
  if (condition.isMember("min_round") && round < integerValue(condition["min_round"])) return false;
  if (condition.isMember("max_round") && round > integerValue(condition["max_round"])) return false;
  // boss_phase_index 是舊 Boss 流程的 0-based 索引；AI 條件 phase=1/2/... 使用玩家看得懂的 1-based 階段。
  const auto phase = std::max<std::int64_t>(integerValue(attacker.entity["boss_phase_index"]) + 1,
                                             integerValue(attacker.entity["rule_boss_phase"], 1));
  if (condition.isMember("boss_phase") && phase != integerValue(condition["boss_phase"])) return false;
  const auto chance = std::clamp<std::int64_t>(integerValue(condition["chance_percent"], 100), 0, 100);
  if (chance <= 0) return false;
  if (chance < 100 && secureCombatRoll(1, 100) > chance) return false;
  const auto resourceCost = std::max<std::int64_t>(0, integerValue(skill["resource_cost"]));
  if (resourceCost > nonNegative(attacker.entity["current_fifth"])) return false;
  return true;
}

Json::Value chooseMonsterCombatSkill(const CombatTarget& attacker,
                                     const CombatTarget& target,
                                     std::int64_t round) {
  Json::Value candidates{Json::arrayValue};
  const auto appendEligible = [&](const Json::Value& list, const std::string& forcedType) {
    if (!list.isArray()) return;
    for (const auto& raw : list) {
      if (!raw.isObject()) continue; // 舊字串技能仍顯示，但不會被 AI 當成可執行資料。
      auto skill = raw;
      if (!forcedType.empty()) skill["type"] = forcedType;
      if (monsterSkillConditionMatches(skill, attacker, target, round)) candidates.append(skill);
    }
  };
  appendEligible(attacker.entity["skills"], "");
  const auto greatWay = attacker.entity["great_way"].isObject()
                            ? attacker.entity["great_way"]
                            : Json::Value{Json::objectValue};
  if (greatWay.get("enabled", false).asBool()) appendEligible(greatWay["skills"], "great_way");
  if (candidates.empty()) return Json::Value{Json::nullValue};

  std::int64_t bestPriority = std::numeric_limits<std::int64_t>::min();
  std::vector<Json::ArrayIndex> best;
  for (Json::ArrayIndex i = 0; i < candidates.size(); ++i) {
    const auto priority = integerValue(candidates[i]["priority"], 100);
    if (priority > bestPriority) { bestPriority = priority; best.clear(); best.push_back(i); }
    else if (priority == bestPriority) best.push_back(i);
  }
  const auto chosen = best[static_cast<std::size_t>(secureCombatRoll(0, static_cast<std::int64_t>(best.size()) - 1))];
  return candidates[chosen];
}


// [功能備註｜73-cpp.34.1 逆命 Boss 正式怪物 AI]
// 無牌者不再依賴額外 boss-act 按鈕；輪到它時，直接在既有 AI 回合中依 HP 階段決策。
bool isAntiFateBoss(const CombatTarget& attacker) {
  if (attacker.type != "monster" || !attacker.entity.isObject()) return false;
  const auto& config = attacker.entity["config"];
  if (config.isObject() && (config.get("anti_fate", false).asBool() ||
                            config.get("fate_interference", false).asBool())) return true;
  return attacker.name == "無牌者";
}

Json::Value executeAntiFateBossAiAction(const std::shared_ptr<CoreService>& service,
                                        std::int64_t roomId,
                                        const CombatTarget& attacker,
                                        const CombatTarget& target,
                                        std::int64_t round,
                                        std::int64_t eventUserId) {
  const auto hp = std::max<std::int64_t>(0, nonNegative(attacker.entity["current_hp"]));
  const auto maxHp = std::max<std::int64_t>(1, nonNegative(attacker.entity["max_hp"]));
  const auto pct = hp * 100 / maxHp;
  const auto phase = pct <= 25 ? 4 : pct <= 50 ? 3 : pct <= 75 ? 2 : 1;
  const auto roll = secureCombatRoll(1, 100);

  std::string phaseName = phase == 1 ? "無牌之初" : phase == 2 ? "因果錯位" :
                          phase == 3 ? "命運吞噬" : "逆寫終局";
  std::string skill;
  std::string text;
  Json::Value effect{Json::objectValue};
  effect["phase"] = phase;
  effect["phase_name"] = phaseName;

  auto combat = resolveBasicCombatAttack(service, roomId, attacker.type, attacker.id, target);
  auto damageAfter = combat["damage_change"].isObject()
                         ? integerValue(combat["damage_change"]["after"], 0)
                         : std::int64_t{0};

  if (phase == 1) {
    skill = roll <= 55 ? "空白預言" : "未定之噪";
    effect["tarot_interference_percent"] = 35;
    effect["forecast_noise_rounds"] = 1;
    text = "牌面被擦成空白，彌夜下一次觀測更容易只看見雜訊。";
    service->database()->execSqlSync(
        "UPDATE room_npcs rn SET state=jsonb_set(COALESCE(rn.state,'{}'::jsonb),'{blank_card_stacks}',"
        "to_jsonb(LEAST(5,COALESCE((rn.state->>'blank_card_stacks')::INT,0)+1)),true)"
        "||jsonb_build_object('fate_noise_bonus',35,'fate_noise_until_round',$1,'tarot_restored',false) "
        "FROM npc_templates nt WHERE rn.room_id=$2 AND rn.npc_template_id=nt.id AND nt.name='彌夜'",
        round + 1, roomId);
  } else if (phase == 2) {
    skill = roll <= 50 ? "因果倒置" : "昨日之傷";
    effect["initiative_disorder"] = true;
    effect["reroll_corruption"] = true;
    effect["reaction_locked"] = true;
    text = "原因與結果交換了位置，目標下一次反應判斷受到因果錯位干擾。";
    if (target.type == "player") {
      service->database()->execSqlSync(
          "UPDATE room_members SET combat_reaction=CASE WHEN combat_reaction='defense' THEN 'dodge' ELSE 'defense' END "
          "WHERE room_id=$1 AND user_id=$2", roomId, target.id);
    }
  } else if (phase == 3) {
    skill = roll <= 55 ? "吞食未來" : "抹去牌義";
    effect["miyoi_silence_rounds"] = 1;
    effect["fate_effect_suppressed"] = true;
    text = "一段尚未發生的未來被吞掉，彌夜的塔羅支援暫時遭到封鎖。";
    service->database()->execSqlSync(
        "UPDATE room_npcs rn SET state=COALESCE(rn.state,'{}'::jsonb)||jsonb_build_object("
        "'miyoi_silenced_until_round',$1,'fate_effect_suppressed',true,'blank_card_stacks',3,'tarot_restored',false) "
        "FROM npc_templates nt WHERE rn.room_id=$2 AND rn.npc_template_id=nt.id AND nt.name='彌夜'",
        round + 1, roomId);
  } else {
    skill = roll <= 50 ? "逆寫終局" : "無牌世界";
    effect["major_rule_flip"] = true;
    effect["all_fate_unstable"] = true;
    effect["bonus_damage_percent"] = 20;
    text = "它直接反寫了牌義，正位與逆位失去界線，攻擊也被扭曲的終局再次放大。";
    // 最終階段追加 20% 基礎傷害；不重跑命中，只在已命中的攻擊上追加。
    if (combat.get("hit", false).asBool() && target.type == "player") {
      const auto baseDamage = std::max<std::int64_t>(1, integerValue(combat["damage"], 1));
      const auto extra = std::max<std::int64_t>(1, baseDamage / 5);
      auto refreshed = loadCombatTarget(service, roomId, target.type, target.id);
      const auto extraChange = applyCombatDamage(service, roomId, refreshed, "hp", extra);
      effect["extra_damage"] = Json::Int64(extra);
      effect["extra_damage_change"] = extraChange;
      damageAfter = integerValue(extraChange["after"], damageAfter);
    }
    service->database()->execSqlSync(
        "UPDATE room_npcs rn SET state=COALESCE(rn.state,'{}'::jsonb)||jsonb_build_object('major_rule_flip',true,'all_fate_unstable',true,'fate_flip_until_round',$1) "
        "FROM npc_templates nt WHERE rn.room_id=$2 AND rn.npc_template_id=nt.id AND nt.name='彌夜'",
        round + 1, roomId);
  }

  const auto stateRows = service->database()->execSqlSync(
      "SELECT COALESCE(state,'{}'::jsonb)::text AS state_text FROM room_monsters WHERE room_id=$1 AND id=$2",
      roomId, attacker.id);
  auto bossState = stateRows.empty() ? Json::Value{Json::objectValue}
                                     : parseJson(stateRows[0]["state_text"].as<std::string>(), Json::Value{Json::objectValue});
  const auto previousPhase = integerValue(bossState["anti_fate_phase"], 0);
  bossState["anti_fate_phase"] = phase;
  bossState["anti_fate_phase_name"] = phaseName;
  bossState["last_anti_fate_skill"] = skill;
  bossState["last_anti_fate_round"] = Json::Int64(round);
  bossState["fate_interference"] = effect;
  service->database()->execSqlSync(
      "UPDATE room_monsters SET state=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE room_id=$2 AND id=$3",
      compactJson(bossState), roomId, attacker.id);

  if (previousPhase != phase) {
    Json::Value transition;
    transition["monster_id"] = Json::Int64(attacker.id);
    transition["phase"] = phase;
    transition["phase_name"] = phaseName;
    transition["text"] = attacker.name + "進入【" + phaseName + "】。";
    service->addEvent(roomId, eventUserId, "anti_fate_phase", transition);
  }

  Json::Value payload;
  payload["monster_id"] = Json::Int64(attacker.id);
  payload["target_type"] = target.type;
  payload["target_id"] = Json::Int64(target.id);
  payload["skill"] = skill;
  payload["effect"] = effect;
  payload["text"] = attacker.name + "使用【" + skill + "】：" + text;
  service->addEvent(roomId, eventUserId, "anti_fate_boss", payload);

  // 普通攻擊的事件鏈仍然保留，讓規則引擎、事件技能與勝負判定正常工作。
  if (combat["event_payload"].isObject()) service->addEvent(roomId, eventUserId, "combat_attack", combat["event_payload"]);
  Json::Value combatRuleEvent = combat;
  combatRuleEvent["source"] = "anti_fate_ai";
  combatRuleEvent["anti_fate_skill"] = skill;
  combatRuleEvent["anti_fate_phase"] = phase;
  try { combat["event_skill_result"] = dispatchEventSkillEvent(service, roomId, attacker.type, attacker.id, "COMBAT_ATTACK_RESOLVED", combatRuleEvent); }
  catch (const std::exception& error) { LOG_WARN << "anti-fate event skill degraded: " << error.what(); }
  evaluateRuleEvent(service, roomId, eventUserId, "COMBAT_ATTACK_RESOLVED", combatRuleEvent, false);
  if (damageAfter <= 0 || (combat["damage_change"].isObject() && combat["damage_change"].get("downed", false).asBool())) {
    try { dispatchEventSkillEvent(service, roomId, attacker.type, attacker.id, "COMBAT_ENTITY_DEFEATED", combatRuleEvent); }
    catch (const std::exception& error) { LOG_WARN << "anti-fate defeat event degraded: " << error.what(); }
    evaluateRuleEvent(service, roomId, eventUserId, "COMBAT_ENTITY_DEFEATED", combatRuleEvent, false);
  }
  evaluateRuleCombatVictory(service, roomId, eventUserId, "COMBAT_ATTACK_RESOLVED");
  combat.removeMember("event_payload");
  advanceCombatAction(service, roomId, attacker.type, attacker.id, eventUserId, "逆命 Boss AI：「" + skill + "」");

  Json::Value out;
  out["combat"] = combat;
  out["skill_name"] = skill;
  out["effect"] = effect;
  out["phase"] = phase;
  out["phase_name"] = phaseName;
  out["text"] = text;
  return out;
}

std::vector<CombatTarget> combatTargetsAgainst(const std::shared_ptr<CoreService>& service,
                                                std::int64_t roomId,
                                                const std::string& side) {
  std::vector<CombatTarget> targets;
  if (side != "ally") {
    const auto players = service->database()->execSqlSync(
        "SELECT user_id FROM room_members WHERE room_id=$1 AND role='player' AND hp>0 "
        "AND COALESCE(combat_state,'active')<>'escaped'",
        roomId);
    for (const auto& row : players) {
      targets.push_back(loadCombatTarget(service, roomId, "player",
                                         row["user_id"].as<std::int64_t>()));
    }
    const auto npcs = service->database()->execSqlSync(
        "SELECT npc_template_id FROM room_npcs WHERE room_id=$1 AND status='active' "
        "AND current_hp>0 AND combat_side='ally'",
        roomId);
    for (const auto& row : npcs) {
      targets.push_back(loadCombatTarget(service, roomId, "npc",
                                         row["npc_template_id"].as<std::int64_t>()));
    }
  } else {
    const auto monsters = service->database()->execSqlSync(
        "SELECT id FROM room_monsters WHERE room_id=$1 AND status='encountered' AND current_hp>0",
        roomId);
    for (const auto& row : monsters) {
      targets.push_back(loadCombatTarget(service, roomId, "monster",
                                         row["id"].as<std::int64_t>()));
    }
    const auto npcs = service->database()->execSqlSync(
        "SELECT npc_template_id FROM room_npcs WHERE room_id=$1 AND status='active' "
        "AND current_hp>0 AND combat_side='enemy'",
        roomId);
    for (const auto& row : npcs) {
      targets.push_back(loadCombatTarget(service, roomId, "npc",
                                         row["npc_template_id"].as<std::int64_t>()));
    }
  }
  return targets;
}

// [功能備註｜60-cpp.21.3 開團安全快照]
// 建立房間／開始跑團屬於核心操作，不應因怪物、NPC、世界系統等後期 schema 漂移而回傳 500。
// 先嘗試完整 roomSnapshot；若失敗則只用 rooms / room_members / events 的核心欄位組出可進房的降級快照。
Json::Value resilientRoomSnapshot(const std::shared_ptr<CoreService>& service,
                                  std::int64_t roomId) {
  try {
    return service->roomSnapshot(roomId);
  } catch (const std::exception& error) {
    LOG_WARN << "full room snapshot degraded for room " << roomId << ": " << error.what();
  }

  const auto db = service->database();
  const auto roomRows = db->execSqlSync(R"SQL(
    SELECT jsonb_build_object(
      'id',r.id,'code',r.code,'name',r.name,'owner_id',r.owner_id,
      'status',COALESCE(to_jsonb(r)->>'status','lobby'),
      'round',COALESCE((to_jsonb(r)->>'round')::bigint,1),
      'current_actor_id',to_jsonb(r)->'current_actor_id',
      'saved',COALESCE((to_jsonb(r)->>'saved')::boolean,FALSE),
      'created_at',r.created_at,
      'updated_at',COALESCE(to_jsonb(r)->'updated_at',to_jsonb(r)->'created_at'),
      'closed_at',to_jsonb(r)->'closed_at',
      'battle_active',COALESCE((to_jsonb(r)->>'battle_active')::boolean,FALSE),
      'current_actor_type',COALESCE(to_jsonb(r)->>'current_actor_type',''),
      'current_actor_ref_id',to_jsonb(r)->'current_actor_ref_id',
      'combat_order',COALESCE(to_jsonb(r)->'combat_order','[]'::jsonb),
      'combat_index',COALESCE((to_jsonb(r)->>'combat_index')::bigint,0),
      'map_name',COALESCE(to_jsonb(r)->>'map_name','未設定地點'),
      'map_image_url',COALESCE(to_jsonb(r)->>'map_image_url',''),
      'map_description',COALESCE(to_jsonb(r)->>'map_description','')
    )::text AS json_text
    FROM rooms r WHERE r.id=$1
  )SQL", roomId);
  if (roomRows.empty()) throw ApiError(drogon::k404NotFound, "找不到房間");

  Json::Value snapshot;
  snapshot["room"] = parseJson(roomRows[0]["json_text"].as<std::string>());
  snapshot["members"] = Json::arrayValue;
  snapshot["events"] = Json::arrayValue;
  snapshot["summons"] = Json::arrayValue;
  snapshot["monsters"] = Json::arrayValue;
  snapshot["npcs"] = Json::arrayValue;
  snapshot["ritual_instances"] = Json::arrayValue;

  const auto members = db->execSqlSync(R"SQL(
    SELECT jsonb_build_object(
      'id',rm.user_id,'user_id',rm.user_id,'display_name',rm.display_name,'role',rm.role,
      'hp',rm.hp,'max_hp',rm.max_hp,'spirit',rm.spirit,'max_spirit',rm.max_spirit,
      'current_sanity',COALESCE((to_jsonb(rm)->>'current_sanity')::bigint,0),
      'max_sanity',COALESCE((to_jsonb(rm)->>'max_sanity')::bigint,0),
      'turn_actions_remaining',COALESCE((to_jsonb(rm)->>'turn_actions_remaining')::bigint,0),
      'combat_state',COALESCE(to_jsonb(rm)->>'combat_state','active'),
      'combat_reaction',COALESCE(to_jsonb(rm)->>'combat_reaction','dodge'),
      'joined_at',rm.joined_at,'username',u.username,'is_admin',u.is_admin
    )::text AS json_text
    FROM room_members rm JOIN users u ON u.id=rm.user_id
    WHERE rm.room_id=$1 ORDER BY rm.joined_at
  )SQL", roomId);
  for (const auto& row : members) {
    snapshot["members"].append(parseJson(row["json_text"].as<std::string>()));
  }

  try {
    const auto events = db->execSqlSync(R"SQL(
      SELECT jsonb_build_object(
        'id',e.id,'user_id',e.user_id,'type',e.type,'payload',e.payload,
        'created_at',e.created_at,'username',u.username
      )::text AS json_text
      FROM (SELECT * FROM events WHERE room_id=$1 ORDER BY id DESC LIMIT 100) e
      LEFT JOIN users u ON u.id=e.user_id ORDER BY e.id ASC
    )SQL", roomId);
    for (const auto& row : events) {
      snapshot["events"].append(parseJson(row["json_text"].as<std::string>()));
    }
  } catch (const std::exception& error) {
    LOG_WARN << "safe room event snapshot degraded for room " << roomId << ": " << error.what();
  }
  snapshot["degraded_snapshot"] = true;
  return snapshot;
}

bool userCanManageRoom(const std::shared_ptr<CoreService>& service,
                       const JwtClaims& user,
                       std::int64_t roomId) {
  if (user.isAdmin) return true;
  const auto rows = service->database()->execSqlSync(
      "SELECT 1 FROM rooms r LEFT JOIN room_members rm ON rm.room_id=r.id AND rm.user_id=$2 "
      "WHERE r.id=$1 AND (r.owner_id=$2 OR rm.role='gm') LIMIT 1",
      roomId, user.id);
  return !rows.empty();
}

}  // namespace


// [功能備註｜62-cpp.23 多段攻擊]
// 統一執行玩家／怪物／NPC 的資料驅動技能。每一段 Hit 都獨立命中判定與傷害，
// 但整個技能只在最後依 turn_cost 消耗一次回合數；procState 會跨所有 Hit／目標共用。
Json::Value executeConfiguredCombatSkill(const std::shared_ptr<CoreService>& service,
                                         std::int64_t roomId,
                                         const std::string& actorType,
                                         std::int64_t actorId,
                                         const Json::Value& skill,
                                         const std::string& targetType,
                                         std::int64_t targetId,
                                         std::int64_t eventUserId) {
  if (!service || roomId <= 0 || actorType.empty() || actorId <= 0 || !skill.isObject()) {
    throw std::runtime_error("技能執行參數不完整");
  }
  auto state = currentCombatState(service, roomId);
  if (!state.exists) throw ApiError(drogon::k404NotFound, "找不到房間");
  if (!state.active) throw ApiError(drogon::k400BadRequest, "目前不是戰鬥階段");
  if (state.actor.isNull() || stringValue(state.actor["type"]) != actorType || integerValue(state.actor["id"]) != actorId) {
    throw ApiError(drogon::k403Forbidden, "現在不是這個單位的行動");
  }
  if (eventUserId <= 0 && actorType == "player") eventUserId = actorId;

  const auto data = skill["data"].isObject() ? skill["data"] : Json::Value{Json::objectValue};
  const auto mode = stringValue(data["combat_target_mode"], "enemy_single");
  const bool combatAttack = data.get("combat_attack", false).asBool() ||
                            !stringValue(skill["damage_formula"]).empty() ||
                            data["hit_sequence"].isArray() || integerValue(data["hit_count"]) > 0;

  static thread_local std::uint64_t sequenceCounter = 0;
  const auto sequenceId = "room" + std::to_string(roomId) + ":" + actorType + ":" +
                          std::to_string(actorId) + ":" + std::to_string(++sequenceCounter);
  Json::Value procState{Json::objectValue};
  Json::Value castContext{Json::objectValue};
  castContext["sequence_id"] = sequenceId;
  castContext["event_chain_id"] = sequenceId;
  castContext["skill"] = skill;
  castContext["actor"]["type"] = actorType;
  castContext["actor"]["id"] = Json::Int64(actorId);
  castContext["attacker"] = castContext["actor"];
  if (!targetType.empty() && targetId > 0) {
    castContext["target"]["type"] = targetType;
    castContext["target"]["id"] = Json::Int64(targetId);
  }

  Json::Value result;
  result["skill"] = skill;
  result["sequence_id"] = sequenceId;
  result["hits"] = Json::arrayValue;
  result["total_damage"] = Json::Int64(0);
  result["hit"] = false;
  result["damage"] = Json::Int64(0);  // 舊前端相容：多段總傷害也放在 damage。
  result["event_skill_cast"] = dispatchEventSkillEvent(service, roomId, actorType, actorId,
                                                        "COMBAT_SKILL_USED", castContext, &procState);

  if (!combatAttack) {
    Json::Value payload;
    payload["text"] = stringValue(skill["name"], "技能") + " 已發動";
    payload["skill"] = skill;
    payload["sequence_id"] = sequenceId;
    service->addEvent(roomId, eventUserId, "combat_skill", payload);
    if (integerValue(data["turn_cost"], 1) != 0) {
      advanceCombatAction(service, roomId, actorType, actorId, eventUserId,
                          stringValue(skill["name"], "技能"));
    }
    result["room"] = service->roomSnapshot(roomId);
    return result;
  }

  std::vector<Json::Value> hitSpecs;
  if (data["hit_sequence"].isArray() && !data["hit_sequence"].empty()) {
    for (const auto& raw : data["hit_sequence"]) {
      if (!raw.isObject()) continue;
      const auto repeat = std::clamp<std::int64_t>(integerValue(raw["repeat"], 1), 1, 20);
      for (std::int64_t n = 0; n < repeat && hitSpecs.size() < 20; ++n) hitSpecs.push_back(raw);
      if (hitSpecs.size() >= 20) break;
    }
  }
  if (hitSpecs.empty()) {
    const auto hitCount = std::clamp<std::int64_t>(integerValue(data["hit_count"], 1), 1, 20);
    for (std::int64_t i = 0; i < hitCount; ++i) hitSpecs.emplace_back(Json::objectValue);
  }

  std::vector<std::pair<std::string, std::int64_t>> targetRefs;
  if (mode == "enemy_all") {
    const auto side = combatActorSide(state.actor);
    for (const auto& target : combatTargetsAgainst(service, roomId, side)) {
      targetRefs.emplace_back(target.type, target.id);
    }
    if (targetRefs.empty()) throw ApiError(drogon::k400BadRequest, "目前沒有可攻擊的敵方目標");
  } else {
    if (targetType.empty() || targetId <= 0) throw ApiError(drogon::k400BadRequest, "請選擇技能目標");
    targetRefs.emplace_back(targetType, targetId);
  }

  castContext["hit_count"] = Json::Int64(static_cast<std::int64_t>(hitSpecs.size()));
  result["event_skill_sequence_start"] = dispatchEventSkillEvent(
      service, roomId, actorType, actorId, "COMBAT_HIT_SEQUENCE_STARTED", castContext, &procState);

  std::int64_t totalDamage = 0;
  std::int64_t resolvedHits = 0;
  bool anyHit = false;
  bool anyDefeat = false;
  Json::Value firstTarget{Json::nullValue};

  for (const auto& [oneTargetType, oneTargetId] : targetRefs) {
    for (std::size_t index = 0; index < hitSpecs.size(); ++index) {
      CombatTarget target;
      try {
        // 每段都重新載入目前 HP／狀態，確保三段傷害真的累加，而不是用第一段前的舊快照。
        target = loadCombatTarget(service, roomId, oneTargetType, oneTargetId);
      } catch (const ApiError&) {
        break;  // 前一段已擊敗該目標時，後續 Hit 不再攻擊屍體／倒地單位。
      }
      if (firstTarget.isNull()) {
        firstTarget["type"] = target.type; firstTarget["id"] = Json::Int64(target.id); firstTarget["name"] = target.name;
      }
      const auto& hit = hitSpecs[index];
      Json::Value action{Json::objectValue};
      action["name"] = stringValue(skill["name"], "技能") + (hitSpecs.size() > 1 ? " 第" + std::to_string(index + 1) + "段" : "");
      action["attack_attribute"] = stringValue(hit["attack_attribute"],
                                                 stringValue(data["combat_check_attribute"], "spirit"));
      action["damage_formula"] = stringValue(hit["damage_formula"], stringValue(skill["damage_formula"], "1D力量"));
      action["damage_target"] = stringValue(hit["damage_target"], stringValue(data["combat_damage_target"], "hp"));
      action["accuracy_bonus"] = Json::Int64(integerValue(hit["accuracy_bonus"], integerValue(data["combat_accuracy_bonus"])));
      const auto hitDamageMultiplier = hit.isMember("damage_multiplier") ? hit["damage_multiplier"].asDouble() : 1.0;
      // [功能備註｜65-cpp.26.14 可成長性裝備] 裝備授予技能的成長增傷由 C++ 套用，前端不可自行偽造最終傷害。
      const auto gearDamageMultiplier = std::clamp(data.get("gear_damage_multiplier", 1.0).asDouble(), 0.0, 1000.0);
      action["damage_multiplier"] = std::clamp(hitDamageMultiplier * gearDamageMultiplier, 0.0, 1000.0);
      action["damage_percent"] = Json::Int64(integerValue(hit["damage_percent"], 100));
      action["element"] = stringValue(hit["element"], stringValue(data["element"]));

      auto combat = resolveBasicCombatAttack(service, roomId, actorType, actorId, target, action);
      ++resolvedHits;
      anyHit = anyHit || combat.get("hit", false).asBool();
      totalDamage += integerValue(combat["damage"]);

      Json::Value hitContext = combat;
      hitContext["skill"] = skill;
      hitContext["sequence_id"] = sequenceId;
      hitContext["event_chain_id"] = sequenceId;
      hitContext["hit_index"] = Json::Int64(static_cast<std::int64_t>(index + 1));
      hitContext["hit_count"] = Json::Int64(static_cast<std::int64_t>(hitSpecs.size()));
      hitContext["is_final_hit"] = index + 1 == hitSpecs.size();
      combat["event_skill_result"] = dispatchEventSkillEvent(service, roomId, actorType, actorId,
                                                              "COMBAT_ATTACK_RESOLVED", hitContext, &procState);
      evaluateRuleEvent(service, roomId, eventUserId, "COMBAT_ATTACK_RESOLVED", hitContext, false);
      if (combat["damage_change"].isObject() && combat["damage_change"].get("downed", false).asBool()) {
        anyDefeat = true;
        Json::Value deathContext = hitContext;
        deathContext["defeated"] = hitContext["target"];
        combat["event_skill_defeat"] = dispatchEventSkillEvent(service, roomId, actorType, actorId,
                                                                "COMBAT_ENTITY_DEFEATED", deathContext, &procState);
        evaluateRuleEvent(service, roomId, eventUserId, "COMBAT_ENTITY_DEFEATED", deathContext, false);
      }
      combat.removeMember("event_payload");
      result["hits"].append(std::move(combat));
      if (anyDefeat && mode != "enemy_all") break;
    }
  }

  Json::Value endContext = castContext;
  endContext["total_damage"] = Json::Int64(totalDamage);
  endContext["resolved_hits"] = Json::Int64(resolvedHits);
  endContext["any_hit"] = anyHit;
  result["event_skill_sequence_end"] = dispatchEventSkillEvent(
      service, roomId, actorType, actorId, "COMBAT_HIT_SEQUENCE_ENDED", endContext, &procState);

  result["target"] = firstTarget;
  result["hit"] = anyHit;
  result["damage"] = Json::Int64(totalDamage);
  result["total_damage"] = Json::Int64(totalDamage);
  result["resolved_hits"] = Json::Int64(resolvedHits);
  result["configured_hits"] = Json::Int64(static_cast<std::int64_t>(hitSpecs.size()));
  result["defeated"] = anyDefeat;

  Json::Value payload;
  payload["text"] = stringValue(skill["name"], "技能") + " 完成 " + std::to_string(resolvedHits) +
                    " 段攻擊，共造成 " + std::to_string(totalDamage) + " 傷害";
  payload["skill_name"] = stringValue(skill["name"], "技能");
  payload["sequence_id"] = sequenceId;
  payload["resolved_hits"] = Json::Int64(resolvedHits);
  payload["total_damage"] = Json::Int64(totalDamage);
  service->addEvent(roomId, eventUserId, "combat_skill", payload);

  evaluateRuleCombatVictory(service, roomId, eventUserId, "COMBAT_SKILL_USED");

  // [功能備註｜62-cpp.23 技能行動／大道資源]
  // 怪物既有 resource_cost / action_cost 仍保留；多段攻擊不會把每一段誤算成一個完整行動。
  const auto resourceCost = std::max<std::int64_t>(0, integerValue(skill["resource_cost"]));
  if (resourceCost > 0 && actorType == "monster") {
    service->database()->execSqlSync(
        "UPDATE room_monsters SET current_fifth=GREATEST(0,current_fifth-$1),updated_at=CURRENT_TIMESTAMP WHERE room_id=$2 AND id=$3",
        resourceCost, roomId, actorId);
    result["resource_cost"] = Json::Int64(resourceCost);
  }
  const auto actionCost = std::clamp<std::int64_t>(integerValue(skill["action_cost"], 1), 1, 5);
  if (actionCost > 1) adjustCombatActions(service, roomId, actorType, actorId, -(actionCost - 1));
  if (integerValue(data["turn_cost"], 1) != 0) {
    advanceCombatAction(service, roomId, actorType, actorId, eventUserId,
                        stringValue(skill["name"], "技能"));
  }
  result["action_cost"] = Json::Int64(actionCost);
  return result;
}


// [功能備註｜規則－戰鬥橋接] 讓世界規則直接啟動戰鬥；沿用原本行動序、HP/精神初始化與通知規則。
Json::Value startBattleFromSystem(const std::shared_ptr<CoreService>& service,
                                  std::int64_t roomId,
                                  std::int64_t actorUserId,
                                  const std::string& reason,
                                  const Json::Value& sourceContext) {
  const auto rooms = service->database()->execSqlSync(
      "SELECT status,battle_active FROM rooms WHERE id=$1", roomId);
  if (rooms.empty()) throw std::runtime_error("找不到房間");
  if (rooms[0]["status"].as<std::string>() != "active") {
    throw std::runtime_error("請先開始跑團");
  }
  if (rooms[0]["battle_active"].as<bool>()) return service->roomSnapshot(roomId);

  service->database()->execSqlSync(R"SQL(
    UPDATE room_members rm SET
      max_hp=GREATEST(1,GREATEST(cc.constitution,0)*2),
      hp=LEAST(rm.hp,GREATEST(1,GREATEST(cc.constitution,0)*2)),
      max_spirit=GREATEST(0,cc.spirit),
      spirit=CASE WHEN rm.max_spirit<=0 THEN GREATEST(0,cc.spirit) ELSE LEAST(rm.spirit,GREATEST(0,cc.spirit)) END,
      max_sanity=GREATEST(1,(GREATEST(cc.spirit,0)*12)/10),
      current_sanity=CASE WHEN rm.max_sanity<=1 THEN GREATEST(1,(GREATEST(cc.spirit,0)*12)/10) ELSE LEAST(rm.current_sanity,GREATEST(1,(GREATEST(cc.spirit,0)*12)/10)) END,
      combat_state=CASE WHEN rm.hp>0 THEN 'active' ELSE 'downed' END,
      skill_charge_state='{}'::jsonb
    FROM character_cards cc
    WHERE rm.room_id=$1 AND rm.role='player' AND cc.user_id=rm.user_id
  )SQL", roomId);

  const auto order = buildCombatOrder(service, roomId);
  bool ally = false;
  bool enemy = false;
  for (const auto& actor : order) {
    const auto side = combatActorSide(actor);
    ally = ally || side == "ally";
    enemy = enemy || side == "enemy";
  }
  if (!ally || !enemy) throw std::runtime_error("規則啟動戰鬥時缺少可戰鬥的友方或敵方");

  resetCombatActions(service, roomId, order);
  const auto first = order[0];
  setCombatActor(service, roomId, first, 0, 1);
  service->database()->execSqlSync(
      "UPDATE rooms SET battle_active=TRUE,round=1,combat_order=$1::jsonb,combat_index=0,updated_at=CURRENT_TIMESTAMP WHERE id=$2",
      compactJson(order), roomId);
  Json::Value event = sourceContext;
  event["text"] = "⚔️ " + reason + "。由 " + stringValue(first["name"], "單位") + " 先行動。";
  event["reason"] = reason;
  event["first_actor"] = first;
  service->addEvent(roomId, actorUserId, "rule_combat_start", event);
  evaluateRuleEvent(service, roomId, actorUserId, "COMBAT_STARTED", event, false);
  // [功能備註｜62-cpp.23.1 戰鬥開始自動技能]
  // AUTO_COMBAT_START 會在這裡建立墨界等場地效果；不需要玩家手動施放。
  try { event["source"] = "system"; event["first_actor"] = first; dispatchEventSkillEvent(service, roomId, "system", 0, "COMBAT_STARTED", event); }
  catch (const std::exception& error) { LOG_WARN << "combat start event skill degraded: " << error.what(); }
  if (stringValue(first["type"]) == "player") {
    Json::Value data; data["room_id"] = Json::Int64(roomId); data["round"] = 1;
    service->database()->execSqlSync(
        "INSERT INTO user_notifications(user_id,notification_type,title,message,data) VALUES($1,'turn','輪到你行動',$2,$3::jsonb)",
        integerValue(first["id"]), "戰鬥已開始，現在輪到你行動。", compactJson(data));
  }
  RoomSocket::broadcastSnapshotForRoom(roomId);
  return service->roomSnapshot(roomId);
}

// [功能備註｜規則－戰鬥橋接] 系統／規則效果結束戰鬥時會反向送 COMBAT_ENDED 給規則引擎。
Json::Value endBattleFromSystem(const std::shared_ptr<CoreService>& service,
                                std::int64_t roomId,
                                std::int64_t actorUserId,
                                const std::string& reason,
                                const Json::Value& sourceContext) {
  const auto rooms = service->database()->execSqlSync(
      "SELECT battle_active FROM rooms WHERE id=$1", roomId);
  if (rooms.empty()) throw std::runtime_error("找不到房間");
  if (!rooms[0]["battle_active"].as<bool>()) return service->roomSnapshot(roomId);
  finishBattle(service, roomId, actorUserId, "🕯️ " + reason);
  Json::Value event = sourceContext;
  event["reason"] = reason;
  evaluateRuleEvent(service, roomId, actorUserId, "COMBAT_ENDED", event, false);
  RoomSocket::broadcastSnapshotForRoom(roomId);
  return service->roomSnapshot(roomId);
}

CoreService::CoreService(Config config)
    : config_(std::move(config)),
      database_(drogon::orm::DbClient::newPgClient(config_.databaseUrl,
                                                   config_.databaseConnections)) {}

void CoreService::setInstance(const std::shared_ptr<CoreService>& service) {
  std::lock_guard lock(instanceMutex);
  serviceInstance = service;
}

std::shared_ptr<CoreService> CoreService::instance() {
  std::lock_guard lock(instanceMutex);
  return serviceInstance.lock();
}

void CoreService::initializeDatabase() {
  const std::vector<std::string> migrations = {
      R"SQL(CREATE TABLE IF NOT EXISTS users (
        id BIGSERIAL PRIMARY KEY,
        username VARCHAR(20) NOT NULL UNIQUE,
        password_hash TEXT NOT NULL,
        is_admin BOOLEAN NOT NULL DEFAULT FALSE,
        created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
      ))SQL",
      R"SQL(CREATE TABLE IF NOT EXISTS realtime_tickets (
        ticket VARCHAR(64) PRIMARY KEY,
        user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        expires_at TIMESTAMPTZ NOT NULL,
        created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
      ))SQL",
      "CREATE INDEX IF NOT EXISTS idx_realtime_tickets_expires ON realtime_tickets(expires_at)",
      R"SQL(CREATE TABLE IF NOT EXISTS rooms (
        id BIGSERIAL PRIMARY KEY,
        code VARCHAR(6) NOT NULL UNIQUE,
        name VARCHAR(60) NOT NULL,
        owner_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        status VARCHAR(20) NOT NULL DEFAULT 'lobby',
        round BIGINT NOT NULL DEFAULT 1,
        current_actor_id BIGINT,
        saved BOOLEAN NOT NULL DEFAULT FALSE,
        created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
        closed_at TIMESTAMPTZ
      ))SQL",
      R"SQL(CREATE TABLE IF NOT EXISTS room_members (
        room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
        user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        role VARCHAR(10) NOT NULL DEFAULT 'player',
        display_name VARCHAR(30) NOT NULL,
        hp BIGINT NOT NULL DEFAULT 100,
        max_hp BIGINT NOT NULL DEFAULT 100,
        spirit BIGINT NOT NULL DEFAULT 100,
        max_spirit BIGINT NOT NULL DEFAULT 100,
        current_sanity BIGINT NOT NULL DEFAULT 100,
        max_sanity BIGINT NOT NULL DEFAULT 100,
        sanity_initialized BOOLEAN NOT NULL DEFAULT TRUE,
        sanity_stage VARCHAR(40) NOT NULL DEFAULT 'stable',
        seven_sin VARCHAR(40),
        class_resources JSONB NOT NULL DEFAULT '{}'::jsonb,
        skill_charge_state JSONB NOT NULL DEFAULT '{}'::jsonb,
        turn_actions_remaining BIGINT NOT NULL DEFAULT 0,
        combat_state VARCHAR(20) NOT NULL DEFAULT 'active',
        joined_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
        PRIMARY KEY(room_id,user_id)
      ))SQL",
      R"SQL(CREATE TABLE IF NOT EXISTS events (
        id BIGSERIAL PRIMARY KEY,
        room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
        user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,
        type VARCHAR(30) NOT NULL,
        payload JSONB NOT NULL,
        created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
      ))SQL",
      R"SQL(CREATE TABLE IF NOT EXISTS character_cards (
        id BIGSERIAL PRIMARY KEY,
        user_id BIGINT NOT NULL UNIQUE REFERENCES users(id) ON DELETE CASCADE,
        name VARCHAR(50) NOT NULL DEFAULT '未命名角色',
        name_locked BOOLEAN NOT NULL DEFAULT FALSE,
        gender VARCHAR(40) NOT NULL DEFAULT '',
        age BIGINT NOT NULL DEFAULT 0,
        height_cm BIGINT NOT NULL DEFAULT 0,
        birthday VARCHAR(40) NOT NULL DEFAULT '',
        self_intro TEXT NOT NULL DEFAULT '',
        faction VARCHAR(20) NOT NULL DEFAULT '東國',
        tier BIGINT NOT NULL DEFAULT 0,
        level BIGINT NOT NULL DEFAULT 1,
        main_class VARCHAR(50) NOT NULL DEFAULT '未設定',
        sub_classes JSONB NOT NULL DEFAULT '[]'::jsonb,
        agility BIGINT NOT NULL DEFAULT 0,
        strength BIGINT NOT NULL DEFAULT 0,
        constitution BIGINT NOT NULL DEFAULT 0,
        spirit BIGINT NOT NULL DEFAULT 0,
        great_way BIGINT NOT NULL DEFAULT 0,
        faith BIGINT NOT NULL DEFAULT 0,
        current_great_way BIGINT,
        current_faith BIGINT,
        great_way_name VARCHAR(100),
        luck BIGINT NOT NULL DEFAULT 10,
        attribute_points BIGINT NOT NULL DEFAULT 0,
        experience BIGINT NOT NULL DEFAULT 0,
        skill_points BIGINT NOT NULL DEFAULT 0,
        skills JSONB NOT NULL DEFAULT '[]'::jsonb,
        equipment JSONB NOT NULL DEFAULT '[]'::jsonb,
        weapons JSONB NOT NULL DEFAULT '[]'::jsonb,
        inventory JSONB NOT NULL DEFAULT '[]'::jsonb,
        gear_loadout JSONB NOT NULL DEFAULT '{"configured":false,"equipment":[],"weapons":[]}'::jsonb,
        class_resources JSONB NOT NULL DEFAULT '{}'::jsonb,
        weird_coins BIGINT NOT NULL DEFAULT 0,
        game_coins BIGINT NOT NULL DEFAULT 0,
        game_coin_unlocked BOOLEAN NOT NULL DEFAULT FALSE,
        pollution BIGINT NOT NULL DEFAULT 0,
        created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
      ))SQL",
      R"SQL(CREATE TABLE IF NOT EXISTS character_room_bindings (
        room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
        character_id BIGINT NOT NULL REFERENCES character_cards(id) ON DELETE CASCADE,
        user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
        PRIMARY KEY(room_id,user_id),
        UNIQUE(room_id,character_id)
      ))SQL",
      "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS gender VARCHAR(40) NOT NULL DEFAULT ''",
      "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS age BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS height_cm BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS birthday VARCHAR(40) NOT NULL DEFAULT ''",
      "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS self_intro TEXT NOT NULL DEFAULT ''",
      "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS name_locked BOOLEAN NOT NULL DEFAULT FALSE",
      "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS faith BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS current_great_way BIGINT",
      "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS current_faith BIGINT",
      "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS gear_loadout JSONB NOT NULL DEFAULT '{\"configured\":false,\"equipment\":[],\"weapons\":[]}'::jsonb",
      "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS inventory JSONB NOT NULL DEFAULT '[]'::jsonb",
      "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS weird_coins BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS game_coins BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS game_coin_unlocked BOOLEAN NOT NULL DEFAULT FALSE",
      "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS pollution BIGINT NOT NULL DEFAULT 0",
      // [功能備註｜64-cpp.25 角色正式等階 migration] 新角色從 0 階開始；既有角色保留目前階級並把小級限制在 1～4。
      "ALTER TABLE character_cards ALTER COLUMN tier SET DEFAULT 0",
      "ALTER TABLE character_cards ALTER COLUMN level SET DEFAULT 1",
      "ALTER TABLE character_cards ALTER COLUMN attribute_points SET DEFAULT 0",
      "UPDATE character_cards SET tier=LEAST(10,GREATEST(0,tier)),level=LEAST(4,GREATEST(1,level)) WHERE tier<0 OR tier>10 OR level<1 OR level>4",
      R"SQL(UPDATE character_cards SET attribute_points=GREATEST(GREATEST(0,attribute_points),
        (CASE LEAST(10,GREATEST(0,tier)) WHEN 0 THEN 0 WHEN 1 THEN 50 WHEN 2 THEN 100 WHEN 3 THEN 180 WHEN 4 THEN 320 WHEN 5 THEN 600 WHEN 6 THEN 1100 WHEN 7 THEN 2000 WHEN 8 THEN 3600 WHEN 9 THEN 6500 ELSE 10000 END)
        - GREATEST(0,agility)-GREATEST(0,strength)-GREATEST(0,constitution)-GREATEST(0,spirit)
        - CASE WHEN faction='西國' THEN GREATEST(0,faith) ELSE GREATEST(0,great_way) END)
      WHERE GREATEST(0,agility)+GREATEST(0,strength)+GREATEST(0,constitution)+GREATEST(0,spirit)
        + CASE WHEN faction='西國' THEN GREATEST(0,faith) ELSE GREATEST(0,great_way) END
        <= CASE LEAST(10,GREATEST(0,tier)) WHEN 0 THEN 0 WHEN 1 THEN 50 WHEN 2 THEN 100 WHEN 3 THEN 180 WHEN 4 THEN 320 WHEN 5 THEN 600 WHEN 6 THEN 1100 WHEN 7 THEN 2000 WHEN 8 THEN 3600 WHEN 9 THEN 6500 ELSE 10000 END)SQL",
      "ALTER TABLE room_members ADD COLUMN IF NOT EXISTS current_sanity BIGINT NOT NULL DEFAULT 100",
      "ALTER TABLE room_members ADD COLUMN IF NOT EXISTS max_sanity BIGINT NOT NULL DEFAULT 100",
      "ALTER TABLE room_members ADD COLUMN IF NOT EXISTS sanity_initialized BOOLEAN NOT NULL DEFAULT TRUE",
      "ALTER TABLE room_members ADD COLUMN IF NOT EXISTS sanity_stage VARCHAR(40) NOT NULL DEFAULT 'stable'",
      "ALTER TABLE room_members ADD COLUMN IF NOT EXISTS seven_sin VARCHAR(40)",
      "ALTER TABLE room_members ADD COLUMN IF NOT EXISTS class_resources JSONB NOT NULL DEFAULT '{}'::jsonb",
      "ALTER TABLE room_members ADD COLUMN IF NOT EXISTS skill_charge_state JSONB NOT NULL DEFAULT '{}'::jsonb",
      "ALTER TABLE room_members ADD COLUMN IF NOT EXISTS turn_actions_remaining BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE room_members ADD COLUMN IF NOT EXISTS combat_state VARCHAR(20) NOT NULL DEFAULT 'active'",
      "ALTER TABLE room_members ADD COLUMN IF NOT EXISTS combat_reaction VARCHAR(20) NOT NULL DEFAULT 'dodge'",
      "ALTER TABLE room_members ADD COLUMN IF NOT EXISTS current_map_node_id BIGINT",
      "ALTER TABLE room_members ADD COLUMN IF NOT EXISTS offline_ai_enabled BOOLEAN NOT NULL DEFAULT TRUE",
      "ALTER TABLE room_members ADD COLUMN IF NOT EXISTS offline_ai_active BOOLEAN NOT NULL DEFAULT FALSE",
      "ALTER TABLE room_members ADD COLUMN IF NOT EXISTS last_seen_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP",
      "ALTER TABLE room_members ADD COLUMN IF NOT EXISTS ai_last_action_at TIMESTAMPTZ",
      R"SQL(UPDATE room_members rm SET
        max_hp=CASE WHEN rm.max_hp<=1 AND cc.constitution>0 THEN cc.constitution*2 ELSE rm.max_hp END,
        hp=CASE WHEN rm.max_hp<=1 AND cc.constitution>0 THEN cc.constitution*2 ELSE LEAST(rm.hp,GREATEST(0,cc.constitution*2)) END,
        max_spirit=CASE WHEN rm.max_spirit<=0 AND cc.spirit>0 THEN cc.spirit ELSE rm.max_spirit END,
        spirit=CASE WHEN rm.max_spirit<=0 AND cc.spirit>0 THEN cc.spirit ELSE LEAST(rm.spirit,GREATEST(0,cc.spirit)) END,
        max_sanity=CASE WHEN rm.max_sanity<=1 AND cc.spirit>0 THEN GREATEST(1,(cc.spirit*12)/10) ELSE rm.max_sanity END,
        current_sanity=CASE WHEN rm.max_sanity<=1 AND cc.spirit>0 THEN GREATEST(1,(cc.spirit*12)/10) ELSE LEAST(rm.current_sanity,GREATEST(1,(cc.spirit*12)/10)) END
      FROM character_cards cc
      WHERE cc.user_id=rm.user_id AND (rm.max_hp<=1 OR rm.max_spirit<=0 OR rm.max_sanity<=1))SQL",
      "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS avatar_url TEXT NOT NULL DEFAULT ''",
      // [功能備註｜65-cpp.26.2 角色立繪] 與頭像分離，外部網址放 character_cards，直接上傳圖片放 portrait_images。
      "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS portrait_url TEXT NOT NULL DEFAULT ''",
      "CREATE TABLE IF NOT EXISTS portrait_images (user_id BIGINT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,mime_type VARCHAR(32) NOT NULL,image_base64 TEXT NOT NULL,size_bytes BIGINT NOT NULL DEFAULT 0,updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP)",
      "CREATE INDEX IF NOT EXISTS idx_portrait_images_updated_at ON portrait_images(updated_at)",
      // [功能備註｜65-cpp.26.4 多立繪] 玩家與 NPC 可保存多張立繪；active 立繪同步回既有 portrait_url，舊介面保持相容。
      R"SQL(CREATE TABLE IF NOT EXISTS character_portrait_variants (
        id BIGSERIAL PRIMARY KEY,
        user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        label VARCHAR(80) NOT NULL DEFAULT '立繪',
        external_url TEXT NOT NULL DEFAULT '',
        mime_type VARCHAR(32) NOT NULL DEFAULT '',
        image_base64 TEXT NOT NULL DEFAULT '',
        size_bytes BIGINT NOT NULL DEFAULT 0,
        is_default BOOLEAN NOT NULL DEFAULT FALSE,
        is_active BOOLEAN NOT NULL DEFAULT FALSE,
        sort_order BIGINT NOT NULL DEFAULT 100,
        created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
      ))SQL",
      "CREATE INDEX IF NOT EXISTS idx_character_portrait_variants_user ON character_portrait_variants(user_id,sort_order,id)",
      "CREATE INDEX IF NOT EXISTS idx_character_portrait_variants_active ON character_portrait_variants(user_id,is_active)",
      R"SQL(INSERT INTO character_portrait_variants(user_id,label,external_url,mime_type,image_base64,size_bytes,is_default,is_active,sort_order)
        SELECT cc.user_id,'預設',CASE WHEN COALESCE(cc.portrait_url,'') LIKE '/api/portrait-images/%' THEN '' ELSE COALESCE(cc.portrait_url,'') END,
               COALESCE(pi.mime_type,''),COALESCE(pi.image_base64,''),COALESCE(pi.size_bytes,0),TRUE,TRUE,0
        FROM character_cards cc LEFT JOIN portrait_images pi ON pi.user_id=cc.user_id
        WHERE (COALESCE(cc.portrait_url,'')<>'' OR pi.user_id IS NOT NULL)
          AND NOT EXISTS(SELECT 1 FROM character_portrait_variants v WHERE v.user_id=cc.user_id))SQL",
      R"SQL(UPDATE character_cards cc SET portrait_url=CASE WHEN v.external_url<>'' THEN v.external_url ELSE '/api/character-portrait-variants/'||v.id::text||'/image' END
        FROM character_portrait_variants v WHERE v.user_id=cc.user_id AND v.is_active=TRUE)SQL",
      "ALTER TABLE rooms ADD COLUMN IF NOT EXISTS status VARCHAR(20) NOT NULL DEFAULT 'lobby'",
      "ALTER TABLE rooms ADD COLUMN IF NOT EXISTS round BIGINT NOT NULL DEFAULT 1",
      "ALTER TABLE rooms ADD COLUMN IF NOT EXISTS current_actor_id BIGINT",
      "ALTER TABLE rooms ADD COLUMN IF NOT EXISTS saved BOOLEAN NOT NULL DEFAULT FALSE",
      "ALTER TABLE rooms ADD COLUMN IF NOT EXISTS updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP",
      "ALTER TABLE rooms ADD COLUMN IF NOT EXISTS closed_at TIMESTAMPTZ",
      "ALTER TABLE rooms ADD COLUMN IF NOT EXISTS battle_active BOOLEAN NOT NULL DEFAULT FALSE",
      "ALTER TABLE rooms ADD COLUMN IF NOT EXISTS current_actor_type VARCHAR(20)",
      "ALTER TABLE rooms ADD COLUMN IF NOT EXISTS current_actor_ref_id BIGINT",
      "ALTER TABLE rooms ADD COLUMN IF NOT EXISTS combat_order JSONB NOT NULL DEFAULT '[]'::jsonb",
      "ALTER TABLE rooms ADD COLUMN IF NOT EXISTS combat_index BIGINT NOT NULL DEFAULT 0",
      // [功能備註｜61-cpp.22 平面地圖核心 migration] 地圖不再只依賴 legacy SQL；C++ 啟動即可補齊底圖與節點座標。
      "ALTER TABLE rooms ADD COLUMN IF NOT EXISTS map_name VARCHAR(120) NOT NULL DEFAULT '未設定地點'",
      "ALTER TABLE rooms ADD COLUMN IF NOT EXISTS map_image_url TEXT NOT NULL DEFAULT ''",
      "ALTER TABLE rooms ADD COLUMN IF NOT EXISTS map_description TEXT NOT NULL DEFAULT ''",
      "ALTER TABLE rooms ADD COLUMN IF NOT EXISTS current_map_node_id BIGINT",
      R"SQL(CREATE TABLE IF NOT EXISTS room_map_nodes (
        id BIGSERIAL PRIMARY KEY,
        room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
        name VARCHAR(120) NOT NULL,
        description TEXT NOT NULL DEFAULT '',
        image_url TEXT NOT NULL DEFAULT '',
        connections JSONB NOT NULL DEFAULT '[]'::jsonb,
        hidden BOOLEAN NOT NULL DEFAULT FALSE,
        x_percent NUMERIC(6,3) NOT NULL DEFAULT 50,
        y_percent NUMERIC(6,3) NOT NULL DEFAULT 50,
        node_type VARCHAR(40) NOT NULL DEFAULT 'location',
        icon VARCHAR(20) NOT NULL DEFAULT '●',
        created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
      ))SQL",
      "ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS x_percent NUMERIC(6,3) NOT NULL DEFAULT 50",
      "ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS y_percent NUMERIC(6,3) NOT NULL DEFAULT 50",
      "ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS node_type VARCHAR(40) NOT NULL DEFAULT 'location'",
      "ALTER TABLE room_map_nodes ADD COLUMN IF NOT EXISTS icon VARCHAR(20) NOT NULL DEFAULT '●'",
      "CREATE INDEX IF NOT EXISTS idx_room_map_nodes_room ON room_map_nodes(room_id)",
      // [功能備註｜60-cpp.21.2 轉盤核心 migration] 轉盤不再只依賴 legacy SQL；C++ 啟動即可自行補齊。
      R"SQL(CREATE TABLE IF NOT EXISTS custom_wheels (
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
      ))SQL",
      R"SQL(CREATE TABLE IF NOT EXISTS wheel_spins (
        id BIGSERIAL PRIMARY KEY,
        wheel_id BIGINT NOT NULL REFERENCES custom_wheels(id) ON DELETE CASCADE,
        room_id BIGINT REFERENCES rooms(id) ON DELETE CASCADE,
        user_id BIGINT REFERENCES users(id) ON DELETE SET NULL,
        result_index BIGINT NOT NULL DEFAULT 0,
        result_label VARCHAR(300) NOT NULL,
        created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
      ))SQL",
      "CREATE INDEX IF NOT EXISTS idx_custom_wheels_room ON custom_wheels(room_id,active)",
      "CREATE INDEX IF NOT EXISTS idx_wheel_spins_wheel ON wheel_spins(wheel_id,created_at DESC)",
      R"SQL(CREATE TABLE IF NOT EXISTS monster_templates (
        id BIGSERIAL PRIMARY KEY,
        name VARCHAR(120) NOT NULL,
        category VARCHAR(80) NOT NULL DEFAULT '怪物',
        rank VARCHAR(10) NOT NULL DEFAULT 'G',
        description TEXT NOT NULL DEFAULT '',
        public_hint TEXT NOT NULL DEFAULT '',
        image_url TEXT NOT NULL DEFAULT '',
        max_hp BIGINT NOT NULL DEFAULT 100,
        investigation_dc BIGINT NOT NULL DEFAULT 50,
        attributes JSONB NOT NULL DEFAULT '{}'::jsonb,
        skills JSONB NOT NULL DEFAULT '[]'::jsonb,
        passive_skills JSONB NOT NULL DEFAULT '[]'::jsonb,
        great_way JSONB NOT NULL DEFAULT '{}'::jsonb,
        boss_phases JSONB NOT NULL DEFAULT '[]'::jsonb,
        drops JSONB NOT NULL DEFAULT '[]'::jsonb,
        ai_level BIGINT NOT NULL DEFAULT 1,
        ai_behavior VARCHAR(40) NOT NULL DEFAULT 'balanced',
        config JSONB NOT NULL DEFAULT '{}'::jsonb,
        element_affinity JSONB NOT NULL DEFAULT '{}'::jsonb,
        effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb,
        created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
      ))SQL",
      R"SQL(CREATE TABLE IF NOT EXISTS npc_templates (
        id BIGSERIAL PRIMARY KEY,
        name VARCHAR(120) NOT NULL,
        role_name VARCHAR(120) NOT NULL DEFAULT '',
        description TEXT NOT NULL DEFAULT '',
        image_url TEXT NOT NULL DEFAULT '',
        portrait_url TEXT NOT NULL DEFAULT '',
        max_hp BIGINT NOT NULL DEFAULT 100,
        attributes JSONB NOT NULL DEFAULT '{}'::jsonb,
        element_affinity JSONB NOT NULL DEFAULT '{}'::jsonb,
        effects JSONB NOT NULL DEFAULT '{"list":[]}'::jsonb,
        skills JSONB NOT NULL DEFAULT '[]'::jsonb,
        ai_level BIGINT NOT NULL DEFAULT 1,
        ai_behavior VARCHAR(40) NOT NULL DEFAULT 'balanced',
        speech_enabled BOOLEAN NOT NULL DEFAULT TRUE,
        in_game_room BOOLEAN NOT NULL DEFAULT FALSE,
        config JSONB NOT NULL DEFAULT '{}'::jsonb,
        created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
      ))SQL",
      // [功能備註｜65-cpp.26.4 多立繪] NPC 表建立後再補多立繪，確保全新 PostgreSQL 也能從零啟動。
      "ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS portrait_url TEXT NOT NULL DEFAULT ''",
      R"SQL(CREATE TABLE IF NOT EXISTS npc_portrait_variants (
        id BIGSERIAL PRIMARY KEY,
        npc_template_id BIGINT NOT NULL REFERENCES npc_templates(id) ON DELETE CASCADE,
        label VARCHAR(80) NOT NULL DEFAULT '立繪',
        external_url TEXT NOT NULL DEFAULT '',
        mime_type VARCHAR(32) NOT NULL DEFAULT '',
        image_base64 TEXT NOT NULL DEFAULT '',
        size_bytes BIGINT NOT NULL DEFAULT 0,
        is_default BOOLEAN NOT NULL DEFAULT FALSE,
        is_active BOOLEAN NOT NULL DEFAULT FALSE,
        sort_order BIGINT NOT NULL DEFAULT 100,
        created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
      ))SQL",
      "CREATE INDEX IF NOT EXISTS idx_npc_portrait_variants_npc ON npc_portrait_variants(npc_template_id,sort_order,id)",
      "CREATE INDEX IF NOT EXISTS idx_npc_portrait_variants_active ON npc_portrait_variants(npc_template_id,is_active)",
      R"SQL(INSERT INTO npc_portrait_variants(npc_template_id,label,external_url,is_default,is_active,sort_order)
        SELECT nt.id,'預設',COALESCE(nt.portrait_url,NULLIF(nt.image_url,''),''),TRUE,TRUE,0
        FROM npc_templates nt
        WHERE COALESCE(nt.portrait_url,NULLIF(nt.image_url,''),'')<>''
          AND NOT EXISTS(SELECT 1 FROM npc_portrait_variants v WHERE v.npc_template_id=nt.id))SQL",
      R"SQL(UPDATE npc_templates nt SET portrait_url=CASE WHEN v.external_url<>'' THEN v.external_url ELSE '/api/npc-portrait-variants/'||v.id::text||'/image' END
        FROM npc_portrait_variants v WHERE v.npc_template_id=nt.id AND v.is_active=TRUE)SQL",
      "ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS merchant_enabled BOOLEAN NOT NULL DEFAULT FALSE",
      "ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS merchant_title VARCHAR(120) NOT NULL DEFAULT ''",
      R"SQL(CREATE TABLE IF NOT EXISTS room_monsters (
        id BIGSERIAL PRIMARY KEY,
        room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
        monster_template_id BIGINT NOT NULL REFERENCES monster_templates(id) ON DELETE CASCADE,
        status VARCHAR(30) NOT NULL DEFAULT 'hidden',
        current_hp BIGINT NOT NULL DEFAULT 100,
        max_hp BIGINT NOT NULL DEFAULT 100,
        state JSONB NOT NULL DEFAULT '{}'::jsonb,
        turn_actions_remaining BIGINT NOT NULL DEFAULT 0,
        skill_charge_state JSONB NOT NULL DEFAULT '{}'::jsonb,
        current_spirit BIGINT NOT NULL DEFAULT 0,
        max_spirit BIGINT NOT NULL DEFAULT 0,
        current_fifth BIGINT NOT NULL DEFAULT 0,
        max_fifth BIGINT NOT NULL DEFAULT 0,
        created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
      ))SQL",
      R"SQL(CREATE TABLE IF NOT EXISTS room_npcs (
        room_id BIGINT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
        npc_template_id BIGINT NOT NULL REFERENCES npc_templates(id) ON DELETE CASCADE,
        status VARCHAR(30) NOT NULL DEFAULT 'active',
        state JSONB NOT NULL DEFAULT '{}'::jsonb,
        combat_side VARCHAR(20) NOT NULL DEFAULT 'neutral',
        current_hp BIGINT NOT NULL DEFAULT 100,
        max_hp BIGINT NOT NULL DEFAULT 100,
        current_spirit BIGINT NOT NULL DEFAULT 0,
        max_spirit BIGINT NOT NULL DEFAULT 0,
        current_fifth BIGINT NOT NULL DEFAULT 0,
        max_fifth BIGINT NOT NULL DEFAULT 0,
        turn_actions_remaining BIGINT NOT NULL DEFAULT 0,
        skill_charge_state JSONB NOT NULL DEFAULT '{}'::jsonb,
        created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
        PRIMARY KEY(room_id,npc_template_id)
      ))SQL",
      "ALTER TABLE monster_templates ADD COLUMN IF NOT EXISTS element_affinity JSONB NOT NULL DEFAULT '{}'::jsonb",
      // [功能備註｜65-cpp.26 元素抗性／弱點] 正值抗性降低對應元素傷害；弱點提高對應元素傷害。
      "ALTER TABLE monster_templates ADD COLUMN IF NOT EXISTS element_resistance JSONB NOT NULL DEFAULT '{}'::jsonb",
      "ALTER TABLE monster_templates ADD COLUMN IF NOT EXISTS element_weakness JSONB NOT NULL DEFAULT '{}'::jsonb",
      "ALTER TABLE monster_templates ADD COLUMN IF NOT EXISTS effects JSONB NOT NULL DEFAULT '{\"list\":[]}'::jsonb",
      // [功能備註｜60-cpp.21 怪物完整戰鬥實體] 五維的 luck 留在 attributes；技能／大道／Boss 階段為正式欄位。
      "ALTER TABLE monster_templates ADD COLUMN IF NOT EXISTS passive_skills JSONB NOT NULL DEFAULT '[]'::jsonb",
      "ALTER TABLE monster_templates ADD COLUMN IF NOT EXISTS great_way JSONB NOT NULL DEFAULT '{}'::jsonb",
      "ALTER TABLE monster_templates ADD COLUMN IF NOT EXISTS boss_phases JSONB NOT NULL DEFAULT '[]'::jsonb",
      "ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS max_hp BIGINT NOT NULL DEFAULT 100",
      "ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS attributes JSONB NOT NULL DEFAULT '{}'::jsonb",
      "ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS element_affinity JSONB NOT NULL DEFAULT '{}'::jsonb",
      "ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS element_resistance JSONB NOT NULL DEFAULT '{}'::jsonb",
      "ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS element_weakness JSONB NOT NULL DEFAULT '{}'::jsonb",
      "ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS effects JSONB NOT NULL DEFAULT '{\"list\":[]}'::jsonb",
      "ALTER TABLE npc_templates ADD COLUMN IF NOT EXISTS skills JSONB NOT NULL DEFAULT '[]'::jsonb",
      "ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS turn_actions_remaining BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS skill_charge_state JSONB NOT NULL DEFAULT '{}'::jsonb",
      "ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS current_spirit BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS max_spirit BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS current_fifth BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS max_fifth BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE rooms ADD COLUMN IF NOT EXISTS combat_victory_condition JSONB NOT NULL DEFAULT '{\"type\":\"eliminate_enemies\",\"auto_end\":false}'::jsonb",
      "ALTER TABLE rooms ADD COLUMN IF NOT EXISTS rule_combat_state JSONB NOT NULL DEFAULT '{\"flags\":[],\"values\":{}}'::jsonb",
      "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS element_resistance JSONB NOT NULL DEFAULT '{\"暗\":0,\"光\":0,\"金\":0,\"木\":0,\"水\":0,\"火\":0,\"土\":0}'::jsonb",
      "ALTER TABLE character_cards ADD COLUMN IF NOT EXISTS element_weakness JSONB NOT NULL DEFAULT '{\"暗\":0,\"光\":0,\"金\":0,\"木\":0,\"水\":0,\"火\":0,\"土\":0}'::jsonb",
      "ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS rule_boss_phase INTEGER NOT NULL DEFAULT 1",
      "ALTER TABLE room_monsters ADD COLUMN IF NOT EXISTS rule_modifiers JSONB NOT NULL DEFAULT '{}'::jsonb",
      "ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS combat_side VARCHAR(20) NOT NULL DEFAULT 'neutral'",
      "ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS current_hp BIGINT NOT NULL DEFAULT 100",
      "ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS max_hp BIGINT NOT NULL DEFAULT 100",
      "ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS turn_actions_remaining BIGINT NOT NULL DEFAULT 0",
      "ALTER TABLE room_npcs ADD COLUMN IF NOT EXISTS skill_charge_state JSONB NOT NULL DEFAULT '{}'::jsonb",
      "CREATE INDEX IF NOT EXISTS idx_room_monsters_room_status ON room_monsters(room_id,status)",
      "CREATE INDEX IF NOT EXISTS idx_room_npcs_room_side ON room_npcs(room_id,combat_side,status)",
      "CREATE INDEX IF NOT EXISTS idx_room_members_room ON room_members(room_id)",
      "CREATE INDEX IF NOT EXISTS idx_room_members_user ON room_members(user_id)",
      "CREATE INDEX IF NOT EXISTS idx_events_room_id_id ON events(room_id,id)",
      "CREATE INDEX IF NOT EXISTS idx_bindings_room ON character_room_bindings(room_id)",
      R"SQL(DO $$
        DECLARE column_row RECORD;
        BEGIN
          FOR column_row IN
            SELECT c.table_schema,c.table_name,c.column_name
            FROM information_schema.columns c
            JOIN information_schema.tables t
              ON t.table_schema=c.table_schema AND t.table_name=c.table_name
            WHERE c.table_schema='public'
              AND t.table_type='BASE TABLE'
              AND c.data_type='integer'
              AND c.column_name<>'id'
              AND c.column_name NOT LIKE '%\_id' ESCAPE '\'
              AND COALESCE(c.column_default,'') NOT LIKE 'nextval(%'
          LOOP
            EXECUTE format(
              'ALTER TABLE %I.%I ALTER COLUMN %I TYPE BIGINT USING %I::BIGINT',
              column_row.table_schema,column_row.table_name,
              column_row.column_name,column_row.column_name
            );
          END LOOP;
        END $$)SQL"};

  for (const auto& sql : migrations) database_->execSqlSync(sql);
  applyLegacyV39Migrations(shared_from_this());
  // [功能備註｜62-cpp.23 事件型技能 migration] legacy 狀態表完成後，再補泛型戰鬥狀態與 triggers 欄位。
  applyEventSkillMigrations(shared_from_this());
  // [功能備註｜通用詞條／裝備槽] 套用特殊貨幣、詞條池、洗煉與裝備槽 migration。
  applyGearAffixMigrations(shared_from_this());
  bootstrapAdmin();
}

void CoreService::bootstrapAdmin() {
  const auto hash = hashPassword(config_.adminPassword, 12);
  const auto rows = database_->execSqlSync(
      "SELECT id FROM users WHERE username=$1", config_.adminUsername);
  std::int64_t userId = 0;
  if (rows.empty()) {
    const auto inserted = database_->execSqlSync(
        "INSERT INTO users(username,password_hash,is_admin) VALUES($1,$2,TRUE) RETURNING id",
        config_.adminUsername, hash);
    userId = inserted[0]["id"].as<std::int64_t>();
    LOG_INFO << "Global DM administrator created: " << config_.adminUsername;
  } else {
    userId = rows[0]["id"].as<std::int64_t>();
    database_->execSqlSync(
        "UPDATE users SET password_hash=$1,is_admin=TRUE WHERE id=$2", hash, userId);
    LOG_INFO << "Global DM administrator updated: " << config_.adminUsername;
  }
  database_->execSqlSync(
      "INSERT INTO character_cards(user_id) VALUES($1) ON CONFLICT(user_id) DO NOTHING",
      userId);
}

std::optional<JwtClaims> CoreService::authenticateToken(const std::string& token) const {
  return verifyJwt(token, config_.jwtSecret);
}

std::optional<JwtClaims> CoreService::authenticateRequest(
    const drogon::HttpRequestPtr& request) const {
  auto header = request->getHeader("Authorization");
  static constexpr std::string_view prefix = "Bearer ";
  if (header.size() <= prefix.size() || header.compare(0, prefix.size(), prefix) != 0) {
    return std::nullopt;
  }
  return authenticateToken(header.substr(prefix.size()));
}

std::optional<JwtClaims> CoreService::currentUser(std::int64_t userId) const {
  const auto rows = database_->execSqlSync(
      "SELECT id,username,is_admin FROM users WHERE id=$1", userId);
  if (rows.empty()) return std::nullopt;
  return JwtClaims{rows[0]["id"].as<std::int64_t>(),
                   rows[0]["username"].as<std::string>(),
                   rows[0]["is_admin"].as<bool>(), 0, 0};
}

bool CoreService::userCanAccessRoom(std::int64_t userId, std::int64_t roomId) const {
  const auto user = currentUser(userId);
  if (!user) return false;
  if (user->isAdmin) {
    return !database_->execSqlSync("SELECT 1 FROM rooms WHERE id=$1", roomId).empty();
  }
  return !database_->execSqlSync(
      "SELECT 1 FROM room_members WHERE room_id=$1 AND user_id=$2", roomId, userId)
              .empty();
}

Json::Value CoreService::characterByUser(std::int64_t userId) const {
  const auto rows = database_->execSqlSync(
      "SELECT row_to_json(c)::text AS json_text FROM character_cards c WHERE user_id=$1",
      userId);
  if (rows.empty()) return Json::nullValue;
  auto character = parseJson(rows[0]["json_text"].as<std::string>());
  // [功能備註｜64-cpp.25.2 玩家頭像自動恢復] avatar_images 有資料時，即使舊 avatar_url 被清空也回傳內部圖片路由。
  if (character.get("avatar_url", "").asString().empty()) {
    try {
      const auto avatarRows = database_->execSqlSync(
          "SELECT 1 FROM avatar_images WHERE user_id=$1 LIMIT 1", userId);
      if (!avatarRows.empty()) {
        character["avatar_url"] = "/api/avatar-images/" + std::to_string(userId);
      }
    } catch (...) {
      // 舊 schema 尚未建立 avatar_images 時保持文字頭像 fallback。
    }
  }
  // [功能備註｜65-cpp.26.2 角色立繪自動恢復] portrait_images 有資料時自動補回內部路由。
  if (character.get("portrait_url", "").asString().empty()) {
    try {
      const auto portraitRows = database_->execSqlSync(
          "SELECT 1 FROM portrait_images WHERE user_id=$1 LIMIT 1", userId);
      if (!portraitRows.empty()) {
        character["portrait_url"] = "/api/portrait-images/" + std::to_string(userId);
      }
    } catch (...) {
      // 舊 schema 尚未建立 portrait_images 時保持空白。
    }
  }
  ensureArray(character, "sub_classes");
  ensureArray(character, "skills");
  ensureArray(character, "equipment");
  ensureArray(character, "weapons");
  ensureArray(character, "inventory");
  ensureObject(character, "class_resources");
  ensureObject(character, "gear_loadout");

  // [功能備註｜裝備槽／武器槽] 56-cpp.17：由 C++ 依 gear_loadout 解析真正生效的主／副武器與 DM 自訂裝備槽。
  // 新槽位只保存 affix_targets.id，角色回傳時再還原原始裝備資料，避免前端自行偽造已裝備狀態。
  character["equipped_equipment"] = Json::arrayValue;
  character["equipped_weapons"] = Json::arrayValue;
  character["blocked_equipment"] = Json::arrayValue;
  character["blocked_weapons"] = Json::arrayValue;
  try {
    const auto loadout = character["gear_loadout"];
    auto targetData = [&](std::int64_t targetId, const std::string& expectedType) -> Json::Value {
      if (targetId <= 0) return Json::Value{};
      const auto targetRows = database_->execSqlSync(
          "SELECT id,target_key,rank,data::text AS data_text FROM affix_targets "
          "WHERE id=$1 AND owner_user_id=$2 AND target_type=$3 LIMIT 1",
          targetId, userId, expectedType);
      if (targetRows.empty() || targetRows[0]["data_text"].isNull()) return Json::Value{};
      auto data = parseJson(targetRows[0]["data_text"].as<std::string>(), Json::Value{Json::objectValue});
      // [功能備註｜65-cpp.26.14 可成長性裝備] 已裝備物回傳 target id 與 C++ 計算後有效品階，
      // 讓裝備技能與前端顯示都使用同一筆成長進度，而不是角色卡裡的 Lv.1 模板快照。
      data["affix_target_id"] = Json::Int64(targetRows[0]["id"].as<std::int64_t>());
      data["target_key"] = targetRows[0]["target_key"].as<std::string>();
      data["rank"] = targetRows[0]["rank"].as<std::string>();
      data["growth_effective_rank"] = targetRows[0]["rank"].as<std::string>();
      return data;
    };

    if (loadout.isObject() && loadout["weapon_slots"].isObject()) {
      const auto mainId = integerValue(loadout["weapon_slots"]["main"]);
      const auto subId = integerValue(loadout["weapon_slots"]["sub"]);
      auto mainWeapon = targetData(mainId, "weapon");
      if (mainWeapon.isObject() && !stringValue(mainWeapon["name"]).empty()) {
        character["equipped_weapons"].append(mainWeapon);
      }
      const bool mainTwoHanded = mainWeapon.isObject() && integerValue(mainWeapon["weapon_hands"], 1) >= 2;
      if (!mainTwoHanded) {
        auto subWeapon = targetData(subId, "weapon");
        if (subWeapon.isObject() && !stringValue(subWeapon["name"]).empty()) {
          character["equipped_weapons"].append(subWeapon);
        }
      }
    }

    if (loadout.isObject() && loadout["equipment_slots"].isObject()) {
      for (const auto& slotCode : loadout["equipment_slots"].getMemberNames()) {
        const auto targetId = integerValue(loadout["equipment_slots"][slotCode]);
        auto item = targetData(targetId, "equipment");
        if (!item.isObject() || stringValue(item["name"]).empty()) continue;
        if (stringValue(item["equip_slot_code"]).empty()) item["equip_slot_code"] = slotCode;
        character["equipped_equipment"].append(std::move(item));
      }
    }
  } catch (const std::exception& error) {
    // 舊資料庫若尚未套用詞條 migration，角色卡仍應可讀取；部署啟動 migration 後會自動恢復完整裝備解析。
    LOG_WARN << "gear loadout degraded for user " << userId << ": " << error.what();
  }

  // [功能備註｜65-cpp.26.14 可成長性裝備] 裝備期間授予技能由 C++ 依「目前實際裝備」整理，
  // 避免前端僅憑角色持有清單偽造 gear_<templateId> 技能。
  character["equipment_granted_skill_template_ids"] = Json::arrayValue;
  {
    std::set<std::int64_t> grantedSkillIds;
    auto collectGranted = [&](const Json::Value& list) {
      if (!list.isArray()) return;
      for (const auto& gear : list) {
        if (!gear.isObject() || stringValue(gear["grant_mode"]) != "equipped" ||
            !gear["granted_skill_template_ids"].isArray()) continue;
        for (const auto& rawId : gear["granted_skill_template_ids"]) {
          const auto id = integerValue(rawId);
          if (id > 0) grantedSkillIds.insert(id);
        }
      }
    };
    collectGranted(character["equipped_weapons"]);
    collectGranted(character["equipped_equipment"]);
    for (const auto id : grantedSkillIds)
      character["equipment_granted_skill_template_ids"].append(Json::Int64(id));
  }

  character["active_statuses"] = Json::arrayValue;
  character["final_agility"] = character.get("agility", 0);
  character["final_strength"] = character.get("strength", 0);
  character["final_constitution"] = character.get("constitution", 0);
  character["final_spirit"] = character.get("spirit", 0);
  character["final_great_way"] = character.get("great_way", 0);
  character["final_faith"] = character.get("faith", 0);
  // [功能備註｜角色衍生值] 58-cpp.19：角色卡衍生值一律由 C++ 依最終體質／精神重算。
  // 不再相信舊資料庫曾留下的 0 值，避免生命上限、耐力、理智、意志顯示成 0。
  const auto derivedConstitution = nonNegative(character["final_constitution"]);
  const auto derivedSpirit = nonNegative(character["final_spirit"]);
  const auto derivedMaxHp = derivedConstitution * 2;
  const auto derivedEndurance = (derivedConstitution + derivedSpirit) / 2;
  const auto derivedWill = derivedSpirit * 8 / 10;
  const auto derivedSanity = derivedSpirit * 12 / 10;
  character["max_hp"] = Json::Int64(derivedMaxHp);
  character["max_spirit"] = Json::Int64(derivedSpirit);
  character["endurance"] = Json::Int64(derivedEndurance);
  character["will"] = Json::Int64(derivedWill);
  character["sanity"] = Json::Int64(derivedSanity);
  character["max_sanity"] = Json::Int64(derivedSanity);
  character["derived_stats"]["max_hp"] = Json::Int64(derivedMaxHp);
  character["derived_stats"]["endurance"] = Json::Int64(derivedEndurance);
  character["derived_stats"]["max_sanity"] = Json::Int64(derivedSanity);
  character["derived_stats"]["will"] = Json::Int64(derivedWill);
  character["round_count"] = Json::Int64(
      combatActionsForAgility(nonNegative(character["agility"])));
  const bool west = stringValue(character["faction"], "東國") == "西國";
  const auto fifthMax = west ? nonNegative(character["faith"])
                             : nonNegative(character["great_way"]);
  const auto& fifthCurrentValue = west ? character["current_faith"]
                                       : character["current_great_way"];
  const auto fifthCurrent = fifthCurrentValue.isNull()
                                ? fifthMax
                                : std::min(fifthMax, nonNegative(fifthCurrentValue));
  character["fifth_max"] = Json::Int64(fifthMax);
  character["fifth_current"] = Json::Int64(fifthCurrent);
  // [功能備註｜64-cpp.25 角色等階快照] 統一回傳大階／小級、神性稱號、固定屬性額度與登階提示。
  const auto characterTier = std::clamp<std::int64_t>(integerValue(character["tier"], 0), 0, 10);
  const auto characterLevel = std::clamp<std::int64_t>(integerValue(character["level"], 1), 1, 4);
  const auto attributeTierBudget = characterTierBudget(characterTier);
  const auto attributeSpent = nonNegative(character["agility"]) + nonNegative(character["strength"]) +
                              nonNegative(character["constitution"]) + nonNegative(character["spirit"]) + fifthMax;
  const auto storedAttributePoints = nonNegative(character["attribute_points"]);
  // [功能備註｜65-cpp.26.5 DM 自由屬性點]
  // 固定大階額度是最低總額；DM 直接增加 attribute_points 時，超出固定額度的部分視為永久額外自由額度。
  // 玩家重新分配時總額固定，因此無法靠自行修改 API 增生點數。
  const auto attributeBudget = std::max<std::int64_t>(attributeTierBudget, attributeSpent + storedAttributePoints);
  const auto effectiveRemaining = std::max<std::int64_t>(0, attributeBudget - attributeSpent);
  const auto attributeBonus = std::max<std::int64_t>(0, attributeBudget - attributeTierBudget);
  character["attribute_points"] = Json::Int64(effectiveRemaining);
  character["tier"] = Json::Int64(characterTier);
  character["level"] = Json::Int64(characterLevel);
  character["tier_level"] = Json::Int64(characterLevel);
  character["tier_display"] = std::to_string(characterTier) + "階" + std::to_string(characterLevel) + "級";
  character["tier_title"] = characterTierTitle(characterTier, characterLevel);
  character["attribute_tier_budget"] = Json::Int64(attributeTierBudget);
  character["attribute_bonus"] = Json::Int64(attributeBonus);
  character["attribute_budget"] = Json::Int64(attributeBudget);
  character["attribute_spent"] = Json::Int64(attributeSpent);
  character["attribute_remaining_expected"] = Json::Int64(effectiveRemaining);
  character["ascension_ready"] = characterTier < 10 && characterLevel >= 4;
  character["ascension_ritual_type"] = characterTier <= 0 ? "無" : ((characterTier + 1) >= 9 ? "登神儀式" : "登階儀式");
  Json::Value affinity;
  affinity["光"] = character.get("affinity_light", 0);
  affinity["暗"] = character.get("affinity_dark", 0);
  affinity["金"] = character.get("affinity_metal", 0);
  affinity["木"] = character.get("affinity_wood", 0);
  affinity["水"] = character.get("affinity_water", 0);
  affinity["火"] = character.get("affinity_fire", 0);
  affinity["土"] = character.get("affinity_earth", 0);
  character["element_affinity"] = std::move(affinity);

  // [功能備註｜65-cpp.26 玩家元素抗性／弱點]
  // 抗性與弱點為百分比資料；兩者分開保存，便於 DM 做「火抗 30% / 暗弱 50%」等設定。
  auto normalizeElementPercentMap=[&](const Json::Value& raw){
    Json::Value out{Json::objectValue};
    const std::array<const char*,7> names={"暗","光","金","木","水","火","土"};
    for(const auto* e:names) out[e]=Json::Int64(std::clamp<std::int64_t>(integerValue(raw[e]),0,900));
    return out;
  };
  character["element_resistance"] = normalizeElementPercentMap(character["element_resistance"]);
  character["element_weakness"] = normalizeElementPercentMap(character["element_weakness"]);

  // [功能備註｜元素儲存] 47-cpp.8：七元素初始容量全為 0。
  // 容量 = 永久基礎 + 技能 + 持有/裝備道具 + 已學魔法/儀式；目前儲存量永遠不超過容量。
  const std::array<const char*,7> storedElements={"暗","光","金","木","水","火","土"};
  auto zeroMap=[&](){Json::Value out{Json::objectValue};for(const auto* e:storedElements)out[e]=Json::Int64(0);return out;};
  auto normalized=[&](const Json::Value& raw){auto out=zeroMap();if(raw.isObject())for(const auto*e:storedElements)out[e]=Json::Int64(std::max<std::int64_t>(0,integerValue(raw[e])));return out;};
  auto addBonus=[&](Json::Value& target,const Json::Value& raw,std::int64_t multiplier=1){if(!raw.isObject()||multiplier==0)return;for(const auto*e:storedElements)target[e]=Json::Int64(std::max<std::int64_t>(0,integerValue(target[e])+integerValue(raw[e])*multiplier));};

  auto baseCaps=normalized(character["element_storage_base_caps"]);
  auto skillBonus=zeroMap(),itemBonus=zeroMap(),studyBonus=zeroMap();
  if(character["skills"].isArray()){
    for(const auto& skill:character["skills"]){
      if(!skill.isObject()||!skill["data"].isObject())continue;
      addBonus(skillBonus,skill["data"]["element_storage_cap_bonus"]);
    }
  }
  std::vector<std::pair<std::string,std::int64_t>> held;
  auto collectHeld=[&](const Json::Value& raw){
    std::string name;std::int64_t qty=1;
    if(raw.isString())name=raw.asString();
    else if(raw.isObject()){name=stringValue(raw["name"],stringValue(raw["title"]));qty=std::max<std::int64_t>(1,integerValue(raw["quantity"],1));}
    name=trimmed(name);if(name.empty())return;
    for(auto& pair:held)if(pair.first==name){pair.second+=qty;return;}
    held.emplace_back(name,qty);
  };
  if(character["inventory"].isArray())for(const auto&x:character["inventory"])collectHeld(x);
  if(character["equipment"].isArray())for(const auto&x:character["equipment"])collectHeld(x);
  if(character["weapons"].isArray())for(const auto&x:character["weapons"])collectHeld(x);
  for(const auto& [name,qty]:held){
    const auto rows=database_->execSqlSync("SELECT effects::text AS effects_text FROM item_templates WHERE name=$1 LIMIT 1",name);
    if(rows.empty()||rows[0]["effects_text"].isNull())continue;
    const auto effects=parseJson(rows[0]["effects_text"].as<std::string>(),Json::Value{Json::objectValue});
    addBonus(itemBonus,effects["element_storage_cap_bonus"],qty);
  }
  const auto magicRows=database_->execSqlSync(
      "SELECT m.effects::text AS effects_text FROM character_magics cm JOIN magic_studies m ON m.id=cm.magic_id WHERE cm.user_id=$1 AND m.active=TRUE",userId);
  for(const auto&row:magicRows)if(!row["effects_text"].isNull())addBonus(studyBonus,parseJson(row["effects_text"].as<std::string>(),Json::Value{Json::objectValue})["element_storage_cap_bonus"]);
  const auto ritualRows=database_->execSqlSync(
      "SELECT r.effects::text AS effects_text FROM character_rituals cr JOIN ritual_studies r ON r.id=cr.ritual_id WHERE cr.user_id=$1 AND r.active=TRUE",userId);
  for(const auto&row:ritualRows)if(!row["effects_text"].isNull())addBonus(studyBonus,parseJson(row["effects_text"].as<std::string>(),Json::Value{Json::objectValue})["element_storage_cap_bonus"]);

  auto caps=baseCaps;addBonus(caps,skillBonus);addBonus(caps,itemBonus);addBonus(caps,studyBonus);
  auto stored=normalized(character["element_storage"]);
  for(const auto*e:storedElements)stored[e]=Json::Int64(std::min<std::int64_t>(integerValue(stored[e]),integerValue(caps[e])));
  Json::Value capSources{Json::objectValue};capSources["base"]=baseCaps;capSources["skills"]=skillBonus;capSources["items"]=itemBonus;capSources["studies"]=studyBonus;
  character["element_storage_base_caps"]=std::move(baseCaps);
  character["element_storage_caps"]=std::move(caps);
  character["element_storage"]=std::move(stored);
  character["element_storage_cap_sources"]=std::move(capSources);
  return character;
}

Json::Value CoreService::roomSnapshot(std::int64_t roomId) const {
  const auto roomRows = database_->execSqlSync(
      "SELECT row_to_json(r)::text AS json_text FROM rooms r WHERE id=$1", roomId);
  if (roomRows.empty()) throw ApiError(drogon::k404NotFound, "找不到房間");

  Json::Value snapshot;
  snapshot["room"] = parseJson(roomRows[0]["json_text"].as<std::string>());
  snapshot["members"] = Json::arrayValue;
  snapshot["events"] = Json::arrayValue;
  snapshot["summons"] = Json::arrayValue;
  snapshot["monsters"] = Json::arrayValue;
  snapshot["npcs"] = Json::arrayValue;
  snapshot["ritual_instances"] = Json::arrayValue;

  const auto members = database_->execSqlSync(R"SQL(
    SELECT jsonb_build_object(
      'id',rm.user_id,'user_id',rm.user_id,'display_name',rm.display_name,
      'role',rm.role,
      'hp',CASE WHEN COALESCE(rm.max_hp,0)<=1 AND COALESCE(cc.constitution,0)>0 THEN GREATEST(0,cc.constitution*2) ELSE rm.hp END,
      'max_hp',CASE WHEN COALESCE(rm.max_hp,0)<=1 AND COALESCE(cc.constitution,0)>0 THEN GREATEST(0,cc.constitution*2) ELSE rm.max_hp END,
      'spirit',CASE WHEN COALESCE(rm.max_spirit,0)<=0 AND COALESCE(cc.spirit,0)>0 THEN GREATEST(0,cc.spirit) ELSE rm.spirit END,
      'max_spirit',CASE WHEN COALESCE(rm.max_spirit,0)<=0 AND COALESCE(cc.spirit,0)>0 THEN GREATEST(0,cc.spirit) ELSE rm.max_spirit END,
      'current_sanity',CASE WHEN COALESCE(rm.max_sanity,0)<=1 AND COALESCE(cc.spirit,0)>0 THEN GREATEST(1,(cc.spirit*12)/10) ELSE rm.current_sanity END,
      'max_sanity',CASE WHEN COALESCE(rm.max_sanity,0)<=1 AND COALESCE(cc.spirit,0)>0 THEN GREATEST(1,(cc.spirit*12)/10) ELSE rm.max_sanity END,
      'sanity_initialized',rm.sanity_initialized,'sanity_stage',rm.sanity_stage,
      'seven_sin',rm.seven_sin,'joined_at',rm.joined_at,
      'turn_actions_remaining',rm.turn_actions_remaining,
      'skill_charge_state',rm.skill_charge_state,
      'combat_state',rm.combat_state,'combat_reaction',rm.combat_reaction,
      'current_map_node_id',to_jsonb(rm)->'current_map_node_id',
      'offline_ai_enabled',COALESCE((to_jsonb(rm)->>'offline_ai_enabled')::boolean,TRUE),
      'offline_ai_active',COALESCE((to_jsonb(rm)->>'offline_ai_active')::boolean,FALSE),
      'last_seen_at',to_jsonb(rm)->'last_seen_at',
      'username',u.username,'is_admin',u.is_admin,
      'character_id',crb.character_id,
      'class_resources',COALESCE(to_jsonb(cc)->'class_resources','{}'::jsonb),
      'avatar_url',CASE WHEN COALESCE(to_jsonb(cc)->>'avatar_url','')<>'' THEN COALESCE(to_jsonb(cc)->>'avatar_url','') WHEN EXISTS(SELECT 1 FROM avatar_images ai WHERE ai.user_id=rm.user_id) THEN '/api/avatar-images/'||rm.user_id::text ELSE '' END,
      'portrait_url',CASE WHEN COALESCE(to_jsonb(cc)->>'portrait_url','')<>'' THEN COALESCE(to_jsonb(cc)->>'portrait_url','') WHEN EXISTS(SELECT 1 FROM portrait_images pi WHERE pi.user_id=rm.user_id) THEN '/api/portrait-images/'||rm.user_id::text ELSE '' END,
      'pollution',COALESCE((to_jsonb(cc)->>'pollution')::bigint,0)
    )::text AS json_text
    FROM room_members rm
    JOIN users u ON u.id=rm.user_id
    LEFT JOIN character_room_bindings crb
      ON crb.room_id=rm.room_id AND crb.user_id=rm.user_id
    LEFT JOIN character_cards cc ON cc.id=crb.character_id
    WHERE rm.room_id=$1 ORDER BY rm.joined_at
  )SQL", roomId);
  for (const auto& row : members) {
    snapshot["members"].append(parseJson(row["json_text"].as<std::string>()));
  }

  try {
    const auto monsters = database_->execSqlSync(R"SQL(
    SELECT jsonb_build_object(
      'id',rm.id,'monster_template_id',rm.monster_template_id,'name',mt.name,
      'category',mt.category,'rank',mt.rank,'status',rm.status,
      'current_hp',rm.current_hp,'max_hp',rm.max_hp,
      'current_spirit',rm.current_spirit,'max_spirit',rm.max_spirit,
      'current_fifth',rm.current_fifth,'max_fifth',rm.max_fifth,
      'turn_actions_remaining',rm.turn_actions_remaining,
      'skill_charge_state',rm.skill_charge_state,'attributes',mt.attributes,
      'skills',mt.skills,'passive_skills',mt.passive_skills,'great_way',mt.great_way,
      'boss_phases',mt.boss_phases,'config',mt.config,
      'boss_phase_index',COALESCE((to_jsonb(rm)->>'boss_phase_index')::bigint,0),
      'boss_phase_name',COALESCE(to_jsonb(rm)->>'boss_phase_name','')
    )::text AS json_text
    FROM room_monsters rm
    JOIN monster_templates mt ON mt.id=rm.monster_template_id
    WHERE rm.room_id=$1 ORDER BY rm.id
  )SQL", roomId);
  for (const auto& row : monsters) {
      snapshot["monsters"].append(parseJson(row["json_text"].as<std::string>()));
    }
  } catch (const std::exception& error) {
    LOG_WARN << "room snapshot monsters degraded for room " << roomId << ": " << error.what();
  }

  try {
    const auto npcs = database_->execSqlSync(R"SQL(
    SELECT jsonb_build_object(
      'id',rn.npc_template_id,'npc_template_id',rn.npc_template_id,'name',nt.name,
      'role_name',nt.role_name,'image_url',nt.image_url,'portrait_url',COALESCE(to_jsonb(nt)->>'portrait_url',''),
      'merchant_enabled',COALESCE((to_jsonb(nt)->>'merchant_enabled')::boolean,FALSE),
      'merchant_title',COALESCE(to_jsonb(nt)->>'merchant_title',''),
      'status',rn.status,'combat_side',rn.combat_side,
      'current_hp',rn.current_hp,'max_hp',rn.max_hp,
      'current_spirit',rn.current_spirit,'max_spirit',rn.max_spirit,
      'current_fifth',rn.current_fifth,'max_fifth',rn.max_fifth,
      'turn_actions_remaining',rn.turn_actions_remaining,
      'skill_charge_state',rn.skill_charge_state,'attributes',nt.attributes,
      'config',nt.config
    )::text AS json_text
    FROM room_npcs rn
    JOIN npc_templates nt ON nt.id=rn.npc_template_id
    WHERE rn.room_id=$1 ORDER BY rn.npc_template_id
  )SQL", roomId);
  for (const auto& row : npcs) {
      snapshot["npcs"].append(parseJson(row["json_text"].as<std::string>()));
    }
  } catch (const std::exception& error) {
    LOG_WARN << "room snapshot npcs degraded for room " << roomId << ": " << error.what();
  }

  // [功能備註｜62-cpp.23 戰鬥狀態快照]
  // 泛型事件技能狀態直接掛回玩家／怪物／NPC 的 combat_statuses，沿用既有卡片 UI 顯示層數。
  try {
    const auto statuses = database_->execSqlSync(
        "SELECT row_to_json(s)::text AS json_text FROM combat_status_instances s "
        "WHERE room_id=$1 AND active=TRUE ORDER BY target_type,target_id,id", roomId);
    for (const auto& row : statuses) {
      const auto status = parseJson(row["json_text"].as<std::string>());
      const auto type = stringValue(status["target_type"]);
      const auto id = integerValue(status["target_id"]);
      auto appendTo = [&](Json::Value& list) {
        if (!list.isArray()) return;
        for (auto& entity : list) {
          const auto entityId = type == "player" ? integerValue(entity["id"])
                                                   : (type == "npc" ? integerValue(entity["npc_template_id"], integerValue(entity["id"]))
                                                                    : integerValue(entity["id"]));
          if (entityId != id) continue;
          if (!entity["combat_statuses"].isArray()) entity["combat_statuses"] = Json::Value{Json::arrayValue};
          entity["combat_statuses"].append(status);
          break;
        }
      };
      if (type == "player") appendTo(snapshot["members"]);
      else if (type == "monster") appendTo(snapshot["monsters"]);
      else if (type == "npc") appendTo(snapshot["npcs"]);
    }
  } catch (const std::exception& error) {
    LOG_WARN << "room snapshot event skill statuses degraded for room " << roomId << ": " << error.what();
  }

  // [功能備註｜62-cpp.23.1 場地快照]
  // 戰鬥中的墨界／血海等領域直接回傳給前端，玩家可看到目前有效場地與剩餘回合。
  snapshot["field_effects"] = activeCombatFieldEffects(*this, roomId);

  const auto events = database_->execSqlSync(R"SQL(
    SELECT jsonb_build_object(
      'id',e.id,'user_id',e.user_id,'type',e.type,'payload',e.payload,
      'created_at',e.created_at,'username',u.username,
      'character_name',COALESCE(c.name,''),
      'avatar_url',CASE
        WHEN COALESCE(c.avatar_url,'')<>'' THEN c.avatar_url
        WHEN e.user_id IS NOT NULL AND EXISTS(SELECT 1 FROM avatar_images ai WHERE ai.user_id=e.user_id)
          THEN '/api/avatar-images/'||e.user_id::text
        ELSE '' END,
      'portrait_url',CASE
        WHEN COALESCE(c.portrait_url,'')<>'' THEN c.portrait_url
        WHEN e.user_id IS NOT NULL AND EXISTS(SELECT 1 FROM portrait_images pi WHERE pi.user_id=e.user_id)
          THEN '/api/portrait-images/'||e.user_id::text
        ELSE '' END
    )::text AS json_text
    FROM (
      SELECT * FROM events WHERE room_id=$1 ORDER BY id DESC LIMIT 100
    ) e
    LEFT JOIN users u ON u.id=e.user_id
    LEFT JOIN character_room_bindings crb ON crb.room_id=$1 AND crb.user_id=e.user_id
    LEFT JOIN character_cards c ON c.id=crb.character_id
    ORDER BY e.id ASC
  )SQL", roomId);
  for (const auto& row : events) {
    snapshot["events"].append(parseJson(row["json_text"].as<std::string>()));
  }
  return snapshot;
}

void CoreService::addEvent(std::int64_t roomId, std::int64_t userId,
                           const std::string& type,
                           const Json::Value& payload) const {
  database_->execSqlSync(
      "INSERT INTO events(room_id,user_id,type,payload) VALUES($1,NULLIF($2,0),$3,$4::jsonb)",
      roomId, userId, type, compactJson(payload));
}

void CoreService::registerRoutes() {
  const auto self = shared_from_this();

  // [功能備註｜系統狀態｜GET /api/health]
  // 用途：Render 健康檢查與資料庫連線狀態。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/health",
      [self](const drogon::HttpRequestPtr&, Callback&& callback) {
        try {
          self->database()->execSqlSync("SELECT 1");
          Json::Value body;
          body["ok"] = true;
          body["service"] = "trpg-online-cpp";
          body["version"] = kVersion;
          body["database"] = "ok";
          body["ready"] = true;
          body["retry_after_ms"] = 0;
          body["realtime"] = "available";
          body["time"] = trantor::Date::now().toDbStringLocal();
          reply(callback, std::move(body));
        } catch (const std::exception& error) {
          LOG_ERROR << "health check failed: " << error.what();
          Json::Value body;
          body["ok"] = false;
          body["service"] = "trpg-online-cpp";
          body["version"] = kVersion;
          body["database"] = "unavailable";
          body["ready"] = false;
          body["retry_after_ms"] = 2500;
          body["realtime"] = "waiting_for_database";
          reply(callback, std::move(body), drogon::k503ServiceUnavailable);
        }
      },
      {drogon::Get});

  // [功能備註｜PWA 更新｜GET /api/version]
  // 用途：已安裝 App 啟動時檢查伺服器版本，讓舊前端可以提示重新整理。
  drogon::app().registerHandler(
      "/api/version",
      [](const drogon::HttpRequestPtr&, Callback&& callback) {
        Json::Value body;
        body["version"] = kVersion;
        body["minimum_client_version"] = kVersion;
        body["force_update"] = false;
        body["pwa_update"] = true;
        reply(callback, std::move(body));
      },
      {drogon::Get});

  // [功能備註｜WebSocket 連線票證｜POST /api/realtime-ticket]
  // 用途：避免把完整 JWT 放進 WebSocket URL；票證兩分鐘有效且握手後即失效。
  drogon::app().registerHandler(
      "/api/realtime-ticket",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback) {
        const auto claims = self->authenticateRequest(request);
        if (!claims) {
          fail(callback, drogon::k401Unauthorized, "登入已失效");
          return;
        }
        try {
          const auto ticket = randomRealtimeTicket();
          self->database()->execSqlSync(
              "DELETE FROM realtime_tickets WHERE expires_at<=CURRENT_TIMESTAMP");
          self->database()->execSqlSync(
              "INSERT INTO realtime_tickets(ticket,user_id,expires_at) "
              "VALUES($1,$2,CURRENT_TIMESTAMP+INTERVAL '2 minutes')",
              ticket, claims->id);
          Json::Value body;
          body["ticket"] = ticket;
          body["expires_in"] = 120;
          reply(callback, std::move(body));
        } catch (const std::exception& error) {
          LOG_ERROR << "realtime ticket failed: " << error.what();
          fail(callback, drogon::k500InternalServerError, "建立即時連線票證失敗");
        }
      },
      {drogon::Post});

  // [功能備註｜系統狀態｜GET /api/cpp/status]
  // 用途：顯示 C++ 移植版本、API 數量與 WebSocket 狀態。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/cpp/status",
      [](const drogon::HttpRequestPtr&, Callback&& callback) {
        Json::Value body;
        body["ok"] = true;
        body["version"] = kVersion;
        body["phase"] = 10;
        body["production_ready"] = false;
        body["code_complete"] = true;
        body["ported_http_route_count"] = kLegacyRouteCount;
        body["legacy_http_route_count"] = kLegacyRouteCount;
        for (const auto* route : kLegacyRouteManifest) body["ported_http_routes"].append(route);
        body["ported_websocket_events"].append("room:enter");
        body["ported_websocket_events"].append("chat:send");
        body["ported_websocket_events"].append("dice:roll");
        reply(callback, std::move(body));
      },
      {drogon::Get});

  // [功能備註｜登入／帳號｜POST /api/auth/register]
  // 用途：建立帳號並初始化角色卡。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/auth/register",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback) {
        runHandler(callback, [&] {
          const auto& body = requireBody(request);
          const auto username = trimmed(stringValue(body["username"]));
          const auto password = stringValue(body["password"]);
          if (!validUsername(username)) {
            throw ApiError(drogon::k400BadRequest,
                           "帳號需為2-20字，可用中文、英文、數字、底線或連字號");
          }
          if (password.size() < 6) {
            throw ApiError(drogon::k400BadRequest, "密碼至少6碼");
          }
          if (password.size() > 72 || password.find('\0') != std::string::npos) {
            throw ApiError(drogon::k400BadRequest, "密碼過長");
          }
          const auto result = self->database()->execSqlSync(
              "INSERT INTO users(username,password_hash,is_admin) VALUES($1,$2,FALSE) "
              "ON CONFLICT(username) DO NOTHING RETURNING id",
              username, hashPassword(password, 10));
          if (result.empty()) {
            throw ApiError(drogon::k409Conflict, "帳號已存在");
          }
          const auto userId = result[0]["id"].as<std::int64_t>();
          self->database()->execSqlSync(
              "INSERT INTO character_cards(user_id) VALUES($1) ON CONFLICT(user_id) DO NOTHING",
              userId);
          JwtClaims claims{userId, username, false, unixNow(), unixNow() + 604800};
          Json::Value response;
          response["token"] = signJwt(claims, self->config().jwtSecret);
          response["user"]["id"] = Json::Int64(userId);
          response["user"]["username"] = username;
          response["user"]["is_admin"] = false;
          reply(callback, std::move(response));
        }, "註冊失敗");
      },
      {drogon::Post});

  // [功能備註｜登入／帳號｜POST /api/auth/login]
  // 用途：處理玩家／DM 登入與 JWT 簽發。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/auth/login",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback) {
        runHandler(callback, [&] {
          const auto& body = requireBody(request);
          const auto username = trimmed(stringValue(body["username"]));
          const auto password = stringValue(body["password"]);
          const auto rows = self->database()->execSqlSync(
              "SELECT id,username,password_hash,is_admin FROM users WHERE username=$1",
              username);
          if (rows.empty() ||
              !verifyPassword(password, rows[0]["password_hash"].as<std::string>())) {
            throw ApiError(drogon::k401Unauthorized, "帳號或密碼錯誤");
          }
          const auto userId = rows[0]["id"].as<std::int64_t>();
          const auto safeUsername = rows[0]["username"].as<std::string>();
          const auto isAdmin = rows[0]["is_admin"].as<bool>();
          self->database()->execSqlSync(
              "INSERT INTO character_cards(user_id) VALUES($1) ON CONFLICT(user_id) DO NOTHING",
              userId);
          JwtClaims claims{userId, safeUsername, isAdmin, unixNow(), unixNow() + 604800};
          Json::Value response;
          response["token"] = signJwt(claims, self->config().jwtSecret);
          response["user"]["id"] = Json::Int64(userId);
          response["user"]["username"] = safeUsername;
          response["user"]["is_admin"] = isAdmin;
          reply(callback, std::move(response));
        }, "登入失敗");
      },
      {drogon::Post});

  // [功能備註｜登入／帳號｜GET /api/me]
  // 用途：讀取/顯示登入／帳號相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/me",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request);
          Json::Value response;
          response["user"]["id"] = Json::Int64(user.id);
          response["user"]["username"] = user.username;
          response["user"]["is_admin"] = user.isAdmin;
          reply(callback, std::move(response));
        }, "讀取使用者資料失敗");
      },
      {drogon::Get});

  // [功能備註｜DM 權限｜GET /api/admin/status]
  // 用途：讀取/顯示DM 權限相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/admin/status",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request, true);
          Json::Value response;
          response["ok"] = true;
          response["username"] = user.username;
          response["is_admin"] = true;
          response["configured_admin"] = user.username == self->config().adminUsername;
          reply(callback, std::move(response));
        }, "讀取 DM 狀態失敗");
      },
      {drogon::Get});

  // [功能備註｜角色卡｜GET /api/character]
  // 用途：讀取目前登入玩家的完整角色卡。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/character",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request);
          Json::Value response;
          response["character"] = self->characterByUser(user.id);
          reply(callback, std::move(response));
        }, "讀取角色卡失敗");
      },
      {drogon::Get});

  // [功能備註｜角色卡｜POST /api/character]
  // 用途：建立/執行角色卡相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/character",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request);
          const auto existing = self->database()->execSqlSync(
              "SELECT id FROM character_cards WHERE user_id=$1", user.id);
          if (!existing.empty()) {
            throw ApiError(drogon::k409Conflict, "你已經有角色卡");
          }
          const auto& body = requireBody(request);
          auto name = clipped(trimmed(stringValue(body["name"], "未命名角色")), 50);
          if (name.empty()) name = "未命名角色";
          const auto gender = clipped(trimmed(stringValue(body["gender"])), 40);
          const auto birthday = clipped(trimmed(stringValue(body["birthday"])), 40);
          const auto faction = stringValue(body["faction"]) == "西國" ? "西國" : "東國";
          const auto age = std::min<std::int64_t>(9999, nonNegative(body["age"]));
          const auto height = std::min<std::int64_t>(9999, nonNegative(body["height_cm"]));
          self->database()->execSqlSync(
              "INSERT INTO character_cards(user_id,name,name_locked,gender,age,height_cm,birthday,faction) "
              "VALUES($1,$2,$3,$4,$5,$6,$7,$8)",
              user.id, name, name != "未命名角色", gender, age, height, birthday,
              faction);
          Json::Value response;
          response["character"] = self->characterByUser(user.id);
          reply(callback, std::move(response));
        }, "建立角色卡失敗");
      },
      {drogon::Post});

  // [功能備註｜角色卡｜PATCH /api/character]
  // 用途：玩家修改允許自行調整的角色資料與屬性上限。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/character",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request);
          const auto& body = requireBody(request);
          const std::set<std::string> allowed = {
              "name",       "gender",          "age",      "height_cm",
              "birthday",   "self_intro",      "agility",  "strength",
              "constitution", "spirit",        "great_way", "faith",
              "attribute_points"};
          const auto memberNames = body.getMemberNames();
          if (memberNames.empty()) {
            throw ApiError(drogon::k400BadRequest, "沒有可更新欄位");
          }
          for (const auto& name : memberNames) {
            if (!allowed.contains(name)) {
              throw ApiError(
                  drogon::k403Forbidden,
                  "玩家只能修改角色基本資料、可分配屬性與大道／信仰上限；目前剩餘值由 DM、技能或事件處理");
            }
          }
          auto current = self->characterByUser(user.id);
          if (current.isNull()) throw ApiError(drogon::k404NotFound, "找不到角色卡");

          auto name = stringValue(current["name"], "未命名角色");
          bool nameLocked = current.get("name_locked", false).asBool();
          if (body.isMember("name")) {
            const auto nextName = clipped(trimmed(stringValue(body["name"])), 50);
            if (nextName.empty()) throw ApiError(drogon::k400BadRequest, "角色名稱不能為空");
            if (nextName != name && nameLocked) {
              throw ApiError(drogon::k403Forbidden,
                             "角色名稱第一次設定後就不能再由玩家修改");
            }
            if (nextName != name) {
              name = nextName;
              nameLocked = true;
            }
          }
          auto gender = stringValue(current["gender"]);
          auto birthday = stringValue(current["birthday"]);
          auto selfIntro = stringValue(current["self_intro"]);
          auto age = nonNegative(current["age"]);
          auto height = nonNegative(current["height_cm"]);
          if (body.isMember("gender"))
            gender = clipped(trimmed(stringValue(body["gender"])), 40);
          if (body.isMember("birthday"))
            birthday = clipped(trimmed(stringValue(body["birthday"])), 40);
          if (body.isMember("self_intro"))
            selfIntro = clipped(trimmed(stringValue(body["self_intro"])), 4000);
          if (body.isMember("age"))
            age = std::min<std::int64_t>(9999, nonNegative(body["age"]));
          if (body.isMember("height_cm"))
            height = std::min<std::int64_t>(9999, nonNegative(body["height_cm"]));

          // [功能備註｜64-cpp.25 五項基礎屬性] 基礎點數只分配到敏捷／力量／體質／精神／大道(東國)或神明信仰(西國)。
          // 幸運屬於出生鎖定的特殊屬性，玩家不能用屬性點修改。
          const std::array<const char*, 4> fields = {"agility", "strength",
                                                     "constitution", "spirit"};
          std::array<std::int64_t, 4> attributes{};
          const auto tier = std::clamp<std::int64_t>(integerValue(current["tier"], 0), 0, 10);
          const auto fixedBudget = characterTierBudget(tier);
          const auto budget = std::max<std::int64_t>(fixedBudget, nonNegative(current["attribute_budget"], fixedBudget));
          const auto oldPoints = nonNegative(current["attribute_points"]);
          const auto newPoints = body.isMember("attribute_points")
                                     ? nonNegative(body["attribute_points"])
                                     : oldPoints;
          for (std::size_t index = 0; index < fields.size(); ++index) {
            attributes[index] = body.isMember(fields[index])
                                    ? nonNegative(body[fields[index]])
                                    : nonNegative(current[fields[index]]);
            if (attributes[index] > budget) {
              throw ApiError(drogon::k400BadRequest, "單項基礎屬性不能超過角色目前可分配的總屬性額度");
            }
          }

          const auto faction = stringValue(current["faction"], "東國");
          auto greatWay = nonNegative(current["great_way"]);
          auto faith = nonNegative(current["faith"]);
          const bool west = faction == "西國";
          const char* allowedFifth = west ? "faith" : "great_way";
          const char* wrongFifth = west ? "great_way" : "faith";
          const char* currentFifth = west ? "current_faith" : "current_great_way";
          if (body.isMember(wrongFifth)) {
            throw ApiError(drogon::k403Forbidden,
                           west ? "你的角色只能分配神明／信仰屬性"
                                : "你的角色只能分配大道屬性");
          }
          if (body.isMember(allowedFifth)) {
            const auto nextMax = nonNegative(body[allowedFifth]);
            const auto oldMax = west ? faith : greatWay;
            const auto currentValue = current[currentFifth].isNull()
                                          ? oldMax
                                          : nonNegative(current[currentFifth]);
            if (nextMax < currentValue) {
              throw ApiError(drogon::k400BadRequest,
                             "第五基礎屬性不能低於目前剩餘值；請先由技能、效果或 DM 降低目前值");
            }
            if (nextMax > budget) {
              throw ApiError(drogon::k400BadRequest, "第五基礎屬性不能超過角色目前可分配的總屬性額度");
            }
            if (west) faith = nextMax; else greatWay = nextMax;
          }

          const bool editsAttributes = body.isMember("agility") || body.isMember("strength") ||
              body.isMember("constitution") || body.isMember("spirit") || body.isMember(allowedFifth) ||
              body.isMember("attribute_points");
          if (editsAttributes) {
            const auto fifth = west ? faith : greatWay;
            const auto spent = attributes[0] + attributes[1] + attributes[2] + attributes[3] + fifth;
            if (spent > budget || spent + newPoints != budget) {
              throw ApiError(drogon::k400BadRequest,
                             "屬性點數不正確：五項基礎屬性與剩餘點數總和必須等於角色目前可分配總額");
            }
          }

          self->database()->execSqlSync(R"SQL(
            UPDATE character_cards SET
              name=$1,name_locked=$2,gender=$3,age=$4,height_cm=$5,
              birthday=$6,self_intro=$7,agility=$8,strength=$9,
              constitution=$10,spirit=$11,attribute_points=$12,
              great_way=$13,faith=$14,updated_at=CURRENT_TIMESTAMP
            WHERE user_id=$15
          )SQL", name, nameLocked, gender, age, height, birthday, selfIntro,
              attributes[0], attributes[1], attributes[2], attributes[3], newPoints,
              greatWay, faith, user.id);
          Json::Value response;
          response["character"] = self->characterByUser(user.id);
          reply(callback, std::move(response));
        }, "更新角色卡失敗");
      },
      {drogon::Patch});

  // [功能備註｜怪物｜GET /api/admin/monster-templates]
  // 用途：讀取/顯示怪物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/admin/monster-templates",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback) {
        runHandler(callback, [&] {
          requireCurrentUser(self, request, true);
          const auto rows = self->database()->execSqlSync(
              "SELECT row_to_json(mt)::text AS json_text FROM monster_templates mt ORDER BY id");
          Json::Value response;
          response["monsters"] = Json::arrayValue;
          for (const auto& row : rows) {
            response["monsters"].append(parseJson(row["json_text"].as<std::string>()));
          }
          reply(callback, std::move(response));
        }, "讀取怪物卡失敗");
      },
      {drogon::Get});

  // [功能備註｜怪物｜POST /api/admin/monster-templates]
  // 用途：建立/執行怪物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/admin/monster-templates",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback) {
        runHandler(callback, [&] {
          requireCurrentUser(self, request, true);
          const auto& body = requireBody(request);
          const auto name = clipped(trimmed(stringValue(body["name"])), 120);
          if (name.empty()) throw ApiError(drogon::k400BadRequest, "請填怪物名稱");
          const auto category = clipped(stringValue(body["category"], "怪物"), 80);
          const auto rank = clipped(stringValue(body["rank"], "G"), 10);
          const auto description = clipped(stringValue(body["description"]), 4000);
          const auto publicHint = clipped(stringValue(body["public_hint"]), 4000);
          const auto imageUrl = clipped(stringValue(body["image_url"]), 4096);
          const auto investigationDc = std::clamp<std::int64_t>(integerValue(body["investigation_dc"], 50), 1, 100);
          Json::Value attributes = body["attributes"].isObject() ? body["attributes"]
                                                                 : Json::Value{Json::objectValue};
          // [功能備註｜60-cpp.21 怪物五維] 力量／敏捷／體質／精神／幸運全部由 C++ 正規化為非負值。
          for (const auto* key : {"strength","agility","constitution","spirit","luck"})
            attributes[key] = Json::Int64(nonNegative(attributes[key]));
          // 73-cpp.34.10：全域衍生屬性規則。生命不可手填，固定由體質計算。
          const auto constitution = nonNegative(attributes["constitution"]);
          const auto spiritValue = nonNegative(attributes["spirit"]);
          const auto maxHp = constitution * 2;
          const auto endurance = (constitution + spiritValue) / 2;
          const auto will = (spiritValue * 8) / 10;
          const auto sanity = (spiritValue * 12) / 10;
          Json::Value config = body["config"].isObject() ? body["config"]
                                                         : Json::Value{Json::objectValue};
          config["derived_stats"]["max_hp"] = Json::Int64(maxHp); config["derived_stats"]["endurance"] = Json::Int64(endurance); config["derived_stats"]["will"] = Json::Int64(will); config["derived_stats"]["sanity"] = Json::Int64(sanity);
          if (!config["special_stats"].isObject()) { config["special_stats"]["endurance"] = true; config["special_stats"]["will"] = true; config["special_stats"]["sanity"] = true; }
          // 相容完整世界編輯器原本的頂層欄位，避免建立／修改後戰鬥設定消失。
          for (const auto* key : {"element","combat_attack_attribute","combat_damage_formula","combat_damage_target",
                                  "combat_accuracy_bonus","combat_defense_attribute","combat_dodge_attribute",
                                  "hearing_threshold","boss_enabled","secret","ai_actions"})
            if (body.isMember(key)) config[key] = body[key];
          Json::Value skills = body["skills"].isArray() ? body["skills"] : Json::Value{Json::arrayValue};
          Json::Value passiveSkills = body["passive_skills"].isArray() ? body["passive_skills"] : Json::Value{Json::arrayValue};
          Json::Value greatWay = body["great_way"].isObject() ? body["great_way"] : Json::Value{Json::objectValue};
          Json::Value bossPhases = body["boss_phases"].isArray() ? body["boss_phases"]
                                                                  : (body["boss_phases"].isNull() && config["boss_phases"].isArray() ? config["boss_phases"] : Json::Value{Json::arrayValue});
          config["boss_phases"] = bossPhases; // 舊前端／舊 Boss 流程相容。
          const auto drops = body["drops"].isArray() ? body["drops"] : Json::Value{Json::arrayValue};
          const auto elementAffinity = body["element_affinity"].isObject() ? body["element_affinity"] : Json::Value{Json::objectValue};
          const auto elementResistance = body["element_resistance"].isObject() ? body["element_resistance"] : Json::Value{Json::objectValue};
          const auto elementWeakness = body["element_weakness"].isObject() ? body["element_weakness"] : Json::Value{Json::objectValue};
          const auto effects = body["effects"].isObject() ? body["effects"] : Json::Value{Json::objectValue};
          const auto aiLevel = std::clamp<std::int64_t>(integerValue(body["ai_level"], 1), 0, 100);
          const auto aiBehavior = clipped(stringValue(body["ai_behavior"], "balanced"), 40);
          const auto inserted = self->database()->execSqlSync(R"SQL(
            INSERT INTO monster_templates(name,category,rank,description,public_hint,image_url,max_hp,investigation_dc,
              attributes,skills,passive_skills,great_way,boss_phases,drops,ai_level,ai_behavior,config,element_affinity,element_resistance,element_weakness,effects)
            VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9::jsonb,$10::jsonb,$11::jsonb,$12::jsonb,$13::jsonb,$14::jsonb,$15,$16,$17::jsonb,$18::jsonb,$19::jsonb,$20::jsonb,$21::jsonb) RETURNING id
          )SQL", name, category, rank, description, publicHint, imageUrl, maxHp, investigationDc,
              compactJson(attributes), compactJson(skills), compactJson(passiveSkills), compactJson(greatWay), compactJson(bossPhases),
              compactJson(drops), aiLevel, aiBehavior, compactJson(config), compactJson(elementAffinity), compactJson(elementResistance), compactJson(elementWeakness), compactJson(effects));
          const auto id = inserted[0]["id"].as<std::int64_t>();
          const auto rows = self->database()->execSqlSync(
              "SELECT row_to_json(mt)::text AS json_text FROM monster_templates mt WHERE id=$1",
              id);
          Json::Value response;
          response["monster"] = parseJson(rows[0]["json_text"].as<std::string>());
          reply(callback, std::move(response));
        }, "建立怪物卡失敗");
      },
      {drogon::Post});

  // [功能備註｜怪物｜DELETE /api/admin/monster-templates/{1}]
  // 用途：刪除怪物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/admin/monster-templates/{1}",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& monsterIdText) {
        runHandler(callback, [&] {
          requireCurrentUser(self, request, true);
          const auto monsterId = parsePositiveId(monsterIdText, "怪物模板 ID 無效");
          const auto deleted = self->database()->execSqlSync(
              "DELETE FROM monster_templates WHERE id=$1 RETURNING id", monsterId);
          if (deleted.empty()) throw ApiError(drogon::k404NotFound, "找不到怪物卡");
          Json::Value response;
          response["ok"] = true;
          reply(callback, std::move(response));
        }, "刪除怪物卡失敗");
      },
      {drogon::Delete});

  // [功能備註｜怪物｜POST /api/admin/rooms/{1}/monsters]
  // 用途：建立/執行怪物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/admin/rooms/{1}/monsters",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& roomIdText) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request, true);
          const auto roomId = parseRoomId(roomIdText);
          const auto& body = requireBody(request);
          const auto templateId = integerValue(body["template_id"]);
          if (templateId <= 0) throw ApiError(drogon::k400BadRequest, "請選擇怪物模板");
          if (self->database()->execSqlSync("SELECT 1 FROM rooms WHERE id=$1", roomId).empty()) {
            throw ApiError(drogon::k404NotFound, "找不到房間");
          }
          const auto templates = self->database()->execSqlSync(
              "SELECT id,name,max_hp,attributes::text AS attributes_text,great_way::text AS great_way_text "
              "FROM monster_templates WHERE id=$1", templateId);
          if (templates.empty()) throw ApiError(drogon::k404NotFound, "找不到怪物卡");
          const auto attributes = parseJson(templates[0]["attributes_text"].as<std::string>());
          const auto hp = std::max<std::int64_t>(1, templates[0]["max_hp"].as<std::int64_t>());
          const auto spirit = nonNegative(attributes["spirit"]);
          const auto greatWay = parseJson(templates[0]["great_way_text"].as<std::string>(), Json::Value{Json::objectValue});
          // [功能備註｜60-cpp.21 怪物大道] 有大道的怪物以大道資源上限初始化第五資源；沒有則保留舊 attributes fifth 相容。
          const auto fifth = greatWay.get("enabled", false).asBool()
              ? std::max<std::int64_t>(0, integerValue(greatWay["resource_max"]))
              : std::max(nonNegative(attributes["great_way"]),
                         std::max(nonNegative(attributes["faith"]), nonNegative(attributes["fifth"])));
          const auto inserted = self->database()->execSqlSync(R"SQL(
            INSERT INTO room_monsters(
              room_id,monster_template_id,status,current_hp,max_hp,
              current_spirit,max_spirit,current_fifth,max_fifth
            ) VALUES($1,$2,'hidden',$3,$3,$4,$4,$5,$5) RETURNING id
          )SQL", roomId, templateId, hp, spirit, fifth);
          const auto instanceId = inserted[0]["id"].as<std::int64_t>();
          Json::Value event;
          event["text"] = "DM 將怪物放入房間：" + templates[0]["name"].as<std::string>();
          event["monster_id"] = Json::Int64(instanceId);
          self->addEvent(roomId, user.id, "system", event);
          RoomSocket::broadcastSnapshotForRoom(roomId);
          Json::Value response;
          response["ok"] = true;
          response["instance_id"] = Json::Int64(instanceId);
          response["room"] = self->roomSnapshot(roomId);
          reply(callback, std::move(response));
        }, "加入怪物失敗");
      },
      {drogon::Post});

  // [功能備註｜怪物｜POST /api/admin/rooms/{1}/monsters/{2}/encounter]
  // 用途：建立/執行怪物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/admin/rooms/{1}/monsters/{2}/encounter",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& roomIdText, const std::string& monsterIdText) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request, true);
          const auto roomId = parseRoomId(roomIdText);
          const auto monsterId = parsePositiveId(monsterIdText, "怪物 ID 無效");
          const auto updated = self->database()->execSqlSync(
              "UPDATE room_monsters SET status='encountered',updated_at=CURRENT_TIMESTAMP "
              "WHERE room_id=$1 AND id=$2 RETURNING id",
              roomId, monsterId);
          if (updated.empty()) throw ApiError(drogon::k404NotFound, "找不到房間怪物");
          Json::Value event;
          event["text"] = "⚠️ 玩家遭遇了怪物。";
          event["monster_id"] = Json::Int64(monsterId);
          self->addEvent(roomId, user.id, "encounter", event);
          RoomSocket::broadcastSnapshotForRoom(roomId);
          Json::Value response;
          response["ok"] = true;
          response["room"] = self->roomSnapshot(roomId);
          reply(callback, std::move(response));
        }, "設定遭遇失敗");
      },
      {drogon::Post});

  // [功能備註｜怪物｜DELETE /api/admin/rooms/{1}/monsters/{2}]
  // 用途：刪除怪物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/admin/rooms/{1}/monsters/{2}",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& roomIdText, const std::string& monsterIdText) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request, true);
          const auto roomId = parseRoomId(roomIdText);
          const auto monsterId = parsePositiveId(monsterIdText, "怪物 ID 無效");
          const auto deleted = self->database()->execSqlSync(
              "DELETE FROM room_monsters WHERE room_id=$1 AND id=$2 RETURNING id",
              roomId, monsterId);
          if (deleted.empty()) throw ApiError(drogon::k404NotFound, "找不到房間怪物");
          endBattleIfResolved(self, roomId, user.id);
          RoomSocket::broadcastSnapshotForRoom(roomId);
          Json::Value response;
          response["ok"] = true;
          response["room"] = self->roomSnapshot(roomId);
          reply(callback, std::move(response));
        }, "移除怪物失敗");
      },
      {drogon::Delete});

  // [功能備註｜房間／跑團｜POST /api/rooms]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/rooms",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request, true);
          const auto& body = requireBody(request);
          auto name = clipped(trimmed(stringValue(body["name"], "新的跑團")), 60);
          if (name.empty()) name = "新的跑團";

          std::int64_t roomId = 0;
          std::string roomCode;
          for (int attempt = 0; attempt < 20 && roomId == 0; ++attempt) {
            roomCode = randomRoomCode();
            const auto inserted = self->database()->execSqlSync(
                "INSERT INTO rooms(code,name,owner_id) VALUES($1,$2,$3) "
                "ON CONFLICT(code) DO NOTHING RETURNING id",
                roomCode, name, user.id);
            if (!inserted.empty()) roomId = inserted[0]["id"].as<std::int64_t>();
          }
          if (roomId == 0) throw std::runtime_error("unable to allocate room code");
          self->database()->execSqlSync(
              "INSERT INTO room_members(room_id,user_id,role,display_name) "
              "VALUES($1,$2,'gm',$3) ON CONFLICT(room_id,user_id) DO UPDATE SET role='gm',display_name=EXCLUDED.display_name",
              roomId, user.id, user.username);
          // [功能備註｜60-cpp.21.3 建立房間核心優先] 角色綁定與事件紀錄是附加功能，失敗不得回滾已建立的房間。
          try {
            const auto character = self->characterByUser(user.id);
            if (!character.isNull()) {
              self->database()->execSqlSync(
                  "INSERT INTO character_room_bindings(room_id,character_id,user_id) "
                  "VALUES($1,$2,$3) ON CONFLICT(room_id,user_id) DO NOTHING",
                  roomId, integerValue(character["id"]), user.id);
            }
          } catch (const std::exception& error) {
            LOG_WARN << "room character binding degraded for room " << roomId << ": " << error.what();
          }
          try {
            Json::Value event;
            event["text"] = "DM 建立了跑團房間";
            event["code"] = roomCode;
            self->addEvent(roomId, user.id, "system", event);
          } catch (const std::exception& error) {
            LOG_WARN << "room create event degraded for room " << roomId << ": " << error.what();
          }
          reply(callback, resilientRoomSnapshot(self, roomId));
        }, "建立房間失敗");
      },
      {drogon::Post});

  // [功能備註｜房間／跑團｜POST /api/rooms/join]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/rooms/join",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request);
          const auto& body = requireBody(request);
          auto code = trimmed(stringValue(body["code"]));
          std::transform(code.begin(), code.end(), code.begin(),
                         [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
          if (code.size() != 6) {
            throw ApiError(drogon::k400BadRequest, "房間代碼必須是6碼");
          }
          const auto rooms = self->database()->execSqlSync(
              "SELECT id,status FROM rooms WHERE code=$1", code);
          if (rooms.empty()) throw ApiError(drogon::k404NotFound, "找不到房間");
          if (rooms[0]["status"].as<std::string>() == "closed") {
            throw ApiError(drogon::k409Conflict, "房間已關閉");
          }
          const auto roomId = rooms[0]["id"].as<std::int64_t>();
          // [功能備註｜60-cpp.21.3 加入房間核心優先] 加入房間只查角色卡核心欄位，避免 characterByUser() 的後期裝備／元素／學習系統拖垮 join。
          const auto characters = self->database()->execSqlSync(
              "SELECT id,name FROM character_cards WHERE user_id=$1 LIMIT 1", user.id);
          if (characters.empty()) throw ApiError(drogon::k409Conflict, "請先建立角色卡");
          const auto characterId = characters[0]["id"].as<std::int64_t>();
          const auto displayName = clipped(characters[0]["name"].as<std::string>(), 30);
          self->database()->execSqlSync(
              "INSERT INTO room_members(room_id,user_id,role,display_name) "
              "VALUES($1,$2,'player',$3) ON CONFLICT(room_id,user_id) DO UPDATE SET "
              "display_name=EXCLUDED.display_name",
              roomId, user.id, displayName.empty() ? user.username : displayName);
          try {
            self->database()->execSqlSync(
                "INSERT INTO character_room_bindings(room_id,character_id,user_id) "
                "VALUES($1,$2,$3) ON CONFLICT(room_id,user_id) DO UPDATE SET "
                "character_id=EXCLUDED.character_id",
                roomId, characterId, user.id);
          } catch (const std::exception& error) {
            LOG_WARN << "room join character binding degraded for room " << roomId << ": " << error.what();
          }
          try {
            Json::Value event;
            event["text"] = (displayName.empty() ? user.username : displayName) + " 加入了房間";
            self->addEvent(roomId, user.id, "system", event);
          } catch (const std::exception& error) {
            LOG_WARN << "room join event degraded for room " << roomId << ": " << error.what();
          }
          RoomSocket::broadcastSnapshotForRoom(roomId);
          reply(callback, resilientRoomSnapshot(self, roomId));
        }, "加入房間失敗");
      },
      {drogon::Post});

  // [功能備註｜房間／跑團｜GET /api/rooms]
  // 用途：讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/rooms",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request);
          // 55-cpp.16：房間清單只查核心穩定欄位；避免後期 schema 漂移讓整個 /api/rooms 500。
          const std::string baseSql = R"SQL(
            SELECT jsonb_build_object(
              'id',r.id,'code',r.code,'name',r.name,'owner_id',r.owner_id,
              'status',r.status,'round',r.round,'current_actor_id',r.current_actor_id,
              'saved',r.saved,'created_at',r.created_at,'updated_at',r.updated_at,
              'closed_at',to_jsonb(r)->'closed_at','owner_username',u.username,
              'member_count',(SELECT COUNT(*) FROM room_members x WHERE x.room_id=r.id)
            )::text AS json_text
            FROM rooms r JOIN users u ON u.id=r.owner_id
          )SQL";
          const auto rows = user.isAdmin
              ? self->database()->execSqlSync(baseSql + " ORDER BY r.id DESC")
              : self->database()->execSqlSync(
                    baseSql + " WHERE EXISTS(SELECT 1 FROM room_members rm WHERE rm.room_id=r.id AND rm.user_id=$1) ORDER BY r.id DESC",
                    user.id);
          Json::Value response;
          response["rooms"] = Json::arrayValue;
          for (const auto& row : rows) {
            response["rooms"].append(parseJson(row["json_text"].as<std::string>()));
          }
          reply(callback, std::move(response));
        }, "讀取房間失敗");
      },
      {drogon::Get});

  // [功能備註｜房間／跑團｜GET /api/rooms/{1}]
  // 用途：讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/rooms/{1}",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& roomIdText) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request);
          const auto roomId = parseRoomId(roomIdText);
          if (!self->userCanAccessRoom(user.id, roomId)) {
            throw ApiError(drogon::k403Forbidden, "你不是這個房間的成員");
          }
          try {
            reply(callback, roomSnapshotForViewer(self, roomId, user.id));
          } catch (const std::exception& error) {
            LOG_WARN << "GET room viewer snapshot degraded for room " << roomId << ": " << error.what();
            reply(callback, resilientRoomSnapshot(self, roomId));
          }
        }, "讀取房間失敗");
      },
      {drogon::Get});

  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/start]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/rooms/{1}/start",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& roomIdText) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request);
          const auto roomId = parseRoomId(roomIdText);
          if (!userCanManageRoom(self, user, roomId)) {
            throw ApiError(drogon::k403Forbidden, "只有房主／房間 GM／全站 DM 可以開團");
          }
          // [功能備註｜60-cpp.21.3 開團核心優先] 核心狀態先成功；事件、戰鬥重置、完整快照全部可降級。
          // 這樣舊資料庫若某個後期戰鬥欄位尚未補齊，也不會讓「DM 開團」整體失敗。
          const auto updated = self->database()->execSqlSync(
              "UPDATE rooms SET status='active',saved=FALSE,round=1,updated_at=CURRENT_TIMESTAMP,"
              "closed_at=NULL WHERE id=$1 RETURNING id", roomId);
          if (updated.empty()) throw ApiError(drogon::k404NotFound, "找不到房間");
          try {
            self->database()->execSqlSync(
                "UPDATE rooms SET battle_active=FALSE,current_actor_id=NULL,current_actor_type=NULL,"
                "current_actor_ref_id=NULL,combat_order='[]'::jsonb,combat_index=0 "
                "WHERE id=$1", roomId);
          } catch (const std::exception& error) {
            LOG_WARN << "room start combat reset degraded for room " << roomId << ": " << error.what();
          }
          try {
            Json::Value event;
            event["text"] = "DM 開始了跑團";
            self->addEvent(roomId, user.id, "system", event);
          } catch (const std::exception& error) {
            LOG_WARN << "room start event degraded for room " << roomId << ": " << error.what();
          }
          RoomSocket::broadcastSnapshotForRoom(roomId);
          reply(callback, resilientRoomSnapshot(self, roomId));
        }, "開團失敗");
      },
      {drogon::Post});

  // [功能備註｜戰鬥／回合｜POST /api/rooms/{1}/battle/start]
  // 用途：建立戰鬥行動序並開始戰鬥。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/rooms/{1}/battle/start",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& roomIdText) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request, true);
          const auto roomId = parseRoomId(roomIdText);
          const auto rooms = self->database()->execSqlSync(
              "SELECT status,battle_active FROM rooms WHERE id=$1", roomId);
          if (rooms.empty()) throw ApiError(drogon::k404NotFound, "找不到房間");
          if (rooms[0]["status"].as<std::string>() != "active") {
            throw ApiError(drogon::k400BadRequest, "請先開始跑團");
          }
          if (rooms[0]["battle_active"].as<bool>()) {
            throw ApiError(drogon::k400BadRequest, "目前已經在戰鬥中");
          }

          self->database()->execSqlSync(R"SQL(
            UPDATE room_members rm SET
              max_hp=GREATEST(1,GREATEST(cc.constitution,0)*2),
              hp=LEAST(rm.hp,GREATEST(1,GREATEST(cc.constitution,0)*2)),
              max_spirit=GREATEST(0,cc.spirit),
              spirit=CASE WHEN rm.max_spirit<=0 THEN GREATEST(0,cc.spirit) ELSE LEAST(rm.spirit,GREATEST(0,cc.spirit)) END,
              max_sanity=GREATEST(1,(GREATEST(cc.spirit,0)*12)/10),
              current_sanity=CASE WHEN rm.max_sanity<=1 THEN GREATEST(1,(GREATEST(cc.spirit,0)*12)/10) ELSE LEAST(rm.current_sanity,GREATEST(1,(GREATEST(cc.spirit,0)*12)/10)) END,
              combat_state=CASE WHEN rm.hp>0 THEN 'active' ELSE 'downed' END,
              skill_charge_state='{}'::jsonb
            FROM character_cards cc
            WHERE rm.room_id=$1 AND rm.role='player' AND cc.user_id=rm.user_id
          )SQL", roomId);

          const auto order = buildCombatOrder(self, roomId);
          bool ally = false;
          bool enemy = false;
          for (const auto& actor : order) {
            const auto side = combatActorSide(actor);
            ally = ally || side == "ally";
            enemy = enemy || side == "enemy";
          }
          if (!ally) {
            throw ApiError(drogon::k400BadRequest,
                           "至少需要一名仍可戰鬥的玩家或友方 NPC");
          }
          if (!enemy) {
            throw ApiError(drogon::k400BadRequest,
                           "請先讓至少一隻怪物進入遭遇狀態，或加入敵方 NPC");
          }

          resetCombatActions(self, roomId, order);
          const auto first = order[0];
          const auto firstType = stringValue(first["type"]);
          const auto firstId = integerValue(first["id"]);
          if (firstType == "player") {
            self->database()->execSqlSync(
                "UPDATE rooms SET battle_active=TRUE,round=1,current_actor_type=$1,"
                "current_actor_ref_id=$2,current_actor_id=$2,combat_index=0,"
                "combat_order=$3::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=$4",
                firstType, firstId, compactJson(order), roomId);
          } else {
            self->database()->execSqlSync(
                "UPDATE rooms SET battle_active=TRUE,round=1,current_actor_type=$1,"
                "current_actor_ref_id=$2,current_actor_id=NULL,combat_index=0,"
                "combat_order=$3::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=$4",
                firstType, firstId, compactJson(order), roomId);
          }
          Json::Value event;
          event["text"] = "⚔️ DM 開始戰鬥。由 " + stringValue(first["name"], "單位") +
                          " 先行動。";
          self->addEvent(roomId, user.id, "system", event);
          Json::Value combatRuleEvent; combatRuleEvent["source"] = "dm"; combatRuleEvent["first_actor"] = first; combatRuleEvent["text"] = event["text"];
          evaluateRuleEvent(self, roomId, user.id, "COMBAT_STARTED", combatRuleEvent, false);
          // [功能備註｜62-cpp.23.1 戰鬥開始自動技能]
          try { combatRuleEvent["actor_user_id"] = Json::Int64(user.id); dispatchEventSkillEvent(self, roomId, "system", 0, "COMBAT_STARTED", combatRuleEvent); }
          catch (const std::exception& error) { LOG_WARN << "DM combat start event skill degraded: " << error.what(); }
          // [功能備註｜通知中心] 戰鬥開始時若第一位是玩家，立即留下「輪到你」通知。
          if (firstType == "player") {
            Json::Value data; data["room_id"] = Json::Int64(roomId); data["round"] = 1;
            self->database()->execSqlSync(
                "INSERT INTO user_notifications(user_id,notification_type,title,message,data) "
                "VALUES($1,'turn','輪到你行動',$2,$3::jsonb)",
                firstId, "戰鬥已開始，現在輪到你行動。", compactJson(data));
          }
          RoomSocket::broadcastSnapshotForRoom(roomId);
          reply(callback, self->roomSnapshot(roomId));
        }, "無法開始戰鬥");
      },
      {drogon::Post});

  // [功能備註｜戰鬥／回合｜POST /api/rooms/{1}/battle/end]
  // 用途：建立/執行戰鬥／回合相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/rooms/{1}/battle/end",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& roomIdText) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request, true);
          const auto roomId = parseRoomId(roomIdText);
          const auto rooms = self->database()->execSqlSync(
              "SELECT battle_active FROM rooms WHERE id=$1", roomId);
          if (rooms.empty()) throw ApiError(drogon::k404NotFound, "找不到房間");
          if (!rooms[0]["battle_active"].as<bool>()) {
            throw ApiError(drogon::k400BadRequest, "目前不是戰鬥階段");
          }
          finishBattle(self, roomId, user.id, "🕯️ 戰鬥結束，回到探索階段。");
          Json::Value combatRuleEvent; combatRuleEvent["source"] = "dm"; combatRuleEvent["reason"] = "DM 手動結束";
          evaluateRuleEvent(self, roomId, user.id, "COMBAT_ENDED", combatRuleEvent, false);
          RoomSocket::broadcastSnapshotForRoom(roomId);
          reply(callback, self->roomSnapshot(roomId));
        }, "無法結束戰鬥");
      },
      {drogon::Post});

  // [功能備註｜戰鬥／回合｜POST /api/rooms/{1}/next-turn]
  // 用途：推進目前戰鬥角色與回合。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/rooms/{1}/next-turn",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& roomIdText) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request, true);
          const auto roomId = parseRoomId(roomIdText);
          const auto room = self->database()->execSqlSync(
              "SELECT status,battle_active FROM rooms WHERE id=$1", roomId);
          if (room.empty()) throw ApiError(drogon::k404NotFound, "找不到房間");
          if (room[0]["status"].as<std::string>() != "active") {
            throw ApiError(drogon::k400BadRequest, "跑團尚未開始");
          }
          if (!room[0]["battle_active"].as<bool>()) {
            throw ApiError(drogon::k400BadRequest, "目前不是戰鬥階段");
          }
          const auto state = currentCombatState(self, roomId);
          if (state.actor.isNull()) {
            throw ApiError(drogon::k400BadRequest, "目前沒有可行動單位");
          }
          advanceCombatAction(self, roomId, stringValue(state.actor["type"]),
                              integerValue(state.actor["id"]), user.id,
                              "DM 略過／推進");
          RoomSocket::broadcastSnapshotForRoom(roomId);
          reply(callback, self->roomSnapshot(roomId));
        }, "切換回合失敗");
      },
      {drogon::Post});

  // [功能備註｜戰鬥／回合｜POST /api/rooms/{1}/combat/pass]
  // 用途：建立/執行戰鬥／回合相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/rooms/{1}/combat/pass",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& roomIdText) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request);
          const auto roomId = parseRoomId(roomIdText);
          const auto member = self->database()->execSqlSync(
              "SELECT role FROM room_members WHERE room_id=$1 AND user_id=$2",
              roomId, user.id);
          if (member.empty() || member[0]["role"].as<std::string>() != "player") {
            throw ApiError(drogon::k403Forbidden, "你不是這個房間的玩家");
          }
          const auto state = currentCombatState(self, roomId);
          if (!state.active) throw ApiError(drogon::k400BadRequest, "目前不是戰鬥階段");
          if (state.actor.isNull() || stringValue(state.actor["type"]) != "player" ||
              integerValue(state.actor["id"]) != user.id) {
            throw ApiError(drogon::k403Forbidden, "現在不是你的行動");
          }
          advanceCombatAction(self, roomId, "player", user.id, user.id,
                              "放棄這次行動");
          RoomSocket::broadcastSnapshotForRoom(roomId);
          Json::Value response;
          response["ok"] = true;
          response["room"] = self->roomSnapshot(roomId);
          reply(callback, std::move(response));
        }, "略過行動失敗");
      },
      {drogon::Post});

  // [功能備註｜戰鬥／回合｜PATCH /api/rooms/{1}/combat/reaction]
  // 用途：修改戰鬥／回合相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/rooms/{1}/combat/reaction",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& roomIdText) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request);
          const auto roomId = parseRoomId(roomIdText);
          const auto& body = requireBody(request);
          const auto member = self->database()->execSqlSync(
              "SELECT role FROM room_members WHERE room_id=$1 AND user_id=$2",
              roomId, user.id);
          if (member.empty() || member[0]["role"].as<std::string>() != "player") {
            throw ApiError(drogon::k403Forbidden, "你不是這個房間的玩家");
          }
          const auto mode = stringValue(body["mode"], "dodge") == "defense"
                                ? std::string{"defense"}
                                : std::string{"dodge"};
          self->database()->execSqlSync(
              "UPDATE room_members SET combat_reaction=$1 WHERE room_id=$2 AND user_id=$3",
              mode, roomId, user.id);
          RoomSocket::broadcastSnapshotForRoom(roomId);
          Json::Value response;
          response["ok"] = true;
          response["mode"] = mode;
          response["room"] = self->roomSnapshot(roomId);
          reply(callback, std::move(response));
        }, "設定戰鬥反應失敗");
      },
      {drogon::Patch});

  // [功能備註｜戰鬥／回合｜POST /api/rooms/{1}/combat/basic-attack]
  // 用途：處理玩家基本攻擊、命中、反應與傷害。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/rooms/{1}/combat/basic-attack",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& roomIdText) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request);
          const auto roomId = parseRoomId(roomIdText);
          const auto& body = requireBody(request);
          const auto member = self->database()->execSqlSync(
              "SELECT role FROM room_members WHERE room_id=$1 AND user_id=$2",
              roomId, user.id);
          if (member.empty() || member[0]["role"].as<std::string>() != "player") {
            throw ApiError(drogon::k403Forbidden, "你不是這個房間的玩家");
          }
          const auto state = currentCombatState(self, roomId);
          if (!state.active) throw ApiError(drogon::k400BadRequest, "目前不是戰鬥階段");
          if (state.actor.isNull() || stringValue(state.actor["type"]) != "player" ||
              integerValue(state.actor["id"]) != user.id) {
            throw ApiError(drogon::k403Forbidden, "現在不是你的行動");
          }
          const auto targetType = stringValue(body["target_type"], "monster");
          if (targetType != "monster" && targetType != "npc") {
            throw ApiError(drogon::k400BadRequest, "基本攻擊只能選擇怪物或敵方 NPC");
          }
          const auto targetId = integerValue(body["target_id"]);
          const auto target = loadCombatTarget(self, roomId, targetType, targetId);
          if (target.side != "enemy") {
            throw ApiError(drogon::k400BadRequest, "基本攻擊必須選擇敵方目標");
          }
          auto combat = resolveBasicCombatAttack(self, roomId, "player", user.id, target);
          self->addEvent(roomId, user.id, "combat_attack", combat["event_payload"]);
          Json::Value combatRuleEvent = combat; combatRuleEvent["source"] = "player_basic_attack";
          // [功能備註｜62-cpp.23 事件型技能] 基本攻擊命中後先送 Trigger -> Condition -> Effect -> Status。
          try { combat["event_skill_result"] = dispatchEventSkillEvent(self, roomId, "player", user.id, "COMBAT_ATTACK_RESOLVED", combatRuleEvent); }
          catch (const std::exception& error) { LOG_WARN << "event skill attack degraded: " << error.what(); }
          evaluateRuleEvent(self, roomId, user.id, "COMBAT_ATTACK_RESOLVED", combatRuleEvent, false);
          if (combat["damage_change"].isObject() && combat["damage_change"].get("downed", false).asBool()) {
            try { dispatchEventSkillEvent(self, roomId, "player", user.id, "COMBAT_ENTITY_DEFEATED", combatRuleEvent); }
            catch (const std::exception& error) { LOG_WARN << "event skill defeat degraded: " << error.what(); }
            evaluateRuleEvent(self, roomId, user.id, "COMBAT_ENTITY_DEFEATED", combatRuleEvent, false);
          }
          evaluateRuleCombatVictory(self, roomId, user.id, "COMBAT_ATTACK_RESOLVED");
          combat.removeMember("event_payload");
          advanceCombatAction(self, roomId, "player", user.id, user.id, "基本攻擊");
          RoomSocket::broadcastSnapshotForRoom(roomId);
          Json::Value response;
          response["ok"] = true;
          response["combat"] = std::move(combat);
          response["room"] = self->roomSnapshot(roomId);
          reply(callback, std::move(response));
        }, "基本攻擊失敗");
      },
      {drogon::Post});

  // [功能備註｜戰鬥／回合｜POST /api/admin/rooms/{1}/combat/ai-act]
  // 用途：執行怪物／NPC 的 AI 戰鬥行動。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/admin/rooms/{1}/combat/ai-act",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& roomIdText) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request, true);
          const auto roomId = parseRoomId(roomIdText);
          const auto state = currentCombatState(self, roomId);
          if (!state.active) throw ApiError(drogon::k400BadRequest, "目前不是戰鬥階段");
          if (state.actor.isNull()) throw ApiError(drogon::k400BadRequest, "目前沒有可行動單位");
          const auto actorType = stringValue(state.actor["type"]);
          const auto actorId = integerValue(state.actor["id"]);
          if (actorType != "monster" && actorType != "npc") {
            throw ApiError(drogon::k400BadRequest, "目前不是 AI 單位行動");
          }
          const auto attacker = loadCombatTarget(self, roomId, actorType, actorId);
          const auto candidates = combatTargetsAgainst(self, roomId, attacker.side);
          if (candidates.empty()) {
            throw ApiError(drogon::k400BadRequest, "沒有仍可戰鬥的敵對目標");
          }
          const auto picked = static_cast<std::size_t>(
              secureCombatRoll(0, static_cast<std::int64_t>(candidates.size()) - 1));
          const auto target = candidates[picked];

          // [功能備註｜73-cpp.34.1 逆命 Boss 自動 AI]
          // 無牌者現在與一般怪物共用 /combat/ai-act；輪到它時會自動判斷四階段並施放逆命技能。
          if (actorType == "monster" && isAntiFateBoss(attacker)) {
            auto anti = executeAntiFateBossAiAction(self, roomId, attacker, target, state.round, user.id);
            RoomSocket::broadcastSnapshotForRoom(roomId);
            Json::Value response;
            response["ok"] = true;
            response["combat"] = anti["combat"];
            response["ai_result"]["skill_name"] = anti["skill_name"];
            response["ai_result"]["effect"] = anti["effect"];
            response["ai_result"]["phase"] = anti["phase"];
            response["ai_result"]["phase_name"] = anti["phase_name"];
            response["ai_result"]["text"] = anti["text"];
            response["ai_result"]["actor"]["type"] = actorType;
            response["ai_result"]["actor"]["id"] = Json::Int64(actorId);
            response["ai_result"]["actor"]["name"] = attacker.name;
            response["room"] = self->roomSnapshot(roomId);
            reply(callback, std::move(response));
            return;
          }

          Json::Value selectedSkill{Json::nullValue};
          if (actorType == "monster") selectedSkill = chooseMonsterCombatSkill(attacker, target, state.round);

          // [功能備註｜62-cpp.23 怪物多段技能]
          // 怪物 AI 選到資料驅動技能時也走同一個 Hit Sequence 執行器，避免玩家三段、怪物卻只打一段。
          if (selectedSkill.isObject()) {
            auto combat = executeConfiguredCombatSkill(self, roomId, actorType, actorId, selectedSkill,
                                                       target.type, target.id, user.id);
            RoomSocket::broadcastSnapshotForRoom(roomId);
            Json::Value response;
            response["ok"] = true;
            response["combat"] = combat;
            response["ai_result"]["skill_name"] = stringValue(selectedSkill["name"], "怪物技能");
            response["ai_result"]["skill"] = selectedSkill;
            response["ai_result"]["actor"]["type"] = actorType;
            response["ai_result"]["actor"]["id"] = Json::Int64(actorId);
            response["ai_result"]["actor"]["name"] = attacker.name;
            response["room"] = self->roomSnapshot(roomId);
            reply(callback, std::move(response));
            return;
          }

          auto combat = resolveBasicCombatAttack(self, roomId, actorType, actorId, target);
          self->addEvent(roomId, user.id, "combat_attack", combat["event_payload"]);
          Json::Value combatRuleEvent = combat; combatRuleEvent["source"] = "ai_basic_attack"; combatRuleEvent["ai_actor_type"] = actorType;
          try { combat["event_skill_result"] = dispatchEventSkillEvent(self, roomId, actorType, actorId, "COMBAT_ATTACK_RESOLVED", combatRuleEvent); }
          catch (const std::exception& error) { LOG_WARN << "event AI attack degraded: " << error.what(); }
          evaluateRuleEvent(self, roomId, user.id, "COMBAT_ATTACK_RESOLVED", combatRuleEvent, false);
          if (combat["damage_change"].isObject() && combat["damage_change"].get("downed", false).asBool()) {
            try { dispatchEventSkillEvent(self, roomId, actorType, actorId, "COMBAT_ENTITY_DEFEATED", combatRuleEvent); }
            catch (const std::exception& error) { LOG_WARN << "event AI defeat degraded: " << error.what(); }
            evaluateRuleEvent(self, roomId, user.id, "COMBAT_ENTITY_DEFEATED", combatRuleEvent, false);
          }
          evaluateRuleCombatVictory(self, roomId, user.id, "COMBAT_ATTACK_RESOLVED");
          combat.removeMember("event_payload");
          advanceCombatAction(self, roomId, actorType, actorId, user.id, "AI 基本攻擊");
          RoomSocket::broadcastSnapshotForRoom(roomId);
          Json::Value response;
          response["ok"] = true;
          response["combat"] = combat;
          response["ai_result"]["skill_name"] = "基本攻擊";
          response["ai_result"]["actor"]["type"] = actorType;
          response["ai_result"]["actor"]["id"] = Json::Int64(actorId);
          response["ai_result"]["actor"]["name"] = attacker.name;
          response["room"] = self->roomSnapshot(roomId);
          reply(callback, std::move(response));
        }, "AI 行動失敗");
      },
      {drogon::Post});

  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/save]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/rooms/{1}/save",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& roomIdText) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request, true);
          const auto roomId = parseRoomId(roomIdText);
          const auto updated = self->database()->execSqlSync(
              "UPDATE rooms SET saved=TRUE,status='saved',battle_active=FALSE,"
              "current_actor_id=NULL,current_actor_type=NULL,current_actor_ref_id=NULL,"
              "combat_order='[]'::jsonb,combat_index=0,updated_at=CURRENT_TIMESTAMP "
              "WHERE id=$1 RETURNING id",
              roomId);
          if (updated.empty()) throw ApiError(drogon::k404NotFound, "找不到房間");
          Json::Value event;
          event["text"] = "DM 已將跑團存檔。";
          self->addEvent(roomId, user.id, "system", event);
          RoomSocket::broadcastSnapshotForRoom(roomId);
          reply(callback, self->roomSnapshot(roomId));
        }, "存檔失敗");
      },
      {drogon::Post});

  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/leave]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/rooms/{1}/leave",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& roomIdText) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request, true);
          const auto roomId = parseRoomId(roomIdText);
          const auto updated = self->database()->execSqlSync(
              "UPDATE rooms SET saved=TRUE,status='closed',battle_active=FALSE,"
              "current_actor_id=NULL,current_actor_type=NULL,current_actor_ref_id=NULL,"
              "combat_order='[]'::jsonb,combat_index=0,closed_at=CURRENT_TIMESTAMP,"
              "updated_at=CURRENT_TIMESTAMP WHERE id=$1 RETURNING id",
              roomId);
          if (updated.empty()) throw ApiError(drogon::k404NotFound, "找不到房間");
          Json::Value event;
          event["text"] = "DM 已結束本次跑團並保存房間。";
          self->addEvent(roomId, user.id, "system", event);
          RoomSocket::broadcastSnapshotForRoom(roomId);
          reply(callback, self->roomSnapshot(roomId));
        }, "關閉房間失敗");
      },
      {drogon::Post});

  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/reopen]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/rooms/{1}/reopen",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& roomIdText) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request, true);
          const auto roomId = parseRoomId(roomIdText);
          const auto updated = self->database()->execSqlSync(
              "UPDATE rooms SET status='lobby',saved=FALSE,battle_active=FALSE,"
              "current_actor_id=NULL,current_actor_type=NULL,current_actor_ref_id=NULL,"
              "combat_order='[]'::jsonb,combat_index=0,closed_at=NULL,"
              "updated_at=CURRENT_TIMESTAMP WHERE id=$1 RETURNING id",
              roomId);
          if (updated.empty()) throw ApiError(drogon::k404NotFound, "找不到房間");
          Json::Value event;
          event["text"] = "DM 重新開啟了這場跑團。";
          self->addEvent(roomId, user.id, "system", event);
          RoomSocket::broadcastSnapshotForRoom(roomId);
          reply(callback, self->roomSnapshot(roomId));
        }, "重新開啟房間失敗");
      },
      {drogon::Post});

  // [功能備註｜其他系統｜GET /api/admin/users]
  // 用途：讀取/顯示其他系統相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/admin/users",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback) {
        runHandler(callback, [&] {
          requireCurrentUser(self, request, true);
          const auto rows = self->database()->execSqlSync(
              "SELECT jsonb_build_object('id',id,'username',username,'is_admin',is_admin," 
              "'created_at',created_at)::text AS json_text FROM users ORDER BY id");
          Json::Value response;
          response["users"] = Json::arrayValue;
          for (const auto& row : rows)
            response["users"].append(parseJson(row["json_text"].as<std::string>()));
          reply(callback, std::move(response));
        }, "讀取帳號失敗");
      },
      {drogon::Get});

  // [功能備註｜其他系統｜DELETE /api/admin/users/{1}]
  // 用途：刪除其他系統相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/admin/users/{1}",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& userIdText) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request, true);
          std::int64_t targetId = 0;
          try {
            std::size_t consumed = 0;
            targetId = std::stoll(userIdText, &consumed, 10);
            if (consumed != userIdText.size() || targetId <= 0) {
              throw std::invalid_argument{"id"};
            }
          } catch (const std::exception&) {
            throw ApiError(drogon::k400BadRequest, "帳號 ID 無效");
          }
          const auto targets = self->database()->execSqlSync(
              "SELECT id,is_admin FROM users WHERE id=$1", targetId);
          if (targets.empty()) throw ApiError(drogon::k404NotFound, "找不到帳號");
          if (targetId == user.id) {
            throw ApiError(drogon::k400BadRequest, "不能刪除目前登入中的 DM 帳號");
          }
          if (targets[0]["is_admin"].as<bool>()) {
            throw ApiError(drogon::k403Forbidden, "不能刪除其他 DM 帳號");
          }
          self->database()->execSqlSync("DELETE FROM users WHERE id=$1", targetId);
          Json::Value response;
          response["ok"] = true;
          response["message"] = "玩家帳號、角色卡及相關資料已刪除";
          reply(callback, std::move(response));
        }, "刪除帳號失敗");
      },
      {drogon::Delete});

  // [功能備註｜房間／跑團｜GET /api/admin/rooms]
  // 用途：讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/admin/rooms",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback) {
        runHandler(callback, [&] {
          requireCurrentUser(self, request, true);
          const auto rows = self->database()->execSqlSync(R"SQL(
            SELECT jsonb_build_object(
              'id',r.id,'code',r.code,'name',r.name,'owner_id',r.owner_id,
              'status',r.status,'round',r.round,'saved',r.saved,
              'created_at',r.created_at,'updated_at',r.updated_at,
              'owner_username',u.username,
              'member_count',(SELECT COUNT(*) FROM room_members m WHERE m.room_id=r.id)
            )::text AS json_text
            FROM rooms r JOIN users u ON u.id=r.owner_id ORDER BY r.id DESC
          )SQL");
          Json::Value response;
          response["rooms"] = Json::arrayValue;
          for (const auto& row : rows)
            response["rooms"].append(parseJson(row["json_text"].as<std::string>()));
          reply(callback, std::move(response));
        }, "讀取房間失敗");
      },
      {drogon::Get});

  // [功能備註｜房間／跑團｜POST /api/admin/rooms/{1}/close]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/admin/rooms/{1}/close",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& roomIdText) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request, true);
          const auto roomId = parseRoomId(roomIdText);
          const auto updated = self->database()->execSqlSync(
              "UPDATE rooms SET saved=TRUE,status='closed',battle_active=FALSE,"
              "current_actor_id=NULL,current_actor_type=NULL,current_actor_ref_id=NULL,"
              "combat_order='[]'::jsonb,combat_index=0,closed_at=CURRENT_TIMESTAMP,"
              "updated_at=CURRENT_TIMESTAMP WHERE id=$1 RETURNING id",
              roomId);
          if (updated.empty()) throw ApiError(drogon::k404NotFound, "找不到房間");
          Json::Value event;
          event["text"] = "全站 DM 已結束本次跑團並保存房間。";
          self->addEvent(roomId, user.id, "admin", event);
          RoomSocket::broadcastSnapshotForRoom(roomId);
          Json::Value response;
          response["ok"] = true;
          response["room"] = self->roomSnapshot(roomId);
          reply(callback, std::move(response));
        }, "關閉房間失敗");
      },
      {drogon::Post});

  // [功能備註｜房間／跑團｜POST /api/admin/rooms/{1}/reopen]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/admin/rooms/{1}/reopen",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& roomIdText) {
        runHandler(callback, [&] {
          const auto user = requireCurrentUser(self, request, true);
          const auto roomId = parseRoomId(roomIdText);
          const auto updated = self->database()->execSqlSync(
              "UPDATE rooms SET status='lobby',saved=FALSE,battle_active=FALSE,"
              "current_actor_id=NULL,current_actor_type=NULL,current_actor_ref_id=NULL,"
              "combat_order='[]'::jsonb,combat_index=0,closed_at=NULL,"
              "updated_at=CURRENT_TIMESTAMP WHERE id=$1 RETURNING id",
              roomId);
          if (updated.empty()) throw ApiError(drogon::k404NotFound, "找不到房間");
          Json::Value event;
          event["text"] = "全站 DM 重新開啟了這場跑團。";
          self->addEvent(roomId, user.id, "admin", event);
          RoomSocket::broadcastSnapshotForRoom(roomId);
          Json::Value response;
          response["ok"] = true;
          response["room"] = self->roomSnapshot(roomId);
          reply(callback, std::move(response));
        }, "重新開啟房間失敗");
      },
      {drogon::Post});

  // [功能備註｜房間／跑團｜DELETE /api/admin/rooms/{1}]
  // 用途：刪除房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/admin/rooms/{1}",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& roomIdText) {
        runHandler(callback, [&] {
          requireCurrentUser(self, request, true);
          const auto roomId = parseRoomId(roomIdText);
          const auto deleted = self->database()->execSqlSync(
              "DELETE FROM rooms WHERE id=$1 RETURNING id", roomId);
          if (deleted.empty()) throw ApiError(drogon::k404NotFound, "找不到房間");
          Json::Value response;
          response["ok"] = true;
          response["message"] = "房間已永久刪除";
          reply(callback, std::move(response));
        }, "刪除房間失敗");
      },
      {drogon::Delete});

  // [功能備註｜角色卡｜GET /api/admin/characters]
  // 用途：讀取/顯示角色卡相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/admin/characters",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback) {
        runHandler(callback, [&] {
          requireCurrentUser(self, request, true);
          const auto users = self->database()->execSqlSync(
              "SELECT u.id,u.username,u.is_admin FROM users u "
              "JOIN character_cards c ON c.user_id=u.id ORDER BY u.id");
          Json::Value response;
          response["characters"] = Json::arrayValue;
          for (const auto& row : users) {
            auto character = self->characterByUser(row["id"].as<std::int64_t>());
            character["username"] = row["username"].as<std::string>();
            character["is_admin"] = row["is_admin"].as<bool>();
            response["characters"].append(std::move(character));
          }
          reply(callback, std::move(response));
        }, "讀取角色卡失敗");
      },
      {drogon::Get});

  // [功能備註｜房間／跑團｜GET /api/rooms/{1}/characters]
  // 用途：讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  drogon::app().registerHandler(
      "/api/rooms/{1}/characters",
      [self](const drogon::HttpRequestPtr& request, Callback&& callback,
             const std::string& roomIdText) {
        runHandler(callback, [&] {
          requireCurrentUser(self, request, true);
          const auto roomId = parseRoomId(roomIdText);
          const auto users = self->database()->execSqlSync(
              "SELECT rm.user_id,u.username FROM room_members rm "
              "JOIN users u ON u.id=rm.user_id WHERE rm.room_id=$1 ORDER BY rm.joined_at",
              roomId);
          Json::Value response;
          response["characters"] = Json::arrayValue;
          for (const auto& row : users) {
            auto character = self->characterByUser(row["user_id"].as<std::int64_t>());
            if (!character.isNull()) {
              character["username"] = row["username"].as<std::string>();
              response["characters"].append(std::move(character));
            }
          }
          reply(callback, std::move(response));
        }, "讀取房間角色失敗");
      },
      {drogon::Get});

  registerLegacyV39Routes(self);
  registerWorldExpansionRoutes(self);
  // [功能備註｜規則引擎] 註冊怪談／條件式世界規則引擎。
  registerRuleEngineRoutes(self);
  // [功能備註｜規則－戰鬥橋接] 註冊規則戰鬥狀態／勝利條件管理 API。
  registerRuleCombatBridgeRoutes(self);
  // [功能備註｜通用詞條／裝備槽] 註冊主副武器、自訂裝備槽與通用洗煉 API。
  registerGearAffixRoutes(self);
}

}  // namespace trpg
