Warning: truncated output (original token count: 134091)
Total output lines: 6816

#include "trpg/LegacyCompat.h"
#include "trpg/EventSkillSystem.h"
#include "trpg/CoreService.h"
#include "trpg/WorldSystems.h"
#include "trpg/RuleEngine.h"
#include "trpg/RuleCombatBridge.h"

#include <drogon/drogon.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <functional>
#include <limits>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace trpg {
namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr&)>;
using Request = drogon::HttpRequestPtr;
using Db = drogon::orm::DbClientPtr;

struct ApiError final : std::runtime_error {
  drogon::HttpStatusCode status;
  ApiError(drogon::HttpStatusCode code, std::string message)
      : std::runtime_error(std::move(message)), status(code) {}
};

struct UserInfo {
  std::int64_t id{};
  std::string username;
  bool isAdmin{};
  bool isDmAssistant{};
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

template <typename Function>
void runHandler(Callback& callback, Function&& function,
                const std::string& genericMessage = "操作失敗") {
  try {
    function();
  } catch (const ApiError& error) {
    fail(callback, error.status, error.what());
  } catch (const drogon::orm::DrogonDbException& error) {
    LOG_ERROR << "legacy compatibility database error: " << error.base().what();
    fail(callback, drogon::k500InternalServerError, genericMessage);
  } catch (const std::exception& error) {
    LOG_ERROR << "legacy compatibility request error: " << error.what();
    fail(callback, drogon::k500InternalServerError, genericMessage);
  }
}

Json::Value bodyOrEmpty(const Request& request) {
  const auto json = request->getJsonObject();
  return json && json->isObject() ? *json : Json::Value{Json::objectValue};
}

std::string stringValue(const Json::Value& value, std::string fallback = {}) {
  if (value.isString()) return value.asString();
  if (value.isBool()) return value.asBool() ? "true" : "false";
  if (value.isInt64()) return std::to_string(value.asInt64());
  if (value.isUInt64()) return std::to_string(value.asUInt64());
  if (value.isDouble()) return std::to_string(value.asDouble());
  return fallback;
}

std::int64_t intValue(const Json::Value& value, std::int64_t fallback = 0) {
  try {
    if (value.isInt64()) return value.asInt64();
    if (value.isUInt64()) {
      const auto raw = value.asUInt64();
      return raw <= static_cast<Json::UInt64>(std::numeric_limits<std::int64_t>::max())
                 ? static_cast<std::int64_t>(raw)
                 : fallback;
    }
    if (value.isDouble()) return static_cast<std::int64_t>(value.asDouble());
    if (value.isString()) {
      std::size_t used = 0;
      const auto parsed = std::stoll(value.asString(), &used);
      return used == value.asString().size() ? parsed : fallback;
    }
  } catch (...) {
  }
  return fallback;
}

// [功能備註｜64-cpp.25 角色正式等階] Legacy 路由也使用同一份大階固定屬性總額，避免 DM／儀式升階和角色核心計算不同步。
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

Json::Value advanceCharacterTier(Db db, std::int64_t userId, std::int64_t nextTier,
                                 const std::string& source, bool requireLevelFour = true) {
  const auto rows = db->execSqlSync(
      "SELECT tier,level,agility,strength,constitution,spirit,great_way,faith,faction FROM character_cards WHERE user_id=$1", userId);
  if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到角色卡");
  const auto currentTier = std::clamp<std::int64_t>(rows[0]["tier"].as<std::int64_t>(), 0, 10);
  const auto currentLevel = std::clamp<std::int64_t>(rows[0]["level"].as<std::int64_t>(), 1, 4);
  nextTier = std::clamp<std::int64_t>(nextTier, 0, 10);
  if (currentTier >= 10) throw ApiError(drogon::k409Conflict, "角色已是真神階，無法再登階");
  if (nextTier != currentTier + 1) throw ApiError(drogon::k400BadRequest, "只能晉升到下一個大階");
  if (requireLevelFour && currentTier > 0 && currentLevel < 4)
    throw ApiError(drogon::k409Conflict, "必須先到達目前大階 4 級才能進行登階");
  const bool west = rows[0]["faction"].as<std::string>() == "西國";
  const auto fifth = std::max<std::int64_t>(0, west ? rows[0]["faith"].as<std::int64_t>() : rows[0]["great_way"].as<std::int64_t>());
  const auto spent = std::max<std::int64_t>(0, rows[0]["agility"].as<std::int64_t>()) +
                     std::max<std::int64_t>(0, rows[0]["strength"].as<std::int64_t>()) +
                     std::max<std::int64_t>(0, rows[0]["constitution"].as<std::int64_t>()) +
                     std::max<std::int64_t>(0, rows[0]["spirit"].as<std::int64_t>()) + fifth;
  const auto newBudget = characterTierBudget(nextTier);
  if (spent > newBudget) throw ApiError(drogon::k409Conflict, "現有基礎屬性已超過下一階額度，請先由 DM 修正角色卡");
  const auto remaining = newBudget - spent;
  db->execSqlSync(
      "UPDATE character_cards SET tier=$1,level=1,attribute_points=$2,updated_at=CURRENT_TIMESTAMP WHERE user_id=$3",
      nextTier, remaining, userId);
  Json::Value out;
  out["from_tier"] = Json::Int64(currentTier);
  out["from_level"] = Json::Int64(currentLevel);
  out["to_tier"] = Json::Int64(nextTier);
  out["to_level"] = Json::Int64(1);
  out["attribute_budget"] = Json::Int64(newBudget);
  out["attribute_points"] = Json::Int64(remaining);
  out["title"] = characterTierTitle(nextTier, 1);
  out["source"] = source;
  return out;
}
// [功能備註｜61-cpp.22 隊伍節點連線驗證] 舊地圖沒有 connections 時維持自由移動；有連線後隊伍必須沿圖移動。
bool mapNodesAdjacent(const std::shared_ptr<CoreService>& service,
                      std::int64_t roomId,
                      std::int64_t fromNodeId,
                      std::int64_t toNodeId) {
  if (fromNodeId <= 0 || fromNodeId == toNodeId) return true;
  const auto rows = service->database()->execSqlSync(
      "SELECT connections::text AS connections FROM room_map_nodes WHERE id=$1 AND room_id=$2",
      fromNodeId, roomId);
  if (rows.empty() || rows[0]["connections"].isNull()) return true;
  const auto links = parseJson(rows[0]["connections"].as<std::string>(), Json::Value{Json::arrayValue});
  if (!links.isArray() || links.empty()) return true;
  for (const auto& value : links) if (intValue(value) == toNodeId) return true;
  return false;
}


// [功能備註｜64-cpp.25.4 行動＋說話合併] 說話不是場外聊天；它是房間內的角色事件，會產生聲音、更新節點噪音並讓怪物警戒。
std::optional<std::int64_t> effectiveInteractionNode(
    const std::shared_ptr<CoreService>& service,
    std::int64_t roomId,
    std::int64_t userId) {
  const auto rows = service->database()->execSqlSync(R"SQL(
    SELECT COALESCE(t.current_map_node_id,rm.current_map_node_id,r.current_map_node_id) AS node_id
    FROM room_members rm
    JOIN rooms r ON r.id=rm.room_id
    LEFT JOIN room_team_members tm ON tm.room_id=rm.room_id AND tm.user_id=rm.user_id
    LEFT JOIN room_teams t ON t.id=tm.team_id
    WHERE rm.room_id=$1 AND rm.user_id=$2
  )SQL", roomId, userId);
  if (rows.empty() || rows[0]["node_id"].isNull()) return std::nullopt;
  const auto nodeId = rows[0]["node_id"].as<std::int64_t>();
  return nodeId > 0 ? std::optional<std::int64_t>{nodeId} : std::nullopt;
}

std::vector<std::int64_t> interactionAudibleNodes(
    const std::shared_ptr<CoreService>& service,
    std::int64_t roomId,
    std::int64_t senderUserId,
    const std::string& volume) {
  std::vector<std::int64_t> nodes;
  const auto senderNode = effectiveInteractionNode(service, roomId, senderUserId);
  if (!senderNode) return nodes;
  nodes.push_back(*senderNode);
  if (volume != "shout") return nodes;

  try {
    const auto rows = service->database()->execSqlSync(
        "SELECT connections::text AS connections FROM room_map_nodes WHERE id=$1 AND room_id=$2",
        *senderNode, roomId);
    if (!rows.empty() && !rows[0]["connections"].isNull()) {
      const auto links = parseJson(rows[0]["connections"].as<std::string>(),
                                   Json::Value{Json::arrayValue});
      if (links.isArray()) {
        for (const auto& raw : links) {
          const auto id = intValue(raw);
          if (id > 0 && std::find(nodes.begin(), nodes.end(), id) == nodes.end()) {
            nodes.push_back(id);
          }
        }
      }
    }
  } catch (const std::exception& error) {
    LOG_WARN << "speech adjacent-node lookup degraded for room " << roomId
             << ": " << error.what();
  }
  return nodes;
}

std::vector<std::int64_t> physicalSpeechAudience(
    const std::shared_ptr<CoreService>& service,
    std::int64_t roomId,
    std::int64_t senderUserId,
    const std::string& volume) {
  std::vector<std::int64_t> audience;
  const auto audibleNodes = interactionAudibleNodes(service, roomId, senderUserId, volume);
  const auto rows = service->database()->execSqlSync(R"SQL(
    SELECT rm.user_id,
           COALESCE(t.current_map_node_id,rm.current_map_node_id,r.current_map_node_id) AS node_id
    FROM room_members rm
    JOIN rooms r ON r.id=rm.room_id
    LEFT JOIN room_team_members tm ON tm.room_id=rm.room_id AND tm.user_id=rm.user_id
    LEFT JOIN room_teams t ON t.id=tm.team_id
    WHERE rm.room_id=$1
  )SQL", roomId);

  // 沒有建立平面節點的舊房間視為所有人在同一場景，避免升級後突然互相聽不到。
  if (audibleNodes.empty()) {
    for (const auto& row : rows) audience.push_back(row["user_id"].as<std::int64_t>());
  } else {
    for (const auto& row : rows) {
      if (row["node_id"].isNull()) continue;
      const auto nodeId = row["node_id"].as<std::int64_t>();
      if (std::find(audibleNodes.begin(), audibleNodes.end(), nodeId) != audibleNodes.end()) {
        audience.push_back(row["user_id"].as<std::int64_t>());
      }
    }
  }
  if (std::find(audience.begin(), audience.end(), senderUserId) == audience.end()) {
    audience.push_back(senderUserId);
  }
  return audience;
}

void appendAudienceIds(Json::Value& payload,
                       const std::vector<std::int64_t>& audience) {
  payload["audience_user_ids"] = Json::arrayValue;
  for (const auto id : audience) payload["audience_user_ids"].append(Json::Int64(id));
}

void applyInteractionNoise(
    const std::shared_ptr<CoreService>& service,
    std::int64_t roomId,
    std::int64_t senderUserId,
    std::int64_t noise,
    const std::string& volume,
    Json::Value& payload) {
  noise = std::clamp<std::int64_t>(noise, 0, 100);
  payload["noise"] = Json::Int64(noise);
  payload["audible_node_ids"] = Json::arrayValue;
  if (noise <= 0) return;

  const auto nodes = interactionAudibleNodes(service, roomId, senderUserId, volume);
  for (const auto nodeId : nodes) payload["audible_node_ids"].append(Json::Int64(nodeId));
  const auto senderNode = effectiveInteractionNode(service, roomId, senderUserId);
  if (senderNode) payload["sender_node_id"] = Json::Int64(*senderNode);
  else payload["sender_node_id"] = Json::nullValue;

  if (!senderNode) return;
  try {
    service->database()->execSqlSync(
        "UPDATE room_map_nodes SET noise_level=LEAST(999,GREATEST(0,noise_level+$1)),"
        "updated_at=CURRENT_TIMESTAMP WHERE id=$2 AND room_id=$3",
        noise, *senderNode, roomId);

    // 怪物的 hearing_threshold 已存在於怪物卡 config。聽見足夠大的聲音時提高警戒，
    // 讓既有 AI / DM 可以直接讀 alert_level 決定追查或遭遇。
    const auto alertDelta = std::max<std::int64_t>(1, noise / 5);
    for (const auto nodeId : nodes) {
      service->database()->execSqlSync(R"SQL(
        UPDATE room_monsters rm
        SET alert_level=LEAST(100,GREATEST(0,rm.alert_level+$1)),
            updated_at=CURRENT_TIMESTAMP
        FROM monster_templates mt
        WHERE rm.monster_template_id=mt.id
          AND rm.room_id=$2
          AND rm.map_node_id=$3
          AND (CASE
                 WHEN COALESCE(mt.config->>'hearing_threshold','') ~ '^[0-9]+$'
                 THEN (mt.config->>'hearing_threshold')::BIGINT
                 ELSE 30
               END) <= $4
      )SQL", alertDelta, roomId, nodeId, noise);
    }
  } catch (const std::exception& error) {
    // 噪音／AI 警戒屬於附加系統，舊資料庫缺欄位時不能拖垮說話或行動。
    LOG_WARN << "interaction noise/monster hearing degraded for room " << roomId
             << ": " << error.what();
  }
}

// [功能備註｜61-cpp.22 雙向節點連線] DM 編輯 A 的相鄰節點時，同步維護 B→A，避免畫面有線但反向不能走。
void syncBidirectionalMapConnections(const std::shared_ptr<CoreService>& service,
                                     std::int64_t roomId,
                                     std::int64_t nodeId,
                                     const Json::Value& requested) {
  if (!requested.isArray()) return;
  std::vector<std::int64_t> desired;
  for (const auto& value : requested) {
    const auto id = intValue(value);
    if (id > 0 && id != nodeId && std::find(desired.begin(), desired.end(), id) == desired.end()) desired.push_back(id);
  }
  const auto rows = service->database()->execSqlSync(
      "SELECT id,connections::text AS connections FROM room_map_nodes WHERE room_id=$1 AND id<>$2 ORDER BY id",
      roomId, nodeId);
  Json::Value normalized{Json::arrayValue};
  for (const auto id : desired) {
    if (!service->database()->execSqlSync("SELECT 1 FROM room_map_nodes WHERE id=$1 AND room_id=$2", id, roomId).empty()) normalized.append(Json::Int64(id));
  }
  service->database()->execSqlSync("UPDATE room_map_nodes SET connections=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=$2 AND room_id=$3",
                                   compactJson(normalized), nodeId, roomId);
  for (const auto& row : rows) {
    const auto otherId = row["id"].as<std::int64_t>();
    auto links = row["connections"].isNull() ? Json::Value{Json::arrayValue} : parseJson(row["connections"].as<std::string>(), Json::Value{Json::arrayValue});
    if (!links.isArray()) links = Json::Value{Json::arrayValue};
    Json::Value next{Json::arrayValue};
    for (const auto& value : links) {
      const auto id = intValue(value);
      if (id == nodeId) continue;
      if (id > 0 && id != otherId) next.append(Json::Int64(id));
    }
    if (std::find(desired.begin(), desired.end(), otherId) != desired.end()) next.append(Json::Int64(nodeId));
    service->database()->execSqlSync("UPDATE room_map_nodes SET connections=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=$2 AND room_id=$3",
                                     compactJson(next), otherId, roomId);
  }
}




std::int64_t positiveId(const std::string& text, const std::string& message = "ID 無效") {
  try {
    std::size_t used = 0;
    const auto value = std::stoll(text, &used);
    if (used != text.size() || value <= 0) throw std::invalid_argument{"id"};
    return value;
  } catch (...) {
    throw ApiError(drogon::k400BadRequest, message);
  }
}

std::string trimmed(std::string value) {
  const auto nonSpace = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), nonSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), nonSpace).base(), value.end());
  return value;
}

// [功能備註｜60-cpp.21.1 Render 編譯修正] LegacyCompat.cpp 與 CoreService.cpp 為不同翻譯單元；
// 此處需保留自己的 UTF-8 安全截斷 helper，不能呼叫 CoreService.cpp 匿名 namespace 內的 clipped()。
std::string clipped(std::string value, std::size_t bytes) {
  if (value.size() <= bytes) return value;
  value.resize(bytes);
  while (!value.empty() &&
         (static_cast<unsigned char>(value.back()) & 0xc0U) == 0x80U) {
    value.pop_back();
  }
  return value;
}

UserInfo requireUser(const std::shared_ptr<CoreService>& service,
                     const Request& request, bool admin = false) {
  const auto claims = service->authenticateRequest(request);
  if (!claims) throw ApiError(drogon::k401Unauthorized, "請先登入");
  const auto rows = service->database()->execSqlSync(
      "SELECT id,username,is_admin,is_dm_assistant FROM users WHERE id=$1", claims->id);
  if (rows.empty()) throw ApiError(drogon::k401Unauthorized, "帳號不存在");
  UserInfo user{rows[0]["id"].as<std::int64_t>(),
                rows[0]["username"].as<std::string>(),
                rows[0]["is_admin"].as<bool>(),
                rows[0]["is_dm_assistant"].as<bool>()};
  if (admin && !user.isAdmin) throw ApiError(drogon::k403Forbidden, "需要全站 DM 權限");
  return user;
}

UserInfo requireDmAssistant(const std::shared_ptr<CoreService>& service,
                            const Request& request) {
  const auto user = requireUser(service, request);
  if (!user.isAdmin && !user.isDmAssistant)
    throw ApiError(drogon::k403Forbidden, "需要 DM 助手權限");
  return user;
}

void recordDmAssistantCreation(const Db& db, const UserInfo& user,
                               const std::string& type,
                               const Json::Value& entity) {
  if (user.isAdmin) return;
  db->execSqlSync(
      "INSERT INTO dm_assistant_activity(user_id,entity_type,entity_id,entity_name) "
      "VALUES($1,$2,NULLIF($3,0),$4)",
      user.id, type, intValue(entity["id"]), clipped(stringValue(entity["name"]), 180));
}

void requireRoomAccess(const std::shared_ptr<CoreService>& service,
                       const UserInfo& user, std::int64_t roomId) {
  if (user.isAdmin) return;
  if (!service->userCanAccessRoom(user.id, roomId)) {
    throw ApiError(drogon::k403Forbidden, "你不在這個房間");
  }
}

void requireRoomExists(const Db& db, std::int64_t roomId) {
  if (db->execSqlSync("SELECT 1 FROM rooms WHERE id=$1", roomId).empty()) {
    throw ApiError(drogon::k404NotFound, "找不到房間");
  }
}

Json::Value rowJson(const drogon::orm::Result& rows, std::size_t index = 0) {
  if (rows.empty() || index >= rows.size()) return Json::Value{};
  try {
    if (rows[index]["json_text"].isNull()) return Json::Value{};
    return parseJson(rows[index]["json_text"].as<std::string>());
  } catch (...) {
    return Json::Value{};
  }
}

std::string sha256Hex(const std::string& input) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const auto byte : digest) out << std::setw(2) << static_cast<int>(byte);
  return out.str();
}


std::string utcNowIso() {
  const auto now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

bool sensitiveBackupKey(const std::string& key) {
  static const std::regex pattern(
      R"(^(password|password_hash|passwordhash|passwd|token|access_token|refresh_token|jwt|jwt_secret|secret|database_url|authorization|api_key)$)",
      std::regex::icase);
  return std::regex_match(key, pattern);
}

bool containsSensitiveBackupKey(const Json::Value& value) {
  if (value.isArray()) {
    for (const auto& item : value) if (containsSensitiveBackupKey(item)) return true;
    return false;
  }
  if (!value.isObject()) return false;
  for (const auto& name : value.getMemberNames()) {
    if (sensitiveBackupKey(name) || containsSensitiveBackupKey(value[name])) return true;
  }
  return false;
}

Json::Value backupDigestPayload(const Json::Value& backup) {
  Json::Value digest;
  for (const auto* key : {"schema","format_version","app_version","exported_at","scope","target","data"}) {
    digest[key] = backup[key];
  }
  return digest;
}

Json::Value createBackupEnvelope(const std::string& scope, const Json::Value& target,
                                 const Json::Value& data) {
  Json::Value backup;
  backup["schema"] = "trpg-online-backup";
  backup["format_version"] = 1;
  backup["app_version"] = CoreService::kVersion;
  backup["exported_at"] = utcNowIso();
  backup["scope"] = scope;
  backup["target"] = target;
  backup["data"] = data;
  backup["checksum"] = sha256Hex(compactJson(backupDigestPayload(backup)));
  return backup;
}

Json::Value validateBackupEnvelope(const Json::Value& backup) {
  if (!backup.isObject()) throw ApiError(drogon::k400BadRequest, "備份檔格式不正確");
  if (compactJson(backup).size() > 8U * 1024U * 1024U) throw ApiError(drogon::k400BadRequest, "備份檔超過 8 MB 匯入上限");
  if (stringValue(backup["schema"]) != "trpg-online-backup" || intValue(backup["format_version"]) != 1)
    throw ApiError(drogon::k400BadRequest, "這不是可支援的 TRPG 備份檔");
  const auto scope = stringValue(backup["scope"]);
  if (scope != "site" && scope != "room" && scope != "character") throw ApiError(drogon::k400BadRequest, "備份範圍不正確");
  if (!backup["target"].isObject() || !backup["data"].isObject()) throw ApiError(drogon::k400BadRequest, "備份內容不完整");
  if (containsSensitiveBackupKey(backup)) throw ApiError(drogon::k400BadRequest, "備份檔含有禁止匯入的登入憑證欄位");
  const auto expected = sha256Hex(compactJson(backupDigestPayload(backup)));
  const auto actual = stringValue(backup["checksum"]);
  if (actual.size() != 64 || actual != expected) throw ApiError(drogon::k400BadRequest, "備份檔校驗失敗，內容可能已損壞或被修改");
  return backup;
}

std::string createRestoreToken(const Json::Value& backup, std::int64_t actorId) {
  const auto expires = static_cast<std::int64_t>(std::time(nullptr)) + 600;
  const auto payload = stringValue(backup["scope"]) + "|" + stringValue(backup["target"]["id"]) + "|" +
                       stringValue(backup["checksum"]) + "|" + std::to_string(actorId) + "|" + std::to_string(expires);
  return std::to_string(expires) + "." + sha256Hex(payload);
}

bool validRestoreToken(const Json::Value& backup, std::int64_t actorId,
                       const std::string& token) {
  const auto dot = token.find('.');
  if (dot == std::string::npos) return false;
  std::int64_t expires = 0;
  try { expires = std::stoll(token.substr(0, dot)); } catch (...) { return false; }
  if (expires < static_cast<std::int64_t>(std::time(nullptr))) return false;
  const auto payload = stringValue(backup["scope"]) + "|" + stringValue(backup["target"]["id"]) + "|" +
                       stringValue(backup["checksum"]) + "|" + std::to_string(actorId) + "|" + std::to_string(expires);
  return token.substr(dot + 1) == sha256Hex(payload);
}

Json::Value backupCounts(const Json::Value& checkpoint, const std::vector<std::string>& keys) {
  Json::Value counts{Json::objectValue};
  for (const auto& key : keys) counts[key] = checkpoint[key].isArray() ? static_cast<Json::UInt64>(checkpoint[key].size()) : 0;
  return counts;
}

Json::Value selectRows(const Db& db, const std::string& sql) {
  Json::Value out{Json::arrayValue};
  const auto rows = db->execSqlSync(sql);
  for (const auto& row : rows) {
    if (!row["json_text"].isNull()) out.append(parseJson(row["json_text"].as<std::string>()));
  }
  return out;
}

Json::Value selectRows1(const Db& db, const std::string& sql, std::int64_t p1) {
  Json::Value out{Json::arrayValue};
  const auto rows = db->execSqlSync(sql, p1);
  for (const auto& row : rows) {
    if (!row["json_text"].isNull()) out.append(parseJson(row["json_text"].as<std::string>()));
  }
  return out;
}

Json::Value selectRows2(const Db& db, const std::string& sql,
                        std::int64_t p1, std::int64_t p2) {
  Json::Value out{Json::arrayValue};
  const auto rows = db->execSqlSync(sql, p1, p2);
  for (const auto& row : rows) {
    if (!row["json_text"].isNull()) out.append(parseJson(row["json_text"].as<std::string>()));
  }
  return out;
}


// [功能備註｜共用品階] 全站內容品階固定 G<F<E<D<C<B<A<S<SS<SSS；C++ 是唯一正式驗證來源。
const std::array<std::string,10> kContentRanks={"G","F","E","D","C","B","A","S","SS","SSS"};
std::string normalizeContentRank(std::string rank) {
  rank=trimmed(rank);
  std::transform(rank.begin(),rank.end(),rank.begin(),[](unsigned char c){return static_cast<char>(std::toupper(c));});
  if(rank.empty())rank="G";
  if(std::find(kContentRanks.begin(),kContentRanks.end(),rank)==kContentRanks.end())
    throw ApiError(drogon::k400BadRequest,"品階只能是 G/F/E/D/C/B/A/S/SS/SSS");
  return rank;
}


// [功能備註｜七元素魔法技能樹] 魔法節點只能落在七元素樹；舊資料的「無」保留作待整理節點。
const std::array<std::string,7> kMagicTreeElements={"暗","光","金","木","水","火","土"};
std::string normalizeMagicElement(std::string element,bool allowLegacyNone=false){
  element=trimmed(element);
  if(element.empty()&&allowLegacyNone)return "無";
  if(std::find(kMagicTreeElements.begin(),kMagicTreeElements.end(),element)==kMagicTreeElements.end()){
    if(allowLegacyNone&&element=="無")return element;
    throw ApiError(drogon::k400BadRequest,"魔法元素只能是 暗/光/金/木/水/火/土");
  }
  return element;
}

Json::Value normalizeMagicNodeStyle(const Json::Value& source,
                                    const std::string& fallbackBranch = {}) {
  static const std::set<std::string> allowedTypes{
      "normal", "passive", "core", "branch", "ultimate", "limited", "hidden", "awakening"};
  Json::Value style{Json::objectValue};
  const auto rawType = source.isObject() ? trimmed(stringValue(source["type"], "normal")) : std::string{"normal"};
  style["type"] = allowedTypes.contains(rawType) ? rawType : "normal";
  auto branch = source.isObject() ? trimmed(stringValue(source["branch"])) : std::string{};
  if (branch.empty()) branch = trimmed(fallbackBranch);
  if (branch.empty()) branch = "未分類";
  style["branch"] = clipped(branch, 80);
  return style;
}
std::string normalizeDungeonLevel(std::string level){
  level=trimmed(level);if(level.empty())level="1";
  if(level=="神")return level;
  try{const auto n=std::stoi(level);if(n>=1&&n<=10)return std::to_string(n);}catch(...){}
  throw ApiError(drogon::k400BadRequest,"副本等級只能是 1~10 或 神");
}

// [功能備註｜七元素魔法技能樹] 驗證 DM 設計的前置節點必須存在、不能指向自己，且必須位於同一元素樹。
void validateMagicTreePrerequisites(const Db& db, const std::string& element,
                                    const Json::Value& prerequisites,
                                    std::int64_t selfId = 0) {
  if (prerequisites.isNull()) return;
  if (!prerequisites.isArray())
    throw ApiError(drogon::k400BadRequest, "魔法前置節點必須是陣列");
  for (const auto& raw : prerequisites) {
    const auto id = intValue(raw);
    if (id <= 0) continue;
    if (selfId > 0 && id == selfId)
      throw ApiError(drogon::k400BadRequest, "魔法節點不能把自己設為前置");
    const auto rows = db->execSqlSync("SELECT element FROM magic_studies WHERE id=$1", id);
    if (rows.empty())
      throw ApiError(drogon::k400BadRequest, "魔法前置節點不存在");
    const auto parentElement = rows[0]["element"].isNull() ? std::string{"無"} : rows[0]["element"].as<std::string>();
    if (parentElement != element)
      throw ApiError(drogon::k400BadRequest, "前置魔法必須位於同一元素技能樹");
  }
}

std::string knowledgeContentTable(const std::string& type){
  if(type=="skill")return "skill_templates";
  if(type=="magic")return "magic_studies";
  if(type=="ritual")return "ritual_studies";
  throw ApiError(drogon::k400BadRequest,"內容類型只能是 skill/magic/ritual");
}
Json::Value knowledgeContent(const Db& db,const std::string& type,std::int64_t id){
  const auto table=knowledgeContentTable(type);
  const auto rows=db->execSqlSync("SELECT row_to_json(t)::text AS json_text FROM \""+table+"\" t WHERE id=$1",id);
  if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到要販售的技能／魔法／儀式");
  return rowJson(rows);
}
bool knowledgeUnlockRequired(const Db& db,std::int64_t userId,const std::string& type,std::int64_t contentId){
  const auto listed=db->execSqlSync("SELECT 1 FROM knowledge_shop_items WHERE active=TRUE AND content_type=$1 AND content_id=$2 LIMIT 1",type,contentId);
  if(listed.empty())return false;
  const auto owned=db->execSqlSync("SELECT 1 FROM knowledge_shop_purchases WHERE user_id=$1 AND content_type=$2 AND content_id=$3 LIMIT 1",userId,type,contentId);
  return owned.empty();
}
void requireKnowledgeUnlockIfSold(const Db& db,std::int64_t userId,const std::string& type,std::int64_t contentId){
  if(knowledgeUnlockRequired(db,userId,type,contentId))throw ApiError(drogon::k403Forbidden,"需要先在遊戲間／書庫購買對應學習書");
}
Json::Value knowledgeShopRows(const Db& db,const std::string& venue,bool activeOnly,std::int64_t userId=0){
  if(venue!="library"&&venue!="game_room")throw ApiError(drogon::k400BadRequest,"venue 必須是 library 或 game_room");
  Json::Value out{Json::arrayValue};
  auto rows=db->execSqlSync(std::string("SELECT row_to_json(k)::text AS json_text FROM knowledge_shop_items k WHERE venue=$1")+(activeOnly?" AND active=TRUE":"")+" ORDER BY id",venue);
  for(const auto& row:rows){
    auto item=parseJson(row["json_text"].as<std::string>());
    const auto type=stringValue(item["content_type"]);const auto cid=intValue(item["content_id"]);
    try{auto content=knowledgeContent(db,type,cid);item["content"]=content;if(stringValue(item["name"]).empty())item["name"]=content["name"];item["rank"]=normalizeContentRank(stringValue(item["rank"],stringValue(content["rank"],"G")));}catch(...){item["content_missing"]=true;}
    if(userId>0){const auto bought=db->execSqlSync("SELECT COUNT(*)::BIGINT AS n FROM knowledge_shop_purchases WHERE user_id=$1 AND shop_item_id=$2",userId,intValue(item["id"]));item["purchased_count"]=bought.empty()?0:Json::Int64(bought[0]["n"].as<std::int64_t>());}
    out.append(item);
  }
  return out;
}

bool validIdentifier(const std::string& name) {
  static const std::regex pattern("^[a-z_][a-z0-9_]*$");
  return std::regex_match(name, pattern);
}

std::set<std::string> tableColumns(const Db& db, const std::string& table) {
  if (!validIdentifier(table)) throw std::runtime_error("unsafe table identifier");
  std::set<std::string> columns;
  const auto rows = db->execSqlSync(
      "SELECT column_name FROM information_schema.columns "
      "WHERE table_schema=current_schema() AND table_name=$1",
      table);
  for (const auto& row : rows) columns.insert(row["column_name"].as<std::string>());
  return columns;
}

// [功能備註｜61-cpp.22.1 安全刪除地圖節點]
// 刪除前主動清除所有目前已知的節點引用。即使舊 PostgreSQL 的 FK 不是 ON DELETE SET NULL，DM 仍可安全刪除節點。
void clearMapNodeReference(const Db& db,
                           const std::string& table,
                           const std::string& column,
                           std::int64_t roomId,
                           std::int64_t nodeId) {
  if (!validIdentifier(table) || !validIdentifier(column)) return;
  const auto columns = tableColumns(db, table);
  if (!columns.contains(column)) return;
  std::string sql = "UPDATE \"" + table + "\" SET \"" + column + "\"=NULL WHERE \"" + column + "\"=$1";
  if (table == "rooms") {
    sql += " AND id=$2";
    db->execSqlSync(sql, nodeId, roomId);
    return;
  }
  if (columns.contains("room_id")) {
    sql += " AND room_id=$2";
    db->execSqlSync(sql, nodeId, roomId);
    return;
  }
  db->execSqlSync(sql, nodeId);
}

void prepareMapNodeDeletion(const std::shared_ptr<CoreService>& service,
                            std::int64_t roomId,
                            std::int64_t nodeId) {
  const auto db = service->database();
  const auto existing = db->execSqlSync("SELECT 1 FROM room_map_nodes WHERE id=$1 AND room_id=$2", nodeId, roomId);
  if (existing.empty()) throw ApiError(drogon::k404NotFound, "找不到地圖節點");

  // 先移除其他節點 connections 中對此節點的引用。
  Json::Value noLinks{Json::arrayValue};
  syncBidirectionalMapConnections(service, roomId, nodeId, noLinks);

  // 場景／玩家／隊伍目前位置。
  clearMapNodeReference(db, "rooms", "current_map_node_id", roomId, nodeId);
  clearMapNodeReference(db, "room_members", "current_map_node_id", roomId, nodeId);
  clearMapNodeReference(db, "room_teams", "current_map_node_id", roomId, nodeId);

  // 世界單位與掛在節點上的系統。
  clearMapNodeReference(db, "room_monsters", "map_node_id", roomId, nodeId);
  clearMapNodeReference(db, "room_npcs", "map_node_id", roomId, nodeId);
  clearMapNodeReference(db, "room_dungeons", "map_node_id", roomId, nodeId);
  clearMapNodeReference(db, "scheduled_world_events", "map_node_id", roomId, nodeId);
  clearMapNodeReference(db, "search_containers", "map_node_id", roomId, nodeId);
  clearMapNodeReference(db, "clue_templates", "map_node_id", roomId, nodeId);

  // 發現紀錄理論上有 ON DELETE CASCADE；這裡主動清理以相容舊 schema。
  const auto discoveryColumns = tableColumns(db, "map_node_discoveries");
  if (discoveryColumns.contains("node_id")) {
    db->execSqlSync("DELETE FROM map_node_discoveries WHERE node_id=$1", nodeId);
  }
}


Json::Value filterForTable(const Db& db, const std::string& table,
                           const Json::Value& input,
                           const Json::Value& extra = Json::Value{Json::objectValue}) {
  auto columns = tableColumns(db, table);
  Json::Value filtered{Json::objectValue};
  auto copy = [&](const Json::Value& value) {
    if (!value.isObject()) return;
    for (const auto& name : value.getMemberNames()) {
      if (name == "id" || name == "created_at" || name == "updated_at") continue;
      if (columns.contains(name) && validIdentifier(name)) filtered[name] = value[name];
    }
  };
  copy(input);
  copy(extra);
  return filtered;
}


// [功能備註｜65-cpp.26.14 裝備區] 將全域裝備模板轉成角色持有的裝備實例。
Json::Value gearInstanceFromTemplateRow(const Json::Value& row) {
  Json::Value gear = row["gear_data"].isObject() ? row["gear_data"] : Json::Value{Json::objectValue};
  gear["name"] = row["name"];
  gear["rank"] = row.get("rank", "G");
  gear["category"] = row.get("category", "裝備");
  gear["description"] = row.get("description", "");
  gear["effects"] = row["effects"].isObject() ? row["effects"] : Json::Value{Json::objectValue};
  gear["equip_slot_code"] = row.get("equip_slot_code", "");
  gear["weapon_hands"] = row.get("weapon_hands", 1);
  gear["affix_enabled"] = row.get("affix_enabled", false);
  if (!row["affix_pool_id"].isNull()) gear["affix_pool_id"] = row["affix_pool_id"];
  return gear;
}

Json::Value gearTemplateInstanceByName(const Db& db, const std::string& name, const std::string& kind) {
  try {
    const auto rows = db->execSqlSync(
        "SELECT row_to_json(i)::text AS json_text FROM item_templates i WHERE name=$1 AND template_kind=$2 LIMIT 1",
        name, kind);
    if (!rows.empty()) return gearInstanceFromTemplateRow(rowJson(rows));
  } catch (const std::exception& error) {
    LOG_WARN << "gear template lookup degraded for " << name << ": " << error.what();
  }
  Json::Value fallback; fallback["name"] = name; fallback["rank"] = "G";
  return fallback;
}

Json::Value genericInsert(const Db& db, const std::string& table,
                          const Json::Value& body,
                          const Json::Value& extra = Json::Value{Json::objectValue}) {
  if (!validIdentifier(table)) throw std::runtime_error("unsafe table identifier");
  const auto filtered = filterForTable(db, table, body, extra);
  const auto names = filtered.getMemberNames();
  if (names.empty()) throw ApiError(drogon::k400BadRequest, "沒有可建立的欄位");
  std::string columns;
  std::string selects;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i) {
      columns += ',';
      selects += ',';
    }
    columns += "\"" + names[i] + "\"";
    selects += "p.\"" + names[i] + "\"";
  }
  const auto sql = "INSERT INTO \"" + table + "\"(" + columns + ") "
                   "SELECT " + selects + " FROM jsonb_populate_record(NULL::\"" + table +
                   "\",$1::jsonb) p RETURNING row_to_json(\"" + table + "\")::text AS json_text";
  return rowJson(db->execSqlSync(sql, compactJson(filtered)));
}

Json::Value genericUpdate(const Db& db, const std::string& table,
                          std::int64_t id, const Json::Value& body) {
  if (!validIdentifier(table)) throw std::runtime_error("unsafe table identifier");
  const auto filtered = filterForTable(db, table, body);
  const auto names = filtered.getMemberNames();
  if (names.empty()) {
    const auto rows = db->execSqlSync(
        "SELECT row_to_json(t)::text AS json_text FROM \"" + table + "\" t WHERE id=$1", id);
    if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到資料");
    return rowJson(rows);
  }
  const auto columns = tableColumns(db, table);
  std::string assignments;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i) assignments += ',';
    assignments += "\"" + names[i] + "\"=p.\"" + names[i] + "\"";
  }
  if (columns.contains("updated_at")) assignments += ",updated_at=CURRENT_TIMESTAMP";
  const auto sql = "WITH p AS (SELECT * FROM jsonb_populate_record(NULL::\"" + table +
                   "\",$1::jsonb)) UPDATE \"" + table + "\" t SET " + assignments +
                   " FROM p WHERE t.id=$2 RETURNING row_to_json(t)::text AS json_text";
  const auto rows = db->execSqlSync(sql, compactJson(filtered), id);
  if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到資料");
  return rowJson(rows);
}


void genericUpdateByKeys(const Db& db, const std::string& table,
                         const Json::Value& body,
                         const std::vector<std::pair<std::string,std::int64_t>>& keys) {
  if (!validIdentifier(table) || keys.empty()) throw std::runtime_error("unsafe keyed update");
  const auto filtered = filterForTable(db, table, body);
  const auto names = filtered.getMemberNames();
  if (names.empty()) return;
  const auto columns = tableColumns(db, table);
  std::string assignments;
  for (std::size_t i=0;i<names.size();++i) {
    if (i) assignments += ',';
    assignments += "\"" + names[i] + "\"=p.\"" + names[i] + "\"";
  }
  if (columns.contains("updated_at")) assignments += ",updated_at=CURRENT_TIMESTAMP";
  std::string where;
  for (std::size_t i=0;i<keys.size();++i) {
    if (!validIdentifier(keys[i].first)) throw std::runtime_error("unsafe key identifier");
    if (i) where += " AND ";
    where += "t.\"" + keys[i].first + "\"=" + std::to_string(keys[i].second);
  }
  const auto sql = "WITH p AS (SELECT * FROM jsonb_populate_record(NULL::\"" + table +
                   "\",$1::jsonb)) UPDATE \"" + table + "\" t SET " + assignments +
                   " FROM p WHERE " + where;
  db->execSqlSync(sql, compactJson(filtered));
}

void genericDelete(const Db& db, const std::string& table, std::int64_t id) {
  if (!validIdentifier(table)) throw std::runtime_error("unsafe table identifier");
  const auto rows = db->execSqlSync(
      "DELETE FROM \"" + table + "\" WHERE id=$1 RETURNING id", id);
  if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到資料");
}

Json::Value genericList(const Db& db, const std::string& table,
                        const std::string& order = "id ASC",
                        const std::string& where = {}) {
  if (!validIdentifier(table)) throw std::runtime_error("unsafe table identifier");
  std::string sql = "SELECT row_to_json(t)::text AS json_text FROM \"" + table + "\" t";
  if (!where.empty()) sql += " WHERE " + where;
  if (!order.empty()) sql += " ORDER BY " + order;
  return selectRows(db, sql);
}


// [功能備註｜商店分類] 商店分類共用查詢。玩家只看啟用分類，DM 可看全部分類。
Json::Value shopCategoryList(const Db& db, bool activeOnly) {
  return genericList(db, "shop_categories", "sort_order ASC,name ASC,id ASC",
                     activeOnly ? std::string{"active=TRUE"} : std::string{});
}

// [功能備註｜商店分類] 商品查詢會把正式 category_id 與舊版 item_category 合併回傳，確保舊資料仍可顯示。
Json::Value shopItemList(const Db& db, bool activeOnly) {
  std::string sql =
      "SELECT row_to_json(x)::text AS json_text FROM ("
      "SELECT s.*,COALESCE(c.name,NULLIF(BTRIM(s.item_category),''),'未分類') AS category_name,"
      "COALESCE(c.sort_order,9999) AS category_sort "
      "FROM shop_items s LEFT JOIN shop_categories c ON c.id=s.category_id";
  if (activeOnly) sql += " WHERE s.active=TRUE";
  sql += " ORDER BY COALESCE(c.sort_order,9999),COALESCE(c.name,NULLIF(BTRIM(s.item_category),''),'未分類'),s.id) x";
  return selectRows(db, sql);
}

// [功能備註｜商店分類] 新增／修改商品時由 C++ 驗證 category_id，並同步舊版 item_category 文字欄位。
void syncShopItemCategory(const Db& db, Json::Value& body) {
  if (body.isMember("category_id")) {
    if (body["category_id"].isNull() || intValue(body["category_id"]) <= 0) {
      body["category_id"] = Json::nullValue;
      body["item_category"] = "";
      return;
    }
    const auto categoryId = intValue(body["category_id"]);
    const auto rows = db->execSqlSync(
        "SELECT name FROM shop_categories WHERE id=$1 AND active=TRUE", categoryId);
    if (rows.empty()) throw ApiError(drogon::k400BadRequest, "商店分類不存在或已停用");
    body["category_id"] = Json::Int64(categoryId);
    body["item_category"] = rows[0]["name"].as<std::string>();
    return;
  }

  // 相容舊版管理端：若仍送 item_category 文字，自動建立／復用正式分類。
  if (body.isMember("item_category")) {
    const auto name = trimmed(stringValue(body["item_category"]));
    body["item_category"] = name;
    if (name.empty()) return;
    if (name.size() > 80) throw ApiError(drogon::k400BadRequest, "商店分類名稱最多 80 字");
    const auto rows = db->execSqlSync(
        "INSERT INTO shop_categories(name,sort_order,active) VALUES($1,80,TRUE) "
        "ON CONFLICT(name) DO UPDATE SET active=TRUE,updated_at=CURRENT_TIMESTAMP "
        "RETURNING id", name);
    body["category_id"] = Json::Int64(rows[0]["id"].as<std::int64_t>());
  }
}


const std::array<const char*,7> kStoredElements={"暗","光","金","木","水","火","土"};

Json::Value zeroElementMap() {
  Json::Value out{Json::objectValue};
  for (const auto* element : kStoredElements) out[element] = Json::Int64(0);
  return out;
}

void addElementCapBonus(Json::Value& target, const Json::Value& source,
                        std::int64_t multiplier = 1) {
  if (!target.isObject()) target = zeroElementMap();
  if (!source.isObject() || multiplier == 0) return;
  for (const auto* element : kStoredElements) {
    const auto delta = intValue(source[element]) * multiplier;
    target[element] = Json::Int64(std::max<std::int64_t>(
        0, intValue(target[element]) + delta));
  }
}

Json::Value normalizedElementMap(const Json::Value& value) {
  Json::Value out = zeroElementMap();
  if (!value.isObject()) return out;
  for (const auto* element : kStoredElements) {
    out[element] = Json::Int64(std::max<std::int64_t>(0, intValue(value[element])));
  }
  return out;
}

Json::Value enrichElementStorage(const Db& db, Json::Value character) {
  auto baseCaps = normalizedElementMap(character["element_storage_base_caps"]);
  auto skillBonus = zeroElementMap();
  auto itemBonus = zeroElementMap();
  auto studyBonus = zeroElementMap();

  const auto skills = character["skills"].isArray()
      ? character["skills"] : Json::Value{Json::arrayValue};
  for (const auto& skill : skills) {
    if (!skill.isObject()) continue;
    const auto data = skill["data"].isObject()
        ? skill["data"] : Json::Value{Json::objectValue};
    addElementCapBonus(skillBonus, data["element_storage_cap_bonus"]);
  }

  std::unordered_map<std::string,std::int64_t> held;
  const auto collectItem = [&](const Json::Value& raw) {
    std::string name;
    std::int64_t qty = 1;
    if (raw.isString()) name = raw.asString();
    else if (raw.isObject()) {
      name = stringValue(raw["name"], stringValue(raw["title"]));
      qty = std::max<std::int64_t>(1, intValue(raw["quantity"], 1));
    }
    name = trimmed(name);
    if (!name.empty()) held[name] += qty;
  };
  if (character["inventory"].isArray()) for (const auto& item : character["inventory"]) collectItem(item);
  if (character["equipment"].isArray()) for (const auto& item : character["equipment"]) collectItem(item);
  if (character["weapons"].isArray()) for (const auto& item : character["weapons"]) collectItem(item);

  for (const auto& [name, qty] : held) {
    const auto rows = db->execSqlSync(
        "SELECT effects::text AS effects_text FROM item_templates WHERE name=$1 LIMIT 1", name);
    if (rows.empty() || rows[0]["effects_text"].isNull()) continue;
    const auto effects = parseJson(rows[0]["effects_text"].as<std::string>(),
                                   Json::Value{Json::objectValue});
    addElementCapBonus(itemBonus, effects["element_storage_cap_bonus"], qty);
  }

  const auto magicRows = db->execSqlSync(
      "SELECT m.effects::text AS effects_text FROM character_magics cm "
      "JOIN magic_studies m ON m.id=cm.magic_id "
      "WHERE cm.user_id=$1 AND m.active=TRUE", intValue(character["user_id"]));
  for (const auto& row : magicRows) {
    if (row["effects_text"].isNull()) continue;
    const auto effects = parseJson(row["effects_text"].as<std::string>(),
                                   Json::Value{Json::objectValue});
    addElementCapBonus(studyBonus, effects["element_storage_cap_bonus"]);
  }
  const auto ritualRows = db->execSqlSync(
      "SELECT r.effects::text AS effects_text FROM character_rituals cr "
      "JOIN ritual_studies r ON r.id=cr.ritual_id "
      "WHERE cr.user_id=$1 AND r.active=TRUE", intValue(character["user_id"]));
  for (const auto& row : ritualRows) {
    if (row["effects_text"].isNull()) continue;
    const auto effects = parseJson(row["effects_text"].as<std::string>(),
                                   Json::Value{Json::objectValue});
    addElementCapBonus(studyBonus, effects["element_storage_cap_bonus"]);
  }

  auto caps = baseCaps;
  addElementCapBonus(caps, skillBonus);
  addElementCapBonus(caps, itemBonus);
  addElementCapBonus(caps, studyBonus);

  auto current = normalizedElementMap(character["element_storage"]);
  for (const auto* element : kStoredElements) {
    current[element] = Json::Int64(std::min<std::int64_t>(
        intValue(current[element]), intValue(caps[element])));
  }

  Json::Value sources{Json::objectValue};
  sources["base"] = baseCaps;
  sources["skills"] = skillBonus;
  sources["items"] = itemBonus;
  sources["studies"] = studyBonus;

  character["element_storage_base_caps"] = std::move(baseCaps);
  character["element_storage_caps"] = std::move(caps);
  character["element_storage"] = std::move(current);
  character["element_storage_cap_sources"] = std::move(sources);
  return character;
}

// [功能備註｜元素儲存] 回傳角色時即時計算七元素容量。
// 初始 base cap 全為 0；技能 data.element_storage_cap_bonus、道具 effects.element_storage_cap_bonus
// 與已學魔法／儀式可疊加。單一元素道具只需讓其他六項保持 0。
Json::Value characterRaw(const Db& db, std::int64_t userId) {
  const auto rows = db->execSqlSync(
      "SELECT row_to_json(c)::text AS json_text FROM character_cards c WHERE user_id=$1", userId);
  if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到角色卡");
  auto character = enrichElementStorage(db, rowJson(rows));
  // 65-cpp.26.11：raw API 也提供精神力，避免使用技能後前端暫時失去 MP 顯示。
  const auto maxMp = std::max<std::int64_t>(0, intValue(character["spirit"]) * 2 + 10);
  const auto currentMp = character["current_mental_power"].isNull()
                             ? maxMp
                             : std::clamp<std::int64_t>(intValue(character["current_mental_power"]), 0, maxMp);
  character["max_mental_power"] = Json::Int64(maxMp);
  character["mental_power"] = Json::Int64(currentMp);
  character["current_mental_power"] = Json::Int64(currentMp);
  return character;
}

Json::Value applyTimeControlSkill(const std::shared_ptr<CoreService>& service,
                                  std::int64_t roomId, std::int64_t casterUserId,
                                  const Json::Value& skill,
                                  const std::string& requestedTargetType,
                                  std::int64_t requestedTargetId) {
  Json::Value out{Json::objectValue};
  const auto data = skill["data"].isObject() ? skill["data"] : Json::Value{Json::objectValue};
  const auto cfg = data["time_control"].isObject() ? data["time_control"] : Json::Value{Json::objectValue};
  if (!cfg.get("enabled", false).asBool()) return out;
  if (roomId <= 0) throw ApiError(drogon::k400BadRequest, "時間／拉條技能必須在房間內使用");

  const auto targetMode = stringValue(data["combat_target_mode"], "self");
  std::string targetType = requestedTargetType;
  std::int64_t targetId = requestedTargetId;
  if (targetMode == "self") { targetType = "player"; targetId = casterUserId; }
  if (targetId <= 0 || (targetType != "player" && targetType != "npc" && targetType != "monster"))
    throw ApiError(drogon::k400BadRequest, "請選擇時間技能目標");

  // 後端驗證友方／敵方，避免只靠前端下拉選單。
  if (targetMode.rfind("ally", 0) == 0) {
    bool ally = false;
    if (targetType == "player") {
      ally = !service->database()->execSqlSync(
          "SELECT 1 FROM room_members WHERE room_id=$1 AND user_id=$2 AND role='player'", roomId, targetId).empty();
    } else if (targetType == "npc") {
      ally = !service->database()->execSqlSync(
          "SELECT 1 FROM room_npcs WHERE room_id=$1 AND npc_template_id=$2 AND combat_side='ally'", roomId, targetId).empty();
    }
    if (!ally) throw ApiError(drogon::k403Forbidden, "時間加速只能作用在友方目標");
  }
  if (targetMode.rfind("enemy", 0) == 0) {
    bool enemy = false;
    if (targetType == "monster") {
      enemy = !service->database()->execSqlSync(
          "SELECT 1 FROM room_monsters WHERE room_id=$1 AND id=$2 AND status='encountered'", roomId, targetId).empty();
    } else if (targetType == "npc") {
      enemy = !service->database()->execSqlSync(
          "SELECT 1 FROM room_npcs WHERE room_id=$1 AND npc_template_id=$2 AND combat_side='enemy'", roomId, targetId).empty();
    }
    if (!enemy) throw ApiError(drogon::k403Forbidden, "這個時間技能需要敵方目標");
  }

  const auto actionAdvance = std::clamp<std::int64_t>(intValue(cfg["action_advance"]), 0, 20);
  const auto recoveryRounds = std::clamp<std::int64_t>(intValue(cfg["negative_status_recovery_rounds"]), 0, 100);
  const auto mpRestore = std::clamp<std::int64_t>(intValue(cfg["mental_power_restore"]), 0, 1000000000LL);

  if (actionAdvance > 0) {
    if (targetType == "player") service->database()->execSqlSync(
        "UPDATE room_members SET turn_actions_remaining=turn_actions_remaining+$1 WHERE room_id=$2 AND user_id=$3", actionAdvance, roomId, targetId);
    else if (targetType == "monster") service->database()->execSqlSync(
        "UPDATE room_monsters SET turn_actions_remaining=turn_actions_remaining+$1 WHERE room_id=$2 AND id=$3", actionAdvance, roomId, targetId);
    else service->database()->execSqlSync(
        "UPDATE room_npcs SET turn_actions_remaining=turn_actions_remaining+$1 WHERE room_id=$2 AND npc_template_id=$3", actionAdvance, roomId, targetId);
    out["action_advance"] = Json::Int64(actionAdvance);
  }

  if (recoveryRounds > 0) {
    const auto beforeRows = service->database()->execSqlSync(
        "SELECT COUNT(*)::BIGINT AS n FROM combat_status_instances WHERE room_id=$1 AND target_type=$2 AND target_id=$3 AND active=TRUE AND polarity='negative' AND remaining_rounds>0",
        roomId, targetType, targetId);
    const auto affected = beforeRows.empty() ? 0 : beforeRows[0]["n"].as<std::int64_t>();
    service->database()->execSqlSync(
        "UPDATE combat_status_instances SET remaining_rounds=GREATEST(0,remaining_rounds-$1),"
        "active=CASE WHEN remaining_rounds-$1<=0 THEN FALSE ELSE active END,updated_at=CURRENT_TIMESTAMP "
        "WHERE room_id=$2 AND target_type=$3 AND target_id=$4 AND active=TRUE AND polarity='negative' AND remaining_rounds>0",
        recoveryRounds, roomId, targetType, targetId);
    out["negative_status_recovery_rounds"] = Json::Int64(recoveryRounds);
    out["negative_statuses_affected"] = Json::Int64(affected);
  }

  if (mpRestore > 0 && targetType == "player") {
    const auto targetCharacter = service->characterByUser(targetId);
    if (!targetCharacter.isNull()) {
      const auto maxMp = std::max<std::int64_t>(0, intValue(targetCharacter["max_mental_power"]));
      const auto before = std::clamp<std::int64_t>(intValue(targetCharacter["mental_power"]), 0, maxMp);
      const auto after = std::min<std::int64_t>(maxMp, before + mpRestore);
      service->database()->execSqlSync(
          "UPDATE character_cards SET current_mental_power=$1,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2", after, targetId);
      out["mental_power_restore"] = Json::Int64(after - before);
      out["mental_power_before"] = Json::Int64(before);
      out["mental_power_after"] = Json::Int64(after);
    }
  }

  out["enabled"] = true;
  out["target_type"] = targetType;
  out["target_id"] = Json::Int64(targetId);
  Json::Value payload;
  payload["text"] = stringValue(skill["name"], "時間技能") + " 改變了目標的時間進度";
  payload["target_type"] = targetType; payload["target_id"] = Json::Int64(targetId);
  payload["time_control"] = out;
  service->addEvent(roomId, casterUserId, "time_control", payload);
  return out;
}

void ensureCharacter(const Db& db, std::int64_t userId) {
  db->execSqlSync("INSERT INTO character_cards(user_id) VALUES($1) ON CONFLICT(user_id) DO NOTHING", userId);
}

Json::Value inventoryOf(const Json::Value& character) {
  return character["inventory"].isArray() ? character["inventory"] : Json::Value{Json::arrayValue};
}

std::int64_t inventoryQuantity(const Json::Value& inventory,
                               const std::string& name,
                               const std::string& category = {}) {
  std::int64_t total = 0;
  if (!inventory.isArray()) return total;
  for (const auto& item : inventory) {
    const auto itemName = item.isString() ? item.asString() : stringValue(item["name"]);
    const auto itemCategory = item.isObject() ? stringValue(item["category"]) : std::string{};
    if (itemName != name) continue;
    if (!category.empty() && itemCategory != category) continue;
    total += item.isObject() ? std::max<std::int64_t>(1, intValue(item["quantity"], 1)) : 1;
  }
  return total;
}

Json::Value changeInventory(Json::Value inventory, const std::string& name,
                            std::int64_t delta, const std::string& category = {},
                            const Json::Value& data = Json::Value{Json::objectValue}) {
  if (!inventory.isArray()) inventory = Json::arrayValue;
  for (Json::ArrayIndex i = 0; i < inventory.size(); ++i) {
    auto& item = inventory[i];
    const auto itemName = item.isString() ? item.asString() : stringValue(item["name"]);
    const auto itemCategory = item.isObject() ? stringValue(item["category"]) : std::string{};
    if (itemName != name || (!category.empty() && itemCategory != category)) continue;
    const auto next = (item.isObject() ? std::max<std::int64_t>(1, intValue(item["quantity"], 1)) : 1) + delta;
    if (next <= 0) {
      Json::Value nextInventory{Json::arrayValue};
      for (Json::ArrayIndex j = 0; j < inventory.size(); ++j) if (j != i) nextInventory.append(inventory[j]);
      return nextInventory;
    }
    if (!item.isObject()) {
      Json::Value converted;
      converted["name"] = name;
      converted["category"] = category;
      converted["quantity"] = Json::Int64(next);
      item = std::move(converted);
    } else {
      item["quantity"] = Json::Int64(next);
    }
    return inventory;
  }
  if (delta > 0) {
    Json::Value item = data.isObject() ? data : Json::Value{Json::objectValue};
    item["name"] = name;
    if (!category.empty()) item["category"] = category;
    item["quantity"] = Json::Int64(delta);
    inventory.append(std::move(item));
  }
  return in…104091 tokens truncated…est& req,Callback& cb,const std::string&idText){runHandler(cb,[&]{requireUser(service,req,true);genericDelete(service->database(),"magic_studies",positiveId(idText));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"刪除魔法節點失敗");});

  // [功能備註｜儀式學｜GET/POST /api/admin/ritual-studies]
  reg0("/api/admin/ritual-studies",drogon::Get,[service](const Request& req,Callback& cb){
    runHandler(cb,[&]{requireUser(service,req,true);Json::Value out;out["studies"]=genericList(service->database(),"ritual_studies","category ASC,rank ASC,name ASC,id ASC");reply(cb,std::move(out));},"讀取儀式學資料庫失敗");
  });
  reg0("/api/admin/ritual-studies",drogon::Post,[service](const Request& req,Callback& cb){
    runHandler(cb,[&]{
      requireUser(service,req,true);auto body=bodyOrEmpty(req);const auto name=trimmed(stringValue(body["name"]));
      if(name.empty())throw ApiError(drogon::k400BadRequest,"儀式名稱不能空白");
      body["name"]=clipped(name,120);body["rank"]=normalizeContentRank(stringValue(body["rank"],"G"));
      body["point_cost"]=Json::Int64(std::max<std::int64_t>(0,intValue(body["point_cost"],1)));
      body["cast_rounds"]=Json::Int64(std::max<std::int64_t>(0,intValue(body["cast_rounds"])));
      body["ascension_from_tier"]=Json::Int64(std::clamp<std::int64_t>(intValue(body["ascension_from_tier"]),0,9));
      body["ascension_to_tier"]=Json::Int64(std::clamp<std::int64_t>(intValue(body["ascension_to_tier"]),0,10));
      if(intValue(body["ascension_to_tier"])>0&&intValue(body["ascension_from_tier"])>0&&intValue(body["ascension_to_tier"])!=intValue(body["ascension_from_tier"])+1)
        throw ApiError(drogon::k400BadRequest,"登階儀式只能設定相鄰大階");
      body["mystery_name"]=clipped(trimmed(stringValue(body["mystery_name"],"未解析儀式")),120);
      body["research_progress_per_point"]=Json::Int64(std::clamp<std::int64_t>(intValue(body["research_progress_per_point"],10),1,100));
      body["research_xp_per_point"]=Json::Int64(std::clamp<std::int64_t>(intValue(body["research_xp_per_point"],5),0,20));
      body["first_success_xp"]=Json::Int64(std::max<std::int64_t>(0,intValue(body["first_success_xp"])));
      body["repeat_success_xp"]=Json::Int64(std::max<std::int64_t>(0,intValue(body["repeat_success_xp"])));
      body["failure_research_xp"]=Json::Int64(std::max<std::int64_t>(0,intValue(body["failure_research_xp"])));
      body["failure_research_progress"]=Json::Int64(std::clamp<std::int64_t>(intValue(body["failure_research_progress"]),0,100));
      if(!body["research_reveal_rules"].isObject())body["research_reveal_rules"]=parseJson(
        R"({"name":10,"description":10,"materials":20,"elements":30,"layout":50,"steps":70,"success":90,"failure":100})");
      if(!body["layout_definition"].isObject())body["layout_definition"]=Json::Value{Json::objectValue};
      if(!body["layout_definition"]["slots"].isArray())body["layout_definition"]["slots"]=Json::Value{Json::arrayValue};
      if(!body["layout_definition"]["connections"].isArray())body["layout_definition"]["connections"]=Json::Value{Json::arrayValue};
      for(const auto* key:{"material_requirements","ritual_steps","success_effects","failure_effects"})if(!body[key].isArray())body[key]=Json::Value{Json::arrayValue};
      if(!body["element_requirements"].isObject())body["element_requirements"]=Json::Value{Json::objectValue};
      Json::Value out;out["ok"]=true;out["study"]=genericInsert(service->database(),"ritual_studies",body);reply(cb,std::move(out));
    },"建立儀式失敗");
  });
  reg1("/api/admin/ritual-studies/{1}",drogon::Patch,[service](const Request& req,Callback& cb,const std::string&idText){
    runHandler(cb,[&]{
      requireUser(service,req,true);auto body=bodyOrEmpty(req);
      if(body.isMember("rank"))body["rank"]=normalizeContentRank(stringValue(body["rank"]));
      if(body.isMember("cast_rounds"))body["cast_rounds"]=Json::Int64(std::max<std::int64_t>(0,intValue(body["cast_rounds"])));
      if(body.isMember("ascension_from_tier"))body["ascension_from_tier"]=Json::Int64(std::clamp<std::int64_t>(intValue(body["ascension_from_tier"]),0,9));
      if(body.isMember("ascension_to_tier"))body["ascension_to_tier"]=Json::Int64(std::clamp<std::int64_t>(intValue(body["ascension_to_tier"]),0,10));
      if(body.isMember("ascension_from_tier")&&body.isMember("ascension_to_tier")&&intValue(body["ascension_to_tier"])>0&&intValue(body["ascension_from_tier"])>0&&intValue(body["ascension_to_tier"])!=intValue(body["ascension_from_tier"])+1)
        throw ApiError(drogon::k400BadRequest,"登階儀式只能設定相鄰大階");
      if(body.isMember("mystery_name"))body["mystery_name"]=clipped(trimmed(stringValue(body["mystery_name"],"未解析儀式")),120);
      if(body.isMember("research_progress_per_point"))body["research_progress_per_point"]=Json::Int64(std::clamp<std::int64_t>(intValue(body["research_progress_per_point"],10),1,100));
      if(body.isMember("research_xp_per_point"))body["research_xp_per_point"]=Json::Int64(std::clamp<std::int64_t>(intValue(body["research_xp_per_point"],5),0,20));
      for(const auto* key:{"first_success_xp","repeat_success_xp","failure_research_xp"})if(body.isMember(key))body[key]=Json::Int64(std::max<std::int64_t>(0,intValue(body[key])));
      if(body.isMember("failure_research_progress"))body["failure_research_progress"]=Json::Int64(std::clamp<std::int64_t>(intValue(body["failure_research_progress"]),0,100));
      Json::Value out;out["ok"]=true;out["study"]=genericUpdate(service->database(),"ritual_studies",positiveId(idText),body);reply(cb,std::move(out));
    },"更新儀式失敗");
  });
  reg1("/api/admin/ritual-studies/{1}",drogon::Delete,[service](const Request& req,Callback& cb,const std::string&idText){runHandler(cb,[&]{requireUser(service,req,true);genericDelete(service->database(),"ritual_studies",positiveId(idText));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"刪除儀式失敗");});


  // =========================================================
  // [功能備註｜63-cpp.24 儀式場]
  // 第二面板：房間內建立 RitualInstance、放置佈置節點、驗證、開始／推進／中斷儀式。
  // =========================================================
  reg1("/api/rooms/{1}/ritual-field",drogon::Get,[service](const Request& req,Callback& cb,const std::string&roomText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomText);requireRoomAccess(service,user,roomId);Json::Value out;out["instances"]=selectRows1(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT i.*,r.name,r.rank,r.category,r.ritual_type,r.description,r.layout_notes,r.layout_definition,r.material_requirements,r.element_requirements,r.ritual_steps,r.success_effects,r.failure_effects,r.can_cooperate,r.cast_rounds,r.first_success_xp,r.repeat_success_xp,r.failure_research_xp,r.failure_research_progress,r.ascension_from_tier,r.ascension_to_tier FROM ritual_instances i JOIN ritual_studies r ON r.id=i.ritual_id WHERE i.room_id=$1 ORDER BY i.id DESC) x",roomId);for(auto& x:out["instances"])x["validation"]=ritualLayoutValidation(x,x["placements"]);out["learned"]=selectRows1(service->database(),"SELECT row_to_json(r)::text AS json_text FROM character_rituals cr JOIN ritual_studies r ON r.id=cr.ritual_id WHERE cr.user_id=$1 AND r.active=TRUE ORDER BY r.rank,r.name",user.id);reply(cb,std::move(out));},"讀取儀式場失敗");});
  reg1("/api/rooms/{1}/ritual-field",drogon::Post,[service](const Request& req,Callback& cb,const std::string&roomText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomText);requireRoomAccess(service,user,roomId);const auto ritualId=intValue(bodyOrEmpty(req)["ritual_id"]);if(ritualId<=0)throw ApiError(drogon::k400BadRequest,"請選擇儀式");if(!user.isAdmin&&service->database()->execSqlSync("SELECT 1 FROM character_rituals WHERE user_id=$1 AND ritual_id=$2",user.id,ritualId).empty())throw ApiError(drogon::k403Forbidden,"你尚未學會此儀式");const auto rr=service->database()->execSqlSync("SELECT 1 FROM ritual_studies WHERE id=$1 AND active=TRUE",ritualId);if(rr.empty())throw ApiError(drogon::k404NotFound,"找不到此儀式");const auto rows=service->database()->execSqlSync("INSERT INTO ritual_instances(room_id,ritual_id,created_by_user_id) VALUES($1,$2,$3) RETURNING id",roomId,ritualId,user.id);const auto id=rows[0]["id"].as<std::int64_t>();Json::Value ev;ev["text"]="建立儀式場";ev["ritual_instance_id"]=Json::Int64(id);ev["ritual_id"]=Json::Int64(ritualId);service->addEvent(roomId,user.id,"ritual_created",ev);evaluateRuleEvent(service,roomId,user.id,"RITUAL_CREATED",ev,false);Json::Value out;out["ok"]=true;out["instance"]=ritualInstanceJson(service,id);reply(cb,std::move(out));},"建立儀式場失敗");});
  reg2("/api/rooms/{1}/ritual-field/{2}/place",drogon::Post,[service](const Request& req,Callback& cb,const std::string&roomText,const std::string&instanceText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomText),instanceId=positiveId(instanceText);requireRoomAccess(service,user,roomId);auto inst=ritualInstanceJson(service,instanceId);if(intValue(inst["room_id"])!=roomId)throw ApiError(drogon::k404NotFound,"找不到儀式場");if(stringValue(inst["status"])!="placing"&&stringValue(inst["status"])!="ready")throw ApiError(drogon::k409Conflict,"儀式進行中，不能更改佈置");auto body=bodyOrEmpty(req);const auto slotId=trimmed(stringValue(body["slot_id"]));if(slotId.empty())throw ApiError(drogon::k400BadRequest,"缺少儀式節點");bool allowed=false;const auto slots=inst["layout_definition"]["slots"];if(slots.isArray())for(const auto& slot:slots)if(stringValue(slot["id"],stringValue(slot["key"]))==slotId){allowed=true;break;}if(!allowed)throw ApiError(drogon::k400BadRequest,"配方沒有此佈置節點");auto placements=inst["placements"].isObject()?inst["placements"]:Json::Value{Json::objectValue};Json::Value placed;placed["label"]=clipped(trimmed(stringValue(body["label"],stringValue(body["item_name"],"已放置"))),120);placed["kind"]=clipped(trimmed(stringValue(body["kind"],"material")),40);placed["quantity"]=Json::Int64(std::max<std::int64_t>(1,intValue(body["quantity"],1)));placed["placed_by_user_id"]=Json::Int64(user.id);placements[slotId]=placed;service->database()->execSqlSync("UPDATE ritual_instances SET placements=$1::jsonb,status='placing',updated_at=CURRENT_TIMESTAMP WHERE id=$2 AND room_id=$3",compactJson(placements),instanceId,roomId);auto next=ritualInstanceJson(service,instanceId);if(next["validation"]["ready"].asBool())service->database()->execSqlSync("UPDATE ritual_instances SET status='ready',updated_at=CURRENT_TIMESTAMP WHERE id=$1",instanceId);Json::Value ev;ev["ritual_instance_id"]=Json::Int64(instanceId);ev["slot_id"]=slotId;ev["placement"]=placed;service->addEvent(roomId,user.id,"ritual_item_placed",ev);evaluateRuleEvent(service,roomId,user.id,"RITUAL_ITEM_PLACED",ev,false);Json::Value out;out["ok"]=true;out["instance"]=ritualInstanceJson(service,instanceId);reply(cb,std::move(out));},"放置儀式材料失敗");});
  reg2("/api/rooms/{1}/ritual-field/{2}/remove",drogon::Post,[service](const Request& req,Callback& cb,const std::string&roomText,const std::string&instanceText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomText),instanceId=positiveId(instanceText);requireRoomAccess(service,user,roomId);auto inst=ritualInstanceJson(service,instanceId);if(intValue(inst["room_id"])!=roomId)throw ApiError(drogon::k404NotFound,"找不到儀式場");const auto slotId=trimmed(stringValue(bodyOrEmpty(req)["slot_id"]));auto placements=inst["placements"].isObject()?inst["placements"]:Json::Value{Json::objectValue};if(placements.isMember(slotId))placements.removeMember(slotId);service->database()->execSqlSync("UPDATE ritual_instances SET placements=$1::jsonb,status='placing',updated_at=CURRENT_TIMESTAMP WHERE id=$2 AND room_id=$3",compactJson(placements),instanceId,roomId);Json::Value out;out["ok"]=true;out["instance"]=ritualInstanceJson(service,instanceId);reply(cb,std::move(out));},"移除儀式材料失敗");});
  reg2("/api/rooms/{1}/ritual-field/{2}/start",drogon::Post,[service](const Request& req,Callback& cb,const std::string&roomText,const std::string&instanceText){runHandler(cb,[&]{
    const auto user=requireUser(service,req);const auto roomId=positiveId(roomText),instanceId=positiveId(instanceText);requireRoomAccess(service,user,roomId);
    auto inst=ritualInstanceJson(service,instanceId);if(intValue(inst["room_id"])!=roomId)throw ApiError(drogon::k404NotFound,"找不到儀式場");
    const auto ritualOwnerId=intValue(inst["created_by_user_id"],user.id);
    if(!user.isAdmin&&ritualOwnerId!=user.id)throw ApiError(drogon::k403Forbidden,"只有主持者或 DM 可以啟動儀式");
    const auto currentStatus=stringValue(inst["status"]);if(currentStatus!="placing"&&currentStatus!="ready")throw ApiError(drogon::k409Conflict,"此儀式已經開始或結束");
    if(!inst["validation"]["ready"].asBool())throw ApiError(drogon::k400BadRequest,"儀式佈置尚未完成");
    // [功能備註｜64-cpp.25 登階儀式前置驗證] 在扣除材料前確認角色已達本階4級且儀式對應下一階，避免成功標記後才發現不能登階。
    validateAscensionRitual(service->database(),ritualOwnerId,inst);
    // [功能備註｜63-cpp.24 儀式材料／元素消耗] 啟動前由 C++ 再驗證真正資源，成功啟動才扣除。
    auto character=characterRaw(service->database(),user.id);auto inventory=inventoryOf(character);
    const auto materials=inst["material_requirements"].isArray()?inst["material_requirements"]:Json::Value{Json::arrayValue};
    for(const auto& material:materials){const auto name=trimmed(stringValue(material["name"],stringValue(material["item"])));const auto category=trimmed(stringValue(material["category"]));const auto qty=std::max<std::int64_t>(1,intValue(material["quantity"],1));if(!name.empty()&&inventoryQuantity(inventory,name,category)<qty&&inventoryQuantity(inventory,name)<qty)throw ApiError(drogon::k400BadRequest,"儀式材料不足："+name+" ×"+std::to_string(qty));}
    auto elementStorage=character["element_storage"].isObject()?character["element_storage"]:Json::Value{Json::objectValue};const auto elements=inst["element_requirements"].isObject()?inst["element_requirements"]:Json::Value{Json::objectValue};
    for(const auto& element:elements.getMemberNames()){const auto need=std::max<std::int64_t>(0,intValue(elements[element]));if(need>0&&intValue(elementStorage[element])<need)throw ApiError(drogon::k400BadRequest,"儀式元素不足："+element+" 需要 "+std::to_string(need));}
    for(const auto& material:materials){const auto name=trimmed(stringValue(material["name"],stringValue(material["item"])));const auto category=trimmed(stringValue(material["category"]));const auto qty=std::max<std::int64_t>(1,intValue(material["quantity"],1));if(!name.empty())inventory=changeInventory(std::move(inventory),name,-qty,category);}
    for(const auto& element:elements.getMemberNames()){const auto need=std::max<std::int64_t>(0,intValue(elements[element]));if(need>0)elementStorage[element]=Json::Int64(std::max<std::int64_t>(0,intValue(elementStorage[element])-need));}
    service->database()->execSqlSync("UPDATE character_cards SET inventory=$1::jsonb,element_storage=$2::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$3",compactJson(inventory),compactJson(elementStorage),user.id);
    const bool hasSteps=inst["ritual_steps"].isArray()&&!inst["ritual_steps"].empty();const auto castRounds=std::max<std::int64_t>(0,intValue(inst["cast_rounds"]));Json::Value progress;progress["costs_consumed"]=true;progress["target"]=Json::Int64(std::max<std::int64_t>(1,hasSteps?static_cast<std::int64_t>(inst["ritual_steps"].size()):castRounds));progress["completed"]=0;
    const std::string nextStatus=(hasSteps||castRounds>0)?"channeling":"success";service->database()->execSqlSync("UPDATE ritual_instances SET status=$1,current_step=0,progress=$2::jsonb,started_at=CURRENT_TIMESTAMP,completed_at=CASE WHEN $1='success' THEN CURRENT_TIMESTAMP ELSE NULL END,updated_at=CURRENT_TIMESTAMP WHERE id=$3 AND room_id=$4",nextStatus,compactJson(progress),instanceId,roomId);
    Json::Value ev;ev["ritual_instance_id"]=Json::Int64(instanceId);ev["ritual_id"]=inst["ritual_id"];service->addEvent(roomId,user.id,"ritual_started",ev);evaluateRuleEvent(service,roomId,user.id,"RITUAL_STARTED",ev,false);
    if(nextStatus=="success"){
      Json::Value ctx;ctx["ritual"]=inst;auto applied=applyConfiguredEventSkillEffects(service,roomId,"player",user.id,inst["success_effects"],ctx);ev["effects"]=applied;
      ev["ritual_progression_reward"]=rewardRitualSuccess(service->database(),ritualOwnerId,inst);
      ev["character_ascension"]=applyAscensionRitual(service->database(),ritualOwnerId,inst);
      service->addEvent(roomId,user.id,"ritual_succeeded",ev);evaluateRuleEvent(service,roomId,user.id,"RITUAL_SUCCEEDED",ev,false);
    }
    Json::Value out;out["ok"]=true;out["instance"]=ritualInstanceJson(service,instanceId);out["character"]=characterRaw(service->database(),user.id);reply(cb,std::move(out));},"開始儀式失敗");});
  reg2("/api/rooms/{1}/ritual-field/{2}/advance",drogon::Post,[service](const Request& req,Callback& cb,const std::string&roomText,const std::string&instanceText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomText),instanceId=positiveId(instanceText);requireRoomAccess(service,user,roomId);auto inst=ritualInstanceJson(service,instanceId);if(stringValue(inst["status"])!="channeling")throw ApiError(drogon::k409Conflict,"儀式目前不是進行中");auto progress=inst["progress"].isObject()?inst["progress"]:Json::Value{Json::objectValue};const auto nextStep=intValue(inst["current_step"])+1;const auto totalSteps=inst["ritual_steps"].isArray()?static_cast<std::int64_t>(inst["ritual_steps"].size()):0;const auto castRounds=std::max<std::int64_t>(0,intValue(inst["cast_rounds"]));const auto target=std::max<std::int64_t>(1,totalSteps>0?totalSteps:castRounds);progress["target"]=Json::Int64(target);progress["completed"]=Json::Int64(nextStep);const bool done=nextStep>=target;service->database()->execSqlSync("UPDATE ritual_instances SET current_step=$1,progress=$2::jsonb,status=$3,completed_at=CASE WHEN $3='success' THEN CURRENT_TIMESTAMP ELSE completed_at END,updated_at=CURRENT_TIMESTAMP WHERE id=$4 AND room_id=$5",nextStep,compactJson(progress),done?"success":"channeling",instanceId,roomId);Json::Value ev;ev["ritual_instance_id"]=Json::Int64(instanceId);ev["step"]=Json::Int64(nextStep);service->addEvent(roomId,user.id,"ritual_step",ev);evaluateRuleEvent(service,roomId,user.id,"RITUAL_STEP_COMPLETED",ev,false);if(done){Json::Value ctx;ctx["ritual"]=inst;ev["effects"]=applyConfiguredEventSkillEffects(service,roomId,"player",user.id,inst["success_effects"],ctx);const auto ritualOwnerId=intValue(inst["created_by_user_id"],user.id);ev["ritual_progression_reward"]=rewardRitualSuccess(service->database(),ritualOwnerId,inst);ev["character_ascension"]=applyAscensionRitual(service->database(),ritualOwnerId,inst);service->addEvent(roomId,user.id,"ritual_succeeded",ev);evaluateRuleEvent(service,roomId,user.id,"RITUAL_SUCCEEDED",ev,false);}Json::Value out;out["ok"]=true;out["instance"]=ritualInstanceJson(service,instanceId);reply(cb,std::move(out));},"推進儀式失敗");});
  reg2("/api/rooms/{1}/ritual-field/{2}/fail",drogon::Post,[service](const Request& req,Callback& cb,const std::string&roomText,const std::string&instanceText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomText),instanceId=positiveId(instanceText);requireRoomAccess(service,user,roomId);auto inst=ritualInstanceJson(service,instanceId);if(!user.isAdmin&&intValue(inst["created_by_user_id"])!=user.id)throw ApiError(drogon::k403Forbidden,"只有主持者或 DM 可中斷儀式");service->database()->execSqlSync("UPDATE ritual_instances SET status='failed',completed_at=CURRENT_TIMESTAMP,updated_at=CURRENT_TIMESTAMP WHERE id=$1 AND room_id=$2",instanceId,roomId);Json::Value ctx;ctx["ritual"]=inst;Json::Value ev;ev["ritual_instance_id"]=Json::Int64(instanceId);ev["effects"]=applyConfiguredEventSkillEffects(service,roomId,"player",user.id,inst["failure_effects"],ctx);ev["ritual_research_reward"]=rewardRitualFailureResearch(service->database(),intValue(inst["created_by_user_id"],user.id),inst);service->addEvent(roomId,user.id,"ritual_failed",ev);evaluateRuleEvent(service,roomId,user.id,"RITUAL_FAILED",ev,false);Json::Value out;out["ok"]=true;out["instance"]=ritualInstanceJson(service,instanceId);reply(cb,std::move(out));},"中斷儀式失敗");});
  reg2("/api/rooms/{1}/ritual-field/{2}",drogon::Delete,[service](const Request& req,Callback& cb,const std::string&roomText,const std::string&instanceText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomText),instanceId=positiveId(instanceText);requireRoomAccess(service,user,roomId);const auto rows=service->database()->execSqlSync("SELECT created_by_user_id FROM ritual_instances WHERE id=$1 AND room_id=$2",instanceId,roomId);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到儀式場");if(!user.isAdmin&&rows[0]["created_by_user_id"].as<std::int64_t>()!=user.id)throw ApiError(drogon::k403Forbidden,"只有主持者或 DM 可撤除儀式");service->database()->execSqlSync("DELETE FROM ritual_instances WHERE id=$1 AND room_id=$2",instanceId,roomId);Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"撤除儀式失敗");});

  // [功能備註｜魔法學／儀式學｜POST /api/admin/players/{1}/study-points]
  // [功能備註｜63-cpp.24.1 儀式點數帳本] DM 手動發點也會更新累計獲得／已花費，方便角色卡追蹤。
  reg1("/api/admin/players/{1}/study-points",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& userText){
    runHandler(cb,[&]{
      requireUser(service,req,true);const auto userId=positiveId(userText,"玩家 ID 無效");
      const auto body=bodyOrEmpty(req);const auto kind=stringValue(body["kind"]);const auto delta=intValue(body["delta"]);
      if(kind!="magic"&&kind!="ritual")throw ApiError(drogon::k400BadRequest,"kind 必須是 magic 或 ritual");
      if(kind=="magic"){
        service->database()->execSqlSync("UPDATE character_cards SET magic_points=GREATEST(0,magic_points+$1),updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",delta,userId);
      }else{
        const auto beforeRows=service->database()->execSqlSync("SELECT ritual_points FROM character_cards WHERE user_id=$1",userId);
        if(beforeRows.empty())throw ApiError(drogon::k404NotFound,"找不到玩家角色卡");
        const auto before=std::max<std::int64_t>(0,beforeRows[0]["ritual_points"].as<std::int64_t>());
        const auto after=std::max<std::int64_t>(0,before+delta);
        const auto actual=after-before;
        if(actual>=0)service->database()->execSqlSync(
          "UPDATE character_cards SET ritual_points=$1,ritual_points_earned=ritual_points_earned+$2,updated_at=CURRENT_TIMESTAMP WHERE user_id=$3",
          after,actual,userId);
        else service->database()->execSqlSync(
          "UPDATE character_cards SET ritual_points=$1,ritual_points_spent=ritual_points_spent+$2,updated_at=CURRENT_TIMESTAMP WHERE user_id=$3",
          after,-actual,userId);
      }
      Json::Value out;out["ok"]=true;out["character"]=characterRaw(service->database(),userId);
      if(kind=="ritual")out["progression"]=ritualProgressionJson(out["character"]);
      reply(cb,std::move(out));
    },"調整學習點數失敗");
  });

  // [功能備註｜63-cpp.24.1 DM 儀式研究調整]
  // DM／任務／副本可以透過同一資料結構發現儀式、推進研究或直接給儀式學 XP。
  reg1("/api/admin/players/{1}/ritual-research",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& userText){
    runHandler(cb,[&]{
      requireUser(service,req,true);const auto userId=positiveId(userText,"玩家 ID 無效");const auto body=bodyOrEmpty(req);
      const auto ritualId=intValue(body["ritual_id"]);if(ritualId<=0)throw ApiError(drogon::k400BadRequest,"ritual_id 無效");
      if(service->database()->execSqlSync("SELECT 1 FROM ritual_studies WHERE id=$1",ritualId).empty())throw ApiError(drogon::k404NotFound,"找不到儀式");
      const auto delta=std::clamp<std::int64_t>(intValue(body["progress_delta"]),-100,100);const bool discover=body.get("discover",true).asBool();
      service->database()->execSqlSync(
        "INSERT INTO character_ritual_research(user_id,ritual_id,discovered,progress,updated_at) VALUES($1,$2,$3,GREATEST(0,LEAST(100,$4)),CURRENT_TIMESTAMP) "
        "ON CONFLICT(user_id,ritual_id) DO UPDATE SET discovered=character_ritual_research.discovered OR $3,"
        "progress=GREATEST(0,LEAST(100,character_ritual_research.progress+$4)),updated_at=CURRENT_TIMESTAMP",
        userId,ritualId,discover,delta);
      auto progression=grantRitualXp(service->database(),userId,std::max<std::int64_t>(0,intValue(body["xp_delta"])));
      Json::Value out;out["ok"]=true;out["research"]=ritualResearchState(service->database(),userId,ritualId);out["progression"]=progression;reply(cb,std::move(out));
    },"調整儀式研究失敗");
  });

  // [功能備註｜元素儲存｜POST /api/admin/players/{1}/element-storage]
  // mode=current 調整目前儲存量；mode=base_cap 調整永久基礎上限。技能／道具加成仍由 C++ 即時計算。
  reg1("/api/admin/players/{1}/element-storage",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& userText){
    runHandler(cb,[&]{
      requireUser(service,req,true);const auto userId=positiveId(userText,"玩家 ID 無效");
      const auto body=bodyOrEmpty(req);const auto element=stringValue(body["element"]);const auto mode=stringValue(body["mode"],"current");const auto delta=intValue(body["delta"]);
      bool valid=false;for(const auto*e:kStoredElements)if(element==e){valid=true;break;}if(!valid)throw ApiError(drogon::k400BadRequest,"元素名稱無效");
      auto character=characterRaw(service->database(),userId);
      if(mode=="base_cap"){
        auto caps=normalizedElementMap(character["element_storage_base_caps"]);
        caps[element]=Json::Int64(std::max<std::int64_t>(0,intValue(caps[element])+delta));
        service->database()->execSqlSync("UPDATE character_cards SET element_storage_base_caps=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",compactJson(caps),userId);
      }else if(mode=="current"){
        auto stored=normalizedElementMap(character["element_storage"]);
        const auto cap=std::max<std::int64_t>(0,intValue(character["element_storage_caps"][element]));
        stored[element]=Json::Int64(std::clamp<std::int64_t>(intValue(stored[element])+delta,0,cap));
        service->database()->execSqlSync("UPDATE character_cards SET element_storage=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",compactJson(stored),userId);
      }else throw ApiError(drogon::k400BadRequest,"mode 必須是 current 或 base_cap");
      Json::Value out;out["ok"]=true;out["character"]=characterRaw(service->database(),userId);reply(cb,std::move(out));
    },"調整元素儲存失敗");
  });

  // [功能備註｜批量匯入｜POST /api/admin/bulk-import/preview]
  // 支援技能／道具／材料／魔法／儀式／合成配方／商店商品。前端先解析 CSV/TSV/JSON，C++ 再做正式驗證。
  reg0("/api/admin/bulk-import/preview",drogon::Post,[service](const Request& req,Callback& cb){
    runHandler(cb,[&]{
      requireUser(service,req,true);
      const auto body=bodyOrEmpty(req);
      const auto kind=stringValue(body["kind"]);
      const auto rows=body["rows"];
      const std::set<std::string> supported={"skills","items","materials","magics","rituals","recipes","shop","knowledge_shop"};
      if(!supported.contains(kind))throw ApiError(drogon::k400BadRequest,"匯入種類不支援");
      if(!rows.isArray())throw ApiError(drogon::k400BadRequest,"rows 必須是陣列");
      if(rows.size()>5000)throw ApiError(drogon::k400BadRequest,"單次最多 5000 筆");
      Json::Value normalized{Json::arrayValue},errors{Json::arrayValue};
      std::size_t line=0;
      for(const auto&r:rows){
        ++line;
        try{
          if(!r.isObject())throw ApiError(drogon::k400BadRequest,"資料列不是物件");
          const auto name=trimmed(stringValue(r["name"]));
          if(name.empty())throw ApiError(drogon::k400BadRequest,"缺少 name");
          Json::Value x=r;
          x["name"]=name.substr(0,120);
          x["rank"]=normalizeContentRank(stringValue(x["rank"],"G"));
          if(kind=="knowledge_shop"){
            const auto venue=stringValue(x["venue"],"library");if(venue!="library"&&venue!="game_room")throw ApiError(drogon::k400BadRequest,"venue 必須是 library 或 game_room");x["venue"]=venue;
            const auto type=stringValue(x["content_type"]);knowledgeContentTable(type);x["content_type"]=type;
            auto cid=intValue(x["content_id"]);
            if(cid<=0){const auto cname=trimmed(stringValue(x["content_name"]));if(cname.empty())throw ApiError(drogon::k400BadRequest,"需要 content_id 或 content_name");const auto table=knowledgeContentTable(type);const auto found=service->database()->execSqlSync("SELECT id,rank FROM \""+table+"\" WHERE name=$1 ORDER BY id LIMIT 1",cname);if(found.empty())throw ApiError(drogon::k400BadRequest,"找不到 content_name："+cname);cid=found[0]["id"].as<std::int64_t>();if(!r.isMember("rank"))x["rank"]=normalizeContentRank(found[0]["rank"].isNull()?"G":found[0]["rank"].as<std::string>());}
            x["content_id"]=Json::Int64(cid);x.removeMember("content_name");
            const auto currency=stringValue(x["currency"],"weird");if(currency!="weird"&&currency!="game")throw ApiError(drogon::k400BadRequest,"currency 必須是 weird 或 game");x["currency"]=currency;
            // [功能備註｜60-cpp.21.1 Render warning 整理] 拆開條件式，避免 -Wmisleading-indentation。
            if(!x.isMember("price")) x["price"] = Json::Int64(0);
            if(intValue(x["price"]) < 0) throw ApiError(drogon::k400BadRequest,"price 不可小於 0");
            if(!x.isMember("stock")) x["stock"] = -1;
            if(!x.isMember("purchase_limit")) x["purchase_limit"] = 1;
            if(!x.isMember("description")) x["description"] = "";
            if(!x.isMember("active")) x["active"] = true;
          }else if(kind=="skills"){
            if(!x.isMember("category"))x["category"]="一般";
            if(!x.isMember("skill_type"))x["skill_type"]=stringValue(x["type"],"active");
            if(!x.isMember("level"))x["level"]=1;
            if(!x.isMember("description"))x["description"]="";
            if(!x.isMember("resources"))x["resources"]=Json::Value{Json::objectValue};
            if(!x.isMember("data"))x["data"]=Json::Value{Json::objectValue};
          }else if(kind=="magics"||kind=="rituals"){
            if(!x.isMember("category"))x["category"]=(kind=="magics"?"一般魔法":"一般儀式");
            if(kind=="magics"){x["element"]=normalizeMagicElement(stringValue(x["element"]));x["tree_x"]=std::clamp<std::int64_t>(intValue(x["tree_x"]),0,40);x["tree_y"]=std::clamp<std::int64_t>(intValue(x["tree_y"]),0,60);if(!x.isMember("icon"))x["icon"]="✦";x["node_style"]=normalizeMagicNodeStyle(x["node_style"],stringValue(x["category"],"一般魔法"));}
            if(!x.isMember("rank"))x["rank"]="G";
            if(!x.isMember("description"))x["description"]="";
            if(!x.isMember("point_cost"))x["point_cost"]=1;
            if(!x.isMember("prerequisite_ids"))x["prerequisite_ids"]=Json::Value{Json::arrayValue};
            if(!x.isMember("effects"))x["effects"]=Json::Value{Json::objectValue};
            if(!x.isMember("active"))x["active"]=true;
            if(!x["prerequisite_ids"].isArray())throw ApiError(drogon::k400BadRequest,"prerequisite_ids 必須是陣列");
            if(!x["effects"].isObject())throw ApiError(drogon::k400BadRequest,"effects 必須是物件");
          }else if(kind=="recipes"){
            if(!x.isMember("recipe_type"))x["recipe_type"]="item";
            if(!x.isMember("description"))x["description"]="";
            if(!x.isMember("materials"))x["materials"]=Json::Value{Json::arrayValue};
            if(!x.isMember("output"))x["output"]=Json::Value{Json::objectValue};
            if(!x.isMember("config"))x["config"]=Json::Value{Json::objectValue};
            if(!x["materials"].isArray())throw ApiError(drogon::k400BadRequest,"materials 必須是陣列");
            if(!x["output"].isObject())throw ApiError(drogon::k400BadRequest,"output 必須是物件");
            if(!x["config"].isObject())throw ApiError(drogon::k400BadRequest,"config 必須是物件");
          }else if(kind=="shop"){
            if(x.isMember("category")&&!x.isMember("item_category"))x["item_category"]=x["category"];
            if(!x.isMember("item_type"))x["item_type"]="item";
            if(!x.isMember("description"))x["description"]="";
            if(!x.isMember("price_weird"))x["price_weird"]=Json::Int64(0);
            if(!x.isMember("quantity"))x["quantity"]=1;
            if(!x.isMember("stock"))x["stock"]=-1;
            if(!x.isMember("purchase_limit"))x["purchase_limit"]=-1;
            if(!x.isMember("discount_percent"))x["discount_percent"]=0;
            if(!x.isMember("sell_percent"))x["sell_percent"]=50;
            if(!x.isMember("tags"))x["tags"]=Json::Value{Json::arrayValue};
            if(!x.isMember("active"))x["active"]=true;
            if(intValue(x["price_weird"])<0)throw ApiError(drogon::k400BadRequest,"price_weird 不可小於 0");
            if(intValue(x["quantity"],1)<1)throw ApiError(drogon::k400BadRequest,"quantity 至少為 1");
            if(intValue(x["stock"],-1)<-1)throw ApiError(drogon::k400BadRequest,"stock 只能是 -1 或非負數");
            if(intValue(x["purchase_limit"],-1)<-1)throw ApiError(drogon::k400BadRequest,"purchase_limit 只能是 -1 或非負數");
            const auto discount=x.get("discount_percent",0).asDouble();
            const auto sell=x.get("sell_percent",50).asDouble();
            if(discount<0||discount>100)throw ApiError(drogon::k400BadRequest,"discount_percent 必須介於 0~100");
            if(sell<0||sell>100)throw ApiError(drogon::k400BadRequest,"sell_percent 必須介於 0~100");
            if(!x["tags"].isArray())throw ApiError(drogon::k400BadRequest,"tags 必須是陣列");
          }else{
            if(!x.isMember("category")||stringValue(x["category"]).empty())x["category"]=(kind=="materials"?"材料":"道具");
            if(!x.isMember("item_type")||stringValue(x["item_type"]).empty())x["item_type"]=(kind=="materials"?"material":"normal");
            if(!x.isMember("description"))x["description"]="";
            if(!x.isMember("effects"))x["effects"]=Json::Value{Json::objectValue};
            if(!x.isMember("consume_on_use"))x["consume_on_use"]=(kind!="materials");
          }
          normalized.append(x);
        }catch(const std::exception&e){
          Json::Value err;err["row"]=static_cast<Json::UInt64>(line);err["name"]=stringValue(r["name"]);err["error"]=e.what();errors.append(err);
        }
      }
      Json::Value out;out["ok"]=errors.empty();out["kind"]=kind;out["valid_count"]=static_cast<Json::UInt64>(normalized.size());out["error_count"]=static_cast<Json::UInt64>(errors.size());out["rows"]=normalized;out["errors"]=errors;reply(cb,std::move(out));
    },"預覽批量匯入失敗");
  });

  // [功能備註｜批量匯入｜POST /api/admin/bulk-import/commit]
  // 同名資料更新，不存在則新增；商店分類由 C++ 正規化並自動建立／復用分類。

  // [功能備註｜遊戲間／書庫｜GET /api/knowledge-shop/{1}] 玩家讀取技能書／魔法／儀式商品。
  reg1("/api/knowledge-shop/{1}",drogon::Get,[service](const Request& req,Callback& cb,const std::string& venue){
    runHandler(cb,[&]{const auto user=requireUser(service,req);const auto character=characterRaw(service->database(),user.id);Json::Value out;out["venue"]=venue;out["character"]=character;if(venue=="game_room"&&!character.get("game_room_unlocked",false).asBool()){out["locked"]=true;out["items"]=Json::Value{Json::arrayValue};}else{out["locked"]=false;out["items"]=knowledgeShopRows(service->database(),venue,true,user.id);}reply(cb,std::move(out));},"讀取遊戲間／書庫失敗");
  });

  // [功能備註｜遊戲間／書庫｜GET /api/admin/knowledge-shop] DM 管理兩個知識商店。
  reg0("/api/admin/knowledge-shop",drogon::Get,[service](const Request& req,Callback& cb){
    runHandler(cb,[&]{requireUser(service,req,true);Json::Value out;out["library"]=knowledgeShopRows(service->database(),"library",false);out["game_room"]=knowledgeShopRows(service->database(),"game_room",false);reply(cb,std::move(out));},"讀取知識商店管理失敗");
  });

  reg0("/api/admin/knowledge-shop",drogon::Post,[service](const Request& req,Callback& cb){
    runHandler(cb,[&]{requireUser(service,req,true);auto body=bodyOrEmpty(req);const auto venue=stringValue(body["venue"],"library");if(venue!="library"&&venue!="game_room")throw ApiError(drogon::k400BadRequest,"venue 必須是 library 或 game_room");const auto type=stringValue(body["content_type"]);const auto cid=positiveId(std::to_string(intValue(body["content_id"])),"內容 ID 無效");auto content=knowledgeContent(service->database(),type,cid);body["venue"]=venue;body["content_type"]=type;body["content_id"]=Json::Int64(cid);body["name"]=trimmed(stringValue(body["name"],stringValue(content["name"]))).substr(0,160);body["rank"]=normalizeContentRank(stringValue(body["rank"],stringValue(content["rank"],"G")));const auto currency=stringValue(body["currency"],"weird");if(currency!="weird"&&currency!="game")throw ApiError(drogon::k400BadRequest,"currency 必須是 weird 或 game");body["currency"]=currency;body["price"]=Json::Int64(std::max<std::int64_t>(0,intValue(body["price"])));Json::Value out;out["ok"]=true;out["item"]=genericInsert(service->database(),"knowledge_shop_items",body);reply(cb,std::move(out));},"建立遊戲間／書庫商品失敗");
  });

  reg1("/api/admin/knowledge-shop/{1}",drogon::Patch,[service](const Request& req,Callback& cb,const std::string& idText){
    runHandler(cb,[&]{requireUser(service,req,true);auto body=bodyOrEmpty(req);if(body.isMember("rank"))body["rank"]=normalizeContentRank(stringValue(body["rank"]));if(body.isMember("currency")){const auto c=stringValue(body["currency"]);if(c!="weird"&&c!="game")throw ApiError(drogon::k400BadRequest,"currency 必須是 weird 或 game");}Json::Value out;out["ok"]=true;out["item"]=genericUpdate(service->database(),"knowledge_shop_items",positiveId(idText),body);reply(cb,std::move(out));},"修改遊戲間／書庫商品失敗");
  });
  reg1("/api/admin/knowledge-shop/{1}",drogon::Delete,[service](const Request& req,Callback& cb,const std::string& idText){runHandler(cb,[&]{requireUser(service,req,true);genericDelete(service->database(),"knowledge_shop_items",positiveId(idText));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"刪除遊戲間／書庫商品失敗");});

  // [功能備註｜遊戲間／書庫｜POST /api/knowledge-shop/{1}/buy] 購買只解鎖學習書；真正學習仍支付技能／魔法／儀式點。
  reg1("/api/knowledge-shop/{1}/buy",drogon::Post,[service](const Request& req,Callback& cb,const std::string& idText){
    runHandler(cb,[&]{const auto user=requireUser(service,req);const auto id=positiveId(idText);const auto rows=service->database()->execSqlSync("SELECT row_to_json(k)::text AS json_text FROM knowledge_shop_items k WHERE id=$1 AND active=TRUE",id);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到商品");auto item=rowJson(rows);auto character=characterRaw(service->database(),user.id);const auto venue=stringValue(item["venue"]);if(venue=="game_room"&&!character.get("game_room_unlocked",false).asBool())throw ApiError(drogon::k403Forbidden,"遊戲間尚未解鎖");const auto limit=intValue(item["purchase_limit"],1);const auto boughtRows=service->database()->execSqlSync("SELECT COUNT(*)::BIGINT AS n FROM knowledge_shop_purchases WHERE user_id=$1 AND shop_item_id=$2",user.id,id);const auto bought=boughtRows.empty()?0:boughtRows[0]["n"].as<std::int64_t>();if(limit>=0&&bought>=limit)throw ApiError(drogon::k400BadRequest,"已達此學習書限購數量");const auto stock=intValue(item["stock"],-1);if(stock==0)throw ApiError(drogon::k400BadRequest,"商品已售罄");const auto price=std::max<std::int64_t>(0,intValue(item["price"]));const auto currency=stringValue(item["currency"],"weird");if(currency=="game"&&!character.get("game_coin_unlocked",false).asBool())throw ApiError(drogon::k403Forbidden,"遊戲幣尚未解鎖");const auto field=currency=="game"?"game_coins":"weird_coins";const auto balance=std::max<std::int64_t>(0,intValue(character[field]));if(balance<price)throw ApiError(drogon::k400BadRequest,currency=="game"?"遊戲幣不足":"詭幣不足");if(stock>0){const auto dec=service->database()->execSqlSync("UPDATE knowledge_shop_items SET stock=stock-1,updated_at=CURRENT_TIMESTAMP WHERE id=$1 AND stock>0 RETURNING stock",id);if(dec.empty())throw ApiError(drogon::k400BadRequest,"商品已售罄");}try{service->database()->execSqlSync(std::string("UPDATE character_cards SET ")+field+"="+field+"-$1,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",price,user.id);service->database()->execSqlSync("INSERT INTO knowledge_shop_purchases(user_id,shop_item_id,content_type,content_id,currency,unit_price) VALUES($1,$2,$3,$4,$5,$6)",user.id,id,stringValue(item["content_type"]),intValue(item["content_id"]),currency,price);}catch(...){if(stock>0)service->database()->execSqlSync("UPDATE knowledge_shop_items SET stock=stock+1 WHERE id=$1",id);throw;}Json::Value out;out["ok"]=true;out["item"]=item;out["character"]=characterRaw(service->database(),user.id);reply(cb,std::move(out));},"購買學習書失敗");
  });

  // [功能備註｜65-cpp.26.2 回植 命隙／塔羅 API]
  // 大阿爾：重大命運主軸；小阿爾：較具體但可被行動改變的預兆。
  auto requireTarotDungeon=[service](const UserInfo& user,std::int64_t roomId,std::int64_t dungeonId){
    requireRoomAccess(service,user,roomId);
    const auto found=service->database()->execSqlSync("SELECT 1 FROM room_dungeons WHERE room_id=$1 AND dungeon_template_id=$2",roomId,dungeonId);
    if(found.empty())throw ApiError(drogon::k404NotFound,"這個房間沒有此副本");
  };
  auto drawTarotCard=[service](const std::string& arcana){
    const auto rows=service->database()->execSqlSync(
      "SELECT row_to_json(t)::text AS json_text FROM tarot_cards t WHERE arcana=$1 AND enabled=TRUE ORDER BY random() LIMIT 1",arcana);
    if(rows.empty())throw ApiError(drogon::k503ServiceUnavailable,"塔羅牌庫尚未初始化");
    return rowJson(rows);
  };
  auto miyoiId=[service]()->std::int64_t{
    const auto rows=service->database()->execSqlSync("SELECT id FROM npc_templates WHERE name='彌夜' ORDER BY id LIMIT 1");
    return rows.empty()?0:rows[0]["id"].as<std::int64_t>();
  };
  auto tarotOrientation=[service](){
    const auto rows=service->database()->execSqlSync("SELECT CASE WHEN random()<0.5 THEN 'upright' ELSE 'reversed' END AS v");
    return rows[0]["v"].as<std::string>();
  };
  auto tarotProphecy=[](const Json::Value& card,const std::string& orientation,const std::string& type){
    const auto name=stringValue(card["name"]);
    const auto words=orientation=="upright"?stringValue(card["upright_keywords"]):stringValue(card["reversed_keywords"]);
    if(type=="dungeon")return std::string{"副本的命運正向「"}+name+"」靠攏。"+words+"；真正的形式仍未確定。";
    if(type=="omen")return std::string{"近期最清晰的預兆是「"}+name+"」。"+words+"。這條未來仍可被你的選擇改變。";
    return std::string{"今日命運回應了「"}+name+"」。"+words+"。";
  };

  reg0("/api/tarot/cards",drogon::Get,[service](const Request& req,Callback& cb){
    runHandler(cb,[&]{requireUser(service,req);Json::Value out;out["major"]=Json::arrayValue;out["minor"]=Json::arrayValue;const auto rows=service->database()->execSqlSync("SELECT row_to_json(t)::text AS json_text FROM tarot_cards t WHERE enabled=TRUE ORDER BY arcana,card_no,suit");for(const auto&r:rows){auto c=parseJson(r["json_text"].as<std::string>());out[stringValue(c["arcana"])=="major"?"major":"minor"].append(c);}reply(cb,std::move(out));},"讀取塔羅牌庫失敗");
  });

  reg2("/api/tarot/status/{1}/{2}",drogon::Get,[service,requireTarotDungeon](const Request& req,Callback& cb,const std::string& roomText,const std::string& dungeonText){
    runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomText);const auto dungeonId=positiveId(dungeonText);requireTarotDungeon(user,roomId,dungeonId);const auto rows=service->database()->execSqlSync("SELECT reading_type,COUNT(*)::BIGINT AS n FROM tarot_readings WHERE room_id=$1 AND dungeon_template_id=$2 AND ((reading_type='dungeon') OR (user_id=$3 AND reading_date=CURRENT_DATE)) GROUP BY reading_type",roomId,dungeonId,user.id);Json::Value out;out["personal_available"]=true;out["omen_available"]=true;out["dungeon_available"]=true;for(const auto&r:rows){const auto type=r["reading_type"].as<std::string>();const bool used=r["n"].as<std::int64_t>()>0;if(type=="personal")out["personal_available"]=!used;if(type=="omen")out["omen_available"]=!used;if(type=="dungeon")out["dungeon_available"]=!used;}reply(cb,std::move(out));},"讀取占卜狀態失敗");
  });

  reg2("/api/tarot/personal/{1}/{2}",drogon::Post,[service,requireTarotDungeon,drawTarotCard,miyoiId,tarotOrientation,tarotProphecy](const Request& req,Callback& cb,const std::string& roomText,const std::string& dungeonText){
    runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomText);const auto dungeonId=positiveId(dungeonText);requireTarotDungeon(user,roomId,dungeonId);if(!service->database()->execSqlSync("SELECT 1 FROM tarot_readings WHERE user_id=$1 AND room_id=$2 AND dungeon_template_id=$3 AND reading_type='personal' AND reading_date=CURRENT_DATE",user.id,roomId,dungeonId).empty())throw ApiError(drogon::k409Conflict,"今天已經進行過個人命運占卜");auto card=drawTarotCard("major");const auto orientation=tarotOrientation();const auto effect=orientation=="upright"?card["upright_effect"]:card["reversed_effect"];const auto prophecy=tarotProphecy(card,orientation,"personal");const auto id=service->database()->execSqlSync("INSERT INTO tarot_readings(user_id,room_id,dungeon_template_id,npc_template_id,reading_type,card_id,orientation,effect_snapshot,prophecy_text,expires_at) VALUES($1,$2,$3,NULLIF($4,0),'personal',$5,$6,$7::jsonb,$8,CURRENT_DATE+INTERVAL '1 day') RETURNING id",user.id,roomId,dungeonId,miyoiId(),intValue(card["id"]),orientation,compactJson(effect),prophecy)[0]["id"].as<std::int64_t>();Json::Value out;out["ok"]=true;out["reading_id"]=Json::Int64(id);out["type"]="personal";out["card"]=card;out["orientation"]=orientation;out["effect"]=effect;out["prophecy"]=prophecy;reply(cb,std::move(out));},"個人命運占卜失敗");
  });

  reg2("/api/tarot/omen/{1}/{2}",drogon::Post,[service,requireTarotDungeon,drawTarotCard,miyoiId,tarotOrientation,tarotProphecy](const Request& req,Callback& cb,const std::string& roomText,const std::string& dungeonText){
    runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomText);const auto dungeonId=positiveId(dungeonText);requireTarotDungeon(user,roomId,dungeonId);if(!service->database()->execSqlSync("SELECT 1 FROM tarot_readings WHERE user_id=$1 AND room_id=$2 AND dungeon_template_id=$3 AND reading_type='omen' AND reading_date=CURRENT_DATE",user.id,roomId,dungeonId).empty())throw ApiError(drogon::k409Conflict,"今天已經看過一次命運預兆");auto card=drawTarotCard("minor");const auto orientation=tarotOrientation();const auto effect=orientation=="upright"?card["upright_effect"]:card["reversed_effect"];const auto prophecy=tarotProphecy(card,orientation,"omen");const auto id=service->database()->execSqlSync("INSERT INTO tarot_readings(user_id,room_id,dungeon_template_id,npc_template_id,reading_type,card_id,orientation,effect_snapshot,prophecy_text,expires_at) VALUES($1,$2,$3,NULLIF($4,0),'omen',$5,$6,$7::jsonb,$8,CURRENT_DATE+INTERVAL '1 day') RETURNING id",user.id,roomId,dungeonId,miyoiId(),intValue(card["id"]),orientation,compactJson(effect),prophecy)[0]["id"].as<std::int64_t>();Json::Value out;out["ok"]=true;out["reading_id"]=Json::Int64(id);out["type"]="omen";out["card"]=card;out["orientation"]=orientation;out["effect"]=effect;out["prophecy"]=prophecy;reply(cb,std::move(out));},"命運預兆占卜失敗");
  });

  reg2("/api/tarot/dungeon/{1}/{2}",drogon::Post,[service,requireTarotDungeon,drawTarotCard,miyoiId,tarotOrientation,tarotProphecy](const Request& req,Callback& cb,const std::string& roomText,const std::string& dungeonText){
    runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomText);const auto dungeonId=positiveId(dungeonText);requireTarotDungeon(user,roomId,dungeonId);if(!service->database()->execSqlSync("SELECT 1 FROM tarot_readings WHERE room_id=$1 AND dungeon_template_id=$2 AND reading_type='dungeon'",roomId,dungeonId).empty())throw ApiError(drogon::k409Conflict,"這個副本已經完成過副本命運占卜");auto card=drawTarotCard("major");const auto orientation=tarotOrientation();Json::Value effect=orientation=="upright"?card["upright_effect"]:card["reversed_effect"];effect["scope"]="dungeon";effect["rule_engine_tag"]="tarot_fate";const auto prophecy=tarotProphecy(card,orientation,"dungeon");const auto id=service->database()->execSqlSync("INSERT INTO tarot_readings(user_id,room_id,dungeon_template_id,npc_template_id,reading_type,card_id,orientation,effect_snapshot,prophecy_text) VALUES($1,$2,$3,NULLIF($4,0),'dungeon',$5,$6,$7::jsonb,$8) RETURNING id",user.id,roomId,dungeonId,miyoiId(),intValue(card["id"]),orientation,compactJson(effect),prophecy)[0]["id"].as<std::int64_t>();Json::Value out;out["ok"]=true;out["reading_id"]=Json::Int64(id);out["type"]="dungeon";out["card"]=card;out["orientation"]=orientation;out["effect"]=effect;out["prophecy"]=prophecy;reply(cb,std::move(out));},"副本命運占卜失敗");
  });

  reg2("/api/tarot/readings/{1}/{2}",drogon::Get,[service,requireTarotDungeon](const Request& req,Callback& cb,const std::string& roomText,const std::string& dungeonText){
    runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomText);const auto dungeonId=positiveId(dungeonText);requireTarotDungeon(user,roomId,dungeonId);Json::Value out;out["readings"]=Json::arrayValue;const auto rows=service->database()->execSqlSync("SELECT row_to_json(x)::text AS json_text FROM (SELECT r.*,c.name,c.arcana,c.suit,c.card_no,c.image_key FROM tarot_readings r JOIN tarot_cards c ON c.id=r.card_id WHERE r.room_id=$1 AND r.dungeon_template_id=$2 AND (r.reading_type='dungeon' OR r.user_id=$3) ORDER BY r.created_at DESC LIMIT 100) x",roomId,dungeonId,user.id);for(const auto&r:rows)out["readings"].append(parseJson(r["json_text"].as<std::string>()));reply(cb,std::move(out));},"讀取占卜紀錄失敗");
  });

  // 命鬥場第一版：建立等待中的 PvP 房；後續由既有戰鬥系統接管實際回合。
  reg0("/api/fate-gap/pvp",drogon::Post,[service,drawTarotCard,tarotOrientation](const Request& req,Callback& cb){
    runHandler(cb,[&]{const auto user=requireUser(service,req);const auto body=bodyOrEmpty(req);const auto roomId=positiveId(std::to_string(intValue(body["room_id"])),"房間 ID 無效");requireRoomAccess(service,user,roomId);const auto mode=stringValue(body["mode"],"duel");const auto tarotMode=stringValue(body["tarot_mode"],"fate");Json::Value card;std::string orientation;if(tarotMode!="normal"){card=drawTarotCard("major");orientation=tarotOrientation();}Json::Value participants{Json::arrayValue};participants.append(Json::Int64(user.id));const auto result=service->database()->execSqlSync("INSERT INTO fate_gap_pvp_matches(room_id,creator_user_id,mode,tarot_mode,participants,rule_card_id,rule_orientation) VALUES($1,$2,$3,$4,$5::jsonb,NULLIF($6,0),NULLIF($7,'')) RETURNING id",roomId,user.id,mode,tarotMode,compactJson(participants),intValue(card["id"]),orientation);Json::Value out;out["ok"]=true;out["match_id"]=Json::Int64(result[0]["id"].as<std::int64_t>());out["rule_card"]=card;out["orientation"]=orientation;reply(cb,std::move(out));},"建立命鬥場對局失敗");
  });

  // [功能備註｜65-cpp.26.8 DM 助手] 建立資料白名單。
  // DM 助手永遠是玩家；只能新增商店商品／裝備商品、技能、合成配方，沒有 PATCH/DELETE 或玩家發放 API。
  reg0("/api/dm-assistant/skill-templates", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireDmAssistant(service, req);
      auto body = bodyOrEmpty(req);
      body["rank"] = normalizeContentRank(stringValue(body["rank"], "G"));
      auto entity = genericInsert(service->database(), "skill_templates", body);
      recordDmAssistantCreation(service->database(), user, "skill", entity);
      Json::Value out; out["ok"] = true; out["skill"] = entity; reply(cb, std::move(out));
    }, "DM 助手建立技能失敗");
  });

  reg0("/api/dm-assistant/gear-templates", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireDmAssistant(service, req);
      auto body = bodyOrEmpty(req);
      const auto kind = stringValue(body["template_kind"], "equipment");
      if (kind != "equipment" && kind != "weapon") throw ApiError(drogon::k400BadRequest, "只能新增裝備或武器模板");
      body["template_kind"] = kind;
      body["rank"] = normalizeContentRank(stringValue(body["rank"], "G"));
      auto entity = genericInsert(service->database(), "item_templates", body);
      recordDmAssistantCreation(service->database(), user, kind, entity);
      Json::Value out; out["ok"] = true; out["item"] = entity; reply(cb, std::move(out));
    }, "DM 助手建立裝備模板失敗");
  });

  reg0("/api/dm-assistant/recipes", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireDmAssistant(service, req);
      auto body = bodyOrEmpty(req);
      const auto output = body["output"].isObject() ? body["output"] : Json::Value{Json::objectValue};
      const auto outputType = stringValue(output["type"], stringValue(body["recipe_type"], "item"));
      if (outputType != "item" && outputType != "equipment" && outputType != "weapon")
        throw ApiError(drogon::k403Forbidden, "DM 助手的合成配方只能產出道具、裝備或武器，不能產出特殊貨幣");
      body["recipe_type"] = outputType;
      body["output"]["type"] = outputType;
      body["rank"] = normalizeContentRank(stringValue(body["rank"], "G"));
      auto entity = genericInsert(service->database(), "recipes", body);
      recordDmAssistantCreation(service->database(), user, "recipe", entity);
      Json::Value out; out["ok"] = true; out["recipe"] = entity; reply(cb, std::move(out));
    }, "DM 助手建立合成配方失敗");
  });

  reg0("/api/dm-assistant/shop", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireDmAssistant(service, req);
      auto body = bodyOrEmpty(req);
      const auto type = stringValue(body["item_type"], "item");
      if (type != "item" && type != "equipment" && type != "weapon")
        throw ApiError(drogon::k400BadRequest, "DM 助手只能新增商店道具、裝備或武器");
      body["item_type"] = type;
      body["rank"] = normalizeContentRank(stringValue(body["rank"], "G"));
      body["active"] = true;
      syncShopItemCategory(service->database(), body);
      auto entity = genericInsert(service->database(), "shop_items", body);
      recordDmAssistantCreation(service->database(), user, type == "item" ? "shop_item" : "equipment", entity);
      Json::Value out; out["ok"] = true; out["item"] = entity; reply(cb, std::move(out));
    }, "DM 助手建立商店資料失敗");
  });

  reg0("/api/admin/bulk-import/commit",drogon::Post,[service](const Request& req,Callback& cb){
    runHandler(cb,[&]{
      requireUser(service,req,true);
      const auto body=bodyOrEmpty(req);
      const auto kind=stringValue(body["kind"]);
      const auto rows=body["rows"];
      const std::set<std::string> supported={"skills","items","materials","magics","rituals","recipes","shop","knowledge_shop"};
      if(!supported.contains(kind))throw ApiError(drogon::k400BadRequest,"匯入種類不支援");
      if(!rows.isArray()||rows.empty())throw ApiError(drogon::k400BadRequest,"沒有可匯入資料");
      if(rows.size()>5000)throw ApiError(drogon::k400BadRequest,"單次最多 5000 筆");
      const std::string table=kind=="skills"?"skill_templates":kind=="magics"?"magic_studies":kind=="rituals"?"ritual_studies":kind=="recipes"?"recipes":kind=="shop"?"shop_items":kind=="knowledge_shop"?"knowledge_shop_items":"item_templates";
      std::int64_t created=0,updated=0;
      Json::Value failures{Json::arrayValue};
      std::size_t line=0;
      for(const auto&r:rows){
        ++line;
        try{
          if(!r.isObject())throw ApiError(drogon::k400BadRequest,"資料列不是物件");
          Json::Value row=r;
          const auto name=trimmed(stringValue(row["name"]));
          if(name.empty())throw ApiError(drogon::k400BadRequest,"缺少 name");
          row["name"]=name.substr(0,120);
          row["rank"]=normalizeContentRank(stringValue(row["rank"],"G"));
          if(kind=="materials"){
            row["category"]="材料";
            if(!row.isMember("item_type"))row["item_type"]="material";
          }
          if(kind=="shop"){
            if(row.isMember("category")&&!row.isMember("item_category"))row["item_category"]=row["category"];
            syncShopItemCategory(service->database(),row);
          }
          const auto existing=service->database()->execSqlSync("SELECT id FROM \""+table+"\" WHERE name=$1 ORDER BY id LIMIT 1",name);
          if(existing.empty()){
            genericInsert(service->database(),table,row);++created;
          }else{
            genericUpdate(service->database(),table,existing[0]["id"].as<std::int64_t>(),row);++updated;
          }
        }catch(const std::exception&e){
          Json::Value f;f["row"]=static_cast<Json::UInt64>(line);f["name"]=stringValue(r["name"]);f["error"]=e.what();failures.append(f);
        }
      }
      Json::Value out;out["ok"]=failures.empty();out["created"]=Json::Int64(created);out["updated"]=Json::Int64(updated);out["failed"]=static_cast<Json::UInt64>(failures.size());out["failures"]=failures;reply(cb,std::move(out));
    },"批量匯入失敗");
  });

}

}  // namespace trpg

