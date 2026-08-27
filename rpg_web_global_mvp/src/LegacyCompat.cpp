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
      "SELECT id,username,is_admin FROM users WHERE id=$1", claims->id);
  if (rows.empty()) throw ApiError(drogon::k401Unauthorized, "帳號不存在");
  UserInfo user{rows[0]["id"].as<std::int64_t>(),
                rows[0]["username"].as<std::string>(),
                rows[0]["is_admin"].as<bool>()};
  if (admin && !user.isAdmin) throw ApiError(drogon::k403Forbidden, "需要全站 DM 權限");
  return user;
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
  return enrichElementStorage(db, rowJson(rows));
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
  return inventory;
}


// [功能備註｜65-cpp.26 魔法學完整系統]
// 魔法節點可指定七元素施放成本、對應技能模板與實體魔法書要求。
Json::Value normalizedMagicElementCost(const Json::Value& raw) {
  auto out = zeroElementMap();
  if (!raw.isObject()) return out;
  for (const auto* element : kStoredElements) {
    out[element] = Json::Int64(std::clamp<std::int64_t>(intValue(raw[element]), 0, 1000000));
  }
  return out;
}

void validateMagicLinkedSkill(const Db& db, std::int64_t skillTemplateId) {
  if (skillTemplateId <= 0) return;
  const auto rows = db->execSqlSync("SELECT 1 FROM skill_templates WHERE id=$1", skillTemplateId);
  if (rows.empty()) throw ApiError(drogon::k400BadRequest, "連結的技能模板不存在");
}

Json::Value magicElementCostStatus(const Json::Value& character, const Json::Value& cost) {
  const auto storage = normalizedElementMap(character["element_storage"]);
  const auto normalized = normalizedMagicElementCost(cost);
  Json::Value out{Json::objectValue};
  out["ok"] = true;
  out["cost"] = normalized;
  out["storage"] = storage;
  out["missing"] = Json::arrayValue;
  for (const auto* element : kStoredElements) {
    const auto need = intValue(normalized[element]);
    const auto have = intValue(storage[element]);
    if (need > have) {
      out["ok"] = false;
      Json::Value row; row["element"] = element; row["need"] = Json::Int64(need); row["have"] = Json::Int64(have);
      out["missing"].append(row);
    }
  }
  return out;
}

Json::Value spendMagicElements(Json::Value storage, const Json::Value& cost) {
  storage = normalizedElementMap(storage);
  const auto normalized = normalizedMagicElementCost(cost);
  for (const auto* element : kStoredElements) {
    storage[element] = Json::Int64(std::max<std::int64_t>(0, intValue(storage[element]) - intValue(normalized[element])));
  }
  return storage;
}

Json::Value buildSkillFromTemplate(const Json::Value& templ, const Json::Value& acquisition) {
  Json::Value skill;
  const auto templateId = intValue(templ["id"]);
  skill["id"] = "tpl_" + std::to_string(templateId);
  skill["template_id"] = Json::Int64(templateId);
  skill["name"] = templ["name"];
  skill["category"] = templ["category"];
  skill["type"] = templ["skill_type"];
  skill["level"] = templ["level"];
  skill["description"] = templ["description"];
  skill["check"] = templ["check_name"];
  skill["damage_formula"] = templ["damage_formula"];
  skill["extra_tag"] = templ["extra_tag"];
  skill["summon_tag"] = templ["summon_tag"];
  skill["resources"] = templ["resources"];
  skill["data"] = templ["data"];
  skill["acquisition"] = acquisition;
  return skill;
}

Json::Value grantIdentitySkills(const Db& db, std::int64_t userId,
                                const Json::Value& templateIds,
                                const Json::Value& acquisition) {
  auto character = characterRaw(db, userId);
  auto skills = character["skills"].isArray() ? character["skills"] : Json::Value{Json::arrayValue};
  std::set<std::int64_t> existing;
  for (const auto& skill : skills) {
    const auto id = intValue(skill["template_id"]);
    if (id > 0) existing.insert(id);
  }
  Json::Value granted{Json::arrayValue};
  if (templateIds.isArray()) {
    for (const auto& rawId : templateIds) {
      const auto id = intValue(rawId);
      if (id <= 0 || existing.contains(id)) continue;
      const auto rows = db->execSqlSync(
          "SELECT row_to_json(s)::text AS json_text FROM skill_templates s WHERE id=$1", id);
      if (rows.empty()) continue;
      auto skill = buildSkillFromTemplate(rowJson(rows), acquisition);
      skills.append(skill);
      granted.append(skill);
      existing.insert(id);
    }
  }
  if (!granted.empty()) {
    db->execSqlSync(
        "UPDATE character_cards SET skills=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",
        compactJson(skills), userId);
  }
  return granted;
}

void appendEffectList(Json::Value& target, const Json::Value& effects) {
  if (!target.isArray()) target = Json::arrayValue;
  if (effects.isArray()) {
    for (const auto& effect : effects) target.append(effect);
    return;
  }
  if (effects.isObject() && effects["list"].isArray()) {
    for (const auto& effect : effects["list"]) target.append(effect);
  }
}

void syncIdentitySourceEffects(const Db& db, std::int64_t userId) {
  Json::Value greatWayList{Json::arrayValue}, faithList{Json::arrayValue};
  Json::Value greatWays{Json::arrayValue}, faiths{Json::arrayValue};
  const auto greatRows = db->execSqlSync(
      "SELECT row_to_json(x)::text AS json_text FROM ("
      "SELECT c.great_way_id,g.name,c.rank,c.progress,c.active,g.effects "
      "FROM character_great_ways c JOIN great_way_templates g ON g.id=c.great_way_id "
      "WHERE c.user_id=$1 AND c.active=TRUE AND g.active=TRUE ORDER BY c.created_at,g.id) x", userId);
  for (std::size_t i = 0; i < greatRows.size(); ++i) {
    const auto row = rowJson(greatRows, i);
    appendEffectList(greatWayList, row["effects"]);
    Json::Value item;
    item["name"] = row["name"];
    item["rank"] = row["rank"];
    item["great_way_id"] = row["great_way_id"];
    greatWays.append(item);
  }
  const auto faithRows = db->execSqlSync(
      "SELECT row_to_json(x)::text AS json_text FROM ("
      "SELECT c.deity_id,d.name,c.rank,c.devotion,c.favor,c.active,d.effects "
      "FROM character_faiths c JOIN faith_deities d ON d.id=c.deity_id "
      "WHERE c.user_id=$1 AND c.active=TRUE AND d.active=TRUE ORDER BY c.created_at,d.id) x", userId);
  for (std::size_t i = 0; i < faithRows.size(); ++i) {
    const auto row = rowJson(faithRows, i);
    appendEffectList(faithList, row["effects"]);
    Json::Value item;
    item["name"] = row["name"];
    item["rank"] = row["rank"];
    item["deity_id"] = row["deity_id"];
    faiths.append(item);
  }
  Json::Value greatWayEffects{Json::objectValue}; greatWayEffects["list"] = greatWayList;
  Json::Value faithEffects{Json::objectValue}; faithEffects["list"] = faithList;
  db->execSqlSync(
      "UPDATE character_cards SET great_way_effects=$1::jsonb,faith_effects=$2::jsonb,great_ways=$3::jsonb,faiths=$4::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$5",
      compactJson(greatWayEffects), compactJson(faithEffects), compactJson(greatWays), compactJson(faiths), userId);
}

std::string validatedIdentityRank(const Json::Value& requested, const Json::Value& maximum,
                                  const std::string& label) {
  static const std::array<const char*,10> ranks={"G","F","E","D","C","B","A","S","SS","SSS"};
  auto normalize=[&](const Json::Value& value,const std::string& fallback){auto v=trimmed(stringValue(value,fallback));std::transform(v.begin(),v.end(),v.begin(),[](unsigned char ch){return static_cast<char>(std::toupper(ch));});return v;};
  const auto rank=normalize(requested,"G");
  const auto maxRank=normalize(maximum,"SSS");
  auto pos=[&](const std::string& value){for(std::size_t i=0;i<ranks.size();++i)if(value==ranks[i])return static_cast<int>(i);return -1;};
  const auto rankIndex=pos(rank), maxIndex=pos(maxRank);
  if(rankIndex<0) throw ApiError(drogon::k400BadRequest,label+"格式錯誤");
  if(maxIndex>=0 && rankIndex>maxIndex) throw ApiError(drogon::k400BadRequest,label+"不能高於 "+maxRank+"級");
  return rank;
}

int randomInt(int low, int high) {
  static thread_local std::mt19937_64 engine{std::random_device{}()};
  std::uniform_int_distribution<int> distribution(low, high);
  return distribution(engine);
}

[[maybe_unused]] std::string pathParamRoute(std::string route) {
  int index = 1;
  static const std::regex param(R"(:[A-Za-z][A-Za-z0-9_]*)");
  std::string out;
  std::sregex_iterator begin(route.begin(), route.end(), param), end;
  std::size_t cursor = 0;
  for (auto it = begin; it != end; ++it) {
    out.append(route, cursor, static_cast<std::size_t>(it->position()) - cursor);
    out += "{" + std::to_string(index++) + "}";
    cursor = static_cast<std::size_t>(it->position() + it->length());
  }
  out.append(route, cursor, std::string::npos);
  return out;
}



// [功能備註｜63-cpp.24.1 儀式學經驗／等級／點數]
// 每 100 XP 提升 1 級；每次跨級自動增加 1 點 ritual_points。點數消耗與累計獲得／已花費分開記錄。
std::int64_t ritualLevelForXp(std::int64_t xp) {
  return 1 + std::max<std::int64_t>(0, xp) / 100;
}

Json::Value ritualProgressionJson(const Json::Value& character) {
  const auto xp=std::max<std::int64_t>(0,intValue(character["ritual_xp"]));
  const auto level=ritualLevelForXp(xp);
  Json::Value out;
  out["xp"]=Json::Int64(xp);
  out["level"]=Json::Int64(level);
  out["xp_in_level"]=Json::Int64(xp%100);
  out["xp_to_next"]=Json::Int64(100-(xp%100));
  out["points"]=Json::Int64(std::max<std::int64_t>(0,intValue(character["ritual_points"])));
  out["points_earned"]=Json::Int64(std::max<std::int64_t>(0,intValue(character["ritual_points_earned"])));
  out["points_spent"]=Json::Int64(std::max<std::int64_t>(0,intValue(character["ritual_points_spent"])));
  return out;
}

Json::Value grantRitualXp(const Db& db, std::int64_t userId, std::int64_t amount) {
  if(amount<=0){
    const auto rows=db->execSqlSync("SELECT row_to_json(c)::text AS json_text FROM character_cards c WHERE user_id=$1",userId);
    return rows.empty()?Json::Value{Json::objectValue}:ritualProgressionJson(rowJson(rows));
  }
  const auto rows=db->execSqlSync("SELECT ritual_xp FROM character_cards WHERE user_id=$1",userId);
  if(rows.empty())return Json::Value{Json::objectValue};
  const auto oldXp=std::max<std::int64_t>(0,rows[0]["ritual_xp"].as<std::int64_t>());
  const auto oldLevel=ritualLevelForXp(oldXp);
  const auto newXp=oldXp+amount;
  const auto newLevel=ritualLevelForXp(newXp);
  const auto gainedPoints=std::max<std::int64_t>(0,newLevel-oldLevel);
  db->execSqlSync(
      "UPDATE character_cards SET ritual_xp=$1,ritual_level=$2,ritual_points=ritual_points+$3,"
      "ritual_points_earned=ritual_points_earned+$3,updated_at=CURRENT_TIMESTAMP WHERE user_id=$4",
      newXp,newLevel,gainedPoints,userId);
  const auto updated=db->execSqlSync("SELECT row_to_json(c)::text AS json_text FROM character_cards c WHERE user_id=$1",userId);
  Json::Value out=updated.empty()?Json::Value{Json::objectValue}:ritualProgressionJson(rowJson(updated));
  out["xp_gained"]=Json::Int64(amount);
  out["points_gained"]=Json::Int64(gainedPoints);
  return out;
}

Json::Value ritualResearchState(const Db& db,std::int64_t userId,std::int64_t ritualId) {
  const auto rows=db->execSqlSync(
      "SELECT row_to_json(x)::text AS json_text FROM character_ritual_research x WHERE user_id=$1 AND ritual_id=$2",
      userId,ritualId);
  if(!rows.empty())return rowJson(rows);
  Json::Value out;
  out["user_id"]=Json::Int64(userId);out["ritual_id"]=Json::Int64(ritualId);
  out["discovered"]=false;out["progress"]=0;out["points_invested"]=0;
  out["successful_completions"]=0;out["first_success_rewarded"]=false;out["failure_rewarded"]=false;
  return out;
}

std::int64_t ritualRevealThreshold(const Json::Value& study,const std::string& key,std::int64_t fallback) {
  const auto rules=study["research_reveal_rules"].isObject()?study["research_reveal_rules"]:Json::Value{Json::objectValue};
  return std::clamp<std::int64_t>(intValue(rules[key],fallback),0,100);
}

// [功能備註｜63-cpp.24.1 未解析儀式]
// 同一儀式對不同玩家使用獨立 research row；未達門檻的材料、佈置、步驟、成功／失敗結果不送給前端。
Json::Value ritualStudyForViewer(const Json::Value& study,const Json::Value& research,bool learned) {
  auto out=study;
  // [功能備註｜64-cpp.25.1 Render C++20 編譯修正]
  // `requires` 是 C++20 保留關鍵字，不可作為區域變數名稱。
  const bool requiresResearch=study.get("requires_research",false).asBool();
  const auto progress=learned?100:std::clamp<std::int64_t>(intValue(research["progress"]),0,100);
  const bool discovered=learned||research.get("discovered",false).asBool()||!study.get("hidden_until_discovered",false).asBool();
  out["research_progress"]=Json::Int64(progress);
  out["research_discovered"]=discovered;
  out["research_complete"]=!requiresResearch||progress>=100||learned;
  out["learned"]=learned;
  if(!requiresResearch||progress>=100||learned)return out;

  if(progress<ritualRevealThreshold(study,"name",10))out["name"]=stringValue(study["mystery_name"],"未解析儀式");
  if(progress<ritualRevealThreshold(study,"description",10))out["description"]="尚未解析";
  if(progress<ritualRevealThreshold(study,"materials",20))out["material_requirements"]=Json::Value{Json::arrayValue};
  if(progress<ritualRevealThreshold(study,"elements",30))out["element_requirements"]=Json::Value{Json::objectValue};
  if(progress<ritualRevealThreshold(study,"layout",50)){
    out["layout_notes"]="尚未解析佈置方式";
    Json::Value layout;layout["slots"]=Json::Value{Json::arrayValue};layout["connections"]=Json::Value{Json::arrayValue};
    out["layout_definition"]=layout;
  }
  if(progress<ritualRevealThreshold(study,"steps",70))out["ritual_steps"]=Json::Value{Json::arrayValue};
  if(progress<ritualRevealThreshold(study,"success",90))out["success_effects"]=Json::Value{Json::arrayValue};
  if(progress<ritualRevealThreshold(study,"failure",100))out["failure_effects"]=Json::Value{Json::arrayValue};
  return out;
}

std::int64_t defaultRitualFirstSuccessXp(const std::string& rank) {
  static const std::unordered_map<std::string,std::int64_t> values{
    {"G",20},{"F",30},{"E",45},{"D",65},{"C",90},{"B",120},{"A",160},{"S",210},{"SS",280},{"SSS",360}
  };
  const auto it=values.find(rank);return it==values.end()?20:it->second;
}

Json::Value rewardRitualSuccess(const Db& db,std::int64_t userId,const Json::Value& ritual) {
  const auto ritualId=intValue(ritual["ritual_id"],intValue(ritual["id"]));
  if(ritualId<=0)return Json::Value{Json::objectValue};
  auto research=ritualResearchState(db,userId,ritualId);
  const bool first=!research.get("first_success_rewarded",false).asBool();
  const auto configuredFirst=std::max<std::int64_t>(0,intValue(ritual["first_success_xp"]));
  const auto xp=first?(configuredFirst>0?configuredFirst:defaultRitualFirstSuccessXp(stringValue(ritual["rank"],"G")))
                     :std::max<std::int64_t>(0,intValue(ritual["repeat_success_xp"]));
  db->execSqlSync(
      "INSERT INTO character_ritual_research(user_id,ritual_id,discovered,progress,successful_completions,first_success_rewarded,last_completed_at) "
      "VALUES($1,$2,TRUE,100,1,TRUE,CURRENT_TIMESTAMP) "
      "ON CONFLICT(user_id,ritual_id) DO UPDATE SET discovered=TRUE,progress=GREATEST(character_ritual_research.progress,100),"
      "successful_completions=character_ritual_research.successful_completions+1,"
      "first_success_rewarded=TRUE,last_completed_at=CURRENT_TIMESTAMP,updated_at=CURRENT_TIMESTAMP",
      userId,ritualId);
  auto out=grantRitualXp(db,userId,xp);
  out["first_success"]=first;out["ritual_id"]=Json::Int64(ritualId);
  return out;
}

// [功能備註｜64-cpp.25 登階／登神儀式]
// 儀式成功時才真正跨大階；1～8 階使用登階儀式，8→9 與 9→10 使用登神儀式。0→1 不需要儀式。
std::int64_t validateAscensionRitual(const Db& db,std::int64_t userId,const Json::Value& ritual) {
  const auto type=stringValue(ritual["ritual_type"],stringValue(ritual["category"]));
  if(type!="登階儀式"&&type!="登神儀式")return 0;
  const auto rows=db->execSqlSync("SELECT tier,level FROM character_cards WHERE user_id=$1",userId);
  if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到角色卡");
  const auto tier=std::clamp<std::int64_t>(rows[0]["tier"].as<std::int64_t>(),0,10);
  const auto level=std::clamp<std::int64_t>(rows[0]["level"].as<std::int64_t>(),1,4);
  const auto configuredFrom=std::max<std::int64_t>(0,intValue(ritual["ascension_from_tier"]));
  auto target=std::max<std::int64_t>(0,intValue(ritual["ascension_to_tier"]));
  if(target<=0)target=tier+1;
  if(tier==0)throw ApiError(drogon::k409Conflict,"0階升1階不需要登階儀式，可直接由升階功能完成");
  if(level<4)throw ApiError(drogon::k409Conflict,"必須先到達目前大階4級才能完成登階儀式");
  if(configuredFrom>0&&configuredFrom!=tier)throw ApiError(drogon::k409Conflict,"此儀式不是目前大階對應的登階儀式");
  if(target!=tier+1||target>10)throw ApiError(drogon::k409Conflict,"此儀式的目標大階與角色目前進度不符");
  const auto requiredType=target>=9?"登神儀式":"登階儀式";
  if(type!=requiredType)throw ApiError(drogon::k409Conflict,std::string("晉升到 ")+std::to_string(target)+"階必須完成「"+requiredType+"」");
  return target;
}

Json::Value applyAscensionRitual(const Db& db,std::int64_t userId,const Json::Value& ritual) {
  const auto target=validateAscensionRitual(db,userId,ritual);
  if(target<=0)return Json::Value{Json::objectValue};
  return advanceCharacterTier(db,userId,target,"ritual:"+stringValue(ritual["name"],"登階儀式"),true);
}

Json::Value rewardRitualFailureResearch(const Db& db,std::int64_t userId,const Json::Value& ritual) {
  const auto ritualId=intValue(ritual["ritual_id"],intValue(ritual["id"]));
  if(ritualId<=0)return Json::Value{Json::objectValue};
  auto research=ritualResearchState(db,userId,ritualId);
  if(research.get("failure_rewarded",false).asBool())return Json::Value{Json::objectValue};
  const auto xp=std::max<std::int64_t>(0,intValue(ritual["failure_research_xp"]));
  const auto progressGain=std::max<std::int64_t>(0,intValue(ritual["failure_research_progress"]));
  db->execSqlSync(
      "INSERT INTO character_ritual_research(user_id,ritual_id,discovered,progress,failure_rewarded,updated_at) "
      "VALUES($1,$2,TRUE,LEAST(100,$3),TRUE,CURRENT_TIMESTAMP) "
      "ON CONFLICT(user_id,ritual_id) DO UPDATE SET discovered=TRUE,"
      "progress=LEAST(100,character_ritual_research.progress+$3),failure_rewarded=TRUE,updated_at=CURRENT_TIMESTAMP",
      userId,ritualId,progressGain);
  auto out=grantRitualXp(db,userId,xp);
  out["research_progress_gained"]=Json::Int64(progressGain);out["ritual_id"]=Json::Int64(ritualId);
  return out;
}

// [功能備註｜63-cpp.24 儀式佈置驗證]
Json::Value ritualLayoutValidation(const Json::Value& ritual, const Json::Value& placements) {
  Json::Value out; out["ready"]=true; out["missing"]=Json::arrayValue; out["filled"]=0; out["required"]=0;
  const auto layout=ritual["layout_definition"].isObject()?ritual["layout_definition"]:Json::Value{Json::objectValue};
  const auto slots=layout["slots"].isArray()?layout["slots"]:Json::Value{Json::arrayValue};
  for(const auto& slot:slots){
    if(!slot.get("required",true).asBool())continue;
    out["required"]=out["required"].asInt()+1;
    const auto id=stringValue(slot["id"],stringValue(slot["key"]));
    if(!id.empty()&&placements.isObject()&&placements.isMember(id)&&!placements[id].isNull()){
      const auto expected=trimmed(stringValue(slot["expected"]));
      const auto placedLabel=trimmed(stringValue(placements[id]["label"]));
      if(expected.empty()||expected==placedLabel){out["filled"]=out["filled"].asInt()+1;continue;}
      out["ready"]=false;out["missing"].append(stringValue(slot["label"],id)+"（需放置 "+expected+"）");continue;
    }
    out["ready"]=false; out["missing"].append(stringValue(slot["label"],id.empty()?"未命名節點":id));
  }
  return out;
}

Json::Value ritualInstanceJson(const std::shared_ptr<CoreService>& service, std::int64_t instanceId) {
  const auto rows=service->database()->execSqlSync(
    "SELECT row_to_json(x)::text AS json_text FROM (SELECT i.*,r.name,r.rank,r.category,r.ritual_type,r.description,r.layout_notes,r.layout_definition,r.material_requirements,r.element_requirements,r.ritual_steps,r.success_effects,r.failure_effects,r.can_cooperate,r.cast_rounds,r.first_success_xp,r.repeat_success_xp,r.failure_research_xp,r.failure_research_progress,r.ascension_from_tier,r.ascension_to_tier FROM ritual_instances i JOIN ritual_studies r ON r.id=i.ritual_id WHERE i.id=$1) x",instanceId);
  if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到儀式實例");
  auto out=rowJson(rows); out["validation"]=ritualLayoutValidation(out,out["placements"]); return out;
}

using H0 = std::function<void(const Request&, Callback&)>;
using H1 = std::function<void(const Request&, Callback&, const std::string&)>;
using H2 = std::function<void(const Request&, Callback&, const std::string&, const std::string&)>;
using H3 = std::function<void(const Request&, Callback&, const std::string&, const std::string&, const std::string&)>;

void reg0(const std::string& path, drogon::HttpMethod method, H0 handler) {
  drogon::app().registerHandler(path,
      [handler = std::move(handler)](const Request& req, Callback&& cb) mutable { handler(req, cb); },
      {method});
}
void reg1(const std::string& path, drogon::HttpMethod method, H1 handler) {
  drogon::app().registerHandler(path,
      [handler = std::move(handler)](const Request& req, Callback&& cb, const std::string& p1) mutable { handler(req, cb, p1); },
      {method});
}
void reg2(const std::string& path, drogon::HttpMethod method, H2 handler) {
  drogon::app().registerHandler(path,
      [handler = std::move(handler)](const Request& req, Callback&& cb, const std::string& p1, const std::string& p2) mutable { handler(req, cb, p1, p2); },
      {method});
}
void reg3(const std::string& path, drogon::HttpMethod method, H3 handler) {
  drogon::app().registerHandler(path,
      [handler = std::move(handler)](const Request& req, Callback&& cb, const std::string& p1, const std::string& p2, const std::string& p3) mutable { handler(req, cb, p1, p2, p3); },
      {method});
}

void registerAdminCrud0(const std::shared_ptr<CoreService>& service,
                        const std::string& listPath,
                        const std::string& table,
                        const std::string& key,
                        bool createAtSamePath = true) {
  reg0(listPath, drogon::Get, [service, table, key](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out[key] = genericList(service->database(), table);
      reply(cb, std::move(out));
    }, "讀取資料失敗");
  });
  if (createAtSamePath) {
    reg0(listPath, drogon::Post, [service, table, key](const Request& req, Callback& cb) {
      runHandler(cb, [&] {
        requireUser(service, req, true);
        Json::Value out;
        out[key == "items" ? "item" : key == "skills" ? "skill" : key == "recipes" ? "recipe" :
            key == "bloodlines" ? "bloodline" : key == "professions" ? "profession" :
            key == "summon_templates" ? "summon_template" : key == "templates" ? "template" : "entry"] =
            genericInsert(service->database(), table, bodyOrEmpty(req));
        out["ok"] = true;
        reply(cb, std::move(out));
      }, "建立資料失敗");
    });
  }
}

void registerAdminPatchDelete1(const std::shared_ptr<CoreService>& service,
                               const std::string& path,
                               const std::string& table,
                               const std::string& singularKey) {
  reg1(path, drogon::Patch, [service, table, singularKey](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto id = positiveId(idText);
      Json::Value out;
      out["ok"] = true;
      out[singularKey] = genericUpdate(service->database(), table, id, bodyOrEmpty(req));
      reply(cb, std::move(out));
    }, "更新資料失敗");
  });
  reg1(path, drogon::Delete, [service, table](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      genericDelete(service->database(), table, positiveId(idText));
      Json::Value out;
      out["ok"] = true;
      reply(cb, std::move(out));
    }, "刪除資料失敗");
  });
}

std::filesystem::path migrationFilePath() {
  const std::array<std::filesystem::path, 5> candidates = {
      std::filesystem::path{"db/legacy_v39_migrations.sql"},
      std::filesystem::path{"legacy_v39_migrations.sql"},
      std::filesystem::path{"/app/db/legacy_v39_migrations.sql"},
      std::filesystem::path{"../db/legacy_v39_migrations.sql"},
      std::filesystem::path{"../../db/legacy_v39_migrations.sql"}};
  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
  }
  throw std::runtime_error("legacy_v39_migrations.sql not found");
}

std::vector<std::string> splitSqlStatements(const std::string& source) {
  std::vector<std::string> statements;
  std::string current;
  bool singleQuoted = false;
  for (std::size_t i = 0; i < source.size(); ++i) {
    const char ch = source[i];
    if (ch == '\'' && (i == 0 || source[i - 1] != '\\')) {
      if (singleQuoted && i + 1 < source.size() && source[i + 1] == '\'') {
        current.push_back(ch);
        current.push_back(source[++i]);
        continue;
      }
      singleQuoted = !singleQuoted;
    }
    if (ch == ';' && !singleQuoted) {
      auto statement = trimmed(current);
      if (!statement.empty()) statements.push_back(std::move(statement));
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  auto tail = trimmed(current);
  if (!tail.empty()) statements.push_back(std::move(tail));
  return statements;
}

}  // namespace

void applyLegacyV39Migrations(const std::shared_ptr<CoreService>& service) {
  const auto path = migrationFilePath();
  std::ifstream stream(path);
  if (!stream) throw std::runtime_error("cannot open legacy migration file: " + path.string());
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  const auto statements = splitSqlStatements(buffer.str());
  std::size_t applied = 0;
  for (const auto& statement : statements) {
    try {
      service->database()->execSqlSync(statement);
      ++applied;
    } catch (const drogon::orm::DrogonDbException& error) {
      LOG_ERROR << "legacy migration failed at statement " << (applied + 1)
                << ": " << error.base().what() << "\nSQL: " << statement;
      throw;
    }
  }
  LOG_INFO << "legacy v39 schema compatibility migrations applied: " << applied;
}


Json::Value buildCharacterBackupCheckpoint(const Db& db, std::int64_t userId) {
  Json::Value checkpoint;
  const auto users = db->execSqlSync(
      "SELECT row_to_json(x)::text AS json_text FROM (SELECT id,username,is_admin,created_at FROM users WHERE id=$1) x", userId);
  if (users.empty()) throw ApiError(drogon::k404NotFound, "找不到玩家");
  checkpoint["version"] = 39;
  checkpoint["created_at"] = utcNowIso();
  checkpoint["user"] = rowJson(users);
  checkpoint["character"] = characterRaw(db, userId);
  Json::Value related;
  related["statuses"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM character_statuses x WHERE user_id=$1 ORDER BY id", userId);
  related["great_ways"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM character_great_ways x WHERE user_id=$1 ORDER BY great_way_id", userId);
  related["faiths"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM character_faiths x WHERE user_id=$1 ORDER BY deity_id", userId);
  related["reputations"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM character_reputations x WHERE user_id=$1 ORDER BY faction_id", userId);
  related["clues"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM character_clues x WHERE user_id=$1 ORDER BY clue_id", userId);
  related["rules"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM rule_discoveries x WHERE user_id=$1 ORDER BY rule_id", userId);
  related["item_identifications"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM item_identifications x WHERE user_id=$1 ORDER BY item_template_id", userId);
  related["tasks"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM task_participants x WHERE user_id=$1 ORDER BY task_id", userId);
  related["avatar_image"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM avatar_images x WHERE user_id=$1", userId);
  related["portrait_image"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM portrait_images x WHERE user_id=$1", userId);
  // [功能備註｜65-cpp.26.4 多立繪備份] 每張表情／戰鬥立繪一起保存，還原後 active/default 狀態不遺失。
  related["portrait_variants"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM character_portrait_variants x WHERE user_id=$1 ORDER BY sort_order,id", userId);
  // [功能備註｜詞條／特殊貨幣備份] 角色還原時一併保存洗煉貨幣與玩家持有物詞條。
  related["special_currencies"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM character_special_currencies x WHERE user_id=$1 ORDER BY currency_code", userId);
  related["affix_targets"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM affix_targets x WHERE owner_user_id=$1 ORDER BY id", userId);
  checkpoint["related"] = related;
  return checkpoint;
}

Json::Value buildRoomBackupCheckpoint(const std::shared_ptr<CoreService>& service,
                                      std::int64_t roomId) {
  const auto db = service->database();
  Json::Value checkpoint;
  const auto rooms = db->execSqlSync("SELECT row_to_json(r)::text AS json_text FROM rooms r WHERE id=$1", roomId);
  if (rooms.empty()) throw ApiError(drogon::k404NotFound, "找不到房間");
  checkpoint["version"] = 39;
  checkpoint["created_at"] = utcNowIso();
  checkpoint["room"] = rowJson(rooms);
  checkpoint["members"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM room_members x WHERE room_id=$1 ORDER BY user_id", roomId);
  checkpoint["characters"] = selectRows1(db, "SELECT row_to_json(c)::text AS json_text FROM character_cards c JOIN room_members rm ON rm.user_id=c.user_id WHERE rm.room_id=$1 ORDER BY c.user_id", roomId);
  checkpoint["map_nodes"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM room_map_nodes x WHERE room_id=$1 ORDER BY id", roomId);
  checkpoint["teams"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM room_teams x WHERE room_id=$1 ORDER BY id", roomId);
  checkpoint["monsters"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM room_monsters x WHERE room_id=$1 ORDER BY id", roomId);
  checkpoint["npcs"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM room_npcs x WHERE room_id=$1 ORDER BY npc_template_id", roomId);
  checkpoint["dungeons"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM room_dungeons x WHERE room_id=$1 ORDER BY dungeon_template_id", roomId);
  checkpoint["summons"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM summon_instances x WHERE room_id=$1 ORDER BY id", roomId);
  checkpoint["statuses"] = selectRows1(db, "SELECT row_to_json(s)::text AS json_text FROM character_statuses s JOIN room_members rm ON rm.user_id=s.user_id WHERE rm.room_id=$1 ORDER BY s.id", roomId);
  checkpoint["combat_statuses"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM combat_entity_statuses x WHERE room_id=$1 ORDER BY id", roomId);
  // [功能備註｜62-cpp.23 事件技能狀態備份]
  checkpoint["event_skill_statuses"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM combat_status_instances x WHERE room_id=$1 ORDER BY id", roomId);
  // [功能備註｜62-cpp.23.1 場地備份]
  checkpoint["combat_field_effects"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM combat_field_effects x WHERE room_id=$1 ORDER BY id", roomId);
  // [功能備註｜63-cpp.24 儀式場備份]
  checkpoint["ritual_instances"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM ritual_instances x WHERE room_id=$1 ORDER BY id", roomId);
  checkpoint["relations"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM npc_relationships x WHERE room_id=$1 ORDER BY npc_template_id,user_id", roomId);
  checkpoint["containers"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM search_containers x WHERE room_id=$1 ORDER BY id", roomId);
  checkpoint["scheduled"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM scheduled_world_events x WHERE room_id=$1 ORDER BY id", roomId);
  checkpoint["chases"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM room_chases x WHERE room_id=$1 ORDER BY id", roomId);
  checkpoint["map_discoveries"] = selectRows1(db, "SELECT row_to_json(md)::text AS json_text FROM map_node_discoveries md JOIN room_map_nodes n ON n.id=md.node_id WHERE n.room_id=$1 ORDER BY md.node_id,md.user_id", roomId);
  checkpoint["npc_discoveries"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM npc_discoveries x WHERE room_id=$1 ORDER BY npc_template_id,user_id", roomId);
  checkpoint["clue_combinations"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM clue_combinations x WHERE room_id=$1 ORDER BY id", roomId);
  checkpoint["clue_unlocks"] = selectRows1(db, "SELECT row_to_json(u)::text AS json_text FROM clue_combination_unlocks u JOIN clue_combinations c ON c.id=u.combination_id WHERE c.room_id=$1 ORDER BY u.combination_id,u.user_id", roomId);
  checkpoint["great_way_assignments"] = selectRows1(db, "SELECT row_to_json(g)::text AS json_text FROM character_great_ways g JOIN room_members rm ON rm.user_id=g.user_id WHERE rm.room_id=$1 ORDER BY g.user_id,g.great_way_id", roomId);
  checkpoint["faith_assignments"] = selectRows1(db, "SELECT row_to_json(f)::text AS json_text FROM character_faiths f JOIN room_members rm ON rm.user_id=f.user_id WHERE rm.room_id=$1 ORDER BY f.user_id,f.deity_id", roomId);
  // [功能備註｜規則引擎備份] 房間存檔必須連同規則、玩家規則狀態與觸發紀錄保存，避免回溯後條件進度錯位。
  checkpoint["rule_engine_rules"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM rule_engine_rules x WHERE room_id=$1 ORDER BY priority DESC,id", roomId);
  checkpoint["rule_engine_player_state"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM rule_engine_player_state x WHERE room_id=$1 ORDER BY user_id", roomId);
  checkpoint["rule_engine_rule_state"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM rule_engine_rule_state x WHERE room_id=$1 ORDER BY rule_id,user_id", roomId);
  checkpoint["rule_engine_logs"] = selectRows1(db, "SELECT row_to_json(x)::text AS json_text FROM rule_engine_logs x WHERE room_id=$1 ORDER BY id", roomId);
  return checkpoint;
}

void restoreCharacterBackupCheckpoint(const Db& db, std::int64_t userId,
                                      const Json::Value& checkpoint,
                                      std::int64_t actorId) {
  if (!checkpoint["character"].isObject() || intValue(checkpoint["character"]["user_id"]) != userId)
    throw ApiError(drogon::k400BadRequest, "角色備份內容與目標不一致");
  const auto safety = buildCharacterBackupCheckpoint(db, userId);
  db->execSqlSync("INSERT INTO character_savepoints(user_id,label,snapshot,created_by) VALUES($1,$2,$3::jsonb,$4)",
                  userId, "匯入前自動安全備份 " + utcNowIso(), compactJson(safety), actorId);
  const auto cards = db->execSqlSync("SELECT id FROM character_cards WHERE user_id=$1", userId);
  if (cards.empty()) throw ApiError(drogon::k404NotFound, "找不到目標角色");
  genericUpdate(db, "character_cards", cards[0]["id"].as<std::int64_t>(), checkpoint["character"]);
  const auto related = checkpoint["related"].isObject() ? checkpoint["related"] : Json::Value{Json::objectValue};
  const std::array<std::pair<const char*,const char*>,8> groups={{{"statuses","character_statuses"},{"great_ways","character_great_ways"},{"faiths","character_faiths"},{"reputations","character_reputations"},{"clues","character_clues"},{"rules","rule_discoveries"},{"item_identifications","item_identifications"},{"tasks","task_participants"}}};
  for (const auto& [key,table] : groups) {
    db->execSqlSync(std::string("DELETE FROM ") + table + " WHERE user_id=$1", userId);
    if (!related[key].isArray()) continue;
    for (const auto& row : related[key]) {
      Json::Value extra; extra["user_id"] = Json::Int64(userId);
      genericInsert(db, table, row, extra);
    }
  }
  db->execSqlSync("DELETE FROM avatar_images WHERE user_id=$1", userId);
  if (related["avatar_image"].isArray()) {
    for (const auto& row : related["avatar_image"]) {
      Json::Value extra; extra["user_id"] = Json::Int64(userId);
      genericInsert(db, "avatar_images", row, extra);
    }
  }
  db->execSqlSync("DELETE FROM portrait_images WHERE user_id=$1", userId);
  if (related["portrait_image"].isArray()) {
    for (const auto& row : related["portrait_image"]) {
      Json::Value extra; extra["user_id"] = Json::Int64(userId);
      genericInsert(db, "portrait_images", row, extra);
    }
  }
  db->execSqlSync("DELETE FROM character_portrait_variants WHERE user_id=$1", userId);
  if (related["portrait_variants"].isArray()) {
    for (const auto& row : related["portrait_variants"]) {
      Json::Value copy=row;copy.removeMember("id");
      Json::Value extra; extra["user_id"] = Json::Int64(userId);
      genericInsert(db, "character_portrait_variants", copy, extra);
    }
  }
  // [功能備註｜65-cpp.26.4 多立繪備份還原] 特定角色備份不沿用舊 variant id，因此還原後重新綁定 active URL。
  {
    const auto rows=db->execSqlSync("SELECT id,external_url,is_default FROM character_portrait_variants WHERE user_id=$1 ORDER BY is_active DESC,is_default DESC,sort_order,id LIMIT 1",userId);
    if(!rows.empty()){
      const auto id=rows[0]["id"].as<std::int64_t>();const auto external=rows[0]["external_url"].as<std::string>();
      const auto url=external.empty()?"/api/character-portrait-variants/"+std::to_string(id)+"/image":external;
      db->execSqlSync("UPDATE character_portrait_variants SET is_active=(id=$1) WHERE user_id=$2",id,userId);
      if(!rows[0]["is_default"].as<bool>())db->execSqlSync("UPDATE character_portrait_variants SET is_default=(id=$1) WHERE user_id=$2 AND NOT EXISTS (SELECT 1 FROM character_portrait_variants WHERE user_id=$2 AND is_default=TRUE)",id,userId);
      db->execSqlSync("UPDATE character_cards SET portrait_url=$1,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",url,userId);
    }
  }
  db->execSqlSync("DELETE FROM character_special_currencies WHERE user_id=$1", userId);
  if (related["special_currencies"].isArray()) for (const auto& row : related["special_currencies"]) { Json::Value extra; extra["user_id"]=Json::Int64(userId); genericInsert(db,"character_special_currencies",row,extra); }
  db->execSqlSync("DELETE FROM affix_targets WHERE owner_user_id=$1", userId);
  if (related["affix_targets"].isArray()) for (const auto& row : related["affix_targets"]) { Json::Value extra; extra["owner_user_id"]=Json::Int64(userId); genericInsert(db,"affix_targets",row,extra); }
  syncIdentitySourceEffects(db, userId);
}

void restoreRoomBackupCheckpoint(const std::shared_ptr<CoreService>& service,
                                 std::int64_t roomId,
                                 const Json::Value& checkpoint,
                                 std::int64_t actorId) {
  const auto db=service->database();
  if (!checkpoint["room"].isObject() || intValue(checkpoint["room"]["id"]) != roomId)
    throw ApiError(drogon::k400BadRequest, "房間備份內容與目標不一致");
  const auto safety=buildRoomBackupCheckpoint(service,roomId);
  db->execSqlSync("INSERT INTO room_savepoints(room_id,label,snapshot,created_by) VALUES($1,$2,$3::jsonb,$4)",roomId,"匯入前自動安全備份 "+utcNowIso(),compactJson(safety),actorId);
  genericUpdate(db,"rooms",roomId,checkpoint["room"]);
  for(const auto& row:checkpoint["members"]){const auto uid=intValue(row["user_id"]);if(uid>0)genericUpdateByKeys(db,"room_members",row,{{"room_id",roomId},{"user_id",uid}});}
  for(const auto& row:checkpoint["characters"]){const auto uid=intValue(row["user_id"]);if(uid<=0)continue;const auto cards=db->execSqlSync("SELECT id FROM character_cards WHERE user_id=$1",uid);if(!cards.empty())genericUpdate(db,"character_cards",cards[0]["id"].as<std::int64_t>(),row);}
  const auto updateOwned=[&](const char* table,const Json::Value& rows){if(!rows.isArray())return;for(const auto& row:rows){const auto id=intValue(row["id"]);if(id<=0)continue;const auto found=db->execSqlSync(std::string("SELECT 1 FROM ")+table+" WHERE id=$1 AND room_id=$2",id,roomId);if(!found.empty())genericUpdate(db,table,id,row);}};
  updateOwned("room_map_nodes",checkpoint["map_nodes"]); updateOwned("room_teams",checkpoint["teams"]); updateOwned("room_monsters",checkpoint["monsters"]); updateOwned("combat_entity_statuses",checkpoint["combat_statuses"]); updateOwned("search_containers",checkpoint["containers"]); updateOwned("scheduled_world_events",checkpoint["scheduled"]); updateOwned("room_chases",checkpoint["chases"]); updateOwned("clue_combinations",checkpoint["clue_combinations"]);
  db->execSqlSync("DELETE FROM combat_status_instances WHERE room_id=$1",roomId);
  if(checkpoint["event_skill_statuses"].isArray())for(const auto& row:checkpoint["event_skill_statuses"]){Json::Value extra;extra["room_id"]=Json::Int64(roomId);genericInsert(db,"combat_status_instances",row,extra);}
  db->execSqlSync("DELETE FROM combat_field_effects WHERE room_id=$1",roomId);
  if(checkpoint["combat_field_effects"].isArray())for(const auto& row:checkpoint["combat_field_effects"]){Json::Value extra;extra["room_id"]=Json::Int64(roomId);genericInsert(db,"combat_field_effects",row,extra);}
  db->execSqlSync("DELETE FROM ritual_instances WHERE room_id=$1",roomId);
  if(checkpoint["ritual_instances"].isArray())for(const auto& row:checkpoint["ritual_instances"]){Json::Value extra;extra["room_id"]=Json::Int64(roomId);genericInsert(db,"ritual_instances",row,extra);}
  if(checkpoint["npcs"].isArray())for(const auto& row:checkpoint["npcs"]){const auto id=intValue(row["npc_template_id"]);if(id>0)genericUpdateByKeys(db,"room_npcs",row,{{"room_id",roomId},{"npc_template_id",id}});}
  if(checkpoint["dungeons"].isArray())for(const auto& row:checkpoint["dungeons"]){const auto id=intValue(row["dungeon_template_id"]);if(id>0)genericUpdateByKeys(db,"room_dungeons",row,{{"room_id",roomId},{"dungeon_template_id",id}});}
  if(checkpoint["summons"].isArray())for(const auto& row:checkpoint["summons"]){const auto id=intValue(row["id"]);if(id>0&&!db->execSqlSync("SELECT 1 FROM summon_instances WHERE id=$1 AND room_id=$2",id,roomId).empty())genericUpdate(db,"summon_instances",id,row);}
  if(checkpoint["statuses"].isArray())for(const auto& row:checkpoint["statuses"]){const auto id=intValue(row["id"]),uid=intValue(row["user_id"]);if(id>0&&uid>0&&!db->execSqlSync("SELECT 1 FROM room_members WHERE room_id=$1 AND user_id=$2",roomId,uid).empty())genericUpdate(db,"character_statuses",id,row);}
  if(checkpoint["relations"].isArray())for(const auto& row:checkpoint["relations"]){const auto npc=intValue(row["npc_template_id"]),uid=intValue(row["user_id"]);if(npc>0&&uid>0)genericUpdateByKeys(db,"npc_relationships",row,{{"room_id",roomId},{"npc_template_id",npc},{"user_id",uid}});}
  db->execSqlSync("DELETE FROM map_node_discoveries WHERE node_id IN (SELECT id FROM room_map_nodes WHERE room_id=$1)",roomId);
  if(checkpoint["map_discoveries"].isArray())for(const auto& row:checkpoint["map_discoveries"])genericInsert(db,"map_node_discoveries",row);
  db->execSqlSync("DELETE FROM npc_discoveries WHERE room_id=$1",roomId);
  if(checkpoint["npc_discoveries"].isArray())for(const auto& row:checkpoint["npc_discoveries"]){Json::Value extra;extra["room_id"]=Json::Int64(roomId);genericInsert(db,"npc_discoveries",row,extra);}
  db->execSqlSync("DELETE FROM clue_combination_unlocks WHERE combination_id IN (SELECT id FROM clue_combinations WHERE room_id=$1)",roomId);
  if(checkpoint["clue_unlocks"].isArray())for(const auto& row:checkpoint["clue_unlocks"])genericInsert(db,"clue_combination_unlocks",row);
  const auto members=db->execSqlSync("SELECT user_id FROM room_members WHERE room_id=$1",roomId);
  for(const auto& member:members){const auto uid=member["user_id"].as<std::int64_t>();db->execSqlSync("DELETE FROM character_great_ways WHERE user_id=$1",uid);db->execSqlSync("DELETE FROM character_faiths WHERE user_id=$1",uid);}
  if(checkpoint["great_way_assignments"].isArray())for(const auto& row:checkpoint["great_way_assignments"])genericInsert(db,"character_great_ways",row);
  if(checkpoint["faith_assignments"].isArray())for(const auto& row:checkpoint["faith_assignments"])genericInsert(db,"character_faiths",row);
  // [功能備註｜規則引擎還原] 先還原規則，再還原依賴 rule_id 的玩家狀態與紀錄。
  db->execSqlSync("DELETE FROM rule_engine_rules WHERE room_id=$1",roomId);
  db->execSqlSync("DELETE FROM rule_engine_player_state WHERE room_id=$1",roomId);
  if(checkpoint["rule_engine_rules"].isArray())for(const auto& row:checkpoint["rule_engine_rules"]){Json::Value extra;extra["room_id"]=Json::Int64(roomId);genericInsert(db,"rule_engine_rules",row,extra);}
  if(checkpoint["rule_engine_player_state"].isArray())for(const auto& row:checkpoint["rule_engine_player_state"]){Json::Value extra;extra["room_id"]=Json::Int64(roomId);genericInsert(db,"rule_engine_player_state",row,extra);}
  if(checkpoint["rule_engine_rule_state"].isArray())for(const auto& row:checkpoint["rule_engine_rule_state"]){Json::Value extra;extra["room_id"]=Json::Int64(roomId);genericInsert(db,"rule_engine_rule_state",row,extra);}
  if(checkpoint["rule_engine_logs"].isArray())for(const auto& row:checkpoint["rule_engine_logs"]){Json::Value extra;extra["room_id"]=Json::Int64(roomId);genericInsert(db,"rule_engine_logs",row,extra);}
  for(const auto& member:members)syncIdentitySourceEffects(db,member["user_id"].as<std::int64_t>());
}

Json::Value inspectBackupForRestore(const std::shared_ptr<CoreService>& service,
                                    const Json::Value& raw,
                                    std::int64_t actorId,
                                    bool issueToken=true) {
  const auto backup=validateBackupEnvelope(raw);
  Json::Value preview; preview["valid"]=true; preview["scope"]=backup["scope"]; preview["target"]=backup["target"]; preview["checksum"]=backup["checksum"];
  const auto scope=stringValue(backup["scope"]);
  if(scope=="site"){
    preview["restorable"]=false; preview["counts"]=backup["data"]["counts"];
    preview["warning"]="全站備份不含密碼與伺服器密鑰，僅供下載保存與資料庫管理員手動救援；網頁不提供全站覆寫。";
    return preview;
  }
  const auto targetId=intValue(backup["target"]["id"]);
  if(targetId<=0)throw ApiError(drogon::k400BadRequest,"備份目標編號不正確");
  const auto checkpoint=backup["data"]["checkpoint"];
  if(!checkpoint.isObject())throw ApiError(drogon::k400BadRequest,"備份缺少 checkpoint");
  preview["restorable"]=true;
  if(scope=="room"){
    if(service->database()->execSqlSync("SELECT 1 FROM rooms WHERE id=$1",targetId).empty())throw ApiError(drogon::k404NotFound,"目前網站找不到這個房間，房間備份只能回復既有房間");
    if(intValue(checkpoint["room"]["id"])!=targetId)throw ApiError(drogon::k400BadRequest,"房間備份內容與目標不一致");
    preview["counts"]=backupCounts(checkpoint,{"members","characters","map_nodes","teams","monsters","npcs","dungeons","summons","statuses","combat_statuses","relations","containers","scheduled","chases"});
    preview["confirm_text"]="還原房間 #"+std::to_string(targetId);
    preview["warning"]="房間還原會回復既有房間中的可變狀態；已刪除的地圖、怪物或其他結構不會由此重建。執行前會自動建立安全存檔。";
  }else{
    if(service->database()->execSqlSync("SELECT 1 FROM character_cards WHERE user_id=$1",targetId).empty())throw ApiError(drogon::k404NotFound,"目前網站找不到這個玩家角色，角色備份只能回復既有角色");
    if(intValue(checkpoint["character"]["user_id"])!=targetId)throw ApiError(drogon::k400BadRequest,"角色備份內容與目標不一致");
    Json::Value counts; counts["character"]=1; const auto related=checkpoint["related"];
    for(const auto* key:{"statuses","great_ways","faiths","reputations","clues","rules","item_identifications","tasks"})counts[key]=related[key].isArray()?static_cast<Json::UInt64>(related[key].size()):0;
    preview["counts"]=counts; preview["confirm_text"]="還原角色 #"+std::to_string(targetId);
    preview["warning"]="角色還原會替換角色卡、狀態、大道、信仰、聲望、線索、規則情報、道具鑑定與任務參與狀態；帳號密碼及房間成員資格不受影響。";
  }
  if(issueToken)preview["restore_token"]=createRestoreToken(backup,actorId);
  return preview;
}

void registerLegacyV39Routes(const std::shared_ptr<CoreService>& service) {
  const auto db = service->database();

  // Player fifth resource is intentionally DM/event controlled, matching v39.
  // [功能備註｜大道／信仰資源｜PATCH /api/character/fifth-resource]
  // 用途：修改大道／信仰資源相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/character/fifth-resource", drogon::Patch,
       [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req);
      throw ApiError(drogon::k403Forbidden,
                     "玩家不能直接修改大道／信仰目前值；由 DM、技能或事件處理");
    });
  });

  // [功能備註｜大道／信仰資源｜PATCH /api/admin/players/{1}/fifth-resource]
  // 用途：修改大道／信仰資源相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/players/{1}/fifth-resource", drogon::Patch,
       [service](const Request& req, Callback& cb, const std::string& userIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto userId = positiveId(userIdText, "玩家 ID 無效");
      auto body = bodyOrEmpty(req);
      auto character = characterRaw(service->database(), userId);
      const auto west = stringValue(character["faction"]) == "西國";
      const char* currentField = west ? "current_faith" : "current_great_way";
      const char* maxField = west ? "faith" : "great_way";
      std::int64_t next = intValue(character[currentField], intValue(character[maxField]));
      if (body.isMember("value")) next = std::max<std::int64_t>(0, intValue(body["value"]));
      if (body.isMember("delta")) next = std::max<std::int64_t>(0, next + intValue(body["delta"]));
      next = std::min(next, std::max<std::int64_t>(0, intValue(character[maxField])));
      service->database()->execSqlSync(
          std::string{"UPDATE character_cards SET "} + currentField + "=$1,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",
          next, userId);
      Json::Value out;
      out["ok"] = true;
      out["character"] = characterRaw(service->database(), userId);
      reply(cb, std::move(out));
    }, "DM 更新大道／信仰失敗");
  });

  // [功能備註｜技能｜POST /api/character/skill]
  // 用途：建立/執行技能相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/character/skill", drogon::Post,
       [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req);
      throw ApiError(drogon::k403Forbidden,
          "不能直接免費建立技能；請透過技能資料庫支付學習代價、職業自動取得、任務獎勵或由 DM 授予");
    });
  });

  // Public template libraries.
  // [功能備註｜職業｜GET /api/professions]
  // 用途：讀取/顯示職業相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/professions", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req);
      Json::Value out;
      out["professions"] = genericList(service->database(), "profession_templates", "name ASC");
      reply(cb, std::move(out));
    });
  });
  // [功能備註｜技能｜GET /api/skill-templates]
  // 用途：讀取/顯示技能相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/skill-templates", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req);
      Json::Value out;
      out["skills"] = genericList(service->database(), "skill_templates", "category ASC,name ASC,id ASC");
      reply(cb, std::move(out));
    });
  });
  // [功能備註｜召喚物｜GET /api/summon-templates]
  // 用途：讀取/顯示召喚物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/summon-templates", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req);
      Json::Value out;
      out["summon_templates"] = genericList(service->database(), "summon_templates");
      reply(cb, std::move(out));
    });
  });

  // More route groups are registered below.

  // Acquire profession using the same book-gated ownership model as v39.
  // [功能備註｜職業｜POST /api/character/professions/acquire]
  // 用途：建立/執行職業相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/character/professions/acquire", drogon::Post,
       [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto body = bodyOrEmpty(req);
      const auto professionId = intValue(body["profession_id"]);
      const std::string role =
          stringValue(body["role"]) == "sub" ? std::string{"sub"} : std::string{"main"};
      const auto rows = service->database()->execSqlSync(
          "SELECT row_to_json(p)::text AS json_text FROM profession_templates p WHERE id=$1",
          professionId);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到職業");
      const auto profession = rowJson(rows);
      auto character = characterRaw(service->database(), user.id);
      auto progress = character["profession_progress"].isObject()
                          ? character["profession_progress"]
                          : Json::Value{Json::objectValue};
      if (progress.isMember(std::to_string(professionId))) {
        throw ApiError(drogon::k409Conflict, "你已經擁有這個職業");
      }
      const auto category = stringValue(profession["category"], "both");
      auto subClasses = character["sub_classes"].isArray()
                            ? character["sub_classes"]
                            : Json::Value{Json::arrayValue};
      if (role == "main") {
        if (category != "main" && category != "both")
          throw ApiError(drogon::k400BadRequest, "這個職業不能作為主職業");
        const auto currentMain = stringValue(character["main_class"], "未設定");
        if (!currentMain.empty() && currentMain != "未設定")
          throw ApiError(drogon::k400BadRequest, "你已經有主職業，玩家不能自行替換職業");
      } else {
        if (category != "sub" && category != "both")
          throw ApiError(drogon::k400BadRequest, "這個職業不能作為副職業");
        if (subClasses.size() >= 3)
          throw ApiError(drogon::k400BadRequest, "副職業已滿 3 個");
      }
      const auto config = profession["config"].isObject() ? profession["config"] : Json::Value{Json::objectValue};
      const auto professionName = stringValue(profession["name"], "未命名職業");
      const auto bookName = trimmed(stringValue(config["book_name"], professionName + "職業書"));
      const auto bookQty = std::max<std::int64_t>(1, intValue(config["book_quantity"], 1));
      auto inventory = inventoryOf(character);
      if (inventoryQuantity(inventory, bookName) < bookQty) {
        throw ApiError(drogon::k400BadRequest,
                       "需要「" + bookName + "」×" + std::to_string(bookQty) + " 才能取得職業「" + professionName + "」");
      }
      inventory = changeInventory(std::move(inventory), bookName, -bookQty);
      Json::Value entry;
      entry["id"] = Json::Int64(professionId);
      entry["name"] = professionName;
      entry["role"] = role;
      entry["level"] = 1;
      entry["rank"] = stringValue(profession["rank"], "G");
      progress[std::to_string(professionId)] = entry;
      if (role == "main") {
        service->database()->execSqlSync(
            "UPDATE character_cards SET main_class=$1,profession_rank=$2,profession_progress=$3::jsonb,inventory=$4::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$5",
            professionName, stringValue(profession["rank"], "G"), compactJson(progress), compactJson(inventory), user.id);
      } else {
        subClasses.append(professionName);
        service->database()->execSqlSync(
            "UPDATE character_cards SET sub_classes=$1::jsonb,profession_progress=$2::jsonb,inventory=$3::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$4",
            compactJson(subClasses), compactJson(progress), compactJson(inventory), user.id);
      }
      Json::Value out;
      out["ok"] = true;
      out["profession"] = entry;
      out["profession"]["book_name"] = bookName;
      out["profession"]["book_quantity"] = Json::Int64(bookQty);
      out["character"] = characterRaw(service->database(), user.id);
      reply(cb, std::move(out));
    }, "取得職業失敗");
  });

  // Crafting libraries and inventory expansion.
  // [功能備註｜配方／製作｜GET /api/recipes]
  // 用途：讀取/顯示配方／製作相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/recipes", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req);
      Json::Value out;
      out["recipes"] = genericList(service->database(), "recipes");
      reply(cb, std::move(out));
    });
  });

  // [功能備註｜配方／製作｜POST /api/recipes/{1}/craft]
  // 用途：建立/執行配方／製作相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/recipes/{1}/craft", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& recipeIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto recipeId = positiveId(recipeIdText, "配方 ID 無效");
      const auto recipeRows = service->database()->execSqlSync(
          "SELECT row_to_json(r)::text AS json_text FROM recipes r WHERE id=$1", recipeId);
      if (recipeRows.empty()) throw ApiError(drogon::k404NotFound, "找不到配方");
      const auto recipe = rowJson(recipeRows);
      auto character = characterRaw(service->database(), user.id);
      auto inventory = inventoryOf(character);
      const auto materials = recipe["materials"].isArray() ? recipe["materials"] : Json::Value{Json::arrayValue};
      for (const auto& material : materials) {
        const auto name = trimmed(stringValue(material["name"]));
        const auto category = trimmed(stringValue(material["category"]));
        const auto quantity = std::max<std::int64_t>(1, intValue(material.isMember("quantity") ? material["quantity"] : material["qty"], 1));
        if (!name.empty() && inventoryQuantity(inventory, name, category) < quantity)
          throw ApiError(drogon::k400BadRequest, "材料不足：" + name + " 需要 " + std::to_string(quantity));
      }
      const auto config = recipe["config"].isObject() ? recipe["config"] : Json::Value{Json::objectValue};
      auto weirdCoins = std::max<std::int64_t>(0, intValue(character["weird_coins"]));
      auto gameCoins = std::max<std::int64_t>(0, intValue(character["game_coins"]));
      const auto currency = stringValue(config["currency_type"], "none");
      const auto cost = std::max<std::int64_t>(0, intValue(config["currency_cost"]));
      if (currency == "weird" && weirdCoins < cost) throw ApiError(drogon::k400BadRequest, "詭幣不足");
      if (currency == "game" && gameCoins < cost) throw ApiError(drogon::k400BadRequest, "遊戲幣不足");
      const auto successRate = std::clamp<std::int64_t>(intValue(config["success_rate"], 100), 0, 100);
      const auto roll = randomInt(1, 100);
      const bool success = roll <= successRate;
      const bool consume = success || !config.isMember("failure_consumes") || config["failure_consumes"].asBool();
      if (consume) {
        for (const auto& material : materials) {
          const auto name = trimmed(stringValue(material["name"]));
          const auto category = trimmed(stringValue(material["category"]));
          const auto quantity = std::max<std::int64_t>(1, intValue(material.isMember("quantity") ? material["quantity"] : material["qty"], 1));
          if (!name.empty()) inventory = changeInventory(std::move(inventory), name, -quantity, category);
        }
        if (currency == "weird") weirdCoins -= cost;
        if (currency == "game") gameCoins -= cost;
      }
      auto equipment = character["equipment"].isArray() ? character["equipment"] : Json::Value{Json::arrayValue};
      auto weapons = character["weapons"].isArray() ? character["weapons"] : Json::Value{Json::arrayValue};
      if (success) {
        const auto output = recipe["output"].isObject() ? recipe["output"] : Json::Value{Json::objectValue};
        const auto outputType = stringValue(output["type"], "item");
        const auto outputName = trimmed(stringValue(output["name"], stringValue(recipe["name"], "未命名成品")));
        const auto outputQty = std::max<std::int64_t>(1, intValue(output["quantity"], 1));
        const auto outputCategory = stringValue(output["category"]);
        if (outputType == "special_currency") {
          const auto code = trimmed(stringValue(output["currency_code"], stringValue(output["code"], outputName)));
          if (code.empty() || service->database()->execSqlSync("SELECT 1 FROM special_currency_types WHERE code=$1 AND active=TRUE", code).empty())
            throw ApiError(drogon::k400BadRequest, "合成配方指定的特殊貨幣不存在");
          service->database()->execSqlSync(
              "INSERT INTO character_special_currencies(user_id,currency_code,amount) VALUES($1,$2,$3) "
              "ON CONFLICT(user_id,currency_code) DO UPDATE SET amount=character_special_currencies.amount+EXCLUDED.amount,updated_at=CURRENT_TIMESTAMP",
              user.id, code, outputQty);
        } else if (outputType == "equipment") {
          Json::Value crafted = output; crafted["name"] = outputName; crafted["quantity"] = Json::Int64(1);
          for (std::int64_t i = 0; i < outputQty; ++i) equipment.append(crafted);
        } else if (outputType == "weapon") {
          Json::Value crafted = output; crafted["name"] = outputName; crafted["quantity"] = Json::Int64(1);
          for (std::int64_t i = 0; i < outputQty; ++i) weapons.append(crafted);
        } else if (!outputName.empty()) {
          inventory = changeInventory(std::move(inventory), outputName, outputQty, outputCategory, output);
        }
      }
      service->database()->execSqlSync(
          "UPDATE character_cards SET inventory=$1::jsonb,equipment=$2::jsonb,weapons=$3::jsonb,weird_coins=$4,game_coins=$5,updated_at=CURRENT_TIMESTAMP WHERE user_id=$6",
          compactJson(inventory), compactJson(equipment), compactJson(weapons), weirdCoins, gameCoins, user.id);
      Json::Value out;
      out["ok"] = true;
      out["success"] = success;
      out["roll"] = roll;
      out["success_rate"] = Json::Int64(successRate);
      out["failure_consumed"] = !success && consume;
      out["recipe"] = recipe;
      out["character"] = characterRaw(service->database(), user.id);
      reply(cb, std::move(out));
    }, "合成失敗");
  });

  // [功能備註｜道具／裝備／背包｜POST /api/character/inventory-slot]
  // 用途：建立/執行道具／裝備／背包相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/character/inventory-slot", drogon::Post,
       [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      auto character = characterRaw(service->database(), user.id);
      const auto body = bodyOrEmpty(req);
      const auto cost = std::max<std::int64_t>(0, intValue(body["cost"], 10));
      const auto coins = std::max<std::int64_t>(0, intValue(character["weird_coins"]));
      if (coins < cost) throw ApiError(drogon::k400BadRequest, "詭幣不足");
      const auto slots = std::max<std::int64_t>(5, intValue(character["inventory_slots"], 5)) + 1;
      service->database()->execSqlSync(
          "UPDATE character_cards SET weird_coins=$1,inventory_slots=$2,updated_at=CURRENT_TIMESTAMP WHERE user_id=$3",
          coins - cost, slots, user.id);
      Json::Value out;
      out["ok"] = true;
      out["character"] = characterRaw(service->database(), user.id);
      reply(cb, std::move(out));
    }, "擴充背包失敗");
  });

  // [功能備註｜房間／跑團｜GET /api/game-room]
  // 用途：讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/game-room", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      auto character = characterRaw(service->database(), user.id);
      Json::Value out;
      out["unlocked"] = character.get("game_room_unlocked", false);
      out["game_coins"] = character.get("game_coins", 0);
      out["character"] = character;
      reply(cb, std::move(out));
    });
  });

  // [功能備註｜房間／跑團｜POST /api/game-room/enter]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/game-room/enter", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      auto character = characterRaw(service->database(), user.id);
      if (!character.get("game_room_unlocked", false).asBool())
        throw ApiError(drogon::k403Forbidden, "遊戲房尚未解鎖");
      Json::Value out;
      out["ok"] = true;
      out["game_coins"] = character.get("game_coins", 0);
      reply(cb, std::move(out));
    });
  });

  // Shop.
  // [功能備註｜商店分類｜GET /api/shop/categories]
  // 用途：玩家讀取啟用中的商店分類；分類排序與顯示名稱由 C++ 後端統一管理。
  reg0("/api/shop/categories", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req);
      Json::Value out;
      out["categories"] = shopCategoryList(service->database(), true);
      reply(cb, std::move(out));
    }, "讀取商店分類失敗");
  });

  // [功能備註｜商店分類｜GET /api/admin/shop/categories]
  // 用途：DM 讀取全部分類（包含停用分類），方便維護分類名稱與順序。
  reg0("/api/admin/shop/categories", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["categories"] = shopCategoryList(service->database(), false);
      reply(cb, std::move(out));
    }, "讀取商店分類失敗");
  });

  // [功能備註｜商店分類｜POST /api/admin/shop/categories]
  // 用途：DM 新增分類；若名稱已存在則重新啟用並更新排序。
  reg0("/api/admin/shop/categories", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto body = bodyOrEmpty(req);
      const auto name = trimmed(stringValue(body["name"]));
      if (name.empty()) throw ApiError(drogon::k400BadRequest, "請輸入分類名稱");
      if (name.size() > 80) throw ApiError(drogon::k400BadRequest, "分類名稱最多 80 字");
      const auto sortOrder = std::clamp<std::int64_t>(intValue(body["sort_order"], 100), -9999, 9999);
      const auto rows = service->database()->execSqlSync(
          "INSERT INTO shop_categories(name,sort_order,active) VALUES($1,$2,TRUE) "
          "ON CONFLICT(name) DO UPDATE SET sort_order=EXCLUDED.sort_order,active=TRUE,updated_at=CURRENT_TIMESTAMP "
          "RETURNING row_to_json(shop_categories)::text AS json_text", name, sortOrder);
      Json::Value out;
      out["ok"] = true;
      out["category"] = rowJson(rows);
      reply(cb, std::move(out));
    }, "建立商店分類失敗");
  });

  // [功能備註｜商店分類｜PATCH /api/admin/shop/categories/{1}]
  // 用途：DM 修改分類名稱／排序／啟用狀態；改名時會同步商品的 v39 相容文字分類。
  reg1("/api/admin/shop/categories/{1}", drogon::Patch,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto categoryId = positiveId(idText, "分類 ID 無效");
      auto body = bodyOrEmpty(req);
      if (body.isMember("name")) {
        const auto name = trimmed(stringValue(body["name"]));
        if (name.empty()) throw ApiError(drogon::k400BadRequest, "分類名稱不能空白");
        if (name.size() > 80) throw ApiError(drogon::k400BadRequest, "分類名稱最多 80 字");
        body["name"] = name;
      }
      if (body.isMember("sort_order")) {
        body["sort_order"] = Json::Int64(std::clamp<std::int64_t>(intValue(body["sort_order"]), -9999, 9999));
      }
      const auto category = genericUpdate(service->database(), "shop_categories", categoryId, body);
      service->database()->execSqlSync(
          "UPDATE shop_items SET item_category=$1,updated_at=CURRENT_TIMESTAMP WHERE category_id=$2",
          stringValue(category["name"]), categoryId);
      Json::Value out;
      out["ok"] = true;
      out["category"] = category;
      reply(cb, std::move(out));
    }, "更新商店分類失敗");
  });

  // [功能備註｜商店分類｜DELETE /api/admin/shop/categories/{1}]
  // 用途：刪除分類；商品不刪除，只會回到「未分類」，避免誤刪商品。
  reg1("/api/admin/shop/categories/{1}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto categoryId = positiveId(idText, "分類 ID 無效");
      service->database()->execSqlSync(
          "UPDATE shop_items SET category_id=NULL,item_category='',updated_at=CURRENT_TIMESTAMP WHERE category_id=$1",
          categoryId);
      genericDelete(service->database(), "shop_categories", categoryId);
      Json::Value out;
      out["ok"] = true;
      reply(cb, std::move(out));
    }, "刪除商店分類失敗");
  });

  // [功能備註｜商店｜GET /api/shop]
  // 用途：讀取玩家可購買商品與分類；商品已按「分類順序 → 分類名稱 → 商品 ID」排序。
  reg0("/api/shop", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req);
      Json::Value out;
      out["categories"] = shopCategoryList(service->database(), true);
      out["items"] = shopItemList(service->database(), true);
      reply(cb, std::move(out));
    });
  });

  // [功能備註｜商店｜GET /api/admin/shop]
  // 用途：DM 讀取所有商品與全部分類，供分類管理與商品維護頁使用。
  reg0("/api/admin/shop", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["categories"] = shopCategoryList(service->database(), false);
      out["items"] = shopItemList(service->database(), false);
      reply(cb, std::move(out));
    });
  });

  // [功能備註｜商店｜POST /api/admin/shop]
  // 用途：建立商品；category_id 由 C++ 驗證並同步到舊版 item_category，避免分類資料不一致。
  reg0("/api/admin/shop", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      auto body = bodyOrEmpty(req);
      body["rank"]=normalizeContentRank(stringValue(body["rank"],"G"));
      syncShopItemCategory(service->database(), body);
      Json::Value out;
      out["ok"] = true;
      out["item"] = genericInsert(service->database(), "shop_items", body);
      reply(cb, std::move(out));
    });
  });

  // [功能備註｜商店｜PATCH /api/admin/shop/{1}]
  // 用途：修改商品；若變更分類，同樣由 C++ 同步 category_id 與 item_category。
  reg1("/api/admin/shop/{1}", drogon::Patch,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      auto body = bodyOrEmpty(req);
      body["rank"]=normalizeContentRank(stringValue(body["rank"],"G"));
      syncShopItemCategory(service->database(), body);
      Json::Value out;
      out["ok"] = true;
      out["item"] = genericUpdate(service->database(), "shop_items", positiveId(idText, "商品 ID 無效"), body);
      reply(cb, std::move(out));
    }, "更新商品失敗");
  });

  // [功能備註｜商店｜DELETE /api/admin/shop/{1}]
  // 用途：DM 刪除商店商品。
  reg1("/api/admin/shop/{1}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      genericDelete(service->database(), "shop_items", positiveId(idText, "商品 ID 無效"));
      Json::Value out;
      out["ok"] = true;
      reply(cb, std::move(out));
    }, "刪除商品失敗");
  });

  // [功能備註｜商店｜POST /api/shop/{1}/buy]
  // 用途：購買商品。分類只影響搜尋／整理，不改變購買與背包規則。
  reg1("/api/shop/{1}/buy", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& itemIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto itemId = positiveId(itemIdText, "商品 ID 無效");
      const auto rows = service->database()->execSqlSync(
          "SELECT row_to_json(s)::text AS json_text FROM shop_items s WHERE id=$1 AND active=TRUE", itemId);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到商品");
      const auto item = rowJson(rows);
      const auto buyBody = bodyOrEmpty(req);
      const auto ruleRoomId = intValue(buyBody["room_id"]);
      if (ruleRoomId > 0) {
        requireRoomAccess(service, user, ruleRoomId);
        Json::Value rulePayload; rulePayload["shop_item_id"] = Json::Int64(itemId); rulePayload["item"] = item; rulePayload["item_name"] = item["name"];
        auto purchaseAttempt = evaluateRuleEvent(service, ruleRoomId, user.id, "SHOP_PURCHASE_ATTEMPT", rulePayload, false);
        if (purchaseAttempt["blocked"].asBool()) throw ApiError(drogon::k403Forbidden, "此購買行為被規則阻止");
      }
      auto character = characterRaw(service->database(), user.id);
      auto coins = std::max<std::int64_t>(0, intValue(character["weird_coins"]));
      // [功能備註｜商店進階] C++ 統一處理折扣、全站庫存與玩家限購。
      const auto basePrice = std::max<std::int64_t>(0, intValue(item["price_weird"]));
      const auto discount = std::clamp(item.get("discount_percent",0).asDouble(), 0.0, 100.0);
      const auto price = static_cast<std::int64_t>(std::ceil(basePrice * (1.0 - discount / 100.0)));
      const auto purchaseLimit = intValue(item["purchase_limit"], -1);
      if (purchaseLimit >= 0) {
        const auto bought = service->database()->execSqlSync(
            "SELECT COALESCE(SUM(quantity),0)::BIGINT AS n FROM shop_transactions WHERE user_id=$1 AND shop_item_id=$2 AND direction='buy'", user.id, itemId);
        const auto count = bought.empty()?0:bought[0]["n"].as<std::int64_t>();
        if (count >= purchaseLimit) throw ApiError(drogon::k400BadRequest, "已達此商品限購數量");
      }
      const auto stock = intValue(item["stock"], -1);
      if (stock == 0) throw ApiError(drogon::k400BadRequest, "商品已售罄");
      if (coins < price) throw ApiError(drogon::k400BadRequest, "詭幣不足");
      if (stock > 0) {
        const auto dec = service->database()->execSqlSync("UPDATE shop_items SET stock=stock-1,updated_at=CURRENT_TIMESTAMP WHERE id=$1 AND stock>0 RETURNING stock",itemId);
        if (dec.empty()) throw ApiError(drogon::k400BadRequest,"商品已售罄");
      }
      auto inventory = inventoryOf(character);
      const auto itemName = stringValue(item["name"], "未命名商品");
      const auto itemType = stringValue(item["item_type"], "item");
      const auto qty = std::max<std::int64_t>(1, intValue(item["quantity"], 1));
      auto equipment = character["equipment"].isArray() ? character["equipment"] : Json::Value{Json::arrayValue};
      auto weapons = character["weapons"].isArray() ? character["weapons"] : Json::Value{Json::arrayValue};
      if (itemType == "equipment") {
        for (std::int64_t i = 0; i < qty; ++i) equipment.append(itemName);
      } else if (itemType == "weapon") {
        for (std::int64_t i = 0; i < qty; ++i) weapons.append(itemName);
      } else {
        inventory = changeInventory(std::move(inventory), itemName, qty, itemType, item);
      }
      service->database()->execSqlSync(
          "UPDATE character_cards SET weird_coins=$1,inventory=$2::jsonb,equipment=$3::jsonb,weapons=$4::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$5",
          coins - price, compactJson(inventory), compactJson(equipment), compactJson(weapons), user.id);
      service->database()->execSqlSync(
          "INSERT INTO shop_transactions(user_id,shop_item_id,direction,item_name,quantity,unit_price,total_price,meta) VALUES($1,$2,'buy',$3,$4,$5,$6,$7::jsonb)",
          user.id,itemId,itemName,qty,price,price,compactJson(Json::Value{Json::objectValue}));
      if (ruleRoomId > 0) { Json::Value rulePayload; rulePayload["shop_item_id"] = Json::Int64(itemId); rulePayload["item"] = item; rulePayload["item_name"] = itemName; rulePayload["price"] = Json::Int64(price); evaluateRuleEvent(service, ruleRoomId, user.id, "SHOP_PURCHASED", rulePayload, false); }
      Json::Value out;
      out["ok"] = true;
      out["item"] = item;
      out["character"] = characterRaw(service->database(), user.id);
      reply(cb, std::move(out));
    }, "購買失敗");
  });

  // Learn/remove/use character skills.
  // [功能備註｜技能｜POST /api/character/skills/from-template]
  // 用途：建立/執行技能相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/character/skills/from-template", drogon::Post,
       [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto body = bodyOrEmpty(req);
      const auto templateId = intValue(body["template_id"]);
      const auto rows = service->database()->execSqlSync(
          "SELECT row_to_json(s)::text AS json_text FROM skill_templates s WHERE id=$1", templateId);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到技能模板");
      const auto templ = rowJson(rows);
      requireKnowledgeUnlockIfSold(service->database(),user.id,"skill",templateId);
      auto character = characterRaw(service->database(), user.id);
      auto skills = character["skills"].isArray() ? character["skills"] : Json::Value{Json::arrayValue};
      for (const auto& skill : skills) {
        if (intValue(skill["template_id"]) == templateId)
          throw ApiError(drogon::k409Conflict, "你已經擁有這個技能");
      }
      // [功能備註｜技能樹] 學習前由 C++ 驗證 prerequisite_skill_ids，避免只靠前端限制。
      if (templ["prerequisite_skill_ids"].isArray()) {
        for (const auto& required : templ["prerequisite_skill_ids"]) {
          const auto requiredId = intValue(required);
          if (requiredId <= 0) continue;
          bool owned = false;
          for (const auto& skill : skills) if (intValue(skill["template_id"]) == requiredId) { owned = true; break; }
          if (!owned) throw ApiError(drogon::k403Forbidden, "尚未取得技能樹前置技能 #" + std::to_string(requiredId));
        }
      }
      const auto data = templ["data"].isObject() ? templ["data"] : Json::Value{Json::objectValue};
      const auto cost = std::max<std::int64_t>(0, intValue(data["learn_skill_points"], intValue(data["skill_point_cost"], 1)));
      auto points = std::max<std::int64_t>(0, intValue(character["skill_points"]));
      if (points < cost) throw ApiError(drogon::k400BadRequest, "技能點不足");
      Json::Value skill;
      skill["id"] = "tpl_" + std::to_string(templateId);
      skill["template_id"] = Json::Int64(templateId);
      skill["name"] = templ["name"];
      skill["category"] = templ["category"];
      skill["rank"] = normalizeContentRank(stringValue(templ["rank"],"G"));
      skill["type"] = templ["skill_type"];
      skill["level"] = templ["level"];
      skill["description"] = templ["description"];
      skill["check"] = templ["check_name"];
      skill["damage_formula"] = templ["damage_formula"];
      skill["extra_tag"] = templ["extra_tag"];
      skill["summon_tag"] = templ["summon_tag"];
      skill["resources"] = templ["resources"];
      skill["data"] = templ["data"];
      skills.append(skill);
      service->database()->execSqlSync(
          "UPDATE character_cards SET skills=$1::jsonb,skill_points=$2,updated_at=CURRENT_TIMESTAMP WHERE user_id=$3",
          compactJson(skills), points - cost, user.id);
      Json::Value out;
      out["ok"] = true;
      out["skill"] = skill;
      out["character"] = characterRaw(service->database(), user.id);
      reply(cb, std::move(out));
    }, "學習技能失敗");
  });

  // [功能備註｜技能｜DELETE /api/character/skills/{1}]
  // 用途：刪除技能相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/character/skills/{1}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& skillId) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      auto character = characterRaw(service->database(), user.id);
      auto skills = character["skills"].isArray() ? character["skills"] : Json::Value{Json::arrayValue};
      Json::Value next{Json::arrayValue};
      bool removed = false;
      for (const auto& skill : skills) {
        if (stringValue(skill["id"]) == skillId || std::to_string(intValue(skill["template_id"], -1)) == skillId) {
          removed = true;
          continue;
        }
        next.append(skill);
      }
      if (!removed) throw ApiError(drogon::k404NotFound, "找不到技能");
      service->database()->execSqlSync(
          "UPDATE character_cards SET skills=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",
          compactJson(next), user.id);
      Json::Value out;
      out["ok"] = true;
      out["character"] = characterRaw(service->database(), user.id);
      reply(cb, std::move(out));
    }, "移除技能失敗");
  });

  // [功能備註｜角色卡｜PATCH /api/character/class-resource]
  // 用途：修改角色卡相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/character/class-resource", drogon::Patch,
       [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto body = bodyOrEmpty(req);
      auto character = characterRaw(service->database(), user.id);
      auto resources = character["class_resources"].isObject()
                           ? character["class_resources"]
                           : Json::Value{Json::objectValue};
      const auto name = trimmed(stringValue(body["name"], stringValue(body["key"])));
      if (name.empty()) throw ApiError(drogon::k400BadRequest, "資源名稱不能為空");
      auto current = intValue(resources[name]);
      if (body.isMember("value")) current = intValue(body["value"]);
      if (body.isMember("delta")) current += intValue(body["delta"]);
      current = std::max<std::int64_t>(0, current);
      resources[name] = Json::Int64(current);
      service->database()->execSqlSync(
          "UPDATE character_cards SET class_resources=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",
          compactJson(resources), user.id);
      Json::Value out;
      out["ok"] = true;
      out["class_resources"] = resources;
      out["character"] = characterRaw(service->database(), user.id);
      reply(cb, std::move(out));
    }, "更新職業資源失敗");
  });

  // [功能備註｜技能｜POST /api/character/skills/{1}/use]
  // 用途：建立/執行技能相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/character/skills/{1}/use", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& skillId) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      auto character = characterRaw(service->database(), user.id);
      const auto skills = character["skills"].isArray() ? character["skills"] : Json::Value{Json::arrayValue};
      Json::Value selected;
      for (const auto& skill : skills) {
        if (stringValue(skill["id"]) == skillId || std::to_string(intValue(skill["template_id"], -1)) == skillId) {
          selected = skill;
          break;
        }
      }
      if (selected.isNull()) throw ApiError(drogon::k404NotFound, "找不到技能");

      // [功能備註｜62-cpp.23 技能模板即時同步]
      // 角色卡中的技能是取得當下的快照；事件觸發器／多段設定則應沿用目前模板，
      // 因此使用技能時以 template_id 重新補齊最新的戰鬥資料，避免已學技能吃不到新規則。
      const auto templateId = intValue(selected["template_id"]);
      if (templateId > 0) {
        const auto templateRows = service->database()->execSqlSync(
            "SELECT row_to_json(s)::text AS json_text FROM skill_templates s WHERE id=$1", templateId);
        if (!templateRows.empty()) {
          const auto learnedId = selected["id"];
          const auto learnedLevel = selected["level"];
          auto latest = rowJson(templateRows);
          latest["id"] = learnedId;
          latest["template_id"] = Json::Int64(templateId);
          if (!learnedLevel.isNull()) latest["level"] = learnedLevel;
          selected = std::move(latest);
        }
      }

      const auto body = bodyOrEmpty(req);
      const auto roomId = intValue(body["room_id"]);
      Json::Value result;
      result["ok"] = true;
      result["skill"] = selected;
      result["charging"] = false;
      if (roomId > 0) {
        requireRoomAccess(service, user, roomId);
        const auto battleRows = service->database()->execSqlSync("SELECT battle_active,round FROM rooms WHERE id=$1", roomId);
        const bool battleActive = !battleRows.empty() && battleRows[0]["battle_active"].as<bool>();
        const auto skillData = selected["data"].isObject() ? selected["data"] : Json::Value{Json::objectValue};
        const bool hasEventTriggers = skillData["event_triggers"].isArray() && !skillData["event_triggers"].empty();
        const bool configuredCombatSkill = skillData.get("combat_attack", false).asBool() ||
                                           !stringValue(selected["damage_formula"]).empty() ||
                                           skillData["hit_sequence"].isArray() ||
                                           intValue(skillData["hit_count"]) > 0;

        // [功能備註｜62-cpp.23 多段攻擊／事件型技能執行]
        // 戰鬥中的資料驅動技能交由 C++ 共用執行器處理；每段 Hit 會獨立命中與觸發，
        // 整個技能只消耗一次行動。被動事件技能則由戰鬥事件自動觸發，不需要手動按使用。
        if (battleActive && configuredCombatSkill && stringValue(selected["skill_type"], "active") != "passive") {
          result["combat_result"] = executeConfiguredCombatSkill(
              service, roomId, "player", user.id, selected,
              stringValue(body["target_type"]), intValue(body["target_id"]), user.id);
        } else {
          Json::Value payload;
          payload["text"] = stringValue(character["name"], user.username) + " 使用技能「" + stringValue(selected["name"], skillId) + "」";
          payload["skill_id"] = skillId;
          service->addEvent(roomId, user.id, "skill", payload);
          if (battleActive) {
            Json::Value combatSkillEvent = payload;
            combatSkillEvent["skill"] = selected;
            combatSkillEvent["round"] = Json::Int64(battleRows[0]["round"].as<std::int64_t>());
            if (hasEventTriggers) {
              combatSkillEvent["actor"]["type"] = "player";
              combatSkillEvent["actor"]["id"] = Json::Int64(user.id);
              combatSkillEvent["attacker"] = combatSkillEvent["actor"];
              if (!stringValue(body["target_type"]).empty() && intValue(body["target_id"]) > 0) {
                combatSkillEvent["target"]["type"] = stringValue(body["target_type"]);
                combatSkillEvent["target"]["id"] = Json::Int64(intValue(body["target_id"]));
              }
              result["event_skill_result"] = dispatchEventSkillEvent(
                  service, roomId, "player", user.id, "COMBAT_SKILL_USED", combatSkillEvent);
            }
            evaluateRuleEvent(service, roomId, user.id, "COMBAT_SKILL_USED", combatSkillEvent, false);
            evaluateRuleCombatVictory(service, roomId, user.id, "COMBAT_SKILL_USED");
          }
        }
        result["room"] = roomSnapshotForViewer(service, roomId, user.id);
      }
      result["character"] = characterRaw(service->database(), user.id);
      reply(cb, std::move(result));
    }, "使用技能失敗");
  });

  // Summons.
  // [功能備註｜召喚物｜GET /api/character/summons]
  // 用途：讀取/顯示召喚物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/character/summons", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      Json::Value out;
      out["summons"] = selectRows1(service->database(),
          "SELECT row_to_json(s)::text AS json_text FROM summon_instances s WHERE owner_user_id=$1 AND status='active' ORDER BY id ASC",
          user.id);
      reply(cb, std::move(out));
    });
  });

  // [功能備註｜技能｜POST /api/character/skills/{1}/summon]
  // 用途：建立/執行技能相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/character/skills/{1}/summon", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& skillId) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto body = bodyOrEmpty(req);
      auto character = characterRaw(service->database(), user.id);
      const auto skills = character["skills"].isArray() ? character["skills"] : Json::Value{Json::arrayValue};
      Json::Value skill;
      for (const auto& candidate : skills) {
        if (stringValue(candidate["id"]) == skillId || std::to_string(intValue(candidate["template_id"], -1)) == skillId) {
          skill = candidate;
          break;
        }
      }
      if (skill.isNull()) throw ApiError(drogon::k404NotFound, "找不到技能");
      auto ids = skill["data"]["summon_template_ids"].isArray() ? skill["data"]["summon_template_ids"] : Json::Value{Json::arrayValue};
      if (ids.empty() && skill["data"].isMember("summon_template_id")) ids.append(skill["data"]["summon_template_id"]);
      auto templateId = intValue(body["template_id"]);
      if (templateId <= 0 && ids.size() == 1) templateId = intValue(ids[Json::ArrayIndex{0}]);
      if (templateId <= 0) throw ApiError(drogon::k400BadRequest, "請選擇要召喚的召喚物");
      if (!ids.empty()) {
        bool allowed = false;
        for (const auto& id : ids) if (intValue(id) == templateId) allowed = true;
        if (!allowed) throw ApiError(drogon::k403Forbidden, "這個技能不能召喚該召喚物");
      }
      const auto templates = service->database()->execSqlSync(
          "SELECT row_to_json(s)::text AS json_text FROM summon_templates s WHERE id=$1", templateId);
      if (templates.empty()) throw ApiError(drogon::k404NotFound, "召喚物模板已不存在");
      const auto templ = rowJson(templates);
      const auto roomId = intValue(body["room_id"]);
      if (roomId > 0) {
        requireRoomAccess(service, user, roomId);
        requireRoomExists(service->database(), roomId);
      }
      const auto inserted = service->database()->execSqlSync(
          "INSERT INTO summon_instances(template_id,owner_user_id,name,status,overrides,current_state,room_id,source_skill_id) "
          "VALUES($1,$2,$3,'active','{}'::jsonb,$4::jsonb,NULLIF($5,0),$6) RETURNING row_to_json(summon_instances)::text AS json_text",
          templateId, user.id, stringValue(templ["name"], "召喚物"), compactJson(templ),
          roomId, skillId);
      Json::Value summon = rowJson(inserted);
      if (roomId > 0) {
        Json::Value payload;
        payload["text"] = "召喚物「" + stringValue(summon["name"], "召喚物") + "」加入戰場";
        payload["summon_id"] = summon["id"];
        service->addEvent(roomId, user.id, "summon", payload);
      }
      Json::Value out;
      out["ok"] = true;
      out["charging"] = false;
      out["summon"] = summon;
      out["summons"] = selectRows1(service->database(),
          "SELECT row_to_json(s)::text AS json_text FROM summon_instances s WHERE owner_user_id=$1 AND status='active' ORDER BY id ASC",
          user.id);
      if (roomId > 0) out["room"] = service->roomSnapshot(roomId);
      reply(cb, std::move(out));
    }, "召喚失敗");
  });

  // [功能備註｜技能｜POST /api/character/summons/{1}/skills/{2}/use]
  // 用途：建立/執行技能相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/character/summons/{1}/skills/{2}/use", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& summonIdText, const std::string& skillTemplateIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto summonId = positiveId(summonIdText, "召喚物 ID 無效");
      const auto skillTemplateId = positiveId(skillTemplateIdText, "技能模板 ID 無效");
      const auto rows = service->database()->execSqlSync(
          "SELECT row_to_json(s)::text AS json_text FROM summon_instances s WHERE id=$1 AND owner_user_id=$2 AND status='active'",
          summonId, user.id);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到你的召喚物");
      auto summon = rowJson(rows);
      const auto skillRows = service->database()->execSqlSync(
          "SELECT row_to_json(s)::text AS json_text FROM skill_templates s WHERE id=$1", skillTemplateId);
      if (skillRows.empty()) throw ApiError(drogon::k404NotFound, "召喚物技能模板已不存在");
      const auto roomId = intValue(summon["room_id"]);
      if (roomId > 0) {
        requireRoomAccess(service, user, roomId);
        Json::Value payload;
        payload["text"] = "召喚物「" + stringValue(summon["name"], "召喚物") + "」使用技能「" + stringValue(rowJson(skillRows)["name"], "技能") + "」";
        payload["summon_id"] = Json::Int64(summonId);
        payload["skill_template_id"] = Json::Int64(skillTemplateId);
        service->addEvent(roomId, user.id, "summon_skill", payload);
      }
      Json::Value out;
      out["ok"] = true;
      out["charging"] = false;
      if (roomId > 0) out["room"] = service->roomSnapshot(roomId);
      reply(cb, std::move(out));
    }, "召喚物使用技能失敗");
  });

  // [功能備註｜召喚物｜POST /api/character/summons/{1}/dismiss]
  // 用途：建立/執行召喚物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/character/summons/{1}/dismiss", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& summonIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto summonId = positiveId(summonIdText, "召喚物 ID 無效");
      const auto rows = service->database()->execSqlSync(
          "UPDATE summon_instances SET status='dismissed',updated_at=CURRENT_TIMESTAMP WHERE id=$1 AND owner_user_id=$2 RETURNING row_to_json(summon_instances)::text AS json_text",
          summonId, user.id);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到召喚物");
      const auto summon = rowJson(rows);
      const auto roomId = intValue(summon["room_id"]);
      if (roomId > 0) {
        Json::Value payload;
        payload["text"] = "召喚物「" + stringValue(summon["name"], "召喚物") + "」已解除召喚";
        payload["summon_id"] = Json::Int64(summonId);
        service->addEvent(roomId, user.id, "summon", payload);
      }
      Json::Value out;
      out["ok"] = true;
      out["summons"] = selectRows1(service->database(),
          "SELECT row_to_json(s)::text AS json_text FROM summon_instances s WHERE owner_user_id=$1 AND status='active' ORDER BY id ASC",
          user.id);
      if (roomId > 0) out["room"] = service->roomSnapshot(roomId);
      reply(cb, std::move(out));
    }, "解除召喚失敗");
  });

  // Simplified gear use keeps durability/charges/inventory consumption persistent.
  // [功能備註｜道具／裝備／背包｜POST /api/character/gear/{1}/{2}/use]
  // 用途：建立/執行道具／裝備／背包相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/character/gear/{1}/{2}/use", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& kindText, const std::string& indexText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const std::string field = kindText == "equipment" ? "equipment" : kindText == "weapon" ? "weapons" : "";
      if (field.empty()) throw ApiError(drogon::k400BadRequest, "裝備類型錯誤");
      std::int64_t index = 0;
      try { index = std::max<std::int64_t>(0, std::stoll(indexText)); } catch (...) { index = 0; }
      auto character = characterRaw(service->database(), user.id);
      auto list = character[field].isArray() ? character[field] : Json::Value{Json::arrayValue};
      if (index >= static_cast<std::int64_t>(list.size())) throw ApiError(drogon::k404NotFound, "找不到這件裝備／武器");
      auto gear = list[static_cast<Json::ArrayIndex>(index)];
      if (gear.isString()) {
        Json::Value converted;
        converted["name"] = gear.asString();
        converted["equipped"] = true;
        gear = std::move(converted);
      }
      if (!gear.isObject()) throw ApiError(drogon::k400BadRequest, "裝備資料格式錯誤");
      auto inventory = inventoryOf(character);
      const auto consumeName = trimmed(stringValue(gear["consume_item_name"]));
      const auto consumeCategory = trimmed(stringValue(gear["consume_item_category"]));
      const auto consumeQty = std::max<std::int64_t>(0, intValue(gear["consume_quantity"]));
      if (!consumeName.empty() && consumeQty > 0) {
        if (inventoryQuantity(inventory, consumeName, consumeCategory) < consumeQty)
          throw ApiError(drogon::k400BadRequest, stringValue(gear["name"], "裝備") + " 使用失敗：" + consumeName + " 不足");
        inventory = changeInventory(std::move(inventory), consumeName, -consumeQty, consumeCategory);
      }
      if (intValue(gear["charges_max"]) > 0) {
        const auto cost = std::max<std::int64_t>(1, intValue(gear["charge_cost"], 1));
        const auto current = intValue(gear["charges_current"]);
        if (current < cost) throw ApiError(drogon::k400BadRequest, "裝備充能不足");
        gear["charges_current"] = Json::Int64(current - cost);
      }
      if (intValue(gear["durability_max"]) > 0) {
        const auto cost = std::max<std::int64_t>(1, intValue(gear["durability_cost"], 1));
        const auto current = intValue(gear["durability_current"]);
        if (current < cost) throw ApiError(drogon::k400BadRequest, "裝備耐久不足");
        gear["durability_current"] = Json::Int64(current - cost);
      }
      list[static_cast<Json::ArrayIndex>(index)] = gear;
      const auto sql = "UPDATE character_cards SET " + field + "=$1::jsonb,inventory=$2::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$3";
      service->database()->execSqlSync(sql, compactJson(list), compactJson(inventory), user.id);
      Json::Value out;
      out["ok"] = true;
      out["gear"] = gear;
      out["character"] = characterRaw(service->database(), user.id);
      reply(cb, std::move(out));
    }, "使用裝備失敗");
  });

  // Global DM libraries.
  // [功能備註｜其他系統｜GET /api/admin/extra-skills]
  // 用途：讀取/顯示其他系統相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/extra-skills", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["extra_skills"] = genericList(service->database(), "extra_skills");
      reply(cb, std::move(out));
    });
  });
  // [功能備註｜其他系統｜POST /api/admin/extra-skills]
  // 用途：建立/執行其他系統相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/extra-skills", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["ok"] = true;
      out["extra_skill"] = genericInsert(service->database(), "extra_skills", bodyOrEmpty(req));
      reply(cb, std::move(out));
    });
  });

  // [功能備註｜召喚物｜GET/POST /api/admin/summon-templates]
  // 用途：召喚物的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminCrud0(service, "/api/admin/summon-templates", "summon_templates", "summon_templates");
  // [功能備註｜召喚物｜PATCH/DELETE /api/admin/summon-templates/{1}]
  // 用途：召喚物的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminPatchDelete1(service, "/api/admin/summon-templates/{1}", "summon_templates", "summon_template");

  // [功能備註｜配方／製作｜GET /api/admin/recipes]
  // 用途：讀取/顯示配方／製作相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/recipes", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["recipes"] = genericList(service->database(), "recipes");
      reply(cb, std::move(out));
    });
  });
  // [功能備註｜配方／製作｜POST /api/admin/recipes]
  // 用途：建立/執行配方／製作相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/recipes", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["ok"] = true;
      auto body=bodyOrEmpty(req);body["rank"]=normalizeContentRank(stringValue(body["rank"],"G"));out["recipe"] = genericInsert(service->database(), "recipes", body);
      reply(cb, std::move(out));
    });
  });
  // [功能備註｜配方／製作｜DELETE /api/admin/recipes/{1}]
  // 用途：刪除配方／製作相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/recipes/{1}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      genericDelete(service->database(), "recipes", positiveId(idText));
      Json::Value out; out["ok"] = true; reply(cb, std::move(out));
    });
  });

  // [功能備註｜血脈｜GET /api/bloodlines]
  // 用途：讀取/顯示血脈相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/bloodlines", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req);
      Json::Value out;
      out["bloodlines"] = genericList(service->database(), "bloodline_templates", "name ASC");
      reply(cb, std::move(out));
    });
  });
  // [功能備註｜血脈｜GET/POST /api/admin/bloodlines]
  // 用途：血脈的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminCrud0(service, "/api/admin/bloodlines", "bloodline_templates", "bloodlines");
  // [功能備註｜血脈｜PATCH/DELETE /api/admin/bloodlines/{1}]
  // 用途：血脈的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminPatchDelete1(service, "/api/admin/bloodlines/{1}", "bloodline_templates", "bloodline");

  // [功能備註｜職業｜POST /api/admin/professions]
  // 用途：建立/執行職業相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/professions", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["ok"] = true;
      out["profession"] = genericInsert(service->database(), "profession_templates", bodyOrEmpty(req));
      reply(cb, std::move(out));
    });
  });
  // [功能備註｜職業｜PATCH/DELETE /api/admin/professions/{1}]
  // 用途：職業的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminPatchDelete1(service, "/api/admin/professions/{1}", "profession_templates", "profession");

  // [功能備註｜技能｜POST /api/admin/skill-templates]
  // 用途：建立/執行技能相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/skill-templates", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["ok"] = true;
      auto body=bodyOrEmpty(req);body["rank"]=normalizeContentRank(stringValue(body["rank"],"G"));out["skill"] = genericInsert(service->database(), "skill_templates", body);
      reply(cb, std::move(out));
    });
  });
  // [功能備註｜技能｜PATCH/DELETE /api/admin/skill-templates/{1}]
  // 用途：技能的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminPatchDelete1(service, "/api/admin/skill-templates/{1}", "skill_templates", "skill");

  // Profession level-up. Level and rank live inside profession_progress JSONB.
  // [功能備註｜玩家管理｜POST /api/admin/players/{1}/professions/{2}/level-up]
  // 用途：建立/執行玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/players/{1}/professions/{2}/level-up", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& userIdText, const std::string& professionIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto userId = positiveId(userIdText, "玩家 ID 無效");
      const auto professionId = positiveId(professionIdText, "職業 ID 無效");
      auto character = characterRaw(service->database(), userId);
      auto progress = character["profession_progress"].isObject() ? character["profession_progress"] : Json::Value{Json::objectValue};
      auto key = std::to_string(professionId);
      if (!progress.isMember(key)) throw ApiError(drogon::k404NotFound, "玩家尚未取得此職業");
      auto entry = progress[key].isObject() ? progress[key] : Json::Value{Json::objectValue};
      entry["level"] = Json::Int64(std::max<std::int64_t>(1, intValue(entry["level"], 1) + 1));
      progress[key] = entry;
      service->database()->execSqlSync(
          "UPDATE character_cards SET profession_progress=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",
          compactJson(progress), userId);
      Json::Value out;
      out["ok"] = true;
      out["profession"] = entry;
      out["character"] = characterRaw(service->database(), userId);
      reply(cb, std::move(out));
    }, "職業升級失敗");
  });

  // [功能備註｜64-cpp.25 角色小級／0→1 晉升]
  // 1～4 級的小級由 DM 推進；0階可直接升1階。已到4級後跨大階必須走登階／登神儀式。
  reg1("/api/admin/players/{1}/character-rank/advance", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& userIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto userId=positiveId(userIdText,"玩家 ID 無效");
      const auto rows=service->database()->execSqlSync("SELECT tier,level FROM character_cards WHERE user_id=$1",userId);
      if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到角色卡");
      const auto tier=std::clamp<std::int64_t>(rows[0]["tier"].as<std::int64_t>(),0,10);
      const auto level=std::clamp<std::int64_t>(rows[0]["level"].as<std::int64_t>(),1,4);
      Json::Value progression;
      if(tier==0){
        progression=advanceCharacterTier(service->database(),userId,1,"dm:0_to_1",false);
      }else if(level<4){
        const auto next=level+1;
        service->database()->execSqlSync("UPDATE character_cards SET level=$1,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",next,userId);
        progression["from_tier"]=Json::Int64(tier);progression["from_level"]=Json::Int64(level);
        progression["to_tier"]=Json::Int64(tier);progression["to_level"]=Json::Int64(next);
        progression["title"]=characterTierTitle(tier,next);progression["source"]="dm:minor_level";
      }else if(tier>=10){
        throw ApiError(drogon::k409Conflict,"角色已達10階4級或需由 DM 校正，目前沒有更高大階");
      }else{
        throw ApiError(drogon::k409Conflict,(tier+1)>=9?"已達本階4級，請完成登神儀式":"已達本階4級，請完成登階儀式");
      }
      Json::Value out;out["ok"]=true;out["progression"]=progression;out["character"]=characterRaw(service->database(),userId);reply(cb,std::move(out));
    },"角色晉升失敗");
  });

  // Global DM identity and character/player administration.
  // [功能備註｜DM 權限｜GET /api/admin/me]
  // 用途：讀取/顯示DM 權限相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/me", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req, true);
      Json::Value out;
      out["user"]["id"] = Json::Int64(user.id);
      out["user"]["username"] = user.username;
      out["user"]["is_admin"] = true;
      reply(cb, std::move(out));
    });
  });

  // [功能備註｜角色卡｜GET /api/admin/characters/{1}]
  // 用途：讀取/顯示角色卡相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/characters/{1}", drogon::Get,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto id = positiveId(idText, "角色 ID 無效");
      const auto rows = service->database()->execSqlSync(
          "SELECT row_to_json(x)::text AS json_text FROM (SELECT c.*,u.username FROM character_cards c JOIN users u ON u.id=c.user_id WHERE c.id=$1) x", id);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到角色卡");
      Json::Value out; out["character"] = rowJson(rows); reply(cb, std::move(out));
    });
  });

  // [功能備註｜角色卡｜PATCH /api/admin/characters/{1}]
  // 用途：修改角色卡相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/characters/{1}", drogon::Patch,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["ok"] = true;
      out["character"] = genericUpdate(service->database(), "character_cards", positiveId(idText, "角色 ID 無效"), bodyOrEmpty(req));
      reply(cb, std::move(out));
    }, "更新角色卡失敗");
  });

  // [功能備註｜角色卡｜DELETE /api/admin/characters/{1}]
  // 用途：刪除角色卡相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/characters/{1}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      genericDelete(service->database(), "character_cards", positiveId(idText, "角色 ID 無效"));
      Json::Value out; out["ok"] = true; reply(cb, std::move(out));
    }, "刪除角色卡失敗");
  });

  // [功能備註｜玩家管理｜GET /api/admin/players]
  // 用途：讀取/顯示玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/players", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["players"] = selectRows(service->database(),
          "SELECT row_to_json(x)::text AS json_text FROM (SELECT u.id,u.username,u.is_admin,c.id AS character_id,c.name,c.faction,c.tier,c.level,c.main_class FROM users u LEFT JOIN character_cards c ON c.user_id=u.id ORDER BY u.id) x");
      reply(cb, std::move(out));
    });
  });

  // [功能備註｜玩家管理｜GET /api/admin/players/{1}]
  // 用途：讀取/顯示玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/players/{1}", drogon::Get,
       [service](const Request& req, Callback& cb, const std::string& userIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto userId = positiveId(userIdText, "玩家 ID 無效");
      const auto users = service->database()->execSqlSync(
          "SELECT row_to_json(u)::text AS json_text FROM users u WHERE id=$1", userId);
      if (users.empty()) throw ApiError(drogon::k404NotFound, "找不到玩家");
      Json::Value out;
      out["user"] = rowJson(users);
      const auto cards = service->database()->execSqlSync(
          "SELECT row_to_json(c)::text AS json_text FROM character_cards c WHERE user_id=$1", userId);
      out["character"] = cards.empty() ? Json::Value{} : rowJson(cards);
      reply(cb, std::move(out));
    });
  });

  // [功能備註｜玩家管理｜PATCH /api/admin/players/{1}/character]
  // 用途：修改玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/players/{1}/character", drogon::Patch,
       [service](const Request& req, Callback& cb, const std::string& userIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto userId = positiveId(userIdText, "玩家 ID 無效");
      ensureCharacter(service->database(), userId);
      const auto cards = service->database()->execSqlSync("SELECT id FROM character_cards WHERE user_id=$1", userId);
      Json::Value out;
      out["ok"] = true;
      out["character"] = genericUpdate(service->database(), "character_cards", cards[0]["id"].as<std::int64_t>(), bodyOrEmpty(req));
      reply(cb, std::move(out));
    }, "更新玩家角色失敗");
  });

  // [功能備註｜玩家管理｜PATCH /api/admin/players/{1}/class-resource]
  // 用途：修改玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/players/{1}/class-resource", drogon::Patch,
       [service](const Request& req, Callback& cb, const std::string& userIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto userId = positiveId(userIdText, "玩家 ID 無效");
      auto character = characterRaw(service->database(), userId);
      const auto body = bodyOrEmpty(req);
      auto resources = character["class_resources"].isObject() ? character["class_resources"] : Json::Value{Json::objectValue};
      const auto name = trimmed(stringValue(body["name"], stringValue(body["key"])));
      if (name.empty()) throw ApiError(drogon::k400BadRequest, "資源名稱不能為空");
      auto value = intValue(resources[name]);
      if (body.isMember("value")) value = intValue(body["value"]);
      if (body.isMember("delta")) value += intValue(body["delta"]);
      resources[name] = Json::Int64(std::max<std::int64_t>(0, value));
      service->database()->execSqlSync(
          "UPDATE character_cards SET class_resources=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",
          compactJson(resources), userId);
      Json::Value out; out["ok"] = true; out["character"] = characterRaw(service->database(), userId); reply(cb, std::move(out));
    }, "DM 更新職業資源失敗");
  });

  // [功能備註｜玩家管理｜DELETE /api/admin/players/{1}/character]
  // 用途：刪除玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/players/{1}/character", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& userIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto userId = positiveId(userIdText, "玩家 ID 無效");
      service->database()->execSqlSync("DELETE FROM character_cards WHERE user_id=$1", userId);
      Json::Value out; out["ok"] = true; reply(cb, std::move(out));
    }, "刪除玩家角色失敗");
  });

  // [功能備註｜房間／跑團｜GET /api/admin/rooms/{1}]
  // 用途：讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/rooms/{1}", drogon::Get,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      Json::Value out = service->roomSnapshot(roomId);
      reply(cb, std::move(out));
    }, "讀取房間失敗");
  });

  // [功能備註｜房間／跑團｜GET /api/admin/rooms/{1}/characters]
  // 用途：讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/rooms/{1}/characters", drogon::Get,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      Json::Value out;
      out["characters"] = selectRows1(service->database(),
          "SELECT row_to_json(x)::text AS json_text FROM (SELECT c.*,u.username,rm.role,rm.hp,rm.max_hp,rm.spirit,rm.max_spirit FROM room_members rm JOIN users u ON u.id=rm.user_id LEFT JOIN character_cards c ON c.user_id=rm.user_id WHERE rm.room_id=$1 ORDER BY rm.joined_at) x",
          roomId);
      reply(cb, std::move(out));
    });
  });

  // Room scene and time controls.
  // [功能備註｜房間／跑團｜PATCH /api/rooms/{1}/scene]
  // 用途：修改房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/rooms/{1}/scene", drogon::Patch,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req, true);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      requireRoomExists(service->database(), roomId);
      const auto body = bodyOrEmpty(req);
      auto mapName = trimmed(stringValue(body["map_name"], stringValue(body["name"], "未設定地點")));
      auto image = stringValue(body["map_image_url"], stringValue(body["image_url"]));
      auto description = stringValue(body["map_description"], stringValue(body["description"]));
      service->database()->execSqlSync(
          "UPDATE rooms SET map_name=$1,map_image_url=$2,map_description=$3,updated_at=CURRENT_TIMESTAMP WHERE id=$4",
          mapName, image, description, roomId);
      Json::Value payload; payload["text"] = "場景更新為「" + mapName + "」"; payload["map_name"] = mapName;
      service->addEvent(roomId, user.id, "scene", payload);
      reply(cb, service->roomSnapshot(roomId));
    }, "更新場景失敗");
  });

  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/time/advance]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/rooms/{1}/time/advance", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req, true);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      const auto body = bodyOrEmpty(req);
      auto minutes = intValue(body["minutes"]);
      if (minutes == 0) minutes = intValue(body["amount"], 0);
      minutes = std::clamp<std::int64_t>(minutes, -525600, 525600);
      const auto rows = service->database()->execSqlSync(
          "SELECT game_day,game_hour,game_minute FROM rooms WHERE id=$1", roomId);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到房間");
      auto total = (rows[0]["game_day"].as<std::int64_t>() - 1) * 1440 +
                   rows[0]["game_hour"].as<std::int64_t>() * 60 +
                   rows[0]["game_minute"].as<std::int64_t>() + minutes;
      total = std::max<std::int64_t>(0, total);
      const auto day = total / 1440 + 1;
      const auto minuteOfDay = total % 1440;
      const auto hour = minuteOfDay / 60;
      const auto minute = minuteOfDay % 60;
      service->database()->execSqlSync(
          "UPDATE rooms SET game_day=$1,game_hour=$2,game_minute=$3,updated_at=CURRENT_TIMESTAMP WHERE id=$4",
          day, hour, minute, roomId);
      Json::Value payload; payload["minutes"] = Json::Int64(minutes); payload["day"] = Json::Int64(day); payload["hour"] = Json::Int64(hour); payload["minute"] = Json::Int64(minute);
      service->addEvent(roomId, user.id, "time", payload);
      Json::Value out; out["ok"] = true; out["room"] = service->roomSnapshot(roomId); reply(cb, std::move(out));
    }, "推進時間失敗");
  });

  // [功能備註｜房間／跑團｜GET /api/rooms/{1}/time-view]
  // 用途：讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/rooms/{1}/time-view", drogon::Get,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      requireRoomAccess(service, user, roomId);
      const auto rows = service->database()->execSqlSync(
          "SELECT game_day,game_hour,game_minute,time_hidden,time_distortion_offset_minutes,time_rate,time_flow_offset_minutes FROM rooms WHERE id=$1", roomId);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到房間");
      Json::Value out;
      out["game_day"] = Json::Int64(rows[0]["game_day"].as<std::int64_t>());
      out["game_hour"] = Json::Int64(rows[0]["game_hour"].as<std::int64_t>());
      out["game_minute"] = Json::Int64(rows[0]["game_minute"].as<std::int64_t>());
      out["time_hidden"] = rows[0]["time_hidden"].as<bool>();
      out["can_view"] = user.isAdmin || !out["time_hidden"].asBool();
      if (!out["can_view"].asBool()) {
        out.removeMember("game_day"); out.removeMember("game_hour"); out.removeMember("game_minute");
      }
      reply(cb, std::move(out));
    }, "讀取時間失敗");
  });

  // [功能備註｜戰鬥／回合｜POST /api/rooms/{1}/combat/escape]
  // 用途：建立/執行戰鬥／回合相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/rooms/{1}/combat/escape", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      requireRoomAccess(service, user, roomId);
      const auto character = characterRaw(service->database(), user.id);
      const auto target = std::max<std::int64_t>(1, intValue(character["agility"]));
      const auto roll = randomInt(1, 100);
      const bool success = roll <= target;
      if (success) {
        service->database()->execSqlSync(
            "UPDATE room_members SET combat_state='escaped',turn_actions_remaining=0 WHERE room_id=$1 AND user_id=$2",
            roomId, user.id);
      }
      Json::Value payload; payload["text"] = success ? "逃脫成功" : "逃脫失敗"; payload["roll"] = roll; payload["target"] = Json::Int64(target); payload["success"] = success;
      service->addEvent(roomId, user.id, "escape", payload);
      Json::Value out; out["ok"] = true; out["success"] = success; out["roll"] = roll; out["target"] = Json::Int64(target); out["room"] = service->roomSnapshot(roomId); reply(cb, std::move(out));
    }, "逃脫失敗");
  });

  // Legacy monster-act aliases the current AI actor behavior by performing a simple attack event and consuming an action.
  // [功能備註｜怪物｜POST /api/admin/rooms/{1}/combat/monster-act]
  // 用途：建立/執行怪物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/rooms/{1}/combat/monster-act", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req, true);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      const auto body = bodyOrEmpty(req);
      const auto monsterId = intValue(body["monster_id"], intValue(body["id"]));
      const auto targetUserId = intValue(body["target_user_id"]);
      const auto monsters = service->database()->execSqlSync(
          "SELECT rm.id,rm.current_hp,mt.name,mt.attributes::text AS attributes_text FROM room_monsters rm JOIN monster_templates mt ON mt.id=rm.monster_template_id WHERE rm.room_id=$1 AND rm.id=$2",
          roomId, monsterId);
      if (monsters.empty()) throw ApiError(drogon::k404NotFound, "找不到怪物");
      std::int64_t target = targetUserId;
      if (target <= 0) {
        const auto candidates = service->database()->execSqlSync(
            "SELECT user_id FROM room_members WHERE room_id=$1 AND role='player' AND hp>0 AND combat_state NOT IN ('dead','escaped') ORDER BY random() LIMIT 1", roomId);
        if (candidates.empty()) throw ApiError(drogon::k400BadRequest, "沒有可攻擊的玩家");
        target = candidates[0]["user_id"].as<std::int64_t>();
      }
      const auto attrs = parseJson(monsters[0]["attributes_text"].as<std::string>());
      const auto strength = std::max<std::int64_t>(1, intValue(attrs["strength"], 10));
      const auto damage = randomInt(1, static_cast<int>(std::min<std::int64_t>(10000, strength)));
      service->database()->execSqlSync(
          "UPDATE room_members SET hp=GREATEST(0,hp-$1),combat_state=CASE WHEN hp-$1<=0 THEN 'downed' ELSE combat_state END WHERE room_id=$2 AND user_id=$3",
          damage, roomId, target);
      Json::Value payload; payload["text"] = monsters[0]["name"].as<std::string>() + " 發動攻擊，造成 " + std::to_string(damage) + " 傷害"; payload["monster_id"] = Json::Int64(monsterId); payload["target_user_id"] = Json::Int64(target); payload["damage"] = damage;
      service->addEvent(roomId, user.id, "combat_attack", payload);
      Json::Value out; out["ok"] = true; out["damage"] = damage; out["room"] = service->roomSnapshot(roomId); reply(cb, std::move(out));
    }, "怪物行動失敗");
  });

  // [功能備註｜戰鬥／回合｜POST /api/admin/rooms/{1}/combat/revive]
  // 用途：建立/執行戰鬥／回合相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/rooms/{1}/combat/revive", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req, true);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      const auto body = bodyOrEmpty(req);
      const auto targetUserId = intValue(body["user_id"], intValue(body["target_user_id"]));
      if (targetUserId <= 0) throw ApiError(drogon::k400BadRequest, "請指定玩家");
      const auto hp = std::max<std::int64_t>(1, intValue(body["hp"], 1));
      const auto rows = service->database()->execSqlSync(
          "UPDATE room_members SET hp=LEAST(max_hp,$1),combat_state='active' WHERE room_id=$2 AND user_id=$3 RETURNING user_id",
          hp, roomId, targetUserId);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到房間玩家");
      Json::Value payload; payload["text"] = "DM 使玩家重新站起"; payload["target_user_id"] = Json::Int64(targetUserId); payload["hp"] = Json::Int64(hp); service->addEvent(roomId, user.id, "revive", payload);
      Json::Value out; out["ok"] = true; out["room"] = service->roomSnapshot(roomId); reply(cb, std::move(out));
    }, "復活失敗");
  });

  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/resources/rest]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/rooms/{1}/resources/rest", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      requireRoomAccess(service, user, roomId);
      const auto rooms = service->database()->execSqlSync("SELECT battle_active FROM rooms WHERE id=$1", roomId);
      if (rooms.empty()) throw ApiError(drogon::k404NotFound, "找不到房間");
      if (rooms[0]["battle_active"].as<bool>()) throw ApiError(drogon::k400BadRequest, "戰鬥中不能休息");
      service->database()->execSqlSync(
          "UPDATE room_members SET hp=max_hp,spirit=max_spirit,current_sanity=max_sanity,combat_state='active' WHERE room_id=$1 AND user_id=$2",
          roomId, user.id);
      const auto character = characterRaw(service->database(), user.id);
      const bool west = stringValue(character["faction"]) == "西國";
      if (west) service->database()->execSqlSync("UPDATE character_cards SET current_faith=faith WHERE user_id=$1", user.id);
      else service->database()->execSqlSync("UPDATE character_cards SET current_great_way=great_way WHERE user_id=$1", user.id);
      Json::Value payload; payload["text"] = "完成休息並恢復資源"; service->addEvent(roomId, user.id, "rest", payload);
      Json::Value out; out["ok"] = true; out["room"] = service->roomSnapshot(roomId); out["character"] = characterRaw(service->database(), user.id); reply(cb, std::move(out));
    }, "休息失敗");
  });

  // [功能備註｜房間／跑團｜PATCH /api/rooms/{1}/member/{2}]
  // 用途：修改房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/rooms/{1}/member/{2}", drogon::Patch,
       [service](const Request& req, Callback& cb, const std::string& roomIdText, const std::string& userIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      const auto targetUserId = positiveId(userIdText, "玩家 ID 無效");
      const auto body = bodyOrEmpty(req);
      const auto rows = service->database()->execSqlSync(
          "SELECT row_to_json(rm)::text AS json_text FROM room_members rm WHERE room_id=$1 AND user_id=$2", roomId, targetUserId);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到房間玩家");
      auto current = rowJson(rows);
      const std::array<const char*, 12> fields = {"display_name","hp","max_hp","spirit","max_spirit","current_sanity","max_sanity","combat_state","combat_reaction","turn_actions_remaining","sanity_stage","seven_sin"};
      std::string sets;
      std::vector<std::string> names;
      Json::Value payload;
      for (const auto* field : fields) if (body.isMember(field)) { names.emplace_back(field); payload[field] = body[field]; }
      if (names.empty()) throw ApiError(drogon::k400BadRequest, "沒有可更新欄位");
      // jsonb_populate_record avoids manual per-type casts.
      for (std::size_t i=0;i<names.size();++i) { if(i) sets+=','; sets += "\""+names[i]+"\"=p.\""+names[i]+"\""; }
      const auto sql = "WITH p AS (SELECT * FROM jsonb_populate_record(NULL::room_members,$1::jsonb)) UPDATE room_members rm SET "+sets+" FROM p WHERE rm.room_id=$2 AND rm.user_id=$3";
      service->database()->execSqlSync(sql, compactJson(payload), roomId, targetUserId);
      Json::Value out; out["ok"] = true; out["room"] = service->roomSnapshot(roomId); reply(cb, std::move(out));
    }, "更新房間玩家失敗");
  });

  // Template CRUD for NPCs/dungeons and patch support for monsters.
  // [功能備註｜NPC｜GET /api/admin/npc-templates]
  // 用途：讀取/顯示NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/npc-templates", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["npc_templates"] = genericList(service->database(), "npc_templates"); reply(cb, std::move(out)); });
  });
  // [功能備註｜NPC｜POST /api/admin/npc-templates]
  // 用途：建立/執行NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/npc-templates", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["ok"] = true; out["npc_template"] = genericInsert(service->database(), "npc_templates", bodyOrEmpty(req)); reply(cb, std::move(out)); });
  });
  // [功能備註｜NPC｜PATCH/DELETE /api/admin/npc-templates/{1}]
  // 用途：NPC的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminPatchDelete1(service, "/api/admin/npc-templates/{1}", "npc_templates", "npc_template");

  // [功能備註｜副本｜GET /api/admin/dungeon-templates]
  // 用途：讀取/顯示副本相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/dungeon-templates", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["dungeon_templates"] = genericList(service->database(), "dungeon_templates"); reply(cb, std::move(out)); });
  });
  // [功能備註｜副本｜POST /api/admin/dungeon-templates]
  // 用途：建立/執行副本相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/dungeon-templates", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); auto body=bodyOrEmpty(req);body["dungeon_level"]=normalizeDungeonLevel(stringValue(body["dungeon_level"],"1"));Json::Value out; out["ok"] = true; out["dungeon_template"] = genericInsert(service->database(), "dungeon_templates", body); reply(cb, std::move(out)); });
  });
  // [功能備註｜副本｜PATCH/DELETE /api/admin/dungeon-templates/{1}]
  // 用途：副本的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  reg1("/api/admin/dungeon-templates/{1}",drogon::Patch,[service](const Request&req,Callback&cb,const std::string&idText){runHandler(cb,[&]{requireUser(service,req,true);auto body=bodyOrEmpty(req);if(body.isMember("dungeon_level"))body["dungeon_level"]=normalizeDungeonLevel(stringValue(body["dungeon_level"]));Json::Value out;out["ok"]=true;out["dungeon_template"]=genericUpdate(service->database(),"dungeon_templates",positiveId(idText),body);reply(cb,std::move(out));},"修改副本失敗");});
  reg1("/api/admin/dungeon-templates/{1}",drogon::Delete,[service](const Request&req,Callback&cb,const std::string&idText){runHandler(cb,[&]{requireUser(service,req,true);genericDelete(service->database(),"dungeon_templates",positiveId(idText));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"刪除副本失敗");});

  // [功能備註｜怪物｜PATCH /api/admin/monster-templates/{1}]
  // [功能備註｜60-cpp.21 怪物完整戰鬥實體] 修改怪物時同步保存五維、技能、被動、大道、Boss 階段與舊 config 相容欄位。
  reg1("/api/admin/monster-templates/{1}", drogon::Patch,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto id = positiveId(idText);
      auto body = bodyOrEmpty(req);
      const auto currentRows = service->database()->execSqlSync(
          "SELECT config::text AS config_text,attributes::text AS attributes_text,boss_phases::text AS boss_phases_text FROM monster_templates WHERE id=$1", id);
      if (currentRows.empty()) throw ApiError(drogon::k404NotFound, "找不到怪物模板");
      auto config = parseJson(currentRows[0]["config_text"].as<std::string>(), Json::Value{Json::objectValue});
      if (body["config"].isObject())
        for (const auto& key : body["config"].getMemberNames()) config[key] = body["config"][key];
      for (const auto* key : {"element","combat_attack_attribute","combat_damage_formula","combat_damage_target",
                              "combat_accuracy_bonus","combat_defense_attribute","combat_dodge_attribute",
                              "hearing_threshold","boss_enabled","secret","ai_actions"})
        if (body.isMember(key)) config[key] = body[key];
      if (body["attributes"].isObject()) {
        auto attributes = parseJson(currentRows[0]["attributes_text"].as<std::string>(), Json::Value{Json::objectValue});
        for (const auto& key : body["attributes"].getMemberNames()) attributes[key] = body["attributes"][key];
        for (const auto* key : {"strength","agility","constitution","spirit","luck"})
          attributes[key] = Json::Int64(std::max<std::int64_t>(0, intValue(attributes[key])));
        body["attributes"] = attributes; // PATCH 可只改一維，不會把其他四維清成 0。
      }
      if (body["boss_phases"].isArray()) config["boss_phases"] = body["boss_phases"];
      else if (body.isMember("boss_phases") && !body["boss_phases"].isArray()) body.removeMember("boss_phases");
      // 舊版前端只送 config.boss_phases 時，也同步正式欄位。
      if (!body.isMember("boss_phases") && config["boss_phases"].isArray()) body["boss_phases"] = config["boss_phases"];
      body["config"] = config;
      Json::Value out; out["ok"] = true;
      out["monster_template"] = genericUpdate(service->database(), "monster_templates", id, body);
      reply(cb, std::move(out));
    }, "更新怪物模板失敗");
  });

  // Room world composition: NPCs, dungeons, discoveries and lightweight AI/exploration state.
  // [功能備註｜房間／跑團｜GET /api/rooms/{1}/world]
  // 用途：讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/rooms/{1}/world", drogon::Get,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      requireRoomAccess(service, user, roomId);
      requireRoomExists(service->database(), roomId);
      Json::Value out;
      out["room"] = service->roomSnapshot(roomId)["room"];
      out["monsters"] = selectRows1(service->database(),
          "SELECT row_to_json(x)::text AS json_text FROM (SELECT rm.*,mt.name,mt.category,mt.rank,mt.description,mt.public_hint,mt.image_url,mt.attributes,mt.skills,mt.passive_skills,mt.great_way,mt.boss_phases,mt.drops,mt.ai_level,mt.ai_behavior,mt.config,mt.element_affinity,mt.element_resistance,mt.element_weakness,mt.effects FROM room_monsters rm JOIN monster_templates mt ON mt.id=rm.monster_template_id WHERE rm.room_id=$1 ORDER BY rm.id) x", roomId);
      out["npcs"] = selectRows1(service->database(),
          "SELECT row_to_json(x)::text AS json_text FROM (SELECT rn.*,nt.name,nt.role_name,nt.description,nt.image_url,nt.portrait_url,nt.merchant_enabled,nt.merchant_title,nt.attributes,nt.skills,nt.config,nt.element_affinity,nt.element_resistance,nt.element_weakness,nt.effects FROM room_npcs rn JOIN npc_templates nt ON nt.id=rn.npc_template_id WHERE rn.room_id=$1 ORDER BY rn.npc_template_id) x", roomId);
      out["dungeons"] = selectRows1(service->database(),
          "SELECT row_to_json(x)::text AS json_text FROM (SELECT rd.*,dt.name,dt.description,dt.dungeon_level,dt.config FROM room_dungeons rd JOIN dungeon_templates dt ON dt.id=rd.dungeon_template_id WHERE rd.room_id=$1 ORDER BY rd.dungeon_template_id) x", roomId);
      reply(cb, std::move(out));
    }, "讀取世界資料失敗");
  });

  // [功能備註｜NPC｜POST /api/admin/rooms/{1}/npcs]
  // 用途：建立/執行NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/rooms/{1}/npcs", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      const auto body = bodyOrEmpty(req);
      const auto templateId = intValue(body["template_id"], intValue(body["npc_template_id"]));
      if (templateId <= 0) throw ApiError(drogon::k400BadRequest, "請選擇 NPC 模板");
      const auto templ = service->database()->execSqlSync(
          "SELECT max_hp FROM npc_templates WHERE id=$1", templateId);
      if (templ.empty()) throw ApiError(drogon::k404NotFound, "找不到 NPC 模板");
      const auto hp = std::max<std::int64_t>(1, templ[0]["max_hp"].as<std::int64_t>());
      service->database()->execSqlSync(
          "INSERT INTO room_npcs(room_id,npc_template_id,status,state,current_hp,max_hp,current_spirit,max_spirit,current_fifth,max_fifth,combat_side) "
          "VALUES($1,$2,'active','{}'::jsonb,$3,$3,0,0,0,0,'neutral') "
          "ON CONFLICT(room_id,npc_template_id) DO UPDATE SET status='active',current_hp=EXCLUDED.current_hp,max_hp=EXCLUDED.max_hp,updated_at=CURRENT_TIMESTAMP",
          roomId, templateId, hp);
      Json::Value out; out["ok"] = true; out["room"] = service->roomSnapshot(roomId); reply(cb, std::move(out));
    }, "加入 NPC 失敗");
  });

  // [功能備註｜NPC｜PATCH /api/admin/rooms/{1}/npcs/{2}/combat-side]
  // 用途：修改NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/rooms/{1}/npcs/{2}/combat-side", drogon::Patch,
       [service](const Request& req, Callback& cb, const std::string& roomIdText, const std::string& templateIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      const auto templateId = positiveId(templateIdText, "NPC 模板 ID 無效");
      const auto body = bodyOrEmpty(req);
      auto side = stringValue(body["combat_side"], stringValue(body["side"], "neutral"));
      if (side != "ally" && side != "enemy" && side != "neutral") side = "neutral";
      const auto rows = service->database()->execSqlSync(
          "UPDATE room_npcs SET combat_side=$1,updated_at=CURRENT_TIMESTAMP WHERE room_id=$2 AND npc_template_id=$3 RETURNING npc_template_id",
          side, roomId, templateId);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到房間 NPC");
      Json::Value out; out["ok"] = true; out["room"] = service->roomSnapshot(roomId); reply(cb, std::move(out));
    }, "更新 NPC 陣營失敗");
  });

  // [功能備註｜NPC｜DELETE /api/admin/rooms/{1}/npcs/{2}]
  // 用途：刪除NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/rooms/{1}/npcs/{2}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& roomIdText, const std::string& templateIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto rows = service->database()->execSqlSync(
          "DELETE FROM room_npcs WHERE room_id=$1 AND npc_template_id=$2 RETURNING npc_template_id",
          positiveId(roomIdText), positiveId(templateIdText));
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到房間 NPC");
      Json::Value out; out["ok"] = true; reply(cb, std::move(out));
    }, "移除 NPC 失敗");
  });

  // [功能備註｜副本｜POST /api/admin/rooms/{1}/dungeons]
  // 用途：建立/執行副本相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/rooms/{1}/dungeons", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      const auto body = bodyOrEmpty(req);
      const auto templateId = intValue(body["template_id"], intValue(body["dungeon_template_id"]));
      if (templateId <= 0) throw ApiError(drogon::k400BadRequest, "請選擇副本模板");
      if (service->database()->execSqlSync("SELECT 1 FROM dungeon_templates WHERE id=$1", templateId).empty())
        throw ApiError(drogon::k404NotFound, "找不到副本模板");
      service->database()->execSqlSync(
          "INSERT INTO room_dungeons(room_id,dungeon_template_id,status,state) VALUES($1,$2,'available','{}'::jsonb) "
          "ON CONFLICT(room_id,dungeon_template_id) DO UPDATE SET status='available',state='{}'::jsonb",
          roomId, templateId);
      Json::Value out; out["ok"] = true; out["world"] = selectRows1(service->database(),
          "SELECT row_to_json(x)::text AS json_text FROM (SELECT rd.*,dt.name,dt.description,dt.dungeon_level,dt.config FROM room_dungeons rd JOIN dungeon_templates dt ON dt.id=rd.dungeon_template_id WHERE rd.room_id=$1) x", roomId); reply(cb, std::move(out));
    }, "加入副本失敗");
  });

  // [功能備註｜副本｜DELETE /api/admin/rooms/{1}/dungeons/{2}]
  // 用途：刪除副本相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/rooms/{1}/dungeons/{2}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& roomIdText, const std::string& templateIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto rows = service->database()->execSqlSync(
          "DELETE FROM room_dungeons WHERE room_id=$1 AND dungeon_template_id=$2 RETURNING dungeon_template_id",
          positiveId(roomIdText), positiveId(templateIdText));
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到房間副本");
      Json::Value out; out["ok"] = true; reply(cb, std::move(out));
    }, "移除副本失敗");
  });

  // [功能備註｜怪物｜POST /api/rooms/{1}/monsters/{2}/investigate]
  // 用途：建立/執行怪物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/rooms/{1}/monsters/{2}/investigate", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText, const std::string& monsterIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      const auto monsterId = positiveId(monsterIdText, "怪物 ID 無效");
      requireRoomAccess(service, user, roomId);
      const auto rows = service->database()->execSqlSync(
          "SELECT rm.id,mt.investigation_dc,mt.name,mt.public_hint,mt.description FROM room_monsters rm JOIN monster_templates mt ON mt.id=rm.monster_template_id WHERE rm.room_id=$1 AND rm.id=$2",
          roomId, monsterId);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到怪物");
      const auto character = characterRaw(service->database(), user.id);
      const auto target = std::max<std::int64_t>(1, intValue(character["spirit"]) + intValue(character["luck"]));
      const auto roll = randomInt(1, 100);
      const auto dc = rows[0]["investigation_dc"].as<std::int64_t>();
      const bool success = roll <= std::clamp<std::int64_t>(target - dc + 50, 1, 99);
      service->database()->execSqlSync(
          "INSERT INTO monster_discoveries(room_monster_id,user_id,encountered,investigated,last_roll,last_rate) VALUES($1,$2,TRUE,$3,$4,$5) "
          "ON CONFLICT(room_monster_id,user_id) DO UPDATE SET investigated=monster_discoveries.investigated OR EXCLUDED.investigated,last_roll=EXCLUDED.last_roll,last_rate=EXCLUDED.last_rate,updated_at=CURRENT_TIMESTAMP",
          monsterId, user.id, success, roll, target);
      Json::Value out; out["ok"] = true; out["success"] = success; out["roll"] = roll; out["target"] = Json::Int64(target); out["name"] = rows[0]["name"].as<std::string>(); out["hint"] = rows[0]["public_hint"].as<std::string>(); if (success) out["description"] = rows[0]["description"].as<std::string>(); reply(cb, std::move(out));
    }, "調查怪物失敗");
  });

  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/ai/step]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/rooms/{1}/ai/step", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      requireRoomAccess(service, user, roomId);
      Json::Value payload; payload["text"] = "AI 世界步驟已推進"; payload["actor"] = user.username; service->addEvent(roomId, user.id, "ai_step", payload);
      Json::Value out; out["ok"] = true; out["room"] = service->roomSnapshot(roomId); reply(cb, std::move(out));
    }, "AI 推進失敗");
  });

  // [功能備註｜NPC｜GET /api/game-room/npcs]
  // 用途：讀取/顯示NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/game-room/npcs", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req);
      Json::Value out; out["npcs"] = genericList(service->database(), "npc_templates", "name ASC"); reply(cb, std::move(out));
    });
  });

  // [功能備註｜召喚物｜POST /api/rooms/{1}/summons/{2}/mount]
  // 用途：建立/執行召喚物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/rooms/{1}/summons/{2}/mount", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText, const std::string& summonIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      const auto summonId = positiveId(summonIdText, "召喚物 ID 無效");
      requireRoomAccess(service, user, roomId);
      if (service->database()->execSqlSync("SELECT 1 FROM summon_instances WHERE id=$1 AND owner_user_id=$2 AND status='active'", summonId, user.id).empty())
        throw ApiError(drogon::k404NotFound, "找不到可騎乘的召喚物");
      service->database()->execSqlSync("UPDATE room_members SET mounted_summon_id=$1 WHERE room_id=$2 AND user_id=$3", summonId, roomId, user.id);
      Json::Value out; out["ok"] = true; out["room"] = service->roomSnapshot(roomId); reply(cb, std::move(out));
    }, "騎乘失敗");
  });

  // [功能備註｜召喚物｜POST /api/rooms/{1}/summons/unmount]
  // 用途：建立/執行召喚物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/rooms/{1}/summons/unmount", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      requireRoomAccess(service, user, roomId);
      service->database()->execSqlSync("UPDATE room_members SET mounted_summon_id=NULL WHERE room_id=$1 AND user_id=$2", roomId, user.id);
      Json::Value out; out["ok"] = true; out["room"] = service->roomSnapshot(roomId); reply(cb, std::move(out));
    }, "解除騎乘失敗");
  });

  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/charge/cancel]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/rooms/{1}/charge/cancel", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      requireRoomAccess(service, user, roomId);
      service->database()->execSqlSync("UPDATE room_members SET skill_charge_state='{}'::jsonb WHERE room_id=$1 AND user_id=$2", roomId, user.id);
      Json::Value out; out["ok"] = true; out["room"] = service->roomSnapshot(roomId); reply(cb, std::move(out));
    }, "取消蓄力失敗");
  });

  // Task board and rewards.
  // [功能備註｜任務｜GET /api/tasks/public]
  // 用途：讀取/顯示任務相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/tasks/public", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      Json::Value out;
      out["tasks"] = selectRows1(service->database(),
          "SELECT row_to_json(x)::text AS json_text FROM ("
          "SELECT t.*,tp.status AS participant_status FROM tasks t LEFT JOIN task_participants tp ON tp.task_id=t.id AND tp.user_id=$1 "
          "WHERE t.status='open' AND t.task_type='public' ORDER BY t.created_at DESC) x", user.id);
      reply(cb, std::move(out));
    }, "讀取公共任務失敗");
  });

  // [功能備註｜任務｜GET /api/tasks/my]
  // 用途：讀取/顯示任務相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/tasks/my", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      Json::Value out;
      out["tasks"] = selectRows1(service->database(),
          "SELECT row_to_json(x)::text AS json_text FROM ("
          "SELECT DISTINCT t.*,COALESCE(tp.status,CASE WHEN t.target_user_id=$1 THEN 'assigned' END) AS participant_status "
          "FROM tasks t LEFT JOIN task_participants tp ON tp.task_id=t.id AND tp.user_id=$1 "
          "WHERE t.target_user_id=$1 OR tp.user_id=$1 ORDER BY t.created_at DESC) x", user.id);
      reply(cb, std::move(out));
    }, "讀取我的任務失敗");
  });

  // [功能備註｜任務｜POST /api/tasks/{1}/accept]
  // 用途：建立/執行任務相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/tasks/{1}/accept", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& taskIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto taskId = positiveId(taskIdText, "任務 ID 無效");
      const auto tasks = service->database()->execSqlSync(
          "SELECT id,status,task_type,target_user_id,min_tier FROM tasks WHERE id=$1", taskId);
      if (tasks.empty()) throw ApiError(drogon::k404NotFound, "找不到任務");
      if (tasks[0]["status"].as<std::string>() != "open") throw ApiError(drogon::k400BadRequest, "任務目前不能接受");
      if (!tasks[0]["target_user_id"].isNull() && tasks[0]["target_user_id"].as<std::int64_t>() != user.id)
        throw ApiError(drogon::k403Forbidden, "這是指定給其他玩家的任務");
      const auto character = characterRaw(service->database(), user.id);
      if (intValue(character["tier"], 1) < tasks[0]["min_tier"].as<std::int64_t>())
        throw ApiError(drogon::k403Forbidden, "你的階級不足以接受此任務");
      service->database()->execSqlSync(
          "INSERT INTO task_participants(task_id,user_id,status) VALUES($1,$2,'accepted') "
          "ON CONFLICT(task_id,user_id) DO UPDATE SET status='accepted',accepted_at=CURRENT_TIMESTAMP,completed_at=NULL",
          taskId, user.id);
      Json::Value out; out["ok"] = true; reply(cb, std::move(out));
    }, "接受任務失敗");
  });

  // [功能備註｜任務｜GET /api/admin/tasks]
  // 用途：讀取/顯示任務相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/tasks", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["tasks"] = selectRows(service->database(),
          "SELECT row_to_json(x)::text AS json_text FROM (SELECT t.*,u.username AS creator_username,target.username AS target_username,(SELECT count(*) FROM task_participants tp WHERE tp.task_id=t.id)::int AS participant_count FROM tasks t JOIN users u ON u.id=t.creator_id LEFT JOIN users target ON target.id=t.target_user_id ORDER BY t.created_at DESC) x");
      reply(cb, std::move(out));
    }, "讀取任務管理失敗");
  });

  // [功能備註｜任務｜POST /api/admin/tasks]
  // 用途：建立/執行任務相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/tasks", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req, true);
      Json::Value extra; extra["creator_id"] = Json::Int64(user.id);
      auto body = bodyOrEmpty(req);
      if (!body.isMember("task_type")) body["task_type"] = body.isMember("target_user_id") && intValue(body["target_user_id"]) > 0 ? "private" : "public";
      Json::Value out; out["ok"] = true; out["task"] = genericInsert(service->database(), "tasks", body, extra); reply(cb, std::move(out));
    }, "建立任務失敗");
  });

  // [功能備註｜任務｜PATCH /api/admin/tasks/{1}]
  // 用途：修改任務相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/tasks/{1}", drogon::Patch,
       [service](const Request& req, Callback& cb, const std::string& taskIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out; out["ok"] = true; out["task"] = genericUpdate(service->database(), "tasks", positiveId(taskIdText), bodyOrEmpty(req)); reply(cb, std::move(out));
    }, "更新任務失敗");
  });

  // [功能備註｜任務｜POST /api/admin/tasks/{1}/complete]
  // 用途：建立/執行任務相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/tasks/{1}/complete", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& taskIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto taskId = positiveId(taskIdText, "任務 ID 無效");
      const auto body = bodyOrEmpty(req);
      const auto taskRows = service->database()->execSqlSync(
          "SELECT row_to_json(t)::text AS json_text FROM tasks t WHERE id=$1", taskId);
      if (taskRows.empty()) throw ApiError(drogon::k404NotFound, "找不到任務");
      const auto task = rowJson(taskRows);
      auto targetUserId = intValue(body["user_id"], intValue(task["target_user_id"]));
      if (targetUserId <= 0) {
        const auto participants = service->database()->execSqlSync(
            "SELECT user_id FROM task_participants WHERE task_id=$1 AND status='accepted' ORDER BY accepted_at LIMIT 1", taskId);
        if (participants.empty()) throw ApiError(drogon::k400BadRequest, "沒有可完成任務的玩家");
        targetUserId = participants[0]["user_id"].as<std::int64_t>();
      }
      auto character = characterRaw(service->database(), targetUserId);
      const auto rewards = task["rewards"].isObject() ? task["rewards"] : Json::Value{Json::objectValue};
      auto inventory = inventoryOf(character);
      if (rewards["inventory"].isArray()) {
        for (const auto& item : rewards["inventory"]) {
          const auto name = item.isString() ? item.asString() : stringValue(item["name"]);
          const auto category = item.isObject() ? stringValue(item["category"]) : std::string{};
          const auto qty = item.isObject() ? std::max<std::int64_t>(1, intValue(item["quantity"], 1)) : 1;
          if (!name.empty()) inventory = changeInventory(std::move(inventory), name, qty, category, item);
        }
      }
      auto equipment = character["equipment"].isArray() ? character["equipment"] : Json::Value{Json::arrayValue};
      auto weapons = character["weapons"].isArray() ? character["weapons"] : Json::Value{Json::arrayValue};
      if (rewards["equipment"].isArray()) for (const auto& x : rewards["equipment"]) equipment.append(x);
      if (rewards["weapons"].isArray()) for (const auto& x : rewards["weapons"]) weapons.append(x);
      const auto ritualPointReward=std::max<std::int64_t>(0,intValue(rewards["ritual_points"]));
      service->database()->execSqlSync(
          "UPDATE character_cards SET experience=experience+$1,attribute_points=attribute_points+$2,skill_points=skill_points+$3,"
          "magic_points=magic_points+$4,ritual_points=ritual_points+$5,ritual_points_earned=ritual_points_earned+$5,"
          "weird_coins=weird_coins+$6,game_coins=game_coins+$7,"
          "inventory=$8::jsonb,equipment=$9::jsonb,weapons=$10::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$11",
          std::max<std::int64_t>(0,intValue(rewards["experience"])),
          0,  // [功能備註｜64-cpp.25 固定等階屬性額度] 舊任務屬性點獎勵不再突破大階總額。
          std::max<std::int64_t>(0,intValue(rewards["skill_points"])),
          std::max<std::int64_t>(0,intValue(rewards["magic_points"])),
          ritualPointReward,
          std::max<std::int64_t>(0,intValue(rewards["weird_coins"])),
          std::max<std::int64_t>(0,intValue(rewards["game_coins"])),
          compactJson(inventory), compactJson(equipment), compactJson(weapons), targetUserId);
      // [功能備註｜64-cpp.25 固定等階屬性額度] 任務完成後重新依「五項已分配 + 剩餘 = 本階總額」校正，
      // 避免舊任務 rewards.attribute_points 或舊資料把角色推到超額。
      {
        const auto rankRows = service->database()->execSqlSync(
            "SELECT tier,agility,strength,constitution,spirit,great_way,faith,faction FROM character_cards WHERE user_id=$1",
            targetUserId);
        if (!rankRows.empty()) {
          const auto tier = std::clamp<std::int64_t>(rankRows[0]["tier"].as<std::int64_t>(), 0, 10);
          const bool west = rankRows[0]["faction"].as<std::string>() == "西國";
          const auto fifth = std::max<std::int64_t>(0, west ? rankRows[0]["faith"].as<std::int64_t>()
                                                            : rankRows[0]["great_way"].as<std::int64_t>());
          const auto spent = std::max<std::int64_t>(0, rankRows[0]["agility"].as<std::int64_t>()) +
                             std::max<std::int64_t>(0, rankRows[0]["strength"].as<std::int64_t>()) +
                             std::max<std::int64_t>(0, rankRows[0]["constitution"].as<std::int64_t>()) +
                             std::max<std::int64_t>(0, rankRows[0]["spirit"].as<std::int64_t>()) + fifth;
          const auto remaining = std::max<std::int64_t>(0, characterTierBudget(tier) - spent);
          service->database()->execSqlSync(
              "UPDATE character_cards SET attribute_points=$1,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",
              remaining, targetUserId);
        }
      }
      // [功能備註｜63-cpp.24.1 儀式學獎勵來源] 任務 rewards 可使用 ritual_xp，升級後自動轉成可用點數。
      grantRitualXp(service->database(),targetUserId,std::max<std::int64_t>(0,intValue(rewards["ritual_xp"])));
      service->database()->execSqlSync(
          "INSERT INTO task_participants(task_id,user_id,status,completed_at) VALUES($1,$2,'completed',CURRENT_TIMESTAMP) "
          "ON CONFLICT(task_id,user_id) DO UPDATE SET status='completed',completed_at=CURRENT_TIMESTAMP",
          taskId, targetUserId);
      if (stringValue(task["task_type"]) == "private") service->database()->execSqlSync("UPDATE tasks SET status='completed',closed_at=CURRENT_TIMESTAMP,updated_at=CURRENT_TIMESTAMP WHERE id=$1", taskId);
      Json::Value out; out["ok"] = true; out["character"] = characterRaw(service->database(), targetUserId); reply(cb, std::move(out));
    }, "完成任務失敗");
  });

  // [功能備註｜任務｜DELETE /api/admin/tasks/{1}]
  // 用途：刪除任務相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/tasks/{1}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& taskIdText) {
    runHandler(cb, [&] { requireUser(service, req, true); genericDelete(service->database(), "tasks", positiveId(taskIdText)); Json::Value out; out["ok"] = true; reply(cb, std::move(out)); }, "刪除任務失敗");
  });

  // Horror rules.
  // [功能備註｜恐怖／理智規則｜GET /api/admin/horror-rules]
  // 用途：讀取/顯示恐怖／理智規則相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/horror-rules", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["rules"] = genericList(service->database(), "horror_rules"); reply(cb, std::move(out)); });
  });
  // [功能備註｜恐怖／理智規則｜POST /api/admin/horror-rules]
  // 用途：建立/執行恐怖／理智規則相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/horror-rules", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["ok"] = true; out["rule"] = genericInsert(service->database(), "horror_rules", bodyOrEmpty(req)); reply(cb, std::move(out)); });
  });
  // [功能備註｜恐怖／理智規則｜PATCH/DELETE /api/admin/horror-rules/{1}]
  // 用途：恐怖／理智規則的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminPatchDelete1(service, "/api/admin/horror-rules/{1}", "horror_rules", "rule");


// [功能備註｜戰鬥／回合｜POST /api/rooms/{1}/actions]
// 用途：64-cpp.25.4 起同一個互動 API 同時承載「說話／行動／待機」。
// [功能備註｜60-cpp.21.4 行動提交核心優先]
// 玩家「提交行動」的核心仍是把 action event 寫入 events。規則引擎仍可正常 block_action；
// 但若舊房間的規則擴充 schema 或完整房間快照本身故障，不再把已可記錄的玩家行動整體回成 500。
// [功能備註｜64-cpp.25.4 行動＋說話＋待機]
// speech：角色實際開口，觸發 CHARACTER_SPOKE + SOUND_EMITTED，不消耗戰鬥行動。
// action：角色實際行動，觸發 PLAYER_ACTION；noise>0 時同樣產生 SOUND_EMITTED。
// wait：本輪不做動作，觸發 PLAYER_WAITED；戰鬥中的行動消耗仍由既有 /combat/pass 處理。
reg1("/api/rooms/{1}/actions", drogon::Post,
     [service](const Request& req, Callback& cb, const std::string& roomIdText) {
  runHandler(cb, [&] {
    const auto user = requireUser(service, req);
    const auto roomId = positiveId(roomIdText, "房間 ID 無效");
    requireRoomAccess(service, user, roomId);
    const auto body = bodyOrEmpty(req);

    auto interactionType = trimmed(stringValue(body["interaction_type"], "action"));
    std::transform(interactionType.begin(), interactionType.end(), interactionType.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (interactionType != "action" && interactionType != "speech" &&
        interactionType != "wait") {
      throw ApiError(drogon::k400BadRequest, "互動類型只支援說話、行動或待機");
    }

    auto text = trimmed(stringValue(body["action"], stringValue(body["text"])));
    if (interactionType == "wait") {
      text = "待機";
    } else if (text.empty()) {
      throw ApiError(drogon::k400BadRequest,
                     interactionType == "speech" ? "請輸入要說的內容" : "請輸入要提交的行動");
    }
    const std::size_t limit = interactionType == "speech" ? 1000U : 8000U;
    if (text.size() > limit) {
      throw ApiError(drogon::k400BadRequest,
                     interactionType == "speech" ? "說話內容過長" : "行動內容過長");
    }

    std::string volume = trimmed(stringValue(body["volume"], "normal"));
    std::transform(volume.begin(), volume.end(), volume.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (volume != "whisper" && volume != "normal" && volume != "shout") volume = "normal";

    std::int64_t noise = 0;
    if (interactionType == "speech") {
      noise = volume == "whisper" ? 3 : volume == "shout" ? 25 : 10;
    } else if (interactionType == "action") {
      noise = std::clamp<std::int64_t>(intValue(body["noise"]), 0, 100);
    }

    Json::Value payload = body;
    payload["interaction_type"] = interactionType;
    payload["action"] = text;
    payload["text"] = text;
    payload["volume"] = volume;
    payload["noise"] = Json::Int64(noise);

    if (interactionType == "speech") {
      appendAudienceIds(payload, physicalSpeechAudience(service, roomId, user.id, volume));
    }

    Json::Value matched{Json::arrayValue};
    Json::Value ruleEvents{Json::arrayValue};
    bool ruleEngineDegraded = false;
    auto runRule = [&](const std::string& eventType, bool canBlock) {
      Json::Value result{Json::objectValue};
      result["event_type"] = eventType;
      result["matched"] = Json::arrayValue;
      result["blocked"] = false;
      try {
        result = evaluateRuleEvent(service, roomId, user.id, eventType, payload, false);
      } catch (const std::exception& error) {
        ruleEngineDegraded = true;
        LOG_WARN << eventType << " rule engine degraded for room " << roomId
                 << ", user " << user.id << ": " << error.what();
      }
      if (result["matched"].isArray()) {
        for (const auto& item : result["matched"]) matched.append(item);
      }
      ruleEvents.append(result);
      if (canBlock && result.get("blocked", false).asBool()) {
        throw ApiError(drogon::k403Forbidden,
                       interactionType == "speech" ? "這句話被規則阻止"
                       : interactionType == "wait" ? "待機被規則阻止"
                       : "此行動被規則阻止");
      }
    };

    if (interactionType == "speech") {
      runRule("CHARACTER_SPOKE", true);
      runRule("SOUND_EMITTED", true);
      applyInteractionNoise(service, roomId, user.id, noise, volume, payload);
      service->addEvent(roomId, user.id, "speech", payload);
    } else if (interactionType == "wait") {
      runRule("PLAYER_WAITED", true);
      service->addEvent(roomId, user.id, "wait", payload);
    } else {
      // [功能備註｜規則引擎] 玩家宣告任何動作時先送入 C++ 條件式規則引擎。
      runRule("PLAYER_ACTION", true);
      if (noise > 0) {
        runRule("SOUND_EMITTED", true);
        applyInteractionNoise(service, roomId, user.id, noise, volume, payload);
      }
      service->addEvent(roomId, user.id, "action", payload);
    }

    Json::Value out;
    out["ok"] = true;
    out["interaction_type"] = interactionType;
    out["action"] = payload;  // 保留舊前端／整合測試相容欄位。
    out["interaction"] = payload;
    out["triggered"] = matched;
    out["rule_engine_events"] = ruleEvents;
    out["rule_engine_degraded"] = ruleEngineDegraded;
    // 舊 action 用戶仍能從 rule_engine 取得第一個事件結果。
    if (ruleEvents.isArray() && !ruleEvents.empty()) out["rule_engine"] = ruleEvents[0];

    try {
      out["snapshot"] = roomSnapshotForViewer(service, roomId, user.id);
    } catch (const std::exception& error) {
      out["snapshot_degraded"] = true;
      LOG_WARN << "ROOM_INTERACTION snapshot degraded for room " << roomId
               << ": " << error.what();
    }
    reply(cb, std::move(out));
  }, "提交互動失敗");
});

  // [功能備註｜調查／線索｜GET /api/journal]
  // 用途：讀取/顯示調查／線索相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/journal", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      Json::Value out;
      out["clues"] = selectRows1(service->database(),
          "SELECT row_to_json(x)::text AS json_text FROM (SELECT cc.*,ct.name,ct.category,ct.description,ct.image_url,ct.linked_rule_id FROM character_clues cc JOIN clue_templates ct ON ct.id=cc.clue_id WHERE cc.user_id=$1 ORDER BY cc.acquired_at DESC) x", user.id);
      out["rules"] = selectRows1(service->database(),
          "SELECT row_to_json(x)::text AS json_text FROM (SELECT rd.*,hr.name,hr.rule_text,hr.public_hint,hr.consequence FROM rule_discoveries rd JOIN horror_rules hr ON hr.id=rd.rule_id WHERE rd.user_id=$1 ORDER BY rd.discovered_at DESC) x", user.id);
      out["statuses"] = selectRows1(service->database(),
          "SELECT row_to_json(cs)::text AS json_text FROM character_statuses cs WHERE user_id=$1 AND active=TRUE ORDER BY id DESC", user.id);
      out["visions"] = selectRows1(service->database(),
          "SELECT row_to_json(v)::text AS json_text FROM personal_visions v WHERE user_id=$1 ORDER BY created_at DESC LIMIT 100", user.id);
      reply(cb, std::move(out));
    }, "讀取調查日誌失敗");
  });

  // Clues.
  // [功能備註｜調查／線索｜GET /api/admin/clues]
  // 用途：讀取/顯示調查／線索相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/clues", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["clues"] = genericList(service->database(), "clue_templates"); reply(cb, std::move(out)); });
  });
  // [功能備註｜調查／線索｜POST /api/admin/clues]
  // 用途：建立/執行調查／線索相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/clues", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["ok"] = true; out["clue"] = genericInsert(service->database(), "clue_templates", bodyOrEmpty(req)); reply(cb, std::move(out)); });
  });
  // [功能備註｜調查／線索｜DELETE /api/admin/clues/{1}]
  // 用途：刪除調查／線索相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/clues/{1}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] { requireUser(service, req, true); genericDelete(service->database(), "clue_templates", positiveId(idText)); Json::Value out; out["ok"] = true; reply(cb, std::move(out)); });
  });

  // [功能備註｜玩家管理｜POST /api/admin/players/{1}/clues/{2}]
  // 用途：建立/執行玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/players/{1}/clues/{2}", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& userIdText, const std::string& clueIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto userId = positiveId(userIdText, "玩家 ID 無效");
      const auto clueId = positiveId(clueIdText, "線索 ID 無效");
      if (service->database()->execSqlSync("SELECT 1 FROM clue_templates WHERE id=$1", clueId).empty()) throw ApiError(drogon::k404NotFound, "找不到線索");
      service->database()->execSqlSync("INSERT INTO character_clues(clue_id,user_id,note) VALUES($1,$2,$3) ON CONFLICT(clue_id,user_id) DO UPDATE SET note=EXCLUDED.note", clueId, userId, stringValue(bodyOrEmpty(req)["note"]));
      Json::Value out; out["ok"] = true; reply(cb, std::move(out));
    }, "發放線索失敗");
  });

  // [功能備註｜玩家管理｜POST /api/admin/players/{1}/rules/{2}/reveal]
  // 用途：建立/執行玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/players/{1}/rules/{2}/reveal", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& userIdText, const std::string& ruleIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto userId = positiveId(userIdText, "玩家 ID 無效");
      const auto ruleId = positiveId(ruleIdText, "規則 ID 無效");
      const auto level = std::max<std::int64_t>(1, intValue(bodyOrEmpty(req)["knowledge_level"], 1));
      service->database()->execSqlSync(
          "INSERT INTO rule_discoveries(rule_id,user_id,knowledge_level,note) VALUES($1,$2,$3,$4) ON CONFLICT(rule_id,user_id) DO UPDATE SET knowledge_level=GREATEST(rule_discoveries.knowledge_level,EXCLUDED.knowledge_level),note=EXCLUDED.note,updated_at=CURRENT_TIMESTAMP",
          ruleId, userId, level, stringValue(bodyOrEmpty(req)["note"]));
      Json::Value out; out["ok"] = true; reply(cb, std::move(out));
    }, "揭露規則失敗");
  });

  // Status templates and player statuses.
  // [功能備註｜DM 權限｜GET /api/admin/status-templates]
  // 用途：讀取/顯示DM 權限相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/status-templates", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["statuses"] = genericList(service->database(), "status_templates"); out["status_templates"] = out["statuses"]; reply(cb, std::move(out)); });
  });
  // [功能備註｜DM 權限｜POST /api/admin/status-templates]
  // 用途：建立/執行DM 權限相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/status-templates", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["ok"] = true; out["status"] = genericInsert(service->database(), "status_templates", bodyOrEmpty(req)); reply(cb, std::move(out)); });
  });
  // [功能備註｜DM 權限｜PATCH/DELETE /api/admin/status-templates/{1}]
  // 用途：DM 權限的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminPatchDelete1(service, "/api/admin/status-templates/{1}", "status_templates", "status");

  // [功能備註｜玩家管理｜POST /api/admin/players/{1}/statuses]
  // 用途：建立/執行玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/players/{1}/statuses", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& userIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto userId = positiveId(userIdText, "玩家 ID 無效");
      const auto body = bodyOrEmpty(req);
      const auto templateId = intValue(body["status_template_id"], intValue(body["template_id"]));
      Json::Value extra; extra["user_id"] = Json::Int64(userId);
      Json::Value create = body;
      if (templateId > 0) {
        const auto rows = service->database()->execSqlSync("SELECT row_to_json(s)::text AS json_text FROM status_templates s WHERE id=$1", templateId);
        if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到狀態模板");
        const auto templ = rowJson(rows);
        create["status_template_id"] = Json::Int64(templateId);
        if (!create.isMember("name")) create["name"] = templ["name"];
        if (!create.isMember("description")) create["description"] = templ["description"];
        if (!create.isMember("remaining_rounds")) create["remaining_rounds"] = templ["duration_rounds"];
        if (!create.isMember("effects")) create["effects"] = templ["effects"];
      }
      // [功能備註｜狀態自動化] 不論由模板或手動建立，只要有剩餘回合數就預設隨回合倒數。
      if (!create.isMember("expires_by_round")) create["expires_by_round"] = intValue(create["remaining_rounds"]) > 0;
      Json::Value out; out["ok"] = true; out["status"] = genericInsert(service->database(), "character_statuses", create, extra); reply(cb, std::move(out));
    }, "套用狀態失敗");
  });

  // [功能備註｜玩家管理｜DELETE /api/admin/players/{1}/statuses/{2}]
  // 用途：刪除玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/players/{1}/statuses/{2}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& userIdText, const std::string& statusIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto rows = service->database()->execSqlSync("DELETE FROM character_statuses WHERE id=$1 AND user_id=$2 RETURNING id", positiveId(statusIdText), positiveId(userIdText));
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到狀態");
      Json::Value out; out["ok"] = true; reply(cb, std::move(out));
    }, "移除狀態失敗");
  });

  // Item templates and consumable use.
  // [功能備註｜道具／裝備／背包｜GET /api/item-templates]
  // 用途：讀取/顯示道具／裝備／背包相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/item-templates", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req); Json::Value out; out["items"] = genericList(service->database(), "item_templates", "name ASC"); reply(cb, std::move(out)); });
  });
  // [功能備註｜道具／裝備／背包｜GET /api/admin/item-templates]
  // 用途：讀取/顯示道具／裝備／背包相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/item-templates", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["items"] = genericList(service->database(), "item_templates", "name ASC"); reply(cb, std::move(out)); });
  });
  // [功能備註｜道具／裝備／背包｜POST /api/admin/item-templates]
  // 用途：建立/執行道具／裝備／背包相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/item-templates", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["ok"] = true; auto body=bodyOrEmpty(req);body["rank"]=normalizeContentRank(stringValue(body["rank"],"G"));out["item"] = genericInsert(service->database(), "item_templates", body); reply(cb, std::move(out)); });
  });
  // [功能備註｜道具／裝備／背包｜DELETE /api/admin/item-templates/{1}]
  // 用途：刪除道具／裝備／背包相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/item-templates/{1}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] { requireUser(service, req, true); genericDelete(service->database(), "item_templates", positiveId(idText)); Json::Value out; out["ok"] = true; reply(cb, std::move(out)); });
  });

  // [功能備註｜道具／裝備／背包｜POST /api/character/items/use]
  // 用途：建立/執行道具／裝備／背包相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/character/items/use", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto body = bodyOrEmpty(req);
      const auto templateId = intValue(body["template_id"], intValue(body["item_template_id"]));
      const auto rows = service->database()->execSqlSync("SELECT row_to_json(i)::text AS json_text FROM item_templates i WHERE id=$1", templateId);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到道具模板");
      const auto item = rowJson(rows);
      auto character = characterRaw(service->database(), user.id);
      auto inventory = inventoryOf(character);
      const auto name = stringValue(item["name"]);
      const auto category = stringValue(item["category"]);
      if (inventoryQuantity(inventory, name, category) <= 0 && inventoryQuantity(inventory, name) <= 0)
        throw ApiError(drogon::k400BadRequest, "背包中沒有這個道具");
      if (item.get("consume_on_use", true).asBool()) inventory = changeInventory(std::move(inventory), name, -1, category);
      const auto effects = item["effects"].isObject() ? item["effects"] : Json::Value{Json::objectValue};
      auto baseElementCaps = normalizedElementMap(character["element_storage_base_caps"]);
      bool hasPermanentElementGain = false;
      if (effects["element_storage_permanent_bonus"].isObject()) {
        for (const auto* element : kStoredElements) {
          if (intValue(effects["element_storage_permanent_bonus"][element]) > 0) {
            hasPermanentElementGain = true;
            break;
          }
        }
      }
      const auto magicPointGain = std::max<std::int64_t>(0, intValue(effects["magic_points"]));
      const auto ritualPointGain = std::max<std::int64_t>(0, intValue(effects["ritual_points"]));
      const auto ritualXpGain = std::max<std::int64_t>(0, intValue(effects["ritual_xp"]));
      if ((hasPermanentElementGain || magicPointGain > 0 || ritualPointGain > 0 || ritualXpGain > 0) &&
          !item.get("consume_on_use", true).asBool()) {
        throw ApiError(drogon::k400BadRequest, "永久擴容或學習點數道具必須設定為使用後消耗");
      }
      addElementCapBonus(baseElementCaps, effects["element_storage_permanent_bonus"]);
      const auto roomId = intValue(body["room_id"]);
      if (roomId > 0) {
        requireRoomAccess(service, user, roomId);
        Json::Value rulePayload; rulePayload["item_template_id"] = Json::Int64(templateId); rulePayload["item"] = item; rulePayload["item_name"] = name;
        auto attempt = evaluateRuleEvent(service, roomId, user.id, "ITEM_USE_ATTEMPT", rulePayload, false);
        if (attempt["blocked"].asBool()) throw ApiError(drogon::k403Forbidden, "此道具使用被規則阻止");
        const auto healHp = std::max<std::int64_t>(0, intValue(effects["heal_hp"], intValue(effects["hp"])));
        const auto healSpirit = std::max<std::int64_t>(0, intValue(effects["heal_spirit"]));
        if (healHp || healSpirit) service->database()->execSqlSync(
            "UPDATE room_members SET hp=LEAST(max_hp,hp+$1),spirit=LEAST(max_spirit,spirit+$2) WHERE room_id=$3 AND user_id=$4",
            healHp, healSpirit, roomId, user.id);
        Json::Value payload; payload["text"] = stringValue(character["name"], user.username) + " 使用道具「" + name + "」"; payload["item_template_id"] = Json::Int64(templateId); payload["item"] = item; service->addEvent(roomId, user.id, "item_use", payload);
        evaluateRuleEvent(service, roomId, user.id, "ITEM_USED", payload, false);
      }
      service->database()->execSqlSync(
          "UPDATE character_cards SET inventory=$1::jsonb,element_storage_base_caps=$2::jsonb,"
          "magic_points=GREATEST(0,magic_points+$3),ritual_points=GREATEST(0,ritual_points+$4),"
          "ritual_points_earned=ritual_points_earned+$4,updated_at=CURRENT_TIMESTAMP WHERE user_id=$5",
          compactJson(inventory), compactJson(baseElementCaps), magicPointGain, ritualPointGain, user.id);
      if(ritualXpGain>0)grantRitualXp(service->database(),user.id,ritualXpGain);
      auto updatedCharacter = characterRaw(service->database(), user.id);
      service->database()->execSqlSync(
          "UPDATE character_cards SET element_storage=$1::jsonb WHERE user_id=$2",
          compactJson(updatedCharacter["element_storage"]), user.id);
      Json::Value out; out["ok"] = true; out["item"] = item; out["character"] = updatedCharacter; if (roomId > 0) out["room"] = service->roomSnapshot(roomId); reply(cb, std::move(out));
    }, "使用道具失敗");
  });

  // Map nodes.
  // [功能備註｜房間／跑團｜GET /api/rooms/{1}/map-nodes]
  // 用途：讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/rooms/{1}/map-nodes", drogon::Get,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      requireRoomAccess(service, user, roomId);
      Json::Value out;
      const auto sql = user.isAdmin
          ? "SELECT row_to_json(n)::text AS json_text FROM room_map_nodes n WHERE room_id=$1 ORDER BY id"
          : "SELECT row_to_json(n)::text AS json_text FROM room_map_nodes n WHERE room_id=$1 AND hidden=FALSE ORDER BY id";
      out["nodes"] = selectRows1(service->database(), sql, roomId);
      out["map_nodes"] = out["nodes"];
      // [功能備註｜61-cpp.22 平面地圖讀取] 回傳玩家／隊伍有效節點，前端可直接標示「你在這裡」。
      const auto current = service->database()->execSqlSync(
          "SELECT COALESCE(t.current_map_node_id,rm.current_map_node_id,r.current_map_node_id) AS node_id "
          "FROM room_members rm JOIN rooms r ON r.id=rm.room_id "
          "LEFT JOIN room_team_members tm ON tm.room_id=rm.room_id AND tm.user_id=rm.user_id "
          "LEFT JOIN room_teams t ON t.id=tm.team_id WHERE rm.room_id=$1 AND rm.user_id=$2", roomId, user.id);
      out["current_map_node_id"] = (current.empty() || current[0]["node_id"].isNull())
          ? Json::Value(Json::nullValue) : Json::Value(Json::Int64(current[0]["node_id"].as<std::int64_t>()));
      const auto mapRows = service->database()->execSqlSync(
          "SELECT map_name,map_image_url,map_description,current_map_node_id FROM rooms WHERE id=$1", roomId);
      if (!mapRows.empty()) {
        Json::Value map;
        map["name"] = mapRows[0]["map_name"].as<std::string>();
        map["image_url"] = mapRows[0]["map_image_url"].as<std::string>();
        map["description"] = mapRows[0]["map_description"].as<std::string>();
        if (!mapRows[0]["current_map_node_id"].isNull()) map["scene_node_id"] = Json::Int64(mapRows[0]["current_map_node_id"].as<std::int64_t>());
        out["map"] = map;
      }
      reply(cb, std::move(out));
    }, "讀取地圖節點失敗");
  });

  // [功能備註｜房間／跑團｜POST /api/admin/rooms/{1}/map-nodes]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/rooms/{1}/map-nodes", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto roomId = positiveId(roomIdText);
      auto body = bodyOrEmpty(req);
      if (body.isMember("x_percent")) body["x_percent"] = std::clamp(body["x_percent"].asDouble(), 0.0, 100.0);
      if (body.isMember("y_percent")) body["y_percent"] = std::clamp(body["y_percent"].asDouble(), 0.0, 100.0);
      Json::Value extra; extra["room_id"] = Json::Int64(roomId);
      Json::Value out; out["ok"] = true; out["node"] = genericInsert(service->database(), "room_map_nodes", body, extra);
      const auto nodeId = intValue(out["node"]["id"]); if (nodeId > 0 && body.isMember("connections")) syncBidirectionalMapConnections(service, roomId, nodeId, body["connections"]);
      if (nodeId > 0) { const auto refreshed=service->database()->execSqlSync("SELECT row_to_json(n)::text AS json_text FROM room_map_nodes n WHERE id=$1",nodeId); if(!refreshed.empty()) out["node"]=rowJson(refreshed); }
      reply(cb, std::move(out));
    }, "建立地圖節點失敗");
  });

  // [功能備註｜房間／跑團｜POST /api/admin/rooms/{1}/map-nodes/{2}/enter]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/rooms/{1}/map-nodes/{2}/enter", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText, const std::string& nodeIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req, true);
      const auto roomId = positiveId(roomIdText);
      const auto nodeId = positiveId(nodeIdText);
      if (service->database()->execSqlSync("SELECT 1 FROM room_map_nodes WHERE id=$1 AND room_id=$2", nodeId, roomId).empty()) throw ApiError(drogon::k404NotFound, "找不到地圖節點");
      service->database()->execSqlSync("UPDATE rooms SET current_map_node_id=$1,updated_at=CURRENT_TIMESTAMP WHERE id=$2", nodeId, roomId);
      Json::Value payload; payload["text"] = "DM 將場景移動到新的地圖節點"; payload["node_id"] = Json::Int64(nodeId); service->addEvent(roomId, user.id, "map_enter", payload);
      Json::Value out; out["ok"] = true; out["room"] = service->roomSnapshot(roomId); reply(cb, std::move(out));
    }, "進入地圖節點失敗");
  });

  // [功能備註｜房間／跑團｜DELETE /api/admin/rooms/{1}/map-nodes/{2}]
  // 用途：刪除房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/rooms/{1}/map-nodes/{2}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& roomIdText, const std::string& nodeIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto roomId = positiveId(roomIdText);
      const auto nodeId = positiveId(nodeIdText);
      // [功能備註｜61-cpp.22.1 節點刪除 API] 先解除位置／世界物件／定時事件等引用，再刪除本體。
      prepareMapNodeDeletion(service, roomId, nodeId);
      const auto rows = service->database()->execSqlSync(
          "DELETE FROM room_map_nodes WHERE id=$1 AND room_id=$2 RETURNING id", nodeId, roomId);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到地圖節點");
      Json::Value out;
      out["ok"] = true;
      out["deleted_node_id"] = Json::Int64(nodeId);
      const auto scene = service->database()->execSqlSync("SELECT current_map_node_id FROM rooms WHERE id=$1", roomId);
      out["current_map_node_id"] = (scene.empty() || scene[0]["current_map_node_id"].isNull())
          ? Json::Value{Json::nullValue}
          : Json::Value{Json::Int64(scene[0]["current_map_node_id"].as<std::int64_t>())};
      reply(cb, std::move(out));
    }, "刪除地圖節點失敗");
  });

  // [功能備註｜房間／跑團｜PATCH /api/admin/rooms/{1}/map-nodes/{2}]
  // 用途：修改房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/rooms/{1}/map-nodes/{2}", drogon::Patch,
       [service](const Request& req, Callback& cb, const std::string& roomIdText, const std::string& nodeIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto roomId = positiveId(roomIdText); const auto nodeId = positiveId(nodeIdText);
      if (service->database()->execSqlSync("SELECT 1 FROM room_map_nodes WHERE id=$1 AND room_id=$2", nodeId, roomId).empty()) throw ApiError(drogon::k404NotFound, "找不到地圖節點");
      auto body = bodyOrEmpty(req);
      if (body.isMember("x_percent")) body["x_percent"] = std::clamp(body["x_percent"].asDouble(), 0.0, 100.0);
      if (body.isMember("y_percent")) body["y_percent"] = std::clamp(body["y_percent"].asDouble(), 0.0, 100.0);
      Json::Value out; out["ok"] = true; out["node"] = genericUpdate(service->database(), "room_map_nodes", nodeId, body);
      if (body.isMember("connections")) { syncBidirectionalMapConnections(service, roomId, nodeId, body["connections"]); const auto refreshed=service->database()->execSqlSync("SELECT row_to_json(n)::text AS json_text FROM room_map_nodes n WHERE id=$1",nodeId); if(!refreshed.empty()) out["node"]=rowJson(refreshed); }
      reply(cb, std::move(out));
    }, "更新地圖節點失敗");
  });

  // [功能備註｜房間／跑團｜PATCH /api/admin/rooms/{1}/world-location]
  // 用途：修改房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/rooms/{1}/world-location", drogon::Patch,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto roomId = positiveId(roomIdText);
      const auto body = bodyOrEmpty(req);
      const auto rows = service->database()->execSqlSync("SELECT id FROM rooms WHERE id=$1", roomId);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到房間");
      const auto cols = tableColumns(service->database(), "rooms");
      Json::Value filtered;
      for (const auto& name : std::array<const char*,5>{"map_name","map_image_url","map_description","current_map_node_id","time_hidden"}) if (body.isMember(name) && cols.contains(name)) filtered[name]=body[name];
      // Rooms use id PK, generic update works here.
      Json::Value out; out["ok"] = true; out["room_data"] = genericUpdate(service->database(), "rooms", roomId, filtered); out["room"] = service->roomSnapshot(roomId); reply(cb, std::move(out));
    }, "更新世界位置失敗");
  });

  // [功能備註｜房間／跑團｜PATCH /api/admin/rooms/{1}/map-nodes/{2}/noise]
  // 用途：修改房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/rooms/{1}/map-nodes/{2}/noise", drogon::Patch,
       [service](const Request& req, Callback& cb, const std::string& roomIdText, const std::string& nodeIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto roomId = positiveId(roomIdText); const auto nodeId = positiveId(nodeIdText);
      const auto body = bodyOrEmpty(req);
      const auto delta = intValue(body["delta"], intValue(body["noise"]));
      const auto rows = service->database()->execSqlSync("UPDATE room_map_nodes SET noise_level=GREATEST(0,noise_level+$1),updated_at=CURRENT_TIMESTAMP WHERE id=$2 AND room_id=$3 RETURNING row_to_json(room_map_nodes)::text AS json_text", delta, nodeId, roomId);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到地圖節點");
      Json::Value out; out["ok"] = true; out["node"] = rowJson(rows); reply(cb, std::move(out));
    }, "更新噪音失敗");
  });

  // [功能備註｜房間／跑團｜PATCH /api/admin/rooms/{1}/members/{2}/pollution]
  // 用途：修改房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/rooms/{1}/members/{2}/pollution", drogon::Patch,
       [service](const Request& req, Callback& cb, const std::string& roomIdText, const std::string& userIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto roomId = positiveId(roomIdText); const auto userId = positiveId(userIdText);
      if (service->database()->execSqlSync("SELECT 1 FROM room_members WHERE room_id=$1 AND user_id=$2", roomId, userId).empty()) throw ApiError(drogon::k404NotFound, "找不到房間玩家");
      const auto body = bodyOrEmpty(req);
      auto character = characterRaw(service->database(), userId);
      auto value = intValue(character["pollution"]);
      if (body.isMember("value")) value = intValue(body["value"]);
      if (body.isMember("delta")) value += intValue(body["delta"]);
      value = std::clamp<std::int64_t>(value, 0, 100000);
      service->database()->execSqlSync("UPDATE character_cards SET pollution=$1,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2", value, userId);
      Json::Value out; out["ok"] = true; out["pollution"] = Json::Int64(value); out["character"] = characterRaw(service->database(), userId); reply(cb, std::move(out));
    }, "更新污染失敗");
  });

  // Hidden rolls.
  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/hidden-roll]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/rooms/{1}/hidden-roll", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto roomId = positiveId(roomIdText); requireRoomAccess(service, user, roomId);
      const auto body = bodyOrEmpty(req);
      const auto character = characterRaw(service->database(), user.id);
      const auto checkName = trimmed(stringValue(body["check_name"], "暗骰"));
      auto target = intValue(body["target_value"]);
      if (target <= 0) {
        const auto attr = stringValue(body["attribute"], "spirit");
        target = intValue(character[attr], 0);
      }
      const auto roll = randomInt(1,100);
      std::string level = "失敗";
      if (roll == 1) level = "大成功";
      else if (roll <= std::max<std::int64_t>(1,target/5)) level = "極難成功";
      else if (roll <= std::max<std::int64_t>(1,target/2)) level = "困難成功";
      else if (roll <= target) level = "成功";
      const auto inserted = service->database()->execSqlSync(
          "INSERT INTO hidden_rolls(room_id,user_id,character_name,check_name,target_value,roll_value,success_level,note) VALUES($1,$2,$3,$4,$5,$6,$7,$8) RETURNING id",
          roomId,user.id,stringValue(character["name"],user.username),checkName,target,roll,level,stringValue(body["note"]));
      Json::Value out; out["ok"] = true; out["id"] = Json::Int64(inserted[0]["id"].as<std::int64_t>()); out["submitted"] = true; reply(cb, std::move(out));
    }, "暗骰失敗");
  });

  // [功能備註｜房間／跑團｜GET /api/admin/rooms/{1}/hidden-rolls]
  // 用途：讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/rooms/{1}/hidden-rolls", drogon::Get,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["rolls"] = selectRows1(service->database(), "SELECT row_to_json(h)::text AS json_text FROM hidden_rolls h WHERE room_id=$1 ORDER BY created_at DESC LIMIT 200", positiveId(roomIdText)); reply(cb, std::move(out)); });
  });

  // Teams.
  // [功能備註｜隊伍／位置／通訊｜GET /api/rooms/{1}/teams]
  // 用途：隊伍、個人位置、共享情報、可見聊天、邀請與通訊狀態一次載入。
  reg1("/api/rooms/{1}/teams", drogon::Get,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req); const auto roomId = positiveId(roomIdText); requireRoomAccess(service, user, roomId);
      Json::Value out;
      out["teams"] = selectRows1(service->database(),
          "SELECT row_to_json(x)::text AS json_text FROM ("
          "SELECT t.*,n.name AS map_node_name,COUNT(m.user_id)::INTEGER AS member_count,"
          "COALESCE(jsonb_agg(jsonb_build_object('user_id',m.user_id,'username',u.username,'character_name',c.name,'avatar_url',c.avatar_url) ORDER BY m.joined_at) FILTER (WHERE m.user_id IS NOT NULL),'[]'::jsonb) AS members "
          "FROM room_teams t LEFT JOIN room_map_nodes n ON n.id=t.current_map_node_id "
          "LEFT JOIN room_team_members m ON m.team_id=t.id LEFT JOIN users u ON u.id=m.user_id LEFT JOIN character_cards c ON c.user_id=m.user_id "
          "WHERE t.room_id=$1 GROUP BY t.id,n.name ORDER BY t.id) x", roomId);
      out["map_nodes"] = selectRows1(service->database(),
          user.isAdmin
            ? "SELECT row_to_json(n)::text AS json_text FROM room_map_nodes n WHERE room_id=$1 ORDER BY id"
            : "SELECT row_to_json(n)::text AS json_text FROM room_map_nodes n WHERE room_id=$1 AND hidden=FALSE ORDER BY id", roomId);
      const auto mine=service->database()->execSqlSync(
          "SELECT row_to_json(x)::text AS json_text FROM (SELECT t.*,n.name AS map_node_name FROM room_team_members m JOIN room_teams t ON t.id=m.team_id LEFT JOIN room_map_nodes n ON n.id=t.current_map_node_id WHERE m.room_id=$1 AND m.user_id=$2 LIMIT 1) x",
          roomId,user.id);
      out["my_team"] = mine.empty()?Json::nullValue:rowJson(mine);
      Json::Value details;details["shared_clues"]=Json::arrayValue;details["shared_rules"]=Json::arrayValue;details["shared_notes"]=Json::arrayValue;details["messages"]=Json::arrayValue;details["visions"]=Json::arrayValue;
      std::int64_t teamId=mine.empty()?0:intValue(rowJson(mine)["id"]);
      if(teamId>0){
        details["shared_clues"]=selectRows1(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT s.*,c.name,c.category,c.description,u.username AS shared_by_username,cc.name AS shared_by_character FROM team_shared_clues s JOIN clue_templates c ON c.id=s.clue_id LEFT JOIN users u ON u.id=s.shared_by_user_id LEFT JOIN character_cards cc ON cc.user_id=s.shared_by_user_id WHERE s.team_id=$1 ORDER BY s.shared_at DESC) x",teamId);
        details["shared_rules"]=selectRows1(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT s.*,r.name,CASE WHEN s.knowledge_level>=3 THEN r.rule_text WHEN s.knowledge_level=2 THEN COALESCE(NULLIF(r.public_hint,''),'殘缺規則情報') ELSE COALESCE(NULLIF(r.public_hint,''),'只知道這裡存在一條規則') END AS shared_text,u.username AS shared_by_username,cc.name AS shared_by_character FROM team_shared_rules s JOIN horror_rules r ON r.id=s.rule_id LEFT JOIN users u ON u.id=s.shared_by_user_id LEFT JOIN character_cards cc ON cc.user_id=s.shared_by_user_id WHERE s.team_id=$1 ORDER BY s.shared_at DESC) x",teamId);
        details["shared_notes"]=selectRows1(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT n.*,u.username AS shared_by_username,cc.name AS shared_by_character FROM team_shared_notes n LEFT JOIN users u ON u.id=n.shared_by_user_id LEFT JOIN character_cards cc ON cc.user_id=n.shared_by_user_id WHERE n.team_id=$1 ORDER BY n.created_at DESC) x",teamId);
        details["messages"]=selectRows2(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT m.*,u.username,cc.name AS character_name FROM team_messages m LEFT JOIN users u ON u.id=m.user_id LEFT JOIN character_cards cc ON cc.user_id=m.user_id WHERE m.team_id=$1 AND (jsonb_array_length(m.audience_user_ids)=0 OR m.audience_user_ids @> jsonb_build_array($2::BIGINT)) ORDER BY m.created_at) x",teamId,user.id);
      }
      if(user.isAdmin)details["visions"]=selectRows1(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT v.*,u.username,c.name AS character_name FROM personal_visions v JOIN users u ON u.id=v.user_id LEFT JOIN character_cards c ON c.user_id=v.user_id WHERE v.room_id=$1 ORDER BY v.created_at DESC LIMIT 200) x",roomId);
      else details["visions"]=selectRows2(service->database(),"SELECT row_to_json(v)::text AS json_text FROM personal_visions v WHERE room_id=$1 AND user_id=$2 ORDER BY created_at DESC",roomId,user.id);
      out["details"]=details;
      const auto locRows=service->database()->execSqlSync("SELECT COALESCE(t.current_map_node_id,rm.current_map_node_id,r.current_map_node_id) AS node_id FROM room_members rm JOIN rooms r ON r.id=rm.room_id LEFT JOIN room_team_members tm ON tm.room_id=rm.room_id AND tm.user_id=rm.user_id LEFT JOIN room_teams t ON t.id=tm.team_id WHERE rm.room_id=$1 AND rm.user_id=$2",roomId,user.id);
      if(locRows.empty()||locRows[0]["node_id"].isNull()) out["my_location_node_id"]=Json::nullValue; else out["my_location_node_id"]=Json::Int64(locRows[0]["node_id"].as<std::int64_t>());
      out["communication_audience"]=Json::arrayValue;for(const auto uid:communicationAudience(service,roomId,user.id))out["communication_audience"].append(Json::Int64(uid));
      out["invites"]=selectRows2(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT i.*,t.name AS team_name,u.username AS invited_by_username FROM room_team_invites i JOIN room_teams t ON t.id=i.team_id LEFT JOIN users u ON u.id=i.invited_by_user_id WHERE i.room_id=$1 AND i.target_user_id=$2 AND i.status='pending' ORDER BY i.created_at DESC) x",roomId,user.id);
      reply(cb, std::move(out));
    }, "讀取隊伍失敗");
  });

  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/teams]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/rooms/{1}/teams", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req); const auto roomId = positiveId(roomIdText); requireRoomAccess(service,user,roomId);
      auto body=bodyOrEmpty(req); if(!body.isMember("name")) body["name"]="隊伍"; Json::Value extra; extra["room_id"]=Json::Int64(roomId); extra["leader_user_id"]=Json::Int64(user.id);const auto loc=service->database()->execSqlSync("SELECT COALESCE(current_map_node_id,(SELECT current_map_node_id FROM rooms WHERE id=$1)) AS node_id FROM room_members WHERE room_id=$1 AND user_id=$2",roomId,user.id);if(!loc.empty()&&!loc[0]["node_id"].isNull())extra["current_map_node_id"]=Json::Int64(loc[0]["node_id"].as<std::int64_t>());
      auto team=genericInsert(service->database(),"room_teams",body,extra); const auto teamId=intValue(team["id"]);
      service->database()->execSqlSync("DELETE FROM room_team_members WHERE room_id=$1 AND user_id=$2",roomId,user.id);
      service->database()->execSqlSync("INSERT INTO room_team_members(team_id,room_id,user_id) VALUES($1,$2,$3)",teamId,roomId,user.id);
      Json::Value out; out["ok"]=true; out["team"]=team; reply(cb,std::move(out));
    },"建立隊伍失敗");
  });

  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/teams/{2}/join]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/rooms/{1}/teams/{2}/join",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& teamIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto teamId=positiveId(teamIdText);requireRoomAccess(service,user,roomId);if(service->database()->execSqlSync("SELECT 1 FROM room_teams WHERE id=$1 AND room_id=$2",teamId,roomId).empty())throw ApiError(drogon::k404NotFound,"找不到隊伍");service->database()->execSqlSync("DELETE FROM room_team_members WHERE room_id=$1 AND user_id=$2",roomId,user.id);service->database()->execSqlSync("INSERT INTO room_team_members(team_id,room_id,user_id) VALUES($1,$2,$3)",teamId,roomId,user.id);const auto node=service->database()->execSqlSync("SELECT current_map_node_id FROM room_teams WHERE id=$1",teamId);if(!node.empty()&&!node[0]["current_map_node_id"].isNull())service->database()->execSqlSync("UPDATE room_members SET current_map_node_id=$1 WHERE room_id=$2 AND user_id=$3",node[0]["current_map_node_id"].as<std::int64_t>(),roomId,user.id);Json::Value ruleEv;ruleEv["team_id"]=Json::Int64(teamId);evaluateRuleEvent(service,roomId,user.id,"TEAM_JOINED",ruleEv,false);Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"加入隊伍失敗");});

  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/teams/{2}/leave]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/rooms/{1}/teams/{2}/leave",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& teamIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto teamId=positiveId(teamIdText);requireRoomAccess(service,user,roomId);service->database()->execSqlSync("DELETE FROM room_team_members WHERE team_id=$1 AND room_id=$2 AND user_id=$3",teamId,roomId,user.id);Json::Value ruleEv;ruleEv["team_id"]=Json::Int64(teamId);evaluateRuleEvent(service,roomId,user.id,"TEAM_LEFT",ruleEv,false);Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"離開隊伍失敗");});

  // [功能備註｜61-cpp.22 隊伍節點移動｜POST /api/rooms/{1}/teams/{2}/move]
  // 隊伍點平面圖節點移動；有連線資料時只允許相鄰節點，並保留規則引擎 PLAYER_MOVE_*。
  reg2("/api/rooms/{1}/teams/{2}/move",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& teamIdText){
    runHandler(cb,[&]{
      const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto teamId=positiveId(teamIdText);requireRoomAccess(service,user,roomId);
      const auto nodeId=intValue(bodyOrEmpty(req)["node_id"]);if(nodeId<=0)throw ApiError(drogon::k400BadRequest,"請指定地圖節點");
      if(service->database()->execSqlSync("SELECT 1 FROM room_team_members WHERE team_id=$1 AND user_id=$2",teamId,user.id).empty()&&!user.isAdmin)throw ApiError(drogon::k403Forbidden,"你不在這個隊伍");
      const auto target=service->database()->execSqlSync("SELECT 1 FROM room_map_nodes WHERE id=$1 AND room_id=$2 AND hidden=FALSE",nodeId,roomId);if(target.empty())throw ApiError(drogon::k404NotFound,"找不到可前往地點");
      const auto team=service->database()->execSqlSync("SELECT current_map_node_id FROM room_teams WHERE id=$1 AND room_id=$2",teamId,roomId);if(team.empty())throw ApiError(drogon::k404NotFound,"找不到隊伍");
      if(!team[0]["current_map_node_id"].isNull()&&!mapNodesAdjacent(service,roomId,team[0]["current_map_node_id"].as<std::int64_t>(),nodeId))throw ApiError(drogon::k400BadRequest,"此節點與隊伍目前位置沒有連線");
      Json::Value payload=ruleMapNodePayload(service,roomId,nodeId);payload["text"]="隊伍沿平面地圖節點移動";payload["team_id"]=Json::Int64(teamId);
      const auto movedMembers=service->database()->execSqlSync("SELECT user_id FROM room_team_members WHERE team_id=$1 ORDER BY user_id",teamId);
      for(const auto& moved:movedMembers){const auto movedUserId=moved["user_id"].as<std::int64_t>();auto attempt=evaluateRuleEvent(service,roomId,movedUserId,"PLAYER_MOVE_ATTEMPT",payload,false);if(attempt["blocked"].asBool())throw ApiError(drogon::k403Forbidden,"隊伍中有成員受到規則限制，無法前往此地點");}
      service->database()->execSqlSync("UPDATE room_teams SET current_map_node_id=$1,updated_at=CURRENT_TIMESTAMP WHERE id=$2 AND room_id=$3",nodeId,teamId,roomId);
      service->database()->execSqlSync("UPDATE room_members SET current_map_node_id=$1 WHERE room_id=$2 AND user_id IN (SELECT user_id FROM room_team_members WHERE team_id=$3)",nodeId,roomId,teamId);
      service->addEvent(roomId,user.id,"team_move",payload);for(const auto& moved:movedMembers)evaluateRuleEvent(service,roomId,moved["user_id"].as<std::int64_t>(),"PLAYER_MOVED",payload,false);
      Json::Value out;out["ok"]=true;out["node_id"]=Json::Int64(nodeId);reply(cb,std::move(out));
    },"移動隊伍失敗");
  });

  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/teams/{2}/share-clue]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/rooms/{1}/teams/{2}/share-clue",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& teamIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto teamId=positiveId(teamIdText);requireRoomAccess(service,user,roomId);const auto clueId=intValue(bodyOrEmpty(req)["clue_id"]);if(clueId<=0||service->database()->execSqlSync("SELECT 1 FROM character_clues WHERE clue_id=$1 AND user_id=$2",clueId,user.id).empty())throw ApiError(drogon::k403Forbidden,"你沒有這條線索");service->database()->execSqlSync("INSERT INTO team_shared_clues(team_id,clue_id,shared_by_user_id,note) VALUES($1,$2,$3,$4) ON CONFLICT(team_id,clue_id) DO UPDATE SET note=EXCLUDED.note",teamId,clueId,user.id,stringValue(bodyOrEmpty(req)["note"]));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"分享線索失敗");});

  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/teams/{2}/share-rule]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/rooms/{1}/teams/{2}/share-rule",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& teamIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto teamId=positiveId(teamIdText);requireRoomAccess(service,user,roomId);const auto ruleId=intValue(bodyOrEmpty(req)["rule_id"]);const auto known=service->database()->execSqlSync("SELECT knowledge_level FROM rule_discoveries WHERE rule_id=$1 AND user_id=$2",ruleId,user.id);if(known.empty())throw ApiError(drogon::k403Forbidden,"你尚未知道這條規則");service->database()->execSqlSync("INSERT INTO team_shared_rules(team_id,rule_id,knowledge_level,shared_by_user_id,note) VALUES($1,$2,$3,$4,$5) ON CONFLICT(team_id,rule_id) DO UPDATE SET knowledge_level=GREATEST(team_shared_rules.knowledge_level,EXCLUDED.knowledge_level),note=EXCLUDED.note,updated_at=CURRENT_TIMESTAMP",teamId,ruleId,known[0]["knowledge_level"].as<std::int64_t>(),user.id,stringValue(bodyOrEmpty(req)["note"]));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"分享規則失敗");});

  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/teams/{2}/chat]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/rooms/{1}/teams/{2}/chat",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& teamIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto teamId=positiveId(teamIdText);requireRoomAccess(service,user,roomId);const auto body=bodyOrEmpty(req);const auto text=trimmed(stringValue(body.isMember("text")?body["text"]:body["content"]));if(text.empty())throw ApiError(drogon::k400BadRequest,"訊息不能為空");Json::Value audience{Json::arrayValue};for(const auto uid:communicationAudience(service,roomId,user.id)){if(!service->database()->execSqlSync("SELECT 1 FROM room_team_members WHERE team_id=$1 AND user_id=$2",teamId,uid).empty())audience.append(Json::Int64(uid));}service->database()->execSqlSync("INSERT INTO team_messages(team_id,user_id,content,audience_user_ids) VALUES($1,$2,$3,$4::jsonb)",teamId,user.id,text.substr(0,std::min<std::size_t>(2000,text.size())),compactJson(audience));Json::Value out;out["ok"]=true;out["audience_user_ids"]=audience;reply(cb,std::move(out));},"隊伍聊天失敗");});

  // Personal visions.
  // [功能備註｜房間／跑團｜POST /api/admin/rooms/{1}/visions]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/rooms/{1}/visions",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto roomId=positiveId(roomIdText);auto body=bodyOrEmpty(req);const auto userId=intValue(body["user_id"]);if(userId<=0)throw ApiError(drogon::k400BadRequest,"請指定玩家");Json::Value extra;extra["room_id"]=Json::Int64(roomId);extra["user_id"]=Json::Int64(userId);Json::Value out;out["ok"]=true;out["vision"]=genericInsert(service->database(),"personal_visions",body,extra);reply(cb,std::move(out));},"建立私人視野失敗");});

  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/visions/{2}/read]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/rooms/{1}/visions/{2}/read",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& visionIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto visionId=positiveId(visionIdText);requireRoomAccess(service,user,roomId);const auto rows=service->database()->execSqlSync("UPDATE personal_visions SET is_read=TRUE WHERE id=$1 AND room_id=$2 AND user_id=$3 RETURNING row_to_json(personal_visions)::text AS json_text",visionId,roomId,user.id);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到私人視野");Json::Value out;out["ok"]=true;out["vision"]=rowJson(rows);reply(cb,std::move(out));},"閱讀私人視野失敗");});

  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/visions/{2}/share]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/rooms/{1}/visions/{2}/share",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& visionIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto visionId=positiveId(visionIdText);requireRoomAccess(service,user,roomId);const auto visionRows=service->database()->execSqlSync("SELECT title,content,shareable FROM personal_visions WHERE id=$1 AND room_id=$2 AND user_id=$3",visionId,roomId,user.id);if(visionRows.empty())throw ApiError(drogon::k404NotFound,"找不到私人視野");if(!visionRows[0]["shareable"].as<bool>())throw ApiError(drogon::k403Forbidden,"這個視野不能分享");auto teamId=intValue(bodyOrEmpty(req)["team_id"]);if(teamId<=0){const auto teams=service->database()->execSqlSync("SELECT team_id FROM room_team_members WHERE room_id=$1 AND user_id=$2",roomId,user.id);if(teams.empty())throw ApiError(drogon::k400BadRequest,"你目前沒有隊伍");teamId=teams[0]["team_id"].as<std::int64_t>();}service->database()->execSqlSync("INSERT INTO team_shared_notes(team_id,shared_by_user_id,title,content,note_type,source_vision_id) VALUES($1,$2,$3,$4,'vision',$5)",teamId,user.id,visionRows[0]["title"].as<std::string>(),visionRows[0]["content"].as<std::string>(),visionId);Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"分享視野失敗");});

  // [功能備註｜房間／跑團｜GET /api/rooms/{1}/world-systems]
  // 用途：讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/rooms/{1}/world-systems", drogon::Get,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req); const auto roomId = positiveId(roomIdText); requireRoomAccess(service,user,roomId);
      Json::Value out;
      out["scheduled_events"] = selectRows1(service->database(),"SELECT row_to_json(e)::text AS json_text FROM scheduled_world_events e WHERE room_id=$1 AND active=TRUE ORDER BY trigger_day,trigger_minute,id",roomId);
      out["chases"] = selectRows1(service->database(),"SELECT row_to_json(c)::text AS json_text FROM room_chases c WHERE room_id=$1 ORDER BY created_at DESC",roomId);
      out["containers"] = selectRows1(service->database(),user.isAdmin?"SELECT row_to_json(c)::text AS json_text FROM search_containers c WHERE room_id=$1 AND active=TRUE ORDER BY id":"SELECT row_to_json(c)::text AS json_text FROM search_containers c WHERE room_id=$1 AND active=TRUE AND hidden=FALSE ORDER BY id",roomId);
      out["npc_relations"] = selectRows1(service->database(),"SELECT row_to_json(r)::text AS json_text FROM npc_relationships r WHERE room_id=$1 ORDER BY npc_template_id,user_id",roomId);
      reply(cb,std::move(out));
    },"讀取世界系統失敗");
  });

  // [功能備註｜房間／跑團｜POST /api/admin/rooms/{1}/scheduled-events]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/rooms/{1}/scheduled-events",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{requireUser(service,req,true);Json::Value extra;extra["room_id"]=Json::Int64(positiveId(roomIdText));Json::Value out;out["ok"]=true;out["event"]=genericInsert(service->database(),"scheduled_world_events",bodyOrEmpty(req),extra);reply(cb,std::move(out));},"建立排程事件失敗");});
  // [功能備註｜房間／跑團｜DELETE /api/admin/rooms/{1}/scheduled-events/{2}]
  // 用途：刪除房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/rooms/{1}/scheduled-events/{2}",drogon::Delete,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& eventIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto rows=service->database()->execSqlSync("DELETE FROM scheduled_world_events WHERE id=$1 AND room_id=$2 RETURNING id",positiveId(eventIdText),positiveId(roomIdText));if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到排程事件");Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"刪除排程事件失敗");});

  // [功能備註｜房間／跑團｜POST /api/admin/rooms/{1}/chases]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/rooms/{1}/chases",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto roomId=positiveId(roomIdText);Json::Value extra;extra["room_id"]=Json::Int64(roomId);Json::Value out;out["ok"]=true;out["chase"]=genericInsert(service->database(),"room_chases",bodyOrEmpty(req),extra);reply(cb,std::move(out));},"建立追逐失敗");});

  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/chases/{2}/action]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/rooms/{1}/chases/{2}/action",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& chaseIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto chaseId=positiveId(chaseIdText);requireRoomAccess(service,user,roomId);const auto rows=service->database()->execSqlSync("SELECT row_to_json(c)::text AS json_text FROM room_chases c WHERE id=$1 AND room_id=$2 AND status='active'",chaseId,roomId);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到進行中的追逐");auto chase=rowJson(rows);const auto body=bodyOrEmpty(req);const auto action=stringValue(body["action"],"run");const auto character=characterRaw(service->database(),user.id);const auto stat=action=="hide"?intValue(character["spirit"]):intValue(character["agility"]);const auto roll=randomInt(1,100);const bool success=roll<=std::max<std::int64_t>(1,stat);auto distance=intValue(chase["distance"],3);distance+=success?1:-1;distance=std::max<std::int64_t>(0,distance);auto log=chase["log"].isArray()?chase["log"]:Json::Value{Json::arrayValue};Json::Value entry;entry["user_id"]=Json::Int64(user.id);entry["action"]=action;entry["roll"]=roll;entry["target"]=Json::Int64(stat);entry["success"]=success;entry["distance"]=Json::Int64(distance);log.append(entry);auto status=distance>=intValue(chase["escape_distance"],6)?"escaped":distance<=0?"caught":"active";service->database()->execSqlSync("UPDATE room_chases SET distance=$1,status=$2,round=round+1,last_actor_user_id=$3,log=$4::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=$5",distance,status,user.id,compactJson(log),chaseId);Json::Value out;out["ok"]=true;out["success"]=success;out["status"]=status;out["distance"]=Json::Int64(distance);out["roll"]=roll;reply(cb,std::move(out));},"追逐行動失敗");});

  // [功能備註｜房間／跑團｜POST /api/admin/rooms/{1}/chases/{2}/stop]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/rooms/{1}/chases/{2}/stop",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& chaseIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto rows=service->database()->execSqlSync("UPDATE room_chases SET status='stopped',updated_at=CURRENT_TIMESTAMP WHERE id=$1 AND room_id=$2 RETURNING id",positiveId(chaseIdText),positiveId(roomIdText));if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到追逐");Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"停止追逐失敗");});

  // [功能備註｜怪物｜POST /api/admin/rooms/{1}/monsters/{2}/boss-next]
  // 用途：建立/執行怪物相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/rooms/{1}/monsters/{2}/boss-next",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& monsterIdText){runHandler(cb,[&]{const auto user=requireUser(service,req,true);const auto roomId=positiveId(roomIdText);const auto monsterId=positiveId(monsterIdText);const auto body=bodyOrEmpty(req);const auto name=stringValue(body["name"],stringValue(body["phase_name"],"下一階段"));const auto rows=service->database()->execSqlSync("UPDATE room_monsters SET boss_phase_index=boss_phase_index+1,boss_phase_name=$1 WHERE id=$2 AND room_id=$3 RETURNING boss_phase_index",name,monsterId,roomId);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到怪物");Json::Value payload;payload["text"]="Boss 進入「"+name+"」";payload["monster_id"]=Json::Int64(monsterId);payload["phase_index"]=Json::Int64(rows[0]["boss_phase_index"].as<std::int64_t>());service->addEvent(roomId,user.id,"boss_phase",payload);Json::Value out;out["ok"]=true;out["room"]=service->roomSnapshot(roomId);reply(cb,std::move(out));},"切換 Boss 階段失敗");});

  // Search containers.
  // [功能備註｜房間／跑團｜GET /api/rooms/{1}/containers]
  // 用途：讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/rooms/{1}/containers",drogon::Get,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);requireRoomAccess(service,user,roomId);Json::Value out;out["containers"]=selectRows1(service->database(),user.isAdmin?"SELECT row_to_json(c)::text AS json_text FROM search_containers c WHERE room_id=$1 AND active=TRUE ORDER BY id":"SELECT row_to_json(c)::text AS json_text FROM search_containers c WHERE room_id=$1 AND active=TRUE AND hidden=FALSE ORDER BY id",roomId);reply(cb,std::move(out));},"讀取容器失敗");});
  // [功能備註｜房間／跑團｜POST /api/admin/rooms/{1}/containers]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/rooms/{1}/containers",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{requireUser(service,req,true);Json::Value extra;extra["room_id"]=Json::Int64(positiveId(roomIdText));Json::Value out;out["ok"]=true;out["container"]=genericInsert(service->database(),"search_containers",bodyOrEmpty(req),extra);reply(cb,std::move(out));},"建立容器失敗");});
  // [功能備註｜房間／跑團｜DELETE /api/admin/rooms/{1}/containers/{2}]
  // 用途：刪除房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/rooms/{1}/containers/{2}",drogon::Delete,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& containerIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto rows=service->database()->execSqlSync("DELETE FROM search_containers WHERE id=$1 AND room_id=$2 RETURNING id",positiveId(containerIdText),positiveId(roomIdText));if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到容器");Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"刪除容器失敗");});

  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/containers/{2}/search]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/rooms/{1}/containers/{2}/search",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& containerIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto containerId=positiveId(containerIdText);requireRoomAccess(service,user,roomId);const auto rows=service->database()->execSqlSync("SELECT row_to_json(c)::text AS json_text FROM search_containers c WHERE id=$1 AND room_id=$2 AND active=TRUE",containerId,roomId);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到容器");const auto container=rowJson(rows);const auto character=characterRaw(service->database(),user.id);const auto attr=stringValue(container["search_attribute"],"spirit");const auto target=intValue(character[attr]);const auto dc=intValue(container["search_dc"],50);const auto roll=randomInt(1,100);const bool success=roll<=std::clamp<std::int64_t>(target-dc+50,1,99);auto found=success&&container["loot"].isArray()?container["loot"]:Json::Value{Json::arrayValue};const auto existing=service->database()->execSqlSync("SELECT claimed FROM search_records WHERE container_id=$1 AND user_id=$2",containerId,user.id);if(!existing.empty()&&existing[0]["claimed"].as<bool>()&&container.get("one_time",true).asBool())found=Json::arrayValue;auto claimed=false;if(success&&!found.empty()){auto inventory=inventoryOf(character);for(const auto& item:found){const auto name=item.isString()?item.asString():stringValue(item["name"]);const auto category=item.isObject()?stringValue(item["category"]):std::string{};const auto qty=item.isObject()?std::max<std::int64_t>(1,intValue(item["quantity"],1)):1;if(!name.empty())inventory=changeInventory(std::move(inventory),name,qty,category,item);}service->database()->execSqlSync("UPDATE character_cards SET inventory=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",compactJson(inventory),user.id);claimed=true;}service->database()->execSqlSync("INSERT INTO search_records(container_id,user_id,attempts,success,claimed,last_roll,last_target,found) VALUES($1,$2,1,$3,$4,$5,$6,$7::jsonb) ON CONFLICT(container_id,user_id) DO UPDATE SET attempts=search_records.attempts+1,success=search_records.success OR EXCLUDED.success,claimed=search_records.claimed OR EXCLUDED.claimed,last_roll=EXCLUDED.last_roll,last_target=EXCLUDED.last_target,found=EXCLUDED.found,updated_at=CURRENT_TIMESTAMP",containerId,user.id,success,claimed,roll,target,compactJson(found));Json::Value out;out["ok"]=true;out["success"]=success;out["roll"]=roll;out["target"]=Json::Int64(target);out["found"]=found;out["claimed"]=claimed;out["character"]=characterRaw(service->database(),user.id);reply(cb,std::move(out));},"搜索容器失敗");});

  // NPC relationship table.
  // [功能備註｜NPC｜GET /api/rooms/{1}/npc-relations]
  // 用途：讀取/顯示NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/rooms/{1}/npc-relations",drogon::Get,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);requireRoomAccess(service,user,roomId);Json::Value out;out["relations"]=selectRows2(service->database(),"SELECT row_to_json(r)::text AS json_text FROM npc_relationships r WHERE room_id=$1 AND user_id=$2 ORDER BY npc_template_id",roomId,user.id);reply(cb,std::move(out));},"讀取 NPC 關係失敗");});

  // [功能備註｜NPC｜PATCH /api/admin/rooms/{1}/npc-relations]
  // 用途：修改NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/rooms/{1}/npc-relations",drogon::Patch,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto roomId=positiveId(roomIdText);const auto body=bodyOrEmpty(req);const auto npcId=intValue(body["npc_template_id"],intValue(body["npc_id"]));const auto userId=intValue(body["user_id"]);if(npcId<=0||userId<=0)throw ApiError(drogon::k400BadRequest,"請指定 NPC 與玩家");const auto affinity=intValue(body["affinity"]);const auto trust=intValue(body["trust"]);const auto fear=intValue(body["fear"]);const auto hostility=intValue(body["hostility"]);const auto memories=body["memories"].isArray()?body["memories"]:Json::Value{Json::arrayValue};service->database()->execSqlSync("INSERT INTO npc_relationships(room_id,npc_template_id,user_id,affinity,trust,fear,hostility,memories) VALUES($1,$2,$3,$4,$5,$6,$7,$8::jsonb) ON CONFLICT(room_id,npc_template_id,user_id) DO UPDATE SET affinity=EXCLUDED.affinity,trust=EXCLUDED.trust,fear=EXCLUDED.fear,hostility=EXCLUDED.hostility,memories=EXCLUDED.memories,updated_at=CURRENT_TIMESTAMP",roomId,npcId,userId,affinity,trust,fear,hostility,compactJson(memories));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"更新 NPC 關係失敗");});

  // [功能備註｜房間／跑團｜POST /api/admin/rooms/{1}/control]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/rooms/{1}/control",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{const auto user=requireUser(service,req,true);const auto roomId=positiveId(roomIdText);const auto body=bodyOrEmpty(req);Json::Value payload=body;payload["text"]=stringValue(body["text"],stringValue(body["action"],"DM 控制指令"));service->addEvent(roomId,user.id,"control",payload);Json::Value out;out["ok"]=true;out["room"]=service->roomSnapshot(roomId);reply(cb,std::move(out));},"執行 DM 控制失敗");});

  // Room savepoints.
  // [功能備註｜房間／跑團｜GET /api/admin/rooms/{1}/savepoints]
  // 用途：讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/rooms/{1}/savepoints",drogon::Get,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{requireUser(service,req,true);Json::Value out;out["savepoints"]=selectRows1(service->database(),"SELECT row_to_json(s)::text AS json_text FROM room_savepoints s WHERE room_id=$1 ORDER BY created_at DESC LIMIT 100",positiveId(roomIdText));reply(cb,std::move(out));},"讀取房間存檔點失敗");});

  // [功能備註｜房間／跑團｜POST /api/admin/rooms/{1}/savepoints]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/rooms/{1}/savepoints",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{const auto user=requireUser(service,req,true);const auto roomId=positiveId(roomIdText);const auto body=bodyOrEmpty(req);Json::Value snapshot=service->roomSnapshot(roomId);snapshot["map_nodes"]=selectRows1(service->database(),"SELECT row_to_json(n)::text AS json_text FROM room_map_nodes n WHERE room_id=$1 ORDER BY id",roomId);snapshot["dungeons"]=selectRows1(service->database(),"SELECT row_to_json(d)::text AS json_text FROM room_dungeons d WHERE room_id=$1 ORDER BY dungeon_template_id",roomId);snapshot["teams"]=selectRows1(service->database(),"SELECT row_to_json(t)::text AS json_text FROM room_teams t WHERE room_id=$1 ORDER BY id",roomId);snapshot["containers"]=selectRows1(service->database(),"SELECT row_to_json(c)::text AS json_text FROM search_containers c WHERE room_id=$1 ORDER BY id",roomId);const auto inserted=service->database()->execSqlSync("INSERT INTO room_savepoints(room_id,label,snapshot,created_by) VALUES($1,$2,$3::jsonb,$4) RETURNING row_to_json(room_savepoints)::text AS json_text",roomId,stringValue(body["label"],"手動存檔"),compactJson(snapshot),user.id);Json::Value out;out["ok"]=true;out["savepoint"]=rowJson(inserted);reply(cb,std::move(out));},"建立房間存檔點失敗");});

  // [功能備註｜房間／跑團｜POST /api/admin/rooms/{1}/savepoints/{2}/restore]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/rooms/{1}/savepoints/{2}/restore",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& savepointIdText){runHandler(cb,[&]{const auto user=requireUser(service,req,true);const auto roomId=positiveId(roomIdText);const auto savepointId=positiveId(savepointIdText);const auto rows=service->database()->execSqlSync("SELECT snapshot::text AS snapshot_text FROM room_savepoints WHERE id=$1 AND room_id=$2",savepointId,roomId);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到房間存檔點");const auto snapshot=parseJson(rows[0]["snapshot_text"].as<std::string>());const auto room=snapshot["room"];if(room.isObject()){service->database()->execSqlSync("UPDATE rooms SET name=$1,status=$2,round=$3,current_actor_id=$4,saved=$5,map_name=$6,map_image_url=$7,map_description=$8,game_day=$9,game_hour=$10,game_minute=$11,battle_active=$12,current_actor_type=$13,current_actor_ref_id=$14,combat_order=$15::jsonb,combat_index=$16,updated_at=CURRENT_TIMESTAMP WHERE id=$17",stringValue(room["name"],"房間"),stringValue(room["status"],"lobby"),std::max<std::int64_t>(1,intValue(room["round"],1)),intValue(room["current_actor_id"]),room.get("saved",false).asBool(),stringValue(room["map_name"],"未設定地點"),stringValue(room["map_image_url"]),stringValue(room["map_description"]),std::max<std::int64_t>(1,intValue(room["game_day"],1)),std::clamp<std::int64_t>(intValue(room["game_hour"],8),0,23),std::clamp<std::int64_t>(intValue(room["game_minute"]),0,59),room.get("battle_active",false).asBool(),stringValue(room["current_actor_type"],"player"),intValue(room["current_actor_ref_id"]),compactJson(room["combat_order"].isArray()?room["combat_order"]:Json::Value{Json::arrayValue}),std::max<std::int64_t>(0,intValue(room["combat_index"])),roomId);}if(snapshot["members"].isArray())for(const auto&m:snapshot["members"]){const auto uid=intValue(m["user_id"],intValue(m["id"]));if(uid>0)service->database()->execSqlSync("UPDATE room_members SET display_name=$1,hp=$2,max_hp=$3,spirit=$4,max_spirit=$5,current_sanity=$6,max_sanity=$7,combat_state=$8,combat_reaction=$9,turn_actions_remaining=$10 WHERE room_id=$11 AND user_id=$12",stringValue(m["display_name"],"玩家"),std::max<std::int64_t>(0,intValue(m["hp"])),std::max<std::int64_t>(1,intValue(m["max_hp"],1)),std::max<std::int64_t>(0,intValue(m["spirit"])),std::max<std::int64_t>(0,intValue(m["max_spirit"])),std::max<std::int64_t>(0,intValue(m["current_sanity"])),std::max<std::int64_t>(0,intValue(m["max_sanity"])),stringValue(m["combat_state"],"active"),stringValue(m["combat_reaction"],"dodge"),std::max<std::int64_t>(0,intValue(m["turn_actions_remaining"])),roomId,uid);}Json::Value payload;payload["text"]="DM 還原房間存檔點";payload["savepoint_id"]=Json::Int64(savepointId);service->addEvent(roomId,user.id,"restore",payload);Json::Value out;out["ok"]=true;out["room"]=service->roomSnapshot(roomId);reply(cb,std::move(out));},"還原房間存檔點失敗");});

  // [功能備註｜房間／跑團｜DELETE /api/admin/rooms/{1}/savepoints/{2}]
  // 用途：刪除房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/rooms/{1}/savepoints/{2}",drogon::Delete,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& savepointIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto rows=service->database()->execSqlSync("DELETE FROM room_savepoints WHERE id=$1 AND room_id=$2 RETURNING id",positiveId(savepointIdText),positiveId(roomIdText));if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到房間存檔點");Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"刪除房間存檔點失敗");});

  // [功能備註｜房間／跑團｜GET /api/admin/rooms/{1}/timeline]
  // 用途：讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/rooms/{1}/timeline",drogon::Get,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto roomId=positiveId(roomIdText);Json::Value out;out["events"]=selectRows1(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT e.*,u.username FROM events e LEFT JOIN users u ON u.id=e.user_id WHERE e.room_id=$1 ORDER BY e.id DESC LIMIT 500) x",roomId);reply(cb,std::move(out));},"讀取時間線失敗");});

  // V39-compatible backup envelope. Credentials and runtime secrets are never exported.
  // [功能備註｜存檔／備份／稽核｜GET /api/admin/backups/site/export]
  // 用途：讀取/顯示存檔／備份／稽核相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/backups/site/export", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value tables, counts;
      tables["users"] = selectRows(service->database(), "SELECT row_to_json(x)::text AS json_text FROM (SELECT id,username,is_admin,created_at FROM users ORDER BY id) x");
      counts["users"] = static_cast<Json::UInt64>(tables["users"].size());
      const std::array<const char*,103> names={"rooms","room_members","events","character_cards","character_room_bindings","extra_skills","bloodline_templates","summon_templates","summon_instances","recipes","profession_templates","skill_templates","shop_categories","shop_items","monster_templates","npc_templates","dungeon_templates","room_monsters","monster_discoveries","room_npcs","room_dungeons","combat_entity_statuses","combat_status_instances","combat_field_effects","clue_templates","character_clues","status_templates","character_statuses","room_map_nodes","hidden_rolls","item_templates","horror_rules","rule_discoveries","room_teams","room_team_members","team_shared_clues","team_shared_rules","personal_visions","team_shared_notes","team_messages","scheduled_world_events","room_chases","npc_relationships","search_containers","search_records","room_savepoints","character_savepoints","audit_logs","map_node_discoveries","npc_discoveries","item_identifications","clue_combinations","clue_combination_unlocks","great_way_templates","faith_deities","character_great_ways","character_faiths","custom_wheels","wheel_spins","faction_templates","character_reputations","npc_dialogue_entries","npc_dialogue_history","npc_shop_items","npc_shop_stock","faction_relations","tasks","task_participants","content_tags","entity_tags","user_favorites","user_notifications","codex_entries","codex_unlocks","shop_transactions","magic_studies","ritual_studies","ritual_instances","character_magics","character_rituals","character_ritual_research","rulebook_entries","room_team_invites","rule_engine_rules","rule_engine_rule_state","rule_engine_player_state","rule_engine_logs","avatar_images","portrait_images","character_portrait_variants","npc_portrait_variants","knowledge_shop_items","knowledge_shop_purchases","npc_task_offers","npc_avatar_images","special_currency_types","character_special_currencies","equipment_slot_types","affix_definitions","affix_pools","affix_pool_entries","reforge_profiles","affix_targets"};
      for(const auto* table:names){tables[table]=genericList(service->database(),table,"");counts[table]=static_cast<Json::UInt64>(tables[table].size());}
      Json::Value target; target["id"]="site"; target["name"]="全站資料封存";
      Json::Value data; data["mode"]="export_only"; data["credential_policy"]="passwords, tokens and server secrets are never exported"; data["restore_policy"]="site archives require a database administrator and are not restorable from the web interface"; data["counts"]=counts; data["tables"]=tables;
      reply(cb, createBackupEnvelope("site", target, data));
    }, "匯出全站備份失敗");
  });

  // [功能備註｜房間／跑團｜GET /api/admin/backups/rooms/{1}/export]
  // 用途：讀取/顯示房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/backups/rooms/{1}/export", drogon::Get,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true); const auto roomId=positiveId(roomIdText);
      const auto checkpoint=buildRoomBackupCheckpoint(service,roomId);
      Json::Value target; target["id"]=Json::Int64(roomId); target["name"]=checkpoint["room"]["name"]; target["code"]=checkpoint["room"]["code"];
      Json::Value data; data["mode"]="existing_state_restore"; data["checkpoint"]=checkpoint;
      reply(cb, createBackupEnvelope("room",target,data));
    }, "匯出房間備份失敗");
  });

  // [功能備註｜存檔／備份／稽核｜GET /api/admin/backups/characters/{1}/export]
  // 用途：讀取/顯示存檔／備份／稽核相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/backups/characters/{1}/export", drogon::Get,
       [service](const Request& req, Callback& cb, const std::string& userIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true); const auto userId=positiveId(userIdText);
      const auto checkpoint=buildCharacterBackupCheckpoint(service->database(),userId);
      Json::Value target; target["id"]=Json::Int64(userId); target["username"]=checkpoint["user"]["username"]; target["name"]=checkpoint["character"]["name"];
      Json::Value data; data["mode"]="exact_character_restore"; data["checkpoint"]=checkpoint;
      reply(cb, createBackupEnvelope("character",target,data));
    }, "匯出角色備份失敗");
  });

  // [功能備註｜存檔／備份／稽核｜GET /api/admin/backups/character-savepoints]
  // 用途：讀取/顯示存檔／備份／稽核相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/backups/character-savepoints", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service,req,true); Json::Value out;
      out["savepoints"]=selectRows(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT cs.id,cs.user_id,cs.label,cs.created_by,cs.created_at,u.username,c.name AS character_name,creator.username AS created_by_username FROM character_savepoints cs LEFT JOIN users u ON u.id=cs.user_id LEFT JOIN character_cards c ON c.user_id=cs.user_id LEFT JOIN users creator ON creator.id=cs.created_by ORDER BY cs.id DESC LIMIT 200) x");
      out["limit"]=200; reply(cb,std::move(out));
    },"讀取角色安全存檔失敗");
  });

  // [功能備註｜存檔／備份／稽核｜GET /api/admin/backups/character-savepoints/{1}/preview]
  // 用途：讀取/顯示存檔／備份／稽核相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/backups/character-savepoints/{1}/preview", drogon::Get,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] {
      const auto user=requireUser(service,req,true); const auto id=positiveId(idText);
      const auto rows=service->database()->execSqlSync("SELECT row_to_json(s)::text AS json_text FROM character_savepoints s WHERE id=$1",id);
      if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到角色安全存檔");
      const auto savepoint=rowJson(rows); auto checkpoint=savepoint["snapshot"];
      const auto userId=intValue(savepoint["user_id"]);
      if(!checkpoint["character"].isObject()){
        Json::Value normalized; normalized["version"]=39; normalized["created_at"]=savepoint["created_at"];
        const auto u=service->database()->execSqlSync("SELECT row_to_json(x)::text AS json_text FROM (SELECT id,username,is_admin,created_at FROM users WHERE id=$1) x",userId);
        normalized["user"]=rowJson(u); normalized["character"]=checkpoint; normalized["related"]=Json::objectValue; checkpoint=std::move(normalized);
      }
      Json::Value target; target["id"]=Json::Int64(userId); target["username"]=checkpoint["user"]["username"]; target["name"]=checkpoint["character"]["name"];
      Json::Value data; data["mode"]="exact_character_restore"; data["source_savepoint_id"]=Json::Int64(id); data["checkpoint"]=checkpoint;
      const auto backup=createBackupEnvelope("character",target,data); Json::Value out; out["backup"]=backup; out["preview"]=inspectBackupForRestore(service,backup,user.id,true); out["savepoint"]=savepoint; reply(cb,std::move(out));
    },"預覽角色存檔失敗");
  });

  // [功能備註｜存檔／備份／稽核｜POST /api/admin/backups/preview]
  // 用途：建立/執行存檔／備份／稽核相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/backups/preview", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user=requireUser(service,req,true); const auto body=bodyOrEmpty(req);
      if(!body["backup"].isObject())throw ApiError(drogon::k400BadRequest,"請提供備份檔");
      Json::Value out; out["preview"]=inspectBackupForRestore(service,body["backup"],user.id,true); reply(cb,std::move(out));
    },"備份檔預覽失敗");
  });

  // [功能備註｜存檔／備份／稽核｜POST /api/admin/backups/restore]
  // 用途：建立/執行存檔／備份／稽核相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/backups/restore", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user=requireUser(service,req,true); const auto body=bodyOrEmpty(req);
      const auto backup=validateBackupEnvelope(body["backup"]); const auto scope=stringValue(backup["scope"]);
      if(scope=="site")throw ApiError(drogon::k400BadRequest,"全站備份不能從網頁覆寫，請交由資料庫管理員處理");
      const auto preview=inspectBackupForRestore(service,backup,user.id,false);
      if(!validRestoreToken(backup,user.id,stringValue(body["restore_token"])))throw ApiError(drogon::k400BadRequest,"還原許可已失效，請重新預覽備份檔");
      if(stringValue(body["confirm_text"])!=stringValue(preview["confirm_text"]))throw ApiError(drogon::k400BadRequest,"請完整輸入確認文字："+stringValue(preview["confirm_text"]));
      const auto targetId=intValue(preview["target"]["id"]); Json::Value out; out["ok"]=true;
      if(scope=="room"){
        restoreRoomBackupCheckpoint(service,targetId,backup["data"]["checkpoint"],user.id);
        Json::Value event; event["text"]="DM 從外部備份檔還原房間狀態"; event["backup_checksum"]=stringValue(backup["checksum"]).substr(0,16); service->addEvent(targetId,user.id,"restore",event);
        out["message"]="房間狀態已還原；還原前狀態已自動保存。"; out["room"]=service->roomSnapshot(targetId);
      }else{
        restoreCharacterBackupCheckpoint(service->database(),targetId,backup["data"]["checkpoint"],user.id);
        out["message"]="角色資料已還原；還原前狀態已自動保存。"; out["character"]=characterRaw(service->database(),targetId);
      }
      service->database()->execSqlSync("INSERT INTO audit_logs(actor_user_id,actor_username,method,action,path,status_code,summary,metadata) VALUES($1,$2,'POST',$3,'/api/admin/backups/restore',200,$4,$5::jsonb)",user.id,user.username,"backup.restore."+scope,"還原 "+scope+" #"+std::to_string(targetId),compactJson(Json::Value{Json::objectValue}));
      reply(cb,std::move(out));
    },"還原備份失敗");
  });

  // [功能備註｜存檔／備份／稽核｜GET /api/admin/audit-logs]
  // 用途：讀取/顯示存檔／備份／稽核相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/audit-logs",drogon::Get,[service](const Request& req,Callback& cb){runHandler(cb,[&]{requireUser(service,req,true);Json::Value out;out["logs"]=selectRows(service->database(),"SELECT row_to_json(a)::text AS json_text FROM audit_logs a ORDER BY created_at DESC LIMIT 500");reply(cb,std::move(out));},"讀取稽核紀錄失敗");});

  // ---------------------------------------------------------------------------
  // V27 exploration, clue inference and time-flow reset.
  // ---------------------------------------------------------------------------
  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/explore]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/rooms/{1}/explore", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto roomId = positiveId(roomIdText);
      requireRoomAccess(service, user, roomId);
      const auto body = bodyOrEmpty(req);
      const auto character = characterRaw(service->database(), user.id);
      const auto skills = character["skills"].isArray() ? character["skills"] : Json::Value{Json::arrayValue};
      const auto requested = stringValue(body["skill_id"]);
      Json::Value skill;
      for (const auto& candidate : skills) {
        if (stringValue(candidate["id"]) == requested || stringValue(candidate["name"]) == requested) {
          const auto data = candidate["data"].isObject() ? candidate["data"] : Json::Value{Json::objectValue};
          if (data.get("investigation_skill", false).asBool()) skill = candidate;
          break;
        }
      }
      if (skill.isNull()) throw ApiError(drogon::k400BadRequest, "請選擇探查類技能");
      const auto cfg = skill["data"].isObject() ? skill["data"] : Json::Value{Json::objectValue};
      const auto attrName = stringValue(cfg["investigation_attribute"], "spirit");
      std::int64_t attr = 0;
      if (attrName == "agility") attr = intValue(character["final_agility"], intValue(character["agility"]));
      else if (attrName == "luck") attr = intValue(character["final_luck"], intValue(character["luck"]));
      else if (attrName == "fifth") attr = std::max(intValue(character["current_great_way"]), intValue(character["current_faith"]));
      else attr = intValue(character["final_spirit"], intValue(character["spirit"]));
      auto rateFor = [&](std::int64_t dc) {
        const auto attrBonus = std::min<std::int64_t>(40, static_cast<std::int64_t>(std::floor(std::sqrt(static_cast<double>(std::max<std::int64_t>(0, attr))) * 3.0)));
        return std::clamp<std::int64_t>(intValue(cfg["investigation_base_rate"], 40) + intValue(cfg["investigation_bonus"]) + attrBonus - (std::max<std::int64_t>(1, dc) - 50), 5, 95);
      };
      std::int64_t nodeId = 0;
      const auto nodeRows = service->database()->execSqlSync(
          "SELECT COALESCE(t.current_map_node_id,r.current_map_node_id) AS node_id "
          "FROM rooms r LEFT JOIN room_team_members m ON m.room_id=r.id AND m.user_id=$1 "
          "LEFT JOIN room_teams t ON t.id=m.team_id WHERE r.id=$2", user.id, roomId);
      if (!nodeRows.empty() && !nodeRows[0]["node_id"].isNull()) nodeId = nodeRows[0]["node_id"].as<std::int64_t>();
      const auto mode = stringValue(body["mode"], "environment");
      Json::Value found{Json::arrayValue};
      std::int64_t dc = 50;
      std::string targetName = "環境";
      if (mode == "npc") {
        const auto npcId = intValue(body["target_id"]);
        const auto rows = service->database()->execSqlSync(
            "SELECT row_to_json(x)::text AS json_text FROM (SELECT rn.map_node_id,nt.* FROM room_npcs rn JOIN npc_templates nt ON nt.id=rn.npc_template_id WHERE rn.room_id=$1 AND rn.npc_template_id=$2) x",
            roomId, npcId);
        if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到 NPC");
        const auto npc = rowJson(rows);
        if (intValue(npc["map_node_id"]) > 0 && intValue(npc["map_node_id"]) != nodeId)
          throw ApiError(drogon::k403Forbidden, "NPC 不在你目前的位置");
        dc = intValue(npc["investigation_dc"], 50);
        targetName = stringValue(npc["name"], "NPC");
        const auto rate = rateFor(dc); const auto roll = randomInt(1,100); const bool success = roll <= rate;
        service->database()->execSqlSync(
            "INSERT INTO npc_discoveries(room_id,npc_template_id,user_id,investigated,last_roll,last_rate) VALUES($1,$2,$3,$4,$5,$6) "
            "ON CONFLICT(room_id,npc_template_id,user_id) DO UPDATE SET investigated=(npc_discoveries.investigated OR EXCLUDED.investigated),last_roll=EXCLUDED.last_roll,last_rate=EXCLUDED.last_rate,updated_at=CURRENT_TIMESTAMP",
            roomId,npcId,user.id,success,roll,rate);
        if (success) found.append("已掌握 " + targetName + " 的完整情報");
        Json::Value ev; ev["text"]="探查「"+targetName+"」"; ev["mode"]="npc"; ev["roll"]=roll; ev["rate"]=Json::Int64(rate); ev["success"]=success; service->addEvent(roomId,user.id,"investigation",ev);
        Json::Value out; out["ok"]=true; out["mode"]="npc"; out["roll"]=roll; out["rate"]=Json::Int64(rate); out["success"]=success; out["found"]=found; reply(cb,std::move(out)); return;
      }
      if (mode == "item") {
        const auto name = trimmed(stringValue(body["item_name"]));
        if (name.empty() || inventoryQuantity(inventoryOf(character), name) <= 0) throw ApiError(drogon::k400BadRequest, "你沒有這個道具");
        const auto rows = service->database()->execSqlSync("SELECT row_to_json(i)::text AS json_text FROM item_templates i WHERE name=$1", name);
        if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到對應道具模板");
        const auto item=rowJson(rows);
        if (!item.get("requires_identification",false).asBool()) { Json::Value out;out["ok"]=true;out["mode"]="item";out["success"]=true;out["roll"]=0;out["rate"]=100;found.append(name+" 不需要鑑定");out["found"]=found;reply(cb,std::move(out));return; }
        dc=intValue(item["identification_dc"],50); const auto rate=rateFor(dc); const auto roll=randomInt(1,100); const bool success=roll<=rate;
        if(success) service->database()->execSqlSync("INSERT INTO item_identifications(item_template_id,user_id) VALUES($1,$2) ON CONFLICT DO NOTHING",intValue(item["id"]),user.id);
        if(success) found.append("確認物品："+name);
        Json::Value ev;ev["text"]="鑑定物品「"+name+"」";ev["roll"]=roll;ev["rate"]=Json::Int64(rate);ev["success"]=success;service->addEvent(roomId,user.id,"identification",ev);
        Json::Value out;out["ok"]=true;out["mode"]="item";out["roll"]=roll;out["rate"]=Json::Int64(rate);out["success"]=success;out["found"]=found;reply(cb,std::move(out));return;
      }
      Json::Value node;
      if(nodeId>0){const auto rows=service->database()->execSqlSync("SELECT row_to_json(n)::text AS json_text FROM room_map_nodes n WHERE id=$1 AND room_id=$2",nodeId,roomId);if(!rows.empty())node=rowJson(rows);}
      dc=intValue(node["investigation_dc"],50); const auto rate=rateFor(dc); const auto roll=randomInt(1,100); const bool success=roll<=rate;
      if(success){
        const auto hiddenContainers=service->database()->execSqlSync("SELECT id,name FROM search_containers WHERE room_id=$1 AND active=TRUE AND hidden=TRUE AND (map_node_id IS NULL OR map_node_id=$2)",roomId,nodeId);
        for(const auto&r:hiddenContainers){const auto cid=r["id"].as<std::int64_t>();service->database()->execSqlSync("INSERT INTO search_records(container_id,user_id,discovered) VALUES($1,$2,TRUE) ON CONFLICT(container_id,user_id) DO UPDATE SET discovered=TRUE,updated_at=CURRENT_TIMESTAMP",cid,user.id);found.append("可搜索物："+r["name"].as<std::string>());}
        const auto clues=service->database()->execSqlSync("SELECT id,name,linked_rule_id,reveal_level FROM clue_templates WHERE auto_discover=TRUE AND (room_id IS NULL OR room_id=$1) AND (map_node_id IS NULL OR map_node_id=$2) AND investigation_dc<=$3",roomId,nodeId,rate);
        for(const auto&r:clues){const auto clueId=r["id"].as<std::int64_t>();service->database()->execSqlSync("INSERT INTO character_clues(clue_id,user_id,note) VALUES($1,$2,'探索發現') ON CONFLICT(clue_id,user_id) DO NOTHING",clueId,user.id);if(!r["linked_rule_id"].isNull())service->database()->execSqlSync("INSERT INTO rule_discoveries(rule_id,user_id,knowledge_level) VALUES($1,$2,$3) ON CONFLICT(rule_id,user_id) DO UPDATE SET knowledge_level=GREATEST(rule_discoveries.knowledge_level,EXCLUDED.knowledge_level),updated_at=CURRENT_TIMESTAMP",r["linked_rule_id"].as<std::int64_t>(),user.id,std::clamp<std::int64_t>(r["reveal_level"].isNull()?1:r["reveal_level"].as<std::int64_t>(),1,3));found.append("線索："+r["name"].as<std::string>());}
      }
      Json::Value ev;ev["text"]="探查「"+stringValue(node["name"],"目前環境")+"」";ev["mode"]="environment";ev["roll"]=roll;ev["rate"]=Json::Int64(rate);ev["success"]=success;ev["found"]=found;service->addEvent(roomId,user.id,"investigation",ev);
      Json::Value out;out["ok"]=true;out["mode"]="environment";out["roll"]=roll;out["rate"]=Json::Int64(rate);out["success"]=success;out["found"]=found;reply(cb,std::move(out));
    }, "探索失敗");
  });

  // [功能備註｜調查／線索｜GET /api/admin/clue-combinations]
  // 用途：讀取/顯示調查／線索相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/clue-combinations", drogon::Get, [service](const Request& req, Callback& cb){
    runHandler(cb,[&]{requireUser(service,req,true);const auto raw=trimmed(req->getParameter("room_id"));Json::Value out;if(raw.empty()||raw=="0")out["combinations"]=genericList(service->database(),"clue_combinations","id DESC");else out["combinations"]=selectRows1(service->database(),"SELECT row_to_json(c)::text AS json_text FROM clue_combinations c WHERE room_id=$1 ORDER BY id DESC",positiveId(raw));reply(cb,std::move(out));},"讀取線索組合失敗");
  });
  // [功能備註｜調查／線索｜POST /api/admin/clue-combinations]
  // 用途：建立/執行調查／線索相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/clue-combinations", drogon::Post, [service](const Request& req, Callback& cb){
    runHandler(cb,[&]{requireUser(service,req,true);auto body=bodyOrEmpty(req);const auto roomId=intValue(body["room_id"]);requireRoomExists(service->database(),roomId);if(!body["required_clue_ids"].isArray()||body["required_clue_ids"].size()<2)throw ApiError(drogon::k400BadRequest,"線索組合至少需要 2 條線索");Json::Value out;out["ok"]=true;out["combination"]=genericInsert(service->database(),"clue_combinations",body);reply(cb,std::move(out));},"建立線索組合失敗");
  });
  // [功能備註｜調查／線索｜DELETE /api/admin/clue-combinations/{1}]
  // 用途：刪除調查／線索相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/clue-combinations/{1}",drogon::Delete,[service](const Request& req,Callback& cb,const std::string&idText){runHandler(cb,[&]{requireUser(service,req,true);genericDelete(service->database(),"clue_combinations",positiveId(idText));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"刪除線索組合失敗");});
  // [功能備註｜房間／跑團｜POST /api/rooms/{1}/inference]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/rooms/{1}/inference",drogon::Post,[service](const Request& req,Callback& cb,const std::string&roomIdText){
    runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);requireRoomAccess(service,user,roomId);std::set<std::int64_t> known;for(const auto&r:service->database()->execSqlSync("SELECT clue_id FROM character_clues WHERE user_id=$1",user.id))known.insert(r["clue_id"].as<std::int64_t>());const auto shared=service->database()->execSqlSync("SELECT s.clue_id FROM team_shared_clues s JOIN room_team_members m ON m.team_id=s.team_id WHERE m.room_id=$1 AND m.user_id=$2",roomId,user.id);for(const auto&r:shared)known.insert(r["clue_id"].as<std::int64_t>());Json::Value unlocked{Json::arrayValue};const auto rows=service->database()->execSqlSync("SELECT row_to_json(c)::text AS json_text FROM clue_combinations c WHERE room_id=$1 AND active=TRUE ORDER BY id",roomId);for(std::size_t i=0;i<rows.size();++i){auto combo=rowJson(rows,i);const auto comboId=intValue(combo["id"]);if(!service->database()->execSqlSync("SELECT 1 FROM clue_combination_unlocks WHERE combination_id=$1 AND user_id=$2",comboId,user.id).empty())continue;auto need=combo["required_clue_ids"].isArray()?combo["required_clue_ids"]:Json::Value{Json::arrayValue};bool all=!need.empty();for(const auto&id:need)if(!known.contains(intValue(id)))all=false;if(!all)continue;service->database()->execSqlSync("INSERT INTO clue_combination_unlocks(combination_id,user_id) VALUES($1,$2) ON CONFLICT DO NOTHING",comboId,user.id);const auto type=stringValue(combo["result_type"],"vision");const auto resultId=intValue(combo["result_id"]);if(type=="rule"&&resultId>0)service->database()->execSqlSync("INSERT INTO rule_discoveries(rule_id,user_id,knowledge_level) VALUES($1,$2,3) ON CONFLICT(rule_id,user_id) DO UPDATE SET knowledge_level=3,updated_at=CURRENT_TIMESTAMP",resultId,user.id);else if(type=="clue"&&resultId>0)service->database()->execSqlSync("INSERT INTO character_clues(clue_id,user_id,note) VALUES($1,$2,'推理結果') ON CONFLICT DO NOTHING",resultId,user.id);else if(type=="map_node"&&resultId>0)service->database()->execSqlSync("INSERT INTO map_node_discoveries(node_id,user_id) VALUES($1,$2) ON CONFLICT DO NOTHING",resultId,user.id);else service->database()->execSqlSync("INSERT INTO personal_visions(room_id,user_id,title,content,vision_type,shareable) VALUES($1,$2,$3,$4,'memory',TRUE)",roomId,user.id,stringValue(combo["result_title"],stringValue(combo["name"],"推理結果")),stringValue(combo["result_content"]));unlocked.append(combo);Json::Value ev;ev["text"]="完成推理「"+stringValue(combo["name"],"推理")+"」";ev["combination_id"]=Json::Int64(comboId);service->addEvent(roomId,user.id,"inference",ev);}Json::Value out;out["ok"]=true;out["unlocked"]=unlocked;reply(cb,std::move(out));},"整理線索失敗");
  });
  // [功能備註｜房間／跑團｜POST /api/admin/rooms/{1}/time-flow/reset]
  // 用途：建立/執行房間／跑團相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/admin/rooms/{1}/time-flow/reset",drogon::Post,[service](const Request& req,Callback& cb,const std::string&roomIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto roomId=positiveId(roomIdText);service->database()->execSqlSync("UPDATE rooms SET time_flow_offset_minutes=0,updated_at=CURRENT_TIMESTAMP WHERE id=$1",roomId);service->database()->execSqlSync("UPDATE room_map_nodes SET time_flow_offset_minutes=0,updated_at=CURRENT_TIMESTAMP WHERE room_id=$1",roomId);Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"重設時間流失敗");});

  // ---------------------------------------------------------------------------
  // Great Way / Faith identity systems.
  // ---------------------------------------------------------------------------
  // [功能備註｜大道｜GET /api/great-ways]
  // 用途：讀取/顯示大道相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/great-ways",drogon::Get,[service](const Request& req,Callback& cb){runHandler(cb,[&]{const auto user=requireUser(service,req);Json::Value out;out["great_ways"]=user.isAdmin?genericList(service->database(),"great_way_templates"):genericList(service->database(),"great_way_templates","id ASC","active=TRUE");out["my_great_ways"]=selectRows1(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT c.*,g.name,g.principle,g.description,g.max_rank,g.effects,g.granted_skill_template_ids FROM character_great_ways c JOIN great_way_templates g ON g.id=c.great_way_id WHERE c.user_id=$1 ORDER BY c.created_at,g.id) x",user.id);reply(cb,std::move(out));},"讀取大道系統失敗");});
  // [功能備註｜其他系統｜GET /api/pantheon]
  // 用途：讀取/顯示其他系統相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/pantheon",drogon::Get,[service](const Request& req,Callback& cb){runHandler(cb,[&]{const auto user=requireUser(service,req);Json::Value out;out["deities"]=user.isAdmin?genericList(service->database(),"faith_deities"):genericList(service->database(),"faith_deities","id ASC","active=TRUE");out["my_faiths"]=selectRows1(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT c.*,d.name,d.title,d.domains,d.doctrine,d.description,d.max_rank,d.effects,d.granted_skill_template_ids FROM character_faiths c JOIN faith_deities d ON d.id=c.deity_id WHERE c.user_id=$1 ORDER BY c.created_at,d.id) x",user.id);reply(cb,std::move(out));},"讀取諸神殿失敗");});
  // [功能備註｜大道｜GET/POST /api/admin/great-ways]
  // 用途：大道的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminCrud0(service,"/api/admin/great-ways","great_way_templates","great_ways");
  // [功能備註｜大道｜PATCH/DELETE /api/admin/great-ways/{1}]
  // 用途：大道的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminPatchDelete1(service,"/api/admin/great-ways/{1}","great_way_templates","great_way");
  // [功能備註｜信仰／神祇｜GET/POST /api/admin/deities]
  // 用途：信仰／神祇的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminCrud0(service,"/api/admin/deities","faith_deities","deities");
  // [功能備註｜信仰／神祇｜PATCH/DELETE /api/admin/deities/{1}]
  // 用途：信仰／神祇的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminPatchDelete1(service,"/api/admin/deities/{1}","faith_deities","deity");
  // [功能備註｜大道｜POST /api/admin/players/{1}/great-ways/{2}]
  // 用途：建立/執行大道相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/players/{1}/great-ways/{2}", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& userIdText, const std::string& idText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto userId = positiveId(userIdText), id = positiveId(idText);
      const auto character = characterRaw(service->database(), userId);
      if (stringValue(character["faction"]) == "西國") throw ApiError(drogon::k400BadRequest, "西國角色使用信仰系統；不能授予大道");
      const auto rows = service->database()->execSqlSync("SELECT row_to_json(g)::text AS json_text FROM great_way_templates g WHERE id=$1", id);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到大道");
      const auto templ = rowJson(rows), body = bodyOrEmpty(req);
      const auto rank = validatedIdentityRank(body["rank"], templ["max_rank"], "大道等階");
      service->database()->execSqlSync("INSERT INTO character_great_ways(user_id,great_way_id,rank,progress,active) VALUES($1,$2,$3,$4,TRUE) ON CONFLICT(user_id,great_way_id) DO UPDATE SET rank=EXCLUDED.rank,progress=EXCLUDED.progress,active=TRUE,updated_at=CURRENT_TIMESTAMP", userId, id, rank, std::max<std::int64_t>(0, intValue(body["progress"])));
      Json::Value acquisition; acquisition["type"]="great_way"; acquisition["great_way_id"]=Json::Int64(id); acquisition["great_way_name"]=templ["name"]; acquisition["rank"]=rank;
      const auto granted = grantIdentitySkills(service->database(), userId, templ["granted_skill_template_ids"], acquisition);
      syncIdentitySourceEffects(service->database(), userId);
      Json::Value response; response["ok"]=true; response["granted"]=granted; reply(cb, std::move(response));
    }, "授予大道失敗");
  });
  // [功能備註｜大道｜DELETE /api/admin/players/{1}/great-ways/{2}]
  // 用途：刪除大道相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/players/{1}/great-ways/{2}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& userIdText, const std::string& idText) {
    runHandler(cb, [&] {
      requireUser(service, req, true); const auto userId=positiveId(userIdText);
      service->database()->execSqlSync("DELETE FROM character_great_ways WHERE user_id=$1 AND great_way_id=$2", userId, positiveId(idText));
      syncIdentitySourceEffects(service->database(), userId); Json::Value response; response["ok"]=true; reply(cb, std::move(response));
    }, "移除大道失敗");
  });
  // [功能備註｜信仰／神祇｜POST /api/admin/players/{1}/faiths/{2}]
  // 用途：建立/執行信仰／神祇相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/players/{1}/faiths/{2}", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& userIdText, const std::string& idText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto userId = positiveId(userIdText), id = positiveId(idText);
      const auto character = characterRaw(service->database(), userId);
      if (stringValue(character["faction"]) != "西國") throw ApiError(drogon::k400BadRequest, "東國角色使用大道系統；不能授予信仰");
      const auto rows = service->database()->execSqlSync("SELECT row_to_json(d)::text AS json_text FROM faith_deities d WHERE id=$1", id);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到神祇");
      const auto deity = rowJson(rows), body = bodyOrEmpty(req);
      const auto rank = validatedIdentityRank(body["rank"], deity["max_rank"], "信仰等階");
      service->database()->execSqlSync("INSERT INTO character_faiths(user_id,deity_id,rank,devotion,favor,active) VALUES($1,$2,$3,$4,$5,TRUE) ON CONFLICT(user_id,deity_id) DO UPDATE SET rank=EXCLUDED.rank,devotion=EXCLUDED.devotion,favor=EXCLUDED.favor,active=TRUE,updated_at=CURRENT_TIMESTAMP", userId, id, rank, std::max<std::int64_t>(0,intValue(body["devotion"])), std::clamp<std::int64_t>(intValue(body["favor"]),-100,100));
      Json::Value acquisition; acquisition["type"]="faith"; acquisition["deity_id"]=Json::Int64(id); acquisition["deity_name"]=deity["name"]; acquisition["rank"]=rank;
      const auto granted = grantIdentitySkills(service->database(), userId, deity["granted_skill_template_ids"], acquisition);
      syncIdentitySourceEffects(service->database(), userId);
      Json::Value response; response["ok"]=true; response["granted"]=granted; reply(cb, std::move(response));
    }, "授予信仰失敗");
  });
  // [功能備註｜信仰／神祇｜DELETE /api/admin/players/{1}/faiths/{2}]
  // 用途：刪除信仰／神祇相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/players/{1}/faiths/{2}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& userIdText, const std::string& idText) {
    runHandler(cb, [&] {
      requireUser(service, req, true); const auto userId=positiveId(userIdText);
      service->database()->execSqlSync("DELETE FROM character_faiths WHERE user_id=$1 AND deity_id=$2", userId, positiveId(idText));
      syncIdentitySourceEffects(service->database(), userId); Json::Value response; response["ok"]=true; reply(cb, std::move(response));
    }, "移除信仰失敗");
  });

  // Wheels.
  // [功能備註｜輪盤｜GET /api/wheels]
  // 用途：讀取/顯示輪盤相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/wheels",drogon::Get,[service](const Request& req,Callback& cb){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomParam=trimmed(req->getParameter("room_id"));Json::Value out;if(user.isAdmin)out["wheels"]=genericList(service->database(),"custom_wheels","id DESC");else if(!roomParam.empty()&&roomParam!="0")out["wheels"]=selectRows1(service->database(),"SELECT row_to_json(w)::text AS json_text FROM custom_wheels w WHERE active=TRUE AND audience<>'dm' AND (room_id IS NULL OR room_id=$1) ORDER BY id DESC",positiveId(roomParam));else out["wheels"]=genericList(service->database(),"custom_wheels","id DESC","active=TRUE AND audience<>'dm' AND room_id IS NULL");reply(cb,std::move(out));},"讀取輪盤失敗");});
  // [功能備註｜輪盤｜GET/POST /api/admin/wheels]
  // 用途：輪盤的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminCrud0(service,"/api/admin/wheels","custom_wheels","wheels");
  // [功能備註｜輪盤｜PATCH/DELETE /api/admin/wheels/{1}]
  // 用途：輪盤的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminPatchDelete1(service,"/api/admin/wheels/{1}","custom_wheels","wheel");
  // [功能備註｜輪盤｜POST /api/wheels/{1}/spin]
  // 用途：建立/執行輪盤相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg1("/api/wheels/{1}/spin",drogon::Post,[service](const Request& req,Callback& cb,const std::string&idText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto id=positiveId(idText);const auto rows=service->database()->execSqlSync("SELECT row_to_json(w)::text AS json_text FROM custom_wheels w WHERE id=$1 AND active=TRUE",id);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到可用輪盤");const auto wheel=rowJson(rows);if(stringValue(wheel["audience"])=="dm"&&!user.isAdmin)throw ApiError(drogon::k403Forbidden,"這個輪盤只有 DM 可以使用");const auto roomId=intValue(wheel["room_id"]);if(roomId>0)requireRoomAccess(service,user,roomId);const auto options=wheel["options"].isArray()?wheel["options"]:Json::Value{Json::arrayValue};if(options.size()<2)throw ApiError(drogon::k400BadRequest,"輪盤選項不足");std::vector<int> weights;int total=0;for(const auto&o:options){const auto w=static_cast<int>(std::max<std::int64_t>(1,o.isObject()?intValue(o["weight"],1):1));weights.push_back(w);total+=w;}auto pick=randomInt(0,total-1);Json::ArrayIndex index=0;for(Json::ArrayIndex i=0;i<options.size();++i){if(pick<weights[i]){index=i;break;}pick-=weights[i];}const auto result=options[index];const auto label=result.isString()?result.asString():stringValue(result["label"],stringValue(result["name"],"結果"));service->database()->execSqlSync("INSERT INTO wheel_spins(wheel_id,room_id,user_id,result_index,result_label) VALUES($1,NULLIF($2,0),$3,$4,$5)",id,roomId,user.id,static_cast<int>(index),label);if(roomId>0){Json::Value ev;ev["text"]="轉動「"+stringValue(wheel["name"],"輪盤")+"」→ "+label;ev["wheel_id"]=Json::Int64(id);ev["result"]=label;service->addEvent(roomId,user.id,"wheel",ev);}Json::Value out;out["ok"]=true;out["result"]["index"]=index;out["result"]["label"]=label;out["wheel"]=wheel;reply(cb,std::move(out));},"轉動輪盤失敗");});

  // ---------------------------------------------------------------------------
  // Factions, reputation, NPC dialogue and NPC shops.
  // ---------------------------------------------------------------------------
  // [功能備註｜勢力／聲望｜GET /api/factions]
  // 用途：讀取/顯示勢力／聲望相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/factions",drogon::Get,[service](const Request& req,Callback& cb){runHandler(cb,[&]{const auto user=requireUser(service,req);Json::Value out;out["factions"]=genericList(service->database(),"faction_templates","name ASC,id ASC","active=TRUE");out["reputations"]=selectRows1(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT r.*,f.name,f.description,f.alignment FROM character_reputations r JOIN faction_templates f ON f.id=r.faction_id WHERE r.user_id=$1 ORDER BY r.reputation DESC,f.name) x",user.id);out["relations"]=selectRows(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT r.*,a.name AS source_name,b.name AS target_name FROM faction_relations r JOIN faction_templates a ON a.id=r.source_faction_id JOIN faction_templates b ON b.id=r.target_faction_id WHERE a.active=TRUE AND b.active=TRUE ORDER BY a.name,b.name) x");reply(cb,std::move(out));},"讀取勢力／聲望失敗");});
  // [功能備註｜勢力／聲望｜GET /api/admin/factions]
  // 用途：讀取/顯示勢力／聲望相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/factions",drogon::Get,[service](const Request& req,Callback& cb){runHandler(cb,[&]{const auto user=requireUser(service,req,true);Json::Value out;out["factions"]=genericList(service->database(),"faction_templates");out["reputations"]=selectRows1(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT r.*,f.name,f.description,f.alignment FROM character_reputations r JOIN faction_templates f ON f.id=r.faction_id WHERE r.user_id=$1 ORDER BY r.reputation DESC,f.name) x",user.id);out["relations"]=selectRows(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT r.*,a.name AS source_name,b.name AS target_name FROM faction_relations r JOIN faction_templates a ON a.id=r.source_faction_id JOIN faction_templates b ON b.id=r.target_faction_id ORDER BY a.name,b.name) x");reply(cb,std::move(out));},"讀取勢力失敗");});
  // [功能備註｜勢力／聲望｜POST /api/admin/factions]
  // 用途：建立/執行勢力／聲望相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/factions",drogon::Post,[service](const Request& req,Callback& cb){runHandler(cb,[&]{requireUser(service,req,true);auto body=bodyOrEmpty(req);const auto name=trimmed(stringValue(body["name"]));if(name.empty())throw ApiError(drogon::k400BadRequest,"請輸入勢力名稱");Json::Value out;out["ok"]=true;out["faction"]=genericInsert(service->database(),"faction_templates",body);reply(cb,std::move(out));},"建立勢力失敗");});
  // [功能備註｜勢力／聲望｜PATCH/DELETE /api/admin/factions/{1}]
  // 用途：勢力／聲望的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminPatchDelete1(service,"/api/admin/factions/{1}","faction_templates","faction");
  // [功能備註｜勢力／聲望｜PUT /api/admin/faction-relations]
  // 用途：覆寫/同步勢力／聲望相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/faction-relations",drogon::Put,[service](const Request& req,Callback& cb){runHandler(cb,[&]{requireUser(service,req,true);const auto body=bodyOrEmpty(req);const auto source=intValue(body["source_faction_id"]),target=intValue(body["target_faction_id"]);if(source<=0||target<=0||source==target)throw ApiError(drogon::k400BadRequest,"請選擇兩個不同勢力");const auto score=std::clamp<std::int64_t>(intValue(body["relation_score"]),-100,100);const auto rows=service->database()->execSqlSync("INSERT INTO faction_relations(source_faction_id,target_faction_id,relation_score,label,notes) VALUES($1,$2,$3,$4,$5) ON CONFLICT(source_faction_id,target_faction_id) DO UPDATE SET relation_score=EXCLUDED.relation_score,label=EXCLUDED.label,notes=EXCLUDED.notes,updated_at=CURRENT_TIMESTAMP RETURNING row_to_json(faction_relations)::text AS json_text",source,target,score,stringValue(body["label"]),stringValue(body["notes"]));Json::Value out;out["ok"]=true;out["relation"]=rowJson(rows);reply(cb,std::move(out));},"儲存勢力關係失敗");});
  // [功能備註｜勢力／聲望｜DELETE /api/admin/faction-relations/{1}/{2}]
  // 用途：刪除勢力／聲望相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/faction-relations/{1}/{2}",drogon::Delete,[service](const Request& req,Callback& cb,const std::string&sourceText,const std::string&targetText){runHandler(cb,[&]{requireUser(service,req,true);service->database()->execSqlSync("DELETE FROM faction_relations WHERE source_faction_id=$1 AND target_faction_id=$2",positiveId(sourceText),positiveId(targetText));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"刪除勢力關係失敗");});
  // [功能備註｜玩家管理｜PATCH /api/admin/players/{1}/reputations/{2}]
  // 用途：修改玩家管理相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/admin/players/{1}/reputations/{2}",drogon::Patch,[service](const Request& req,Callback& cb,const std::string&userIdText,const std::string&factionIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto userId=positiveId(userIdText),factionId=positiveId(factionIdText);const auto body=bodyOrEmpty(req);auto rep=intValue(body["reputation"]);if(body.isMember("delta")){const auto rows=service->database()->execSqlSync("SELECT reputation FROM character_reputations WHERE user_id=$1 AND faction_id=$2",userId,factionId);rep=(rows.empty()?0:rows[0]["reputation"].as<std::int64_t>())+intValue(body["delta"]);}rep=std::clamp<std::int64_t>(rep,-10000,10000);service->database()->execSqlSync("INSERT INTO character_reputations(user_id,faction_id,reputation,notes) VALUES($1,$2,$3,$4) ON CONFLICT(user_id,faction_id) DO UPDATE SET reputation=EXCLUDED.reputation,notes=CASE WHEN EXCLUDED.notes<>'' THEN EXCLUDED.notes ELSE character_reputations.notes END,updated_at=CURRENT_TIMESTAMP",userId,factionId,rep,stringValue(body["notes"]));Json::Value out;out["ok"]=true;out["reputation"]=Json::Int64(rep);reply(cb,std::move(out));},"更新勢力聲望失敗");});

  // [功能備註｜NPC｜GET /api/admin/npc-social]
  // 用途：讀取/顯示NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg0("/api/admin/npc-social",drogon::Get,[service](const Request& req,Callback& cb){runHandler(cb,[&]{requireUser(service,req,true);Json::Value out;out["dialogues"]=genericList(service->database(),"npc_dialogue_entries","npc_template_id ASC,sort_order ASC,id ASC");out["shop_items"]=genericList(service->database(),"npc_shop_items","npc_template_id ASC,id ASC");out["task_offers"]=selectRows(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT o.*,n.name AS npc_name,t.title AS task_title,t.status AS task_status FROM npc_task_offers o JOIN npc_templates n ON n.id=o.npc_template_id JOIN tasks t ON t.id=o.task_id ORDER BY o.npc_template_id,o.sort_order,o.id) x");out["relationships"]=genericList(service->database(),"npc_relationships","updated_at DESC");reply(cb,std::move(out));},"讀取 NPC 社交系統失敗");});
  // [功能備註｜NPC｜GET/POST /api/admin/npc-dialogues]
  // 用途：NPC的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminCrud0(service,"/api/admin/npc-dialogues","npc_dialogue_entries","dialogues");
  // [功能備註｜NPC｜PATCH/DELETE /api/admin/npc-dialogues/{1}]
  // 用途：NPC的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminPatchDelete1(service,"/api/admin/npc-dialogues/{1}","npc_dialogue_entries","dialogue");
  // [功能備註｜NPC｜GET/POST /api/admin/npc-shop-items]
  // 用途：NPC的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminCrud0(service,"/api/admin/npc-shop-items","npc_shop_items","items");
  // [功能備註｜NPC｜PATCH/DELETE /api/admin/npc-shop-items/{1}]
  // 用途：NPC的共用管理 CRUD；修改欄位規則時請同步檢查資料表與前端管理頁。
  registerAdminPatchDelete1(service,"/api/admin/npc-shop-items/{1}","npc_shop_items","item");

  // [功能備註｜NPC 任務發布] DM 把既有任務綁到 NPC；玩家互動時可直接接受。
  registerAdminCrud0(service,"/api/admin/npc-task-offers","npc_task_offers","offers");
  registerAdminPatchDelete1(service,"/api/admin/npc-task-offers/{1}","npc_task_offers","offer");


  // [功能備註｜NPC｜GET /api/rooms/{1}/npcs/{2}/interact]
  // 用途：讀取/顯示NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg2("/api/rooms/{1}/npcs/{2}/interact",drogon::Get,[service](const Request& req,Callback& cb,const std::string&roomIdText,const std::string&npcIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText),npcId=positiveId(npcIdText);requireRoomAccess(service,user,roomId);const auto rows=service->database()->execSqlSync("SELECT row_to_json(x)::text AS json_text FROM (SELECT rn.map_node_id,rn.status AS room_status,nt.* FROM room_npcs rn JOIN npc_templates nt ON nt.id=rn.npc_template_id WHERE rn.room_id=$1 AND rn.npc_template_id=$2) x",roomId,npcId);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到房間 NPC");const auto npc=rowJson(rows);Json::Value relation;const auto relRows=service->database()->execSqlSync("SELECT row_to_json(r)::text AS json_text FROM npc_relationships r WHERE room_id=$1 AND npc_template_id=$2 AND user_id=$3",roomId,npcId,user.id);if(relRows.empty()){relation["room_id"]=Json::Int64(roomId);relation["npc_template_id"]=Json::Int64(npcId);relation["user_id"]=Json::Int64(user.id);relation["affinity"]=0;relation["trust"]=0;relation["fear"]=0;relation["hostility"]=0;relation["memories"]=Json::arrayValue;}else relation=rowJson(relRows);const auto config=npc["config"].isObject()?npc["config"]:Json::Value{Json::objectValue};const auto factionId=intValue(config["faction_id"]);std::int64_t reputation=0;if(factionId>0){const auto rr=service->database()->execSqlSync("SELECT reputation FROM character_reputations WHERE user_id=$1 AND faction_id=$2",user.id,factionId);if(!rr.empty())reputation=rr[0]["reputation"].as<std::int64_t>();}const auto score=static_cast<int>(std::lround(intValue(relation["affinity"])*0.42+intValue(relation["trust"])*0.30-intValue(relation["hostility"])*0.62-intValue(relation["fear"])*0.12+std::clamp<double>(reputation/10.0,-30,30)));std::string key=score>=35?"friendly":score>=15?"warm":score>-15?"neutral":score>-35?"wary":"hostile";const std::unordered_map<std::string,std::string> labels={{"friendly","友善"},{"warm","親近"},{"neutral","中立"},{"wary","戒備"},{"hostile","敵對"}};Json::Value attitude;attitude["key"]=key;attitude["label"]=labels.at(key);attitude["score"]=score;Json::Value dialogues{Json::arrayValue};const auto drows=service->database()->execSqlSync("SELECT row_to_json(d)::text AS json_text FROM npc_dialogue_entries d WHERE npc_template_id=$1 AND active=TRUE ORDER BY sort_order,id",npcId);for(std::size_t i=0;i<drows.size();++i){auto d=rowJson(drows,i);if(intValue(relation["affinity"])<intValue(d["min_affinity"],-100)||intValue(relation["trust"])<intValue(d["min_trust"],-100)||intValue(relation["hostility"])>intValue(d["max_hostility"],100))continue;if(d.get("once_per_user",false).asBool()&&!service->database()->execSqlSync("SELECT 1 FROM npc_dialogue_history WHERE room_id=$1 AND user_id=$2 AND dialogue_entry_id=$3",roomId,user.id,intValue(d["id"])).empty())continue;dialogues.append(d);}Json::Value shop{Json::arrayValue};const auto srows=service->database()->execSqlSync("SELECT row_to_json(s)::text AS json_text FROM npc_shop_items s WHERE npc_template_id=$1 AND active=TRUE ORDER BY id",npcId);for(std::size_t i=0;i<srows.size();++i){auto item=rowJson(srows,i);const auto stock=intValue(item["stock"],-1);auto remaining=stock;if(stock>=0){const auto sr=service->database()->execSqlSync("SELECT remaining FROM npc_shop_stock WHERE room_id=$1 AND shop_item_id=$2",roomId,intValue(item["id"]));if(!sr.empty())remaining=sr[0]["remaining"].as<std::int64_t>();}item["remaining"]=Json::Int64(remaining);item["can_buy"]=(remaining!=0&&key!="hostile"&&intValue(relation["affinity"])>=intValue(item["min_affinity"],-100)&&intValue(relation["hostility"])<=intValue(item["max_hostility"],100));shop.append(item);}Json::Value taskOffers{Json::arrayValue};const auto offerRows=service->database()->execSqlSync("SELECT row_to_json(x)::text AS json_text FROM (SELECT o.*,t.title,t.description,t.min_tier,t.requirements,t.rewards,t.status FROM npc_task_offers o JOIN tasks t ON t.id=o.task_id WHERE o.npc_template_id=$1 AND o.active=TRUE AND t.status='open' ORDER BY o.sort_order,o.id) x",npcId);for(std::size_t i=0;i<offerRows.size();++i){auto offer=rowJson(offerRows,i);const auto accepted=!service->database()->execSqlSync("SELECT 1 FROM task_participants WHERE task_id=$1 AND user_id=$2",intValue(offer["task_id"]),user.id).empty();offer["accepted"]=accepted;taskOffers.append(offer);}Json::Value out;out["npc"]=npc;out["reputation"]=Json::Int64(reputation);out["relationship"]=relation;out["attitude"]=attitude;out["dialogues"]=dialogues;out["shop_items"]=shop;out["task_offers"]=taskOffers;reply(cb,std::move(out));},"讀取 NPC 互動失敗");});

  // [功能備註｜NPC｜POST /api/rooms/{1}/npcs/{2}/dialogue/{3}]
  // 用途：建立/執行NPC相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg3("/api/rooms/{1}/npcs/{2}/dialogue/{3}",drogon::Post,[service](const Request& req,Callback& cb,const std::string&roomIdText,const std::string&npcIdText,const std::string&entryIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText),npcId=positiveId(npcIdText),entryId=positiveId(entryIdText);requireRoomAccess(service,user,roomId);if(service->database()->execSqlSync("SELECT 1 FROM room_npcs WHERE room_id=$1 AND npc_template_id=$2",roomId,npcId).empty())throw ApiError(drogon::k404NotFound,"找不到房間 NPC");const auto erows=service->database()->execSqlSync("SELECT row_to_json(d)::text AS json_text FROM npc_dialogue_entries d WHERE id=$1 AND npc_template_id=$2 AND active=TRUE",entryId,npcId);if(erows.empty())throw ApiError(drogon::k404NotFound,"找不到可用對話");const auto entry=rowJson(erows);Json::Value rulePayload;rulePayload["npc_template_id"]=Json::Int64(npcId);rulePayload["dialogue_entry_id"]=Json::Int64(entryId);rulePayload["dialogue"]=entry;const auto npcMetaRows=service->database()->execSqlSync("SELECT row_to_json(n)::text AS json_text FROM npc_templates n WHERE id=$1",npcId);if(!npcMetaRows.empty())rulePayload["npc"]=rowJson(npcMetaRows);auto dialogueAttempt=evaluateRuleEvent(service,roomId,user.id,"NPC_DIALOGUE_ATTEMPT",rulePayload,false);if(dialogueAttempt["blocked"].asBool())throw ApiError(drogon::k403Forbidden,"這段對話被規則阻止");Json::Value relation;const auto rr=service->database()->execSqlSync("SELECT row_to_json(r)::text AS json_text FROM npc_relationships r WHERE room_id=$1 AND npc_template_id=$2 AND user_id=$3",roomId,npcId,user.id);if(rr.empty()){relation["affinity"]=0;relation["trust"]=0;relation["fear"]=0;relation["hostility"]=0;relation["memories"]=Json::arrayValue;}else relation=rowJson(rr);if(intValue(relation["affinity"])<intValue(entry["min_affinity"],-100)||intValue(relation["trust"])<intValue(entry["min_trust"],-100)||intValue(relation["hostility"])>intValue(entry["max_hostility"],100))throw ApiError(drogon::k403Forbidden,"目前關係條件不足");if(entry.get("once_per_user",false).asBool()&&!service->database()->execSqlSync("SELECT 1 FROM npc_dialogue_history WHERE room_id=$1 AND user_id=$2 AND dialogue_entry_id=$3",roomId,user.id,entryId).empty())throw ApiError(drogon::k403Forbidden,"這段對話已經使用過");const auto delta=entry["relation_delta"].isObject()?entry["relation_delta"]:Json::Value{Json::objectValue};const auto affinity=std::clamp<std::int64_t>(intValue(relation["affinity"])+intValue(delta["affinity"]),-100,100);const auto trust=std::clamp<std::int64_t>(intValue(relation["trust"])+intValue(delta["trust"]),-100,100);const auto fear=std::clamp<std::int64_t>(intValue(relation["fear"])+intValue(delta["fear"]),0,100);const auto hostility=std::clamp<std::int64_t>(intValue(relation["hostility"])+intValue(delta["hostility"]),0,100);auto memories=relation["memories"].isArray()?relation["memories"]:Json::Value{Json::arrayValue};Json::Value memory;memory["text"]="玩家選擇「"+stringValue(entry["choice_text"])+"」；NPC 回應「"+stringValue(entry["npc_text"])+"」";memory["type"]="dialogue";memories.append(memory);while(memories.size()>100){Json::Value cut{Json::arrayValue};for(Json::ArrayIndex i=1;i<memories.size();++i)cut.append(memories[i]);memories=std::move(cut);}service->database()->execSqlSync("INSERT INTO npc_relationships(room_id,npc_template_id,user_id,affinity,trust,fear,hostility,memories) VALUES($1,$2,$3,$4,$5,$6,$7,$8::jsonb) ON CONFLICT(room_id,npc_template_id,user_id) DO UPDATE SET affinity=EXCLUDED.affinity,trust=EXCLUDED.trust,fear=EXCLUDED.fear,hostility=EXCLUDED.hostility,memories=EXCLUDED.memories,updated_at=CURRENT_TIMESTAMP",roomId,npcId,user.id,affinity,trust,fear,hostility,compactJson(memories));if(entry.get("once_per_user",false).asBool())service->database()->execSqlSync("INSERT INTO npc_dialogue_history(room_id,user_id,npc_template_id,dialogue_entry_id) VALUES($1,$2,$3,$4) ON CONFLICT DO NOTHING",roomId,user.id,npcId,entryId);Json::Value ev;ev["text"]="與 NPC 交談";ev["npc_template_id"]=Json::Int64(npcId);ev["dialogue_entry_id"]=Json::Int64(entryId);service->addEvent(roomId,user.id,"npc_dialogue",ev);ev["dialogue"]=entry;ev["npc_template_id"]=Json::Int64(npcId);evaluateRuleEvent(service,roomId,user.id,"NPC_DIALOGUE",ev,false);Json::Value out;out["ok"]=true;out["reply"]=stringValue(entry["npc_text"]);out["relationship"]["affinity"]=Json::Int64(affinity);out["relationship"]["trust"]=Json::Int64(trust);out["relationship"]["fear"]=Json::Int64(fear);out["relationship"]["hostility"]=Json::Int64(hostility);out["granted_task"]=Json::nullValue;reply(cb,std::move(out));},"NPC 對話失敗");});


  // [功能備註｜NPC 任務發布｜POST /api/rooms/{1}/npcs/{2}/tasks/{3}/accept] 接受 NPC 發布任務，沿用 task_participants。
  reg3("/api/rooms/{1}/npcs/{2}/tasks/{3}/accept",drogon::Post,[service](const Request&req,Callback&cb,const std::string&roomText,const std::string&npcText,const std::string&taskText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomText),npcId=positiveId(npcText),taskId=positiveId(taskText);requireRoomAccess(service,user,roomId);if(service->database()->execSqlSync("SELECT 1 FROM room_npcs WHERE room_id=$1 AND npc_template_id=$2",roomId,npcId).empty())throw ApiError(drogon::k404NotFound,"找不到房間 NPC");const auto offered=service->database()->execSqlSync("SELECT 1 FROM npc_task_offers o JOIN tasks t ON t.id=o.task_id WHERE o.npc_template_id=$1 AND o.task_id=$2 AND o.active=TRUE AND t.status='open'",npcId,taskId);if(offered.empty())throw ApiError(drogon::k404NotFound,"此 NPC 沒有發布這個任務");const auto character=characterRaw(service->database(),user.id);const auto taskRows=service->database()->execSqlSync("SELECT min_tier,title FROM tasks WHERE id=$1",taskId);if(taskRows.empty())throw ApiError(drogon::k404NotFound,"找不到任務");if(intValue(character["tier"],1)<taskRows[0]["min_tier"].as<std::int64_t>())throw ApiError(drogon::k403Forbidden,"角色階級不足");service->database()->execSqlSync("INSERT INTO task_participants(task_id,user_id,status) VALUES($1,$2,'accepted') ON CONFLICT(task_id,user_id) DO UPDATE SET status=CASE WHEN task_participants.status='completed' THEN task_participants.status ELSE 'accepted' END",taskId,user.id);Json::Value ev;ev["text"]="從 NPC 接受任務「"+taskRows[0]["title"].as<std::string>()+"」";ev["npc_template_id"]=Json::Int64(npcId);ev["task_id"]=Json::Int64(taskId);service->addEvent(roomId,user.id,"npc_task_accept",ev);Json::Value out;out["ok"]=true;out["task_id"]=Json::Int64(taskId);reply(cb,std::move(out));},"接受 NPC 任務失敗");});

  // [功能備註｜商店｜POST /api/rooms/{1}/npcs/{2}/shop/{3}/buy]
  // 用途：建立/執行商店相關資料或操作。 修改時請同步檢查前端呼叫、權限驗證與相關資料表。
  reg3("/api/rooms/{1}/npcs/{2}/shop/{3}/buy",drogon::Post,[service](const Request& req,Callback& cb,const std::string&roomIdText,const std::string&npcIdText,const std::string&itemIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText),npcId=positiveId(npcIdText),itemId=positiveId(itemIdText);requireRoomAccess(service,user,roomId);if(service->database()->execSqlSync("SELECT 1 FROM room_npcs WHERE room_id=$1 AND npc_template_id=$2",roomId,npcId).empty())throw ApiError(drogon::k404NotFound,"找不到房間 NPC");const auto rows=service->database()->execSqlSync("SELECT row_to_json(s)::text AS json_text FROM npc_shop_items s WHERE id=$1 AND npc_template_id=$2 AND active=TRUE",itemId,npcId);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到 NPC 商品");auto item=rowJson(rows);Json::Value purchaseRulePayload;purchaseRulePayload["npc_template_id"]=Json::Int64(npcId);purchaseRulePayload["shop_item_id"]=Json::Int64(itemId);purchaseRulePayload["item"]=item;auto purchaseAttempt=evaluateRuleEvent(service,roomId,user.id,"SHOP_PURCHASE_ATTEMPT",purchaseRulePayload,false);if(purchaseAttempt["blocked"].asBool())throw ApiError(drogon::k403Forbidden,"此購買行為被規則阻止");const auto relRows=service->database()->execSqlSync("SELECT affinity,hostility FROM npc_relationships WHERE room_id=$1 AND npc_template_id=$2 AND user_id=$3",roomId,npcId,user.id);const auto affinity=relRows.empty()?0:relRows[0]["affinity"].as<std::int64_t>();const auto hostility=relRows.empty()?0:relRows[0]["hostility"].as<std::int64_t>();if(affinity<intValue(item["min_affinity"],-100)||hostility>intValue(item["max_hostility"],100))throw ApiError(drogon::k403Forbidden,"NPC 目前拒絕交易");const auto stock=intValue(item["stock"],-1);
        auto character=characterRaw(service->database(),user.id);const auto currency=stringValue(item["currency"],"weird");auto basePrice=std::max<std::int64_t>(0,intValue(item["price"]));double discount=0.0;const auto discountFaction=intValue(item["discount_faction_id"]);if(discountFaction>0){const auto repRows=service->database()->execSqlSync("SELECT reputation FROM character_reputations WHERE user_id=$1 AND faction_id=$2",user.id,discountFaction);const auto rep=repRows.empty()?0:repRows[0]["reputation"].as<std::int64_t>();discount=std::min<double>(static_cast<double>(intValue(item["max_discount_percent"])),std::max<double>(0.0,rep/100.0*item["discount_per_100"].asDouble()));}const auto price=static_cast<std::int64_t>(std::ceil(basePrice*(1.0-discount/100.0)));const char* coinField=currency=="game"?"game_coins":"weird_coins";const auto coins=intValue(character[coinField]);if(currency=="game"&&!character.get("game_coin_unlocked",false).asBool())throw ApiError(drogon::k403Forbidden,"你尚未解鎖遊戲幣");if(coins<price)throw ApiError(drogon::k400BadRequest,(currency=="game"?"遊戲幣不足":"詭幣不足"));if(stock>=0){service->database()->execSqlSync("INSERT INTO npc_shop_stock(room_id,shop_item_id,remaining) VALUES($1,$2,$3) ON CONFLICT(room_id,shop_item_id) DO NOTHING",roomId,itemId,stock);const auto dec=service->database()->execSqlSync("UPDATE npc_shop_stock SET remaining=remaining-1,updated_at=CURRENT_TIMESTAMP WHERE room_id=$1 AND shop_item_id=$2 AND remaining>0 RETURNING remaining",roomId,itemId);if(dec.empty())throw ApiError(drogon::k400BadRequest,"商品已售罄");}auto inventory=inventoryOf(character);auto equipment=character["equipment"].isArray()?character["equipment"]:Json::Value{Json::arrayValue};auto weapons=character["weapons"].isArray()?character["weapons"]:Json::Value{Json::arrayValue};const auto qty=std::max<std::int64_t>(1,intValue(item["quantity"],1));if(stringValue(item["item_type"])=="equipment")for(std::int64_t i=0;i<qty;++i)equipment.append(stringValue(item["name"]));else if(stringValue(item["item_type"])=="weapon")for(std::int64_t i=0;i<qty;++i)weapons.append(stringValue(item["name"]));else inventory=changeInventory(std::move(inventory),stringValue(item["name"]),qty,stringValue(item["item_category"]));service->database()->execSqlSync(std::string("UPDATE character_cards SET ")+coinField+"=$1,inventory=$2::jsonb,equipment=$3::jsonb,weapons=$4::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$5",coins-price,compactJson(inventory),compactJson(equipment),compactJson(weapons),user.id);Json::Value ev;ev["text"]="向 NPC 購買「"+stringValue(item["name"])+"」";ev["npc_template_id"]=Json::Int64(npcId);ev["shop_item_id"]=Json::Int64(itemId);ev["price"]=Json::Int64(price);service->addEvent(roomId,user.id,"npc_shop",ev);ev["item"]=item;evaluateRuleEvent(service,roomId,user.id,"SHOP_PURCHASED",ev,false);item["effective_price"]=Json::Int64(price);item["discount_percent"]=discount;Json::Value out;out["ok"]=true;out["item"]=item;out["character"]=characterRaw(service->database(),user.id);reply(cb,std::move(out));},"NPC 商店購買失敗");});
  // =========================================================
  // 46-cpp.7：效率中心／收藏／通知／百科／批量匯入
  // =========================================================

  // [功能備註｜全站搜尋｜GET /api/search]
  // 用途：跨技能、道具、商店、怪物、NPC、任務、百科快速搜尋；避免資料量大時逐頁翻找。
  reg0("/api/search", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      auto q = trimmed(req->getParameter("q"));
      if (q.size() > 120) q.resize(120);
      Json::Value out;
      out["query"] = q;
      out["results"] = Json::Value{Json::arrayValue};
      if (q.empty()) { reply(cb, std::move(out)); return; }
      const auto rows = service->database()->execSqlSync(
          "SELECT row_to_json(x)::text AS json_text FROM ("
          "SELECT 'skill'::text entity_type,id::text entity_key,name title,category subtitle,description body FROM skill_templates WHERE name ILIKE '%'||$1||'%' OR category ILIKE '%'||$1||'%' OR description ILIKE '%'||$1||'%' OR tags::text ILIKE '%'||$1||'%' "
          "UNION ALL SELECT 'item',id::text,name,category,description FROM item_templates WHERE name ILIKE '%'||$1||'%' OR category ILIKE '%'||$1||'%' OR description ILIKE '%'||$1||'%' OR tags::text ILIKE '%'||$1||'%' "
          "UNION ALL SELECT 'magic',id::text,name,category,description FROM magic_studies WHERE active=TRUE AND (name ILIKE '%'||$1||'%' OR category ILIKE '%'||$1||'%' OR description ILIKE '%'||$1||'%') "
          "UNION ALL SELECT 'ritual',id::text,name,category,description FROM ritual_studies WHERE active=TRUE AND (name ILIKE '%'||$1||'%' OR category ILIKE '%'||$1||'%' OR description ILIKE '%'||$1||'%') "
          "UNION ALL SELECT 'shop',id::text,name,COALESCE(item_category,''),description FROM shop_items WHERE name ILIKE '%'||$1||'%' OR COALESCE(item_category,'') ILIKE '%'||$1||'%' OR description ILIKE '%'||$1||'%' OR tags::text ILIKE '%'||$1||'%' "
          "UNION ALL SELECT 'monster',id::text,name,category,description FROM monster_templates m WHERE name ILIKE '%'||$1||'%' OR category ILIKE '%'||$1||'%' OR description ILIKE '%'||$1||'%' OR EXISTS(SELECT 1 FROM entity_tags et JOIN content_tags ct ON ct.id=et.tag_id WHERE et.entity_type='monster' AND et.entity_key=m.id::text AND ct.name ILIKE '%'||$1||'%') "
          "UNION ALL SELECT 'npc',id::text,name,COALESCE(role_name,''),description FROM npc_templates n WHERE name ILIKE '%'||$1||'%' OR COALESCE(role_name,'') ILIKE '%'||$1||'%' OR description ILIKE '%'||$1||'%' OR EXISTS(SELECT 1 FROM entity_tags et JOIN content_tags ct ON ct.id=et.tag_id WHERE et.entity_type='npc' AND et.entity_key=n.id::text AND ct.name ILIKE '%'||$1||'%') "
          "UNION ALL SELECT 'task',id::text,title,COALESCE(chain_name,''),description FROM tasks t WHERE title ILIKE '%'||$1||'%' OR COALESCE(chain_name,'') ILIKE '%'||$1||'%' OR description ILIKE '%'||$1||'%' OR EXISTS(SELECT 1 FROM entity_tags et JOIN content_tags ct ON ct.id=et.tag_id WHERE et.entity_type='task' AND et.entity_key=t.id::text AND ct.name ILIKE '%'||$1||'%') "
          "UNION ALL SELECT 'codex',id::text,title,category,summary FROM codex_entries WHERE active=TRUE AND (public_by_default=TRUE OR EXISTS(SELECT 1 FROM codex_unlocks u WHERE u.entry_id=codex_entries.id AND u.user_id=$2)) AND (title ILIKE '%'||$1||'%' OR category ILIKE '%'||$1||'%' OR summary ILIKE '%'||$1||'%' OR tags::text ILIKE '%'||$1||'%')"
          ") x ORDER BY entity_type,title LIMIT 100", q, user.id);
      for (std::size_t i=0;i<rows.size();++i) out["results"].append(rowJson(rows,i));
      reply(cb, std::move(out));
    }, "搜尋失敗");
  });

  // [功能備註｜標籤｜GET /api/tags]
  reg0("/api/tags", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service,req); Json::Value out; out["tags"]=genericList(service->database(),"content_tags","name ASC,id ASC","active=TRUE"); reply(cb,std::move(out)); }, "讀取標籤失敗");
  });
  // [功能備註｜標籤｜POST /api/admin/tags]
  reg0("/api/admin/tags", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb,[&]{ requireUser(service,req,true); auto body=bodyOrEmpty(req); auto name=trimmed(stringValue(body["name"])); if(name.empty())throw ApiError(drogon::k400BadRequest,"標籤名稱不能空白"); body["name"]=name.substr(0,80); Json::Value out; out["ok"]=true; out["tag"]=genericInsert(service->database(),"content_tags",body); reply(cb,std::move(out)); },"建立標籤失敗");
  });
  reg1("/api/admin/tags/{1}", drogon::Patch, [service](const Request& req, Callback& cb,const std::string&idText){runHandler(cb,[&]{requireUser(service,req,true);Json::Value out;out["ok"]=true;out["tag"]=genericUpdate(service->database(),"content_tags",positiveId(idText),bodyOrEmpty(req));reply(cb,std::move(out));},"更新標籤失敗");});
  reg1("/api/admin/tags/{1}", drogon::Delete, [service](const Request& req, Callback& cb,const std::string&idText){runHandler(cb,[&]{requireUser(service,req,true);genericDelete(service->database(),"content_tags",positiveId(idText));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"刪除標籤失敗");});

  // [功能備註｜標籤｜GET/POST/DELETE /api/admin/entity-tags]
  // 用途：把共用標籤掛到任何實體；entity_type 例如 monster/npc/task，entity_key 通常放模板 ID。
  reg0("/api/admin/entity-tags", drogon::Get, [service](const Request& req, Callback& cb){
    runHandler(cb,[&]{requireUser(service,req,true);const auto type=trimmed(req->getParameter("entity_type"));const auto key=trimmed(req->getParameter("entity_key"));Json::Value out;out["tags"]=Json::Value{Json::arrayValue};if(!type.empty()&&!key.empty()){const auto rows=service->database()->execSqlSync("SELECT row_to_json(x)::text AS json_text FROM (SELECT ct.*,et.entity_type,et.entity_key FROM entity_tags et JOIN content_tags ct ON ct.id=et.tag_id WHERE et.entity_type=$1 AND et.entity_key=$2 ORDER BY ct.name) x",type,key);for(std::size_t i=0;i<rows.size();++i)out["tags"].append(rowJson(rows,i));}reply(cb,std::move(out));},"讀取實體標籤失敗");
  });
  reg0("/api/admin/entity-tags", drogon::Post, [service](const Request& req, Callback& cb){
    runHandler(cb,[&]{requireUser(service,req,true);const auto body=bodyOrEmpty(req);const auto tagId=intValue(body["tag_id"]);const auto type=trimmed(stringValue(body["entity_type"]));const auto key=trimmed(stringValue(body["entity_key"]));if(tagId<=0||type.empty()||key.empty())throw ApiError(drogon::k400BadRequest,"請指定標籤、實體類型與實體 ID");service->database()->execSqlSync("INSERT INTO entity_tags(tag_id,entity_type,entity_key) VALUES($1,$2,$3) ON CONFLICT DO NOTHING",tagId,type.substr(0,40),key.substr(0,120));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"套用標籤失敗");
  });
  reg0("/api/admin/entity-tags", drogon::Delete, [service](const Request& req, Callback& cb){
    runHandler(cb,[&]{requireUser(service,req,true);const auto body=bodyOrEmpty(req);service->database()->execSqlSync("DELETE FROM entity_tags WHERE tag_id=$1 AND entity_type=$2 AND entity_key=$3",intValue(body["tag_id"]),stringValue(body["entity_type"]),stringValue(body["entity_key"]));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"移除標籤失敗");
  });

  // [功能備註｜收藏｜GET/POST/DELETE /api/favorites]
  reg0("/api/favorites", drogon::Get, [service](const Request& req, Callback& cb){
    runHandler(cb,[&]{const auto user=requireUser(service,req);Json::Value out;out["favorites"]=selectRows1(service->database(),"SELECT row_to_json(f)::text AS json_text FROM user_favorites f WHERE user_id=$1 ORDER BY created_at DESC",user.id);reply(cb,std::move(out));},"讀取收藏失敗");
  });
  reg0("/api/favorites", drogon::Post, [service](const Request& req, Callback& cb){
    runHandler(cb,[&]{const auto user=requireUser(service,req);auto body=bodyOrEmpty(req);const auto type=trimmed(stringValue(body["entity_type"]));const auto key=trimmed(stringValue(body["entity_key"]));if(type.empty()||key.empty())throw ApiError(drogon::k400BadRequest,"收藏類型與 ID 不能空白");const auto label=trimmed(stringValue(body["label"]));service->database()->execSqlSync("INSERT INTO user_favorites(user_id,entity_type,entity_key,label) VALUES($1,$2,$3,$4) ON CONFLICT(user_id,entity_type,entity_key) DO UPDATE SET label=EXCLUDED.label",user.id,type.substr(0,40),key.substr(0,120),label.substr(0,160));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"加入收藏失敗");
  });
  reg0("/api/favorites", drogon::Delete, [service](const Request& req, Callback& cb){
    runHandler(cb,[&]{const auto user=requireUser(service,req);auto body=bodyOrEmpty(req);service->database()->execSqlSync("DELETE FROM user_favorites WHERE user_id=$1 AND entity_type=$2 AND entity_key=$3",user.id,stringValue(body["entity_type"]),stringValue(body["entity_key"]));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"移除收藏失敗");
  });

  // [功能備註｜通知中心｜GET /api/notifications]
  reg0("/api/notifications", drogon::Get, [service](const Request& req, Callback& cb){
    runHandler(cb,[&]{const auto user=requireUser(service,req);Json::Value out;out["notifications"]=selectRows1(service->database(),"SELECT row_to_json(n)::text AS json_text FROM user_notifications n WHERE user_id=$1 ORDER BY created_at DESC LIMIT 100",user.id);reply(cb,std::move(out));},"讀取通知失敗");
  });
  reg1("/api/notifications/{1}/read", drogon::Post, [service](const Request& req, Callback& cb,const std::string&idText){
    runHandler(cb,[&]{const auto user=requireUser(service,req);service->database()->execSqlSync("UPDATE user_notifications SET read_at=COALESCE(read_at,CURRENT_TIMESTAMP) WHERE id=$1 AND user_id=$2",positiveId(idText),user.id);Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"讀取通知失敗");
  });
  reg0("/api/admin/notifications", drogon::Post, [service](const Request& req, Callback& cb){
    runHandler(cb,[&]{requireUser(service,req,true);auto body=bodyOrEmpty(req);const auto userId=intValue(body["user_id"]);const auto title=trimmed(stringValue(body["title"]));if(userId<=0||title.empty())throw ApiError(drogon::k400BadRequest,"請指定玩家與通知標題");Json::Value extra;extra["user_id"]=Json::Int64(userId);body["title"]=title.substr(0,160);Json::Value out;out["ok"]=true;out["notification"]=genericInsert(service->database(),"user_notifications",body,extra);reply(cb,std::move(out));},"發送通知失敗");
  });

  // [功能備註｜世界百科｜GET /api/codex]
  reg0("/api/codex", drogon::Get, [service](const Request& req, Callback& cb){
    runHandler(cb,[&]{const auto user=requireUser(service,req);Json::Value out;out["entries"]=selectRows1(service->database(),"SELECT row_to_json(c)::text AS json_text FROM codex_entries c WHERE active=TRUE AND (public_by_default=TRUE OR EXISTS(SELECT 1 FROM codex_unlocks u WHERE u.entry_id=c.id AND u.user_id=$1)) ORDER BY category,sort_order,id",user.id);reply(cb,std::move(out));},"讀取百科失敗");
  });
  reg0("/api/admin/codex", drogon::Get, [service](const Request& req, Callback& cb){runHandler(cb,[&]{requireUser(service,req,true);Json::Value out;out["entries"]=genericList(service->database(),"codex_entries","category ASC,sort_order ASC,id ASC");reply(cb,std::move(out));},"讀取百科管理失敗");});
  reg0("/api/admin/codex", drogon::Post, [service](const Request& req, Callback& cb){runHandler(cb,[&]{requireUser(service,req,true);auto body=bodyOrEmpty(req);if(trimmed(stringValue(body["title"])).empty())throw ApiError(drogon::k400BadRequest,"百科標題不能空白");Json::Value out;out["ok"]=true;out["entry"]=genericInsert(service->database(),"codex_entries",body);reply(cb,std::move(out));},"建立百科失敗");});
  reg1("/api/admin/codex/{1}",drogon::Patch,[service](const Request& req,Callback& cb,const std::string&idText){runHandler(cb,[&]{requireUser(service,req,true);Json::Value out;out["ok"]=true;out["entry"]=genericUpdate(service->database(),"codex_entries",positiveId(idText),bodyOrEmpty(req));reply(cb,std::move(out));},"更新百科失敗");});
  reg1("/api/admin/codex/{1}",drogon::Delete,[service](const Request& req,Callback& cb,const std::string&idText){runHandler(cb,[&]{requireUser(service,req,true);genericDelete(service->database(),"codex_entries",positiveId(idText));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"刪除百科失敗");});
  reg1("/api/admin/codex/{1}/unlock",drogon::Post,[service](const Request& req,Callback& cb,const std::string&idText){runHandler(cb,[&]{requireUser(service,req,true);const auto body=bodyOrEmpty(req);const auto userId=intValue(body["user_id"]);if(userId<=0)throw ApiError(drogon::k400BadRequest,"玩家 ID 無效");service->database()->execSqlSync("INSERT INTO codex_unlocks(entry_id,user_id,note) VALUES($1,$2,$3) ON CONFLICT(entry_id,user_id) DO UPDATE SET note=EXCLUDED.note,unlocked_at=CURRENT_TIMESTAMP",positiveId(idText),userId,stringValue(body["note"]));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"解鎖百科失敗");});

  // [功能備註｜自訂資源條｜PATCH /api/character/custom-resources]
  reg0("/api/character/custom-resources",drogon::Patch,[service](const Request& req,Callback& cb){
    runHandler(cb,[&]{const auto user=requireUser(service,req);const auto body=bodyOrEmpty(req);auto resources=body["resources"];if(!resources.isArray())throw ApiError(drogon::k400BadRequest,"resources 必須是陣列");if(resources.size()>30)throw ApiError(drogon::k400BadRequest,"自訂資源最多 30 條");Json::Value clean{Json::arrayValue};for(const auto&r:resources){if(!r.isObject())continue;auto name=trimmed(stringValue(r["name"]));if(name.empty())continue;Json::Value x;x["name"]=name.substr(0,60);x["current"]=std::max<std::int64_t>(0,intValue(r["current"]));x["max"]=std::max<std::int64_t>(0,intValue(r["max"]));if(intValue(x["max"])>0&&intValue(x["current"])>intValue(x["max"]))x["current"]=x["max"];x["unit"]=stringValue(r["unit"]).substr(0,20);clean.append(x);}service->database()->execSqlSync("UPDATE character_cards SET custom_resources=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",compactJson(clean),user.id);Json::Value out;out["ok"]=true;out["resources"]=clean;out["character"]=characterRaw(service->database(),user.id);reply(cb,std::move(out));},"更新自訂資源失敗");
  });

  // [功能備註｜跑團時間線｜GET /api/rooms/{1}/timeline]
  reg1("/api/rooms/{1}/timeline",drogon::Get,[service](const Request& req,Callback& cb,const std::string&roomIdText){
    runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);requireRoomAccess(service,user,roomId);Json::Value out;out["events"]=selectRows1(service->database(),"SELECT row_to_json(e)::text AS json_text FROM events e WHERE room_id=$1 ORDER BY id DESC LIMIT 200",roomId);reply(cb,std::move(out));},"讀取跑團時間線失敗");
  });

  // [功能備註｜模板中心｜GET /api/admin/template-center]
  reg0("/api/admin/template-center",drogon::Get,[service](const Request& req,Callback& cb){
    runHandler(cb,[&]{requireUser(service,req,true);Json::Value out;const std::array<std::pair<const char*,const char*>,13> groups={{{"skills","skill_templates"},{"items","item_templates"},{"magics","magic_studies"},{"rituals","ritual_studies"},{"monsters","monster_templates"},{"npcs","npc_templates"},{"professions","profession_templates"},{"summons","summon_templates"},{"recipes","recipes"},{"statuses","status_templates"},{"clues","clue_templates"},{"codex","codex_entries"},{"shop","shop_items"}}};for(const auto&[key,table]:groups){const auto rows=service->database()->execSqlSync(std::string("SELECT COUNT(*)::BIGINT AS n FROM ")+table);out[key]=rows.empty()?0:Json::Int64(rows[0]["n"].as<std::int64_t>());}reply(cb,std::move(out));},"讀取模板中心失敗");
  });

  // [功能備註｜商店進階｜GET /api/shop/transactions]
  reg0("/api/shop/transactions",drogon::Get,[service](const Request& req,Callback& cb){runHandler(cb,[&]{const auto user=requireUser(service,req);Json::Value out;out["transactions"]=selectRows1(service->database(),"SELECT row_to_json(t)::text AS json_text FROM shop_transactions t WHERE user_id=$1 ORDER BY created_at DESC LIMIT 100",user.id);reply(cb,std::move(out));},"讀取交易紀錄失敗");});

  // [功能備註｜商店進階｜POST /api/shop/sell]
  reg0("/api/shop/sell",drogon::Post,[service](const Request& req,Callback& cb){
    runHandler(cb,[&]{const auto user=requireUser(service,req);const auto body=bodyOrEmpty(req);const auto name=trimmed(stringValue(body["name"]));const auto quantity=std::clamp<std::int64_t>(intValue(body["quantity"],1),1,9999);if(name.empty())throw ApiError(drogon::k400BadRequest,"請指定出售道具");auto character=characterRaw(service->database(),user.id);auto inventory=inventoryOf(character);if(inventoryQuantity(inventory,name)<quantity)throw ApiError(drogon::k400BadRequest,"持有數量不足");const auto rows=service->database()->execSqlSync("SELECT id,price_weird,sell_percent FROM shop_items WHERE name=$1 ORDER BY active DESC,id DESC LIMIT 1",name);if(rows.empty())throw ApiError(drogon::k400BadRequest,"此物品沒有商店回收價格");const auto shopId=rows[0]["id"].as<std::int64_t>();const auto base=std::max<std::int64_t>(0,rows[0]["price_weird"].as<std::int64_t>());const auto percent=rows[0]["sell_percent"].as<double>();const auto unit=static_cast<std::int64_t>(std::floor(base*std::clamp(percent,0.0,100.0)/100.0));const auto total=unit*quantity;inventory=changeInventory(std::move(inventory),name,-quantity);const auto coins=std::max<std::int64_t>(0,intValue(character["weird_coins"]))+total;service->database()->execSqlSync("UPDATE character_cards SET inventory=$1::jsonb,weird_coins=$2,updated_at=CURRENT_TIMESTAMP WHERE user_id=$3",compactJson(inventory),coins,user.id);service->database()->execSqlSync("INSERT INTO shop_transactions(user_id,shop_item_id,direction,item_name,quantity,unit_price,total_price) VALUES($1,$2,'sell',$3,$4,$5,$6)",user.id,shopId,name,quantity,unit,total);Json::Value out;out["ok"]=true;out["earned"]=Json::Int64(total);out["character"]=characterRaw(service->database(),user.id);reply(cb,std::move(out));},"出售物品失敗");
  });


  // =========================================================
  // 47-cpp.8 魔法學／儀式學／元素儲存
  // =========================================================

  // [功能備註｜魔法學｜GET /api/magic-studies]
  // 用途：玩家查看可學魔法、已學魔法與剩餘魔法學點數。
  reg0("/api/magic-studies", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user=requireUser(service,req);
      auto character=characterRaw(service->database(),user.id);
      Json::Value out;
      out["points"]=Json::Int64(std::max<std::int64_t>(0,intValue(character["magic_points"])));
      out["studies"]=genericList(service->database(),"magic_studies","element ASC,tree_y ASC,tree_x ASC,rank ASC,name ASC,id ASC","active=TRUE");
      out["learned"]=selectRows1(service->database(),
        "SELECT row_to_json(x)::text AS json_text FROM (SELECT cm.learned_at,m.* FROM character_magics cm JOIN magic_studies m ON m.id=cm.magic_id WHERE cm.user_id=$1 ORDER BY cm.learned_at,m.id) x",user.id);
      reply(cb,std::move(out));
    },"讀取魔法學失敗");
  });

  // [功能備註｜儀式學｜GET /api/ritual-studies]
  // [功能備註｜63-cpp.24.1 儀式研究進度] 玩家只會收到自己目前研究進度已揭露的配方內容。
  reg0("/api/ritual-studies", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user=requireUser(service,req);
      auto character=characterRaw(service->database(),user.id);
      const auto learnedRows=service->database()->execSqlSync("SELECT ritual_id FROM character_rituals WHERE user_id=$1",user.id);
      std::set<std::int64_t> learnedIds;for(const auto& row:learnedRows)learnedIds.insert(row["ritual_id"].as<std::int64_t>());
      const auto rows=service->database()->execSqlSync(
        "SELECT row_to_json(r)::text AS json_text FROM ritual_studies r WHERE active=TRUE ORDER BY category,rank,name,id");
      Json::Value studies{Json::arrayValue};
      for(std::size_t i=0;i<rows.size();++i){
        auto study=rowJson(rows,i);const auto ritualId=intValue(study["id"]);
        const bool learned=learnedIds.count(ritualId)>0;auto research=ritualResearchState(service->database(),user.id,ritualId);
        const bool discovered=learned||research.get("discovered",false).asBool();
        if(study.get("hidden_until_discovered",false).asBool()&&!discovered)continue;
        studies.append(ritualStudyForViewer(study,research,learned));
      }
      Json::Value out;
      out["points"]=Json::Int64(std::max<std::int64_t>(0,intValue(character["ritual_points"])));
      out["progression"]=ritualProgressionJson(character);
      out["studies"]=studies;
      out["learned"]=selectRows1(service->database(),
        "SELECT row_to_json(x)::text AS json_text FROM (SELECT cr.learned_at,r.* FROM character_rituals cr JOIN ritual_studies r ON r.id=cr.ritual_id WHERE cr.user_id=$1 ORDER BY cr.learned_at,r.id) x",user.id);
      reply(cb,std::move(out));
    },"讀取儀式學失敗");
  });

  // [功能備註｜63-cpp.24.1 未解析儀式｜POST /api/ritual-studies/{1}/research]
  // 使用可用儀式學點數推進「自己的」解析進度；研究本身給少量 XP，但不允許形成無限刷點循環。
  reg1("/api/ritual-studies/{1}/research",drogon::Post,[service](const Request& req,Callback& cb,const std::string& idText){
    runHandler(cb,[&]{
      const auto user=requireUser(service,req);const auto ritualId=positiveId(idText,"儀式 ID 無效");const auto body=bodyOrEmpty(req);
      const auto rows=service->database()->execSqlSync("SELECT row_to_json(r)::text AS json_text FROM ritual_studies r WHERE id=$1 AND active=TRUE",ritualId);
      if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到此儀式");
      const auto study=rowJson(rows);auto research=ritualResearchState(service->database(),user.id,ritualId);
      if(study.get("hidden_until_discovered",false).asBool()&&!research.get("discovered",false).asBool())
        throw ApiError(drogon::k403Forbidden,"你尚未發現這個儀式");
      if(!study.get("requires_research",false).asBool()||intValue(research["progress"])>=100)
        throw ApiError(drogon::k409Conflict,"此儀式不需要繼續解析");
      const auto points=std::clamp<std::int64_t>(intValue(body["points"],1),1,50);
      const auto perPoint=std::max<std::int64_t>(1,intValue(study["research_progress_per_point"],10));
      const auto xpPerPoint=std::clamp<std::int64_t>(intValue(study["research_xp_per_point"],5),0,20);
      const auto oldProgress=std::clamp<std::int64_t>(intValue(research["progress"]),0,100);
      const auto neededPoints=std::max<std::int64_t>(1,(100-oldProgress+perPoint-1)/perPoint);
      const auto spend=std::min(points,neededPoints);
      const auto updated=service->database()->execSqlSync(
        "UPDATE character_cards SET ritual_points=ritual_points-$1,ritual_points_spent=ritual_points_spent+$1,"
        "updated_at=CURRENT_TIMESTAMP WHERE user_id=$2 AND ritual_points>=$1 RETURNING ritual_points",
        spend,user.id);
      if(updated.empty())throw ApiError(drogon::k400BadRequest,"儀式學點數不足");
      const auto progressGain=std::min<std::int64_t>(100-oldProgress,spend*perPoint);
      try{
        service->database()->execSqlSync(
          "INSERT INTO character_ritual_research(user_id,ritual_id,discovered,progress,points_invested,last_researched_at) "
          "VALUES($1,$2,TRUE,$3,$4,CURRENT_TIMESTAMP) "
          "ON CONFLICT(user_id,ritual_id) DO UPDATE SET discovered=TRUE,"
          "progress=LEAST(100,character_ritual_research.progress+$3),points_invested=character_ritual_research.points_invested+$4,"
          "last_researched_at=CURRENT_TIMESTAMP,updated_at=CURRENT_TIMESTAMP",
          user.id,ritualId,progressGain,spend);
      }catch(...){
        service->database()->execSqlSync(
          "UPDATE character_cards SET ritual_points=ritual_points+$1,ritual_points_spent=GREATEST(0,ritual_points_spent-$1),updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",
          spend,user.id);
        throw;
      }
      auto progression=grantRitualXp(service->database(),user.id,spend*xpPerPoint);
      auto finalResearch=ritualResearchState(service->database(),user.id,ritualId);
      Json::Value out;out["ok"]=true;out["research"]=finalResearch;out["progression"]=progression;
      out["study"]=ritualStudyForViewer(study,finalResearch,false);out["character"]=characterRaw(service->database(),user.id);
      reply(cb,std::move(out));
    },"研究儀式失敗");
  });

  // [功能備註｜魔法學｜POST /api/magic-studies/{1}/learn]
  // 用途：使用魔法學點數學習；C++ 驗證前置、點數與重複學習。
  reg1("/api/magic-studies/{1}/learn", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb,[&]{
      const auto user=requireUser(service,req);
      const auto id=positiveId(idText,"魔法 ID 無效");
      const auto rows=service->database()->execSqlSync(
        "SELECT row_to_json(m)::text AS json_text FROM magic_studies m WHERE id=$1 AND active=TRUE",id);
      if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到此魔法");
      const auto study=rowJson(rows);
      requireKnowledgeUnlockIfSold(service->database(),user.id,"magic",id);
      if(!service->database()->execSqlSync("SELECT 1 FROM character_magics WHERE user_id=$1 AND magic_id=$2",user.id,id).empty())
        throw ApiError(drogon::k409Conflict,"你已經學會這個魔法");
      if(study["prerequisite_ids"].isArray()){
        for(const auto& raw:study["prerequisite_ids"]){
          const auto required=intValue(raw);
          if(required>0&&service->database()->execSqlSync("SELECT 1 FROM character_magics WHERE user_id=$1 AND magic_id=$2",user.id,required).empty())
            throw ApiError(drogon::k400BadRequest,"尚未滿足魔法前置條件");
        }
      }
      // [功能備註｜65-cpp.26 魔法書學習] 可要求玩家背包真的持有指定魔法書；consume_book=true 時學會後消耗一本。
      const auto requiredBook=trimmed(stringValue(study["required_book_item"]));
      auto character=characterRaw(service->database(),user.id);
      auto inventory=inventoryOf(character);
      if(!requiredBook.empty()&&inventoryQuantity(inventory,requiredBook)<=0)
        throw ApiError(drogon::k400BadRequest,"需要先取得魔法書：「"+requiredBook+"」");
      const auto cost=std::max<std::int64_t>(0,intValue(study["point_cost"]));
      const auto update=service->database()->execSqlSync(
        "UPDATE character_cards SET magic_points=magic_points-$1,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2 AND magic_points>=$1 RETURNING magic_points",cost,user.id);
      if(update.empty())throw ApiError(drogon::k400BadRequest,"魔法學點數不足");
      try{
        service->database()->execSqlSync("INSERT INTO character_magics(user_id,magic_id) VALUES($1,$2)",user.id,id);
        if(!requiredBook.empty()&&study.get("consume_book",false).asBool()){
          inventory=changeInventory(std::move(inventory),requiredBook,-1);
          service->database()->execSqlSync("UPDATE character_cards SET inventory=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",compactJson(inventory),user.id);
        }
      }catch(...){
        service->database()->execSqlSync("DELETE FROM character_magics WHERE user_id=$1 AND magic_id=$2",user.id,id);
        service->database()->execSqlSync("UPDATE character_cards SET magic_points=magic_points+$1 WHERE user_id=$2",cost,user.id);
        throw;
      }
      Json::Value out;out["ok"]=true;out["study"]=study;out["character"]=service->characterByUser(user.id);reply(cb,std::move(out));
    },"學習魔法失敗");
  });

  // [功能備註｜65-cpp.26 魔法施放｜POST /api/magic-studies/{1}/cast]
  // 已學魔法可連結既有技能模板；施放時先驗證七元素儲存，再共用多段攻擊／事件型技能核心。
  reg1("/api/magic-studies/{1}/cast", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb,[&]{
      const auto user=requireUser(service,req);const auto id=positiveId(idText,"魔法 ID 無效");const auto body=bodyOrEmpty(req);
      if(service->database()->execSqlSync("SELECT 1 FROM character_magics WHERE user_id=$1 AND magic_id=$2",user.id,id).empty())
        throw ApiError(drogon::k403Forbidden,"你尚未學會這個魔法");
      const auto rows=service->database()->execSqlSync("SELECT row_to_json(m)::text AS json_text FROM magic_studies m WHERE id=$1 AND active=TRUE",id);
      if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到此魔法");
      const auto study=rowJson(rows);
      const auto linkedSkillId=intValue(study["linked_skill_template_id"]);
      if(linkedSkillId<=0)throw ApiError(drogon::k409Conflict,"此魔法是被動／研究節點，尚未連結可施放技能模板");
      const auto skillRows=service->database()->execSqlSync("SELECT row_to_json(s)::text AS json_text FROM skill_templates s WHERE id=$1",linkedSkillId);
      if(skillRows.empty())throw ApiError(drogon::k409Conflict,"此魔法連結的技能模板已不存在");
      auto character=service->characterByUser(user.id);
      const auto costStatus=magicElementCostStatus(character,study["element_cost"]);
      if(!costStatus.get("ok",false).asBool()){
        const auto first=costStatus["missing"].isArray()&&!costStatus["missing"].empty()?costStatus["missing"][0]:Json::Value{Json::objectValue};
        throw ApiError(drogon::k400BadRequest,"元素不足："+stringValue(first["element"],"元素")+" 需要 "+std::to_string(intValue(first["need"]))+"，目前 "+std::to_string(intValue(first["have"])));
      }
      Json::Value acquisition;acquisition["source"]="magic_study";acquisition["magic_id"]=Json::Int64(id);
      auto skill=buildSkillFromTemplate(rowJson(skillRows),acquisition);
      if(!skill["data"].isObject())skill["data"]=Json::Value{Json::objectValue};
      skill["data"]["element"]=stringValue(study["element"]);
      skill["magic_study_id"]=Json::Int64(id);
      skill["magic_rank"]=study["rank"];
      const auto roomId=intValue(body["room_id"]);
      if(roomId<=0)throw ApiError(drogon::k400BadRequest,"施放魔法需要指定房間");
      requireRoomAccess(service,user,roomId);
      Json::Value result;result["ok"]=true;result["magic"]=study;result["skill"]=skill;
      const auto battleRows=service->database()->execSqlSync("SELECT battle_active FROM rooms WHERE id=$1",roomId);
      const bool battleActive=!battleRows.empty()&&battleRows[0]["battle_active"].as<bool>();
      const auto data=skill["data"].isObject()?skill["data"]:Json::Value{Json::objectValue};
      const bool configured=data.get("combat_attack",false).asBool()||!stringValue(skill["damage_formula"]).empty()||data["hit_sequence"].isArray()||intValue(data["hit_count"])>0;
      if(battleActive&&configured&&stringValue(skill["skill_type"],"active")!="passive"){
        result["combat_result"]=executeConfiguredCombatSkill(service,roomId,"player",user.id,skill,stringValue(body["target_type"]),intValue(body["target_id"]),user.id);
      }else{
        Json::Value payload;payload["text"]=stringValue(character["name"],user.username)+" 施放魔法「"+stringValue(study["name"],"魔法")+"」";payload["magic_id"]=Json::Int64(id);payload["element"]=study["element"];
        service->addEvent(roomId,user.id,"magic",payload);result["event"]=payload;
      }
      const auto nextStorage=spendMagicElements(character["element_storage"],study["element_cost"]);
      service->database()->execSqlSync("UPDATE character_cards SET element_storage=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",compactJson(nextStorage),user.id);
      result["element_cost"]=study["element_cost"];
      result["character"]=service->characterByUser(user.id);
      result["room"]=roomSnapshotForViewer(service,roomId,user.id);
      reply(cb,std::move(result));
    },"施放魔法失敗");
  });

  // [功能備註｜儀式學｜POST /api/ritual-studies/{1}/learn]
  reg1("/api/ritual-studies/{1}/learn", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb,[&]{
      const auto user=requireUser(service,req);
      const auto id=positiveId(idText,"儀式 ID 無效");
      const auto rows=service->database()->execSqlSync(
        "SELECT row_to_json(r)::text AS json_text FROM ritual_studies r WHERE id=$1 AND active=TRUE",id);
      if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到此儀式");
      const auto study=rowJson(rows);
      requireKnowledgeUnlockIfSold(service->database(),user.id,"ritual",id);
      if(!service->database()->execSqlSync("SELECT 1 FROM character_rituals WHERE user_id=$1 AND ritual_id=$2",user.id,id).empty())
        throw ApiError(drogon::k409Conflict,"你已經學會這個儀式");
      if(study["prerequisite_ids"].isArray()){
        for(const auto& raw:study["prerequisite_ids"]){
          const auto required=intValue(raw);
          if(required>0&&service->database()->execSqlSync("SELECT 1 FROM character_rituals WHERE user_id=$1 AND ritual_id=$2",user.id,required).empty())
            throw ApiError(drogon::k400BadRequest,"尚未滿足儀式前置條件");
        }
      }
      // [功能備註｜63-cpp.24.1 學習前解析驗證] 需要研究的儀式必須先達 100%，避免只存點數直接跳過解析。
      if(study.get("requires_research",false).asBool()){
        const auto research=ritualResearchState(service->database(),user.id,id);
        if(intValue(research["progress"])<100)throw ApiError(drogon::k400BadRequest,"此儀式尚未完全解析");
      }
      const auto cost=std::max<std::int64_t>(0,intValue(study["point_cost"]));
      const auto update=service->database()->execSqlSync(
        "UPDATE character_cards SET ritual_points=ritual_points-$1,ritual_points_spent=ritual_points_spent+$1,"
        "updated_at=CURRENT_TIMESTAMP WHERE user_id=$2 AND ritual_points>=$1 RETURNING ritual_points",cost,user.id);
      if(update.empty())throw ApiError(drogon::k400BadRequest,"儀式學點數不足");
      try{
        service->database()->execSqlSync("INSERT INTO character_rituals(user_id,ritual_id) VALUES($1,$2)",user.id,id);
        service->database()->execSqlSync(
          "INSERT INTO character_ritual_research(user_id,ritual_id,discovered,progress) VALUES($1,$2,TRUE,100) "
          "ON CONFLICT(user_id,ritual_id) DO UPDATE SET discovered=TRUE,progress=100,updated_at=CURRENT_TIMESTAMP",user.id,id);
      }catch(...){
        service->database()->execSqlSync(
          "UPDATE character_cards SET ritual_points=ritual_points+$1,ritual_points_spent=GREATEST(0,ritual_points_spent-$1) WHERE user_id=$2",cost,user.id);
        throw;
      }
      Json::Value out;out["ok"]=true;out["study"]=study;out["character"]=characterRaw(service->database(),user.id);reply(cb,std::move(out));
    },"學習儀式失敗");
  });

  // [功能備註｜魔法學｜GET/POST /api/admin/magic-studies]
  reg0("/api/admin/magic-studies",drogon::Get,[service](const Request& req,Callback& cb){
    runHandler(cb,[&]{requireUser(service,req,true);Json::Value out;out["studies"]=genericList(service->database(),"magic_studies","element ASC,tree_y ASC,tree_x ASC,rank ASC,name ASC,id ASC");reply(cb,std::move(out));},"讀取魔法學資料庫失敗");
  });
  reg0("/api/admin/magic-studies",drogon::Post,[service](const Request& req,Callback& cb){
    runHandler(cb,[&]{
      requireUser(service,req,true);auto body=bodyOrEmpty(req);const auto name=trimmed(stringValue(body["name"]));if(name.empty())throw ApiError(drogon::k400BadRequest,"魔法名稱不能空白");
      body["name"]=name.substr(0,120);body["rank"]=normalizeContentRank(stringValue(body["rank"],"G"));body["element"]=normalizeMagicElement(stringValue(body["element"]));
      body["tree_x"]=std::clamp<std::int64_t>(intValue(body["tree_x"]),0,40);body["tree_y"]=std::clamp<std::int64_t>(intValue(body["tree_y"]),0,60);body["icon"]=trimmed(stringValue(body["icon"],"✦")).substr(0,40);
      body["node_style"]=normalizeMagicNodeStyle(body["node_style"],stringValue(body["category"],"一般魔法"));body["point_cost"]=Json::Int64(std::max<std::int64_t>(0,intValue(body["point_cost"],1)));
      body["element_cost"]=normalizedMagicElementCost(body["element_cost"]);body["required_book_item"]=trimmed(stringValue(body["required_book_item"])).substr(0,180);body["consume_book"]=body.get("consume_book",false).asBool();
      const auto linkedSkill=std::max<std::int64_t>(0,intValue(body["linked_skill_template_id"]));validateMagicLinkedSkill(service->database(),linkedSkill);if(linkedSkill>0)body["linked_skill_template_id"]=Json::Int64(linkedSkill);else body["linked_skill_template_id"]=Json::Value{Json::nullValue};
      validateMagicTreePrerequisites(service->database(),stringValue(body["element"]),body["prerequisite_ids"]);Json::Value out;out["ok"]=true;out["study"]=genericInsert(service->database(),"magic_studies",body);reply(cb,std::move(out));
    },"建立魔法失敗");
  });
  reg1("/api/admin/magic-studies/{1}",drogon::Patch,[service](const Request& req,Callback& cb,const std::string&idText){runHandler(cb,[&]{
    requireUser(service,req,true);const auto id=positiveId(idText);auto body=bodyOrEmpty(req);
    if(body.isMember("rank"))body["rank"]=normalizeContentRank(stringValue(body["rank"]));if(body.isMember("element"))body["element"]=normalizeMagicElement(stringValue(body["element"]));
    if(body.isMember("tree_x"))body["tree_x"]=std::clamp<std::int64_t>(intValue(body["tree_x"]),0,40);if(body.isMember("tree_y"))body["tree_y"]=std::clamp<std::int64_t>(intValue(body["tree_y"]),0,60);
    if(body.isMember("node_style"))body["node_style"]=normalizeMagicNodeStyle(body["node_style"],stringValue(body["category"]));if(body.isMember("element_cost"))body["element_cost"]=normalizedMagicElementCost(body["element_cost"]);
    if(body.isMember("required_book_item"))body["required_book_item"]=trimmed(stringValue(body["required_book_item"])).substr(0,180);if(body.isMember("consume_book"))body["consume_book"]=body["consume_book"].asBool();
    if(body.isMember("linked_skill_template_id")){const auto linked=std::max<std::int64_t>(0,intValue(body["linked_skill_template_id"]));validateMagicLinkedSkill(service->database(),linked);body["linked_skill_template_id"]=linked>0?Json::Value{Json::Int64(linked)}:Json::Value{Json::nullValue};}
    const auto existing=service->database()->execSqlSync("SELECT element,prerequisite_ids FROM magic_studies WHERE id=$1",id);if(existing.empty())throw ApiError(drogon::k404NotFound,"找不到魔法節點");
    const auto element=body.isMember("element")?stringValue(body["element"]):(existing[0]["element"].isNull()?std::string{"無"}:existing[0]["element"].as<std::string>());Json::Value prereqs;if(body.isMember("prerequisite_ids"))prereqs=body["prerequisite_ids"];else if(!existing[0]["prerequisite_ids"].isNull())prereqs=parseJson(existing[0]["prerequisite_ids"].as<std::string>());validateMagicTreePrerequisites(service->database(),element,prereqs,id);
    Json::Value out;out["ok"]=true;out["study"]=genericUpdate(service->database(),"magic_studies",id,body);reply(cb,std::move(out));
  },"更新魔法節點失敗");});
  reg1("/api/admin/magic-studies/{1}",drogon::Delete,[service](const Request& req,Callback& cb,const std::string&idText){runHandler(cb,[&]{requireUser(service,req,true);genericDelete(service->database(),"magic_studies",positiveId(idText));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"刪除魔法節點失敗");});

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
