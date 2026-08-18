#include "trpg/LegacyCompat.h"
#include "trpg/CoreService.h"

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

Json::Value characterRaw(const Db& db, std::int64_t userId) {
  const auto rows = db->execSqlSync(
      "SELECT row_to_json(c)::text AS json_text FROM character_cards c WHERE user_id=$1", userId);
  if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到角色卡");
  return rowJson(rows);
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

std::string pathParamRoute(std::string route) {
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
  const std::array<std::filesystem::path, 4> candidates = {
      std::filesystem::path{"db/legacy_v39_migrations.sql"},
      std::filesystem::path{"/app/db/legacy_v39_migrations.sql"},
      std::filesystem::path{"../db/legacy_v39_migrations.sql"},
      std::filesystem::path{"../../db/legacy_v39_migrations.sql"}};
  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
  }
  throw std::runtime_error("db/legacy_v39_migrations.sql not found");
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
  reg0("/api/character/fifth-resource", drogon::Patch,
       [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req);
      throw ApiError(drogon::k403Forbidden,
                     "玩家不能直接修改大道／信仰目前值；由 DM、技能或事件處理");
    });
  });

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

  reg0("/api/character/skill", drogon::Post,
       [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req);
      throw ApiError(drogon::k403Forbidden,
          "不能直接免費建立技能；請透過技能資料庫支付學習代價、職業自動取得、任務獎勵或由 DM 授予");
    });
  });

  // Public template libraries.
  reg0("/api/professions", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req);
      Json::Value out;
      out["professions"] = genericList(service->database(), "profession_templates", "name ASC");
      reply(cb, std::move(out));
    });
  });
  reg0("/api/skill-templates", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req);
      Json::Value out;
      out["skills"] = genericList(service->database(), "skill_templates", "category ASC,name ASC,id ASC");
      reply(cb, std::move(out));
    });
  });
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
  reg0("/api/character/professions/acquire", drogon::Post,
       [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto body = bodyOrEmpty(req);
      const auto professionId = intValue(body["profession_id"]);
      const auto role = stringValue(body["role"]) == "sub" ? "sub" : "main";
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
  reg0("/api/recipes", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req);
      Json::Value out;
      out["recipes"] = genericList(service->database(), "recipes");
      reply(cb, std::move(out));
    });
  });

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
        if (outputType == "equipment") {
          for (std::int64_t i = 0; i < outputQty; ++i) equipment.append(outputName);
        } else if (outputType == "weapon") {
          for (std::int64_t i = 0; i < outputQty; ++i) weapons.append(outputName);
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
  reg0("/api/shop", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req);
      Json::Value out;
      out["items"] = genericList(service->database(), "shop_items", "id ASC", "active=TRUE");
      reply(cb, std::move(out));
    });
  });
  reg0("/api/admin/shop", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["items"] = genericList(service->database(), "shop_items");
      reply(cb, std::move(out));
    });
  });
  reg0("/api/admin/shop", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["ok"] = true;
      out["item"] = genericInsert(service->database(), "shop_items", bodyOrEmpty(req));
      reply(cb, std::move(out));
    });
  });
  registerAdminPatchDelete1(service, "/api/admin/shop/{1}", "shop_items", "item");

  reg1("/api/shop/{1}/buy", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& itemIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto itemId = positiveId(itemIdText, "商品 ID 無效");
      const auto rows = service->database()->execSqlSync(
          "SELECT row_to_json(s)::text AS json_text FROM shop_items s WHERE id=$1 AND active=TRUE", itemId);
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到商品");
      const auto item = rowJson(rows);
      auto character = characterRaw(service->database(), user.id);
      auto coins = std::max<std::int64_t>(0, intValue(character["weird_coins"]));
      const auto price = std::max<std::int64_t>(0, intValue(item["price_weird"]));
      if (coins < price) throw ApiError(drogon::k400BadRequest, "詭幣不足");
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
      Json::Value out;
      out["ok"] = true;
      out["item"] = item;
      out["character"] = characterRaw(service->database(), user.id);
      reply(cb, std::move(out));
    }, "購買失敗");
  });

  // Learn/remove/use character skills.
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
      auto character = characterRaw(service->database(), user.id);
      auto skills = character["skills"].isArray() ? character["skills"] : Json::Value{Json::arrayValue};
      for (const auto& skill : skills) {
        if (intValue(skill["template_id"]) == templateId)
          throw ApiError(drogon::k409Conflict, "你已經擁有這個技能");
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
      const auto body = bodyOrEmpty(req);
      const auto roomId = intValue(body["room_id"]);
      Json::Value result;
      result["ok"] = true;
      result["skill"] = selected;
      result["charging"] = false;
      if (roomId > 0) {
        requireRoomAccess(service, user, roomId);
        Json::Value payload;
        payload["text"] = stringValue(character["name"], user.username) + " 使用技能「" + stringValue(selected["name"], skillId) + "」";
        payload["skill_id"] = skillId;
        service->addEvent(roomId, user.id, "skill", payload);
        result["room"] = service->roomSnapshot(roomId);
      }
      result["character"] = characterRaw(service->database(), user.id);
      reply(cb, std::move(result));
    }, "使用技能失敗");
  });

  // Summons.
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
  reg0("/api/admin/extra-skills", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["extra_skills"] = genericList(service->database(), "extra_skills");
      reply(cb, std::move(out));
    });
  });
  reg0("/api/admin/extra-skills", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["ok"] = true;
      out["extra_skill"] = genericInsert(service->database(), "extra_skills", bodyOrEmpty(req));
      reply(cb, std::move(out));
    });
  });

  registerAdminCrud0(service, "/api/admin/summon-templates", "summon_templates", "summon_templates");
  registerAdminPatchDelete1(service, "/api/admin/summon-templates/{1}", "summon_templates", "summon_template");

  reg0("/api/admin/recipes", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["recipes"] = genericList(service->database(), "recipes");
      reply(cb, std::move(out));
    });
  });
  reg0("/api/admin/recipes", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["ok"] = true;
      out["recipe"] = genericInsert(service->database(), "recipes", bodyOrEmpty(req));
      reply(cb, std::move(out));
    });
  });
  reg1("/api/admin/recipes/{1}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      genericDelete(service->database(), "recipes", positiveId(idText));
      Json::Value out; out["ok"] = true; reply(cb, std::move(out));
    });
  });

  reg0("/api/bloodlines", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req);
      Json::Value out;
      out["bloodlines"] = genericList(service->database(), "bloodline_templates", "name ASC");
      reply(cb, std::move(out));
    });
  });
  registerAdminCrud0(service, "/api/admin/bloodlines", "bloodline_templates", "bloodlines");
  registerAdminPatchDelete1(service, "/api/admin/bloodlines/{1}", "bloodline_templates", "bloodline");

  reg0("/api/admin/professions", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["ok"] = true;
      out["profession"] = genericInsert(service->database(), "profession_templates", bodyOrEmpty(req));
      reply(cb, std::move(out));
    });
  });
  registerAdminPatchDelete1(service, "/api/admin/professions/{1}", "profession_templates", "profession");

  reg0("/api/admin/skill-templates", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["ok"] = true;
      out["skill"] = genericInsert(service->database(), "skill_templates", bodyOrEmpty(req));
      reply(cb, std::move(out));
    });
  });
  registerAdminPatchDelete1(service, "/api/admin/skill-templates/{1}", "skill_templates", "skill");

  // Profession level-up. Level and rank live inside profession_progress JSONB.
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

  // Global DM identity and character/player administration.
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

  reg1("/api/admin/characters/{1}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      genericDelete(service->database(), "character_cards", positiveId(idText, "角色 ID 無效"));
      Json::Value out; out["ok"] = true; reply(cb, std::move(out));
    }, "刪除角色卡失敗");
  });

  reg0("/api/admin/players", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["players"] = selectRows(service->database(),
          "SELECT row_to_json(x)::text AS json_text FROM (SELECT u.id,u.username,u.is_admin,c.id AS character_id,c.name,c.faction,c.tier,c.level,c.main_class FROM users u LEFT JOIN character_cards c ON c.user_id=u.id ORDER BY u.id) x");
      reply(cb, std::move(out));
    });
  });

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

  reg1("/api/admin/players/{1}/character", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& userIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto userId = positiveId(userIdText, "玩家 ID 無效");
      service->database()->execSqlSync("DELETE FROM character_cards WHERE user_id=$1", userId);
      Json::Value out; out["ok"] = true; reply(cb, std::move(out));
    }, "刪除玩家角色失敗");
  });

  reg1("/api/admin/rooms/{1}", drogon::Get,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      Json::Value out = service->roomSnapshot(roomId);
      reply(cb, std::move(out));
    }, "讀取房間失敗");
  });

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
  reg0("/api/admin/npc-templates", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["npc_templates"] = genericList(service->database(), "npc_templates"); reply(cb, std::move(out)); });
  });
  reg0("/api/admin/npc-templates", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["ok"] = true; out["npc_template"] = genericInsert(service->database(), "npc_templates", bodyOrEmpty(req)); reply(cb, std::move(out)); });
  });
  registerAdminPatchDelete1(service, "/api/admin/npc-templates/{1}", "npc_templates", "npc_template");

  reg0("/api/admin/dungeon-templates", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["dungeon_templates"] = genericList(service->database(), "dungeon_templates"); reply(cb, std::move(out)); });
  });
  reg0("/api/admin/dungeon-templates", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["ok"] = true; out["dungeon_template"] = genericInsert(service->database(), "dungeon_templates", bodyOrEmpty(req)); reply(cb, std::move(out)); });
  });
  registerAdminPatchDelete1(service, "/api/admin/dungeon-templates/{1}", "dungeon_templates", "dungeon_template");

  reg1("/api/admin/monster-templates/{1}", drogon::Patch,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["ok"] = true; out["monster_template"] = genericUpdate(service->database(), "monster_templates", positiveId(idText), bodyOrEmpty(req)); reply(cb, std::move(out)); }, "更新怪物模板失敗");
  });

  // Room world composition: NPCs, dungeons, discoveries and lightweight AI/exploration state.
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
          "SELECT row_to_json(x)::text AS json_text FROM (SELECT rm.*,mt.name,mt.category,mt.rank,mt.description,mt.public_hint,mt.image_url,mt.attributes,mt.skills,mt.drops,mt.ai_level,mt.ai_behavior,mt.config FROM room_monsters rm JOIN monster_templates mt ON mt.id=rm.monster_template_id WHERE rm.room_id=$1 ORDER BY rm.id) x", roomId);
      out["npcs"] = selectRows1(service->database(),
          "SELECT row_to_json(x)::text AS json_text FROM (SELECT rn.*,nt.name,nt.role_name,nt.description,nt.attributes,nt.skills,nt.config FROM room_npcs rn JOIN npc_templates nt ON nt.id=rn.npc_template_id WHERE rn.room_id=$1 ORDER BY rn.npc_template_id) x", roomId);
      out["dungeons"] = selectRows1(service->database(),
          "SELECT row_to_json(x)::text AS json_text FROM (SELECT rd.*,dt.name,dt.description,dt.config FROM room_dungeons rd JOIN dungeon_templates dt ON dt.id=rd.dungeon_template_id WHERE rd.room_id=$1 ORDER BY rd.dungeon_template_id) x", roomId);
      reply(cb, std::move(out));
    }, "讀取世界資料失敗");
  });

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
          "SELECT row_to_json(x)::text AS json_text FROM (SELECT rd.*,dt.name,dt.description,dt.config FROM room_dungeons rd JOIN dungeon_templates dt ON dt.id=rd.dungeon_template_id WHERE rd.room_id=$1) x", roomId); reply(cb, std::move(out));
    }, "加入副本失敗");
  });

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

  reg0("/api/game-room/npcs", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req);
      Json::Value out; out["npcs"] = genericList(service->database(), "npc_templates", "name ASC"); reply(cb, std::move(out));
    });
  });

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

  reg0("/api/admin/tasks", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out;
      out["tasks"] = selectRows(service->database(),
          "SELECT row_to_json(x)::text AS json_text FROM (SELECT t.*,u.username AS creator_username,target.username AS target_username,(SELECT count(*) FROM task_participants tp WHERE tp.task_id=t.id)::int AS participant_count FROM tasks t JOIN users u ON u.id=t.creator_id LEFT JOIN users target ON target.id=t.target_user_id ORDER BY t.created_at DESC) x");
      reply(cb, std::move(out));
    }, "讀取任務管理失敗");
  });

  reg0("/api/admin/tasks", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req, true);
      Json::Value extra; extra["creator_id"] = Json::Int64(user.id);
      auto body = bodyOrEmpty(req);
      if (!body.isMember("task_type")) body["task_type"] = body.isMember("target_user_id") && intValue(body["target_user_id"]) > 0 ? "private" : "public";
      Json::Value out; out["ok"] = true; out["task"] = genericInsert(service->database(), "tasks", body, extra); reply(cb, std::move(out));
    }, "建立任務失敗");
  });

  reg1("/api/admin/tasks/{1}", drogon::Patch,
       [service](const Request& req, Callback& cb, const std::string& taskIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value out; out["ok"] = true; out["task"] = genericUpdate(service->database(), "tasks", positiveId(taskIdText), bodyOrEmpty(req)); reply(cb, std::move(out));
    }, "更新任務失敗");
  });

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
      service->database()->execSqlSync(
          "UPDATE character_cards SET experience=experience+$1,attribute_points=attribute_points+$2,skill_points=skill_points+$3,weird_coins=weird_coins+$4,game_coins=game_coins+$5,inventory=$6::jsonb,equipment=$7::jsonb,weapons=$8::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$9",
          std::max<std::int64_t>(0,intValue(rewards["experience"])),
          std::max<std::int64_t>(0,intValue(rewards["attribute_points"])),
          std::max<std::int64_t>(0,intValue(rewards["skill_points"])),
          std::max<std::int64_t>(0,intValue(rewards["weird_coins"])),
          std::max<std::int64_t>(0,intValue(rewards["game_coins"])),
          compactJson(inventory), compactJson(equipment), compactJson(weapons), targetUserId);
      service->database()->execSqlSync(
          "INSERT INTO task_participants(task_id,user_id,status,completed_at) VALUES($1,$2,'completed',CURRENT_TIMESTAMP) "
          "ON CONFLICT(task_id,user_id) DO UPDATE SET status='completed',completed_at=CURRENT_TIMESTAMP",
          taskId, targetUserId);
      if (stringValue(task["task_type"]) == "private") service->database()->execSqlSync("UPDATE tasks SET status='completed',closed_at=CURRENT_TIMESTAMP,updated_at=CURRENT_TIMESTAMP WHERE id=$1", taskId);
      Json::Value out; out["ok"] = true; out["character"] = characterRaw(service->database(), targetUserId); reply(cb, std::move(out));
    }, "完成任務失敗");
  });

  reg1("/api/admin/tasks/{1}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& taskIdText) {
    runHandler(cb, [&] { requireUser(service, req, true); genericDelete(service->database(), "tasks", positiveId(taskIdText)); Json::Value out; out["ok"] = true; reply(cb, std::move(out)); }, "刪除任務失敗");
  });

  // Horror rules.
  reg0("/api/admin/horror-rules", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["rules"] = genericList(service->database(), "horror_rules"); reply(cb, std::move(out)); });
  });
  reg0("/api/admin/horror-rules", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["ok"] = true; out["rule"] = genericInsert(service->database(), "horror_rules", bodyOrEmpty(req)); reply(cb, std::move(out)); });
  });
  registerAdminPatchDelete1(service, "/api/admin/horror-rules/{1}", "horror_rules", "rule");

  reg1("/api/rooms/{1}/actions", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto roomId = positiveId(roomIdText, "房間 ID 無效");
      requireRoomAccess(service, user, roomId);
      const auto body = bodyOrEmpty(req);
      Json::Value payload = body;
      if (!payload.isMember("text")) payload["text"] = stringValue(body["action"], "玩家行動");
      service->addEvent(roomId, user.id, "action", payload);
      Json::Value out; out["ok"] = true; out["room"] = service->roomSnapshot(roomId); reply(cb, std::move(out));
    }, "提交行動失敗");
  });

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
  reg0("/api/admin/clues", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["clues"] = genericList(service->database(), "clue_templates"); reply(cb, std::move(out)); });
  });
  reg0("/api/admin/clues", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["ok"] = true; out["clue"] = genericInsert(service->database(), "clue_templates", bodyOrEmpty(req)); reply(cb, std::move(out)); });
  });
  reg1("/api/admin/clues/{1}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] { requireUser(service, req, true); genericDelete(service->database(), "clue_templates", positiveId(idText)); Json::Value out; out["ok"] = true; reply(cb, std::move(out)); });
  });

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
  reg0("/api/admin/status-templates", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["statuses"] = genericList(service->database(), "status_templates"); out["status_templates"] = out["statuses"]; reply(cb, std::move(out)); });
  });
  reg0("/api/admin/status-templates", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["ok"] = true; out["status"] = genericInsert(service->database(), "status_templates", bodyOrEmpty(req)); reply(cb, std::move(out)); });
  });
  registerAdminPatchDelete1(service, "/api/admin/status-templates/{1}", "status_templates", "status");

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
      Json::Value out; out["ok"] = true; out["status"] = genericInsert(service->database(), "character_statuses", create, extra); reply(cb, std::move(out));
    }, "套用狀態失敗");
  });

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
  reg0("/api/item-templates", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req); Json::Value out; out["items"] = genericList(service->database(), "item_templates", "name ASC"); reply(cb, std::move(out)); });
  });
  reg0("/api/admin/item-templates", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["items"] = genericList(service->database(), "item_templates", "name ASC"); reply(cb, std::move(out)); });
  });
  reg0("/api/admin/item-templates", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["ok"] = true; out["item"] = genericInsert(service->database(), "item_templates", bodyOrEmpty(req)); reply(cb, std::move(out)); });
  });
  reg1("/api/admin/item-templates/{1}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] { requireUser(service, req, true); genericDelete(service->database(), "item_templates", positiveId(idText)); Json::Value out; out["ok"] = true; reply(cb, std::move(out)); });
  });

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
      const auto roomId = intValue(body["room_id"]);
      if (roomId > 0) {
        requireRoomAccess(service, user, roomId);
        const auto healHp = std::max<std::int64_t>(0, intValue(effects["heal_hp"], intValue(effects["hp"])));
        const auto healSpirit = std::max<std::int64_t>(0, intValue(effects["heal_spirit"]));
        if (healHp || healSpirit) service->database()->execSqlSync(
            "UPDATE room_members SET hp=LEAST(max_hp,hp+$1),spirit=LEAST(max_spirit,spirit+$2) WHERE room_id=$3 AND user_id=$4",
            healHp, healSpirit, roomId, user.id);
        Json::Value payload; payload["text"] = stringValue(character["name"], user.username) + " 使用道具「" + name + "」"; payload["item_template_id"] = Json::Int64(templateId); service->addEvent(roomId, user.id, "item_use", payload);
      }
      service->database()->execSqlSync("UPDATE character_cards SET inventory=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2", compactJson(inventory), user.id);
      Json::Value out; out["ok"] = true; out["item"] = item; out["character"] = characterRaw(service->database(), user.id); if (roomId > 0) out["room"] = service->roomSnapshot(roomId); reply(cb, std::move(out));
    }, "使用道具失敗");
  });

  // Map nodes.
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
      reply(cb, std::move(out));
    }, "讀取地圖節點失敗");
  });

  reg1("/api/admin/rooms/{1}/map-nodes", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value extra; extra["room_id"] = Json::Int64(positiveId(roomIdText));
      Json::Value out; out["ok"] = true; out["node"] = genericInsert(service->database(), "room_map_nodes", bodyOrEmpty(req), extra); reply(cb, std::move(out));
    }, "建立地圖節點失敗");
  });

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

  reg2("/api/admin/rooms/{1}/map-nodes/{2}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& roomIdText, const std::string& nodeIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto rows = service->database()->execSqlSync("DELETE FROM room_map_nodes WHERE id=$1 AND room_id=$2 RETURNING id", positiveId(nodeIdText), positiveId(roomIdText));
      if (rows.empty()) throw ApiError(drogon::k404NotFound, "找不到地圖節點");
      Json::Value out; out["ok"] = true; reply(cb, std::move(out));
    }, "刪除地圖節點失敗");
  });

  reg2("/api/admin/rooms/{1}/map-nodes/{2}", drogon::Patch,
       [service](const Request& req, Callback& cb, const std::string& roomIdText, const std::string& nodeIdText) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      const auto roomId = positiveId(roomIdText); const auto nodeId = positiveId(nodeIdText);
      if (service->database()->execSqlSync("SELECT 1 FROM room_map_nodes WHERE id=$1 AND room_id=$2", nodeId, roomId).empty()) throw ApiError(drogon::k404NotFound, "找不到地圖節點");
      Json::Value out; out["ok"] = true; out["node"] = genericUpdate(service->database(), "room_map_nodes", nodeId, bodyOrEmpty(req)); reply(cb, std::move(out));
    }, "更新地圖節點失敗");
  });

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

  reg1("/api/admin/rooms/{1}/hidden-rolls", drogon::Get,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] { requireUser(service, req, true); Json::Value out; out["rolls"] = selectRows1(service->database(), "SELECT row_to_json(h)::text AS json_text FROM hidden_rolls h WHERE room_id=$1 ORDER BY created_at DESC LIMIT 200", positiveId(roomIdText)); reply(cb, std::move(out)); });
  });

  // Teams.
  reg1("/api/rooms/{1}/teams", drogon::Get,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req); const auto roomId = positiveId(roomIdText); requireRoomAccess(service, user, roomId);
      Json::Value out;
      out["teams"] = selectRows1(service->database(),
          "SELECT row_to_json(x)::text AS json_text FROM (SELECT t.*,(SELECT jsonb_agg(jsonb_build_object('user_id',m.user_id,'username',u.username)) FROM room_team_members m JOIN users u ON u.id=m.user_id WHERE m.team_id=t.id) AS members FROM room_teams t WHERE t.room_id=$1 ORDER BY t.id) x", roomId);
      reply(cb, std::move(out));
    }, "讀取隊伍失敗");
  });

  reg1("/api/rooms/{1}/teams", drogon::Post,
       [service](const Request& req, Callback& cb, const std::string& roomIdText) {
    runHandler(cb, [&] {
      const auto user = requireUser(service, req); const auto roomId = positiveId(roomIdText); requireRoomAccess(service,user,roomId);
      auto body=bodyOrEmpty(req); if(!body.isMember("name")) body["name"]="隊伍"; Json::Value extra; extra["room_id"]=Json::Int64(roomId); extra["leader_user_id"]=Json::Int64(user.id);
      auto team=genericInsert(service->database(),"room_teams",body,extra); const auto teamId=intValue(team["id"]);
      service->database()->execSqlSync("DELETE FROM room_team_members WHERE room_id=$1 AND user_id=$2",roomId,user.id);
      service->database()->execSqlSync("INSERT INTO room_team_members(team_id,room_id,user_id) VALUES($1,$2,$3)",teamId,roomId,user.id);
      Json::Value out; out["ok"]=true; out["team"]=team; reply(cb,std::move(out));
    },"建立隊伍失敗");
  });

  reg2("/api/rooms/{1}/teams/{2}/join",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& teamIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto teamId=positiveId(teamIdText);requireRoomAccess(service,user,roomId);if(service->database()->execSqlSync("SELECT 1 FROM room_teams WHERE id=$1 AND room_id=$2",teamId,roomId).empty())throw ApiError(drogon::k404NotFound,"找不到隊伍");service->database()->execSqlSync("DELETE FROM room_team_members WHERE room_id=$1 AND user_id=$2",roomId,user.id);service->database()->execSqlSync("INSERT INTO room_team_members(team_id,room_id,user_id) VALUES($1,$2,$3)",teamId,roomId,user.id);Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"加入隊伍失敗");});

  reg2("/api/rooms/{1}/teams/{2}/leave",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& teamIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto teamId=positiveId(teamIdText);requireRoomAccess(service,user,roomId);service->database()->execSqlSync("DELETE FROM room_team_members WHERE team_id=$1 AND room_id=$2 AND user_id=$3",teamId,roomId,user.id);Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"離開隊伍失敗");});

  reg2("/api/rooms/{1}/teams/{2}/move",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& teamIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto teamId=positiveId(teamIdText);requireRoomAccess(service,user,roomId);const auto nodeId=intValue(bodyOrEmpty(req)["node_id"]);if(nodeId<=0)throw ApiError(drogon::k400BadRequest,"請指定地圖節點");if(service->database()->execSqlSync("SELECT 1 FROM room_team_members WHERE team_id=$1 AND user_id=$2",teamId,user.id).empty()&&!user.isAdmin)throw ApiError(drogon::k403Forbidden,"你不在這個隊伍");service->database()->execSqlSync("UPDATE room_teams SET current_map_node_id=$1,updated_at=CURRENT_TIMESTAMP WHERE id=$2 AND room_id=$3",nodeId,teamId,roomId);Json::Value payload;payload["text"]="隊伍移動到新的地圖節點";payload["team_id"]=Json::Int64(teamId);payload["node_id"]=Json::Int64(nodeId);service->addEvent(roomId,user.id,"team_move",payload);Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"移動隊伍失敗");});

  reg2("/api/rooms/{1}/teams/{2}/share-clue",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& teamIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto teamId=positiveId(teamIdText);requireRoomAccess(service,user,roomId);const auto clueId=intValue(bodyOrEmpty(req)["clue_id"]);if(clueId<=0||service->database()->execSqlSync("SELECT 1 FROM character_clues WHERE clue_id=$1 AND user_id=$2",clueId,user.id).empty())throw ApiError(drogon::k403Forbidden,"你沒有這條線索");service->database()->execSqlSync("INSERT INTO team_shared_clues(team_id,clue_id,shared_by_user_id,note) VALUES($1,$2,$3,$4) ON CONFLICT(team_id,clue_id) DO UPDATE SET note=EXCLUDED.note",teamId,clueId,user.id,stringValue(bodyOrEmpty(req)["note"]));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"分享線索失敗");});

  reg2("/api/rooms/{1}/teams/{2}/share-rule",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& teamIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto teamId=positiveId(teamIdText);requireRoomAccess(service,user,roomId);const auto ruleId=intValue(bodyOrEmpty(req)["rule_id"]);const auto known=service->database()->execSqlSync("SELECT knowledge_level FROM rule_discoveries WHERE rule_id=$1 AND user_id=$2",ruleId,user.id);if(known.empty())throw ApiError(drogon::k403Forbidden,"你尚未知道這條規則");service->database()->execSqlSync("INSERT INTO team_shared_rules(team_id,rule_id,knowledge_level,shared_by_user_id,note) VALUES($1,$2,$3,$4,$5) ON CONFLICT(team_id,rule_id) DO UPDATE SET knowledge_level=GREATEST(team_shared_rules.knowledge_level,EXCLUDED.knowledge_level),note=EXCLUDED.note,updated_at=CURRENT_TIMESTAMP",teamId,ruleId,known[0]["knowledge_level"].as<std::int64_t>(),user.id,stringValue(bodyOrEmpty(req)["note"]));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"分享規則失敗");});

  reg2("/api/rooms/{1}/teams/{2}/chat",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& teamIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto teamId=positiveId(teamIdText);requireRoomAccess(service,user,roomId);const auto text=trimmed(stringValue(bodyOrEmpty(req)["text"]));if(text.empty())throw ApiError(drogon::k400BadRequest,"訊息不能為空");service->database()->execSqlSync("INSERT INTO team_messages(team_id,user_id,content) VALUES($1,$2,$3)",teamId,user.id,text.substr(0,std::min<std::size_t>(2000,text.size())));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"隊伍聊天失敗");});

  // Personal visions.
  reg1("/api/admin/rooms/{1}/visions",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto roomId=positiveId(roomIdText);auto body=bodyOrEmpty(req);const auto userId=intValue(body["user_id"]);if(userId<=0)throw ApiError(drogon::k400BadRequest,"請指定玩家");Json::Value extra;extra["room_id"]=Json::Int64(roomId);extra["user_id"]=Json::Int64(userId);Json::Value out;out["ok"]=true;out["vision"]=genericInsert(service->database(),"personal_visions",body,extra);reply(cb,std::move(out));},"建立私人視野失敗");});

  reg2("/api/rooms/{1}/visions/{2}/read",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& visionIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto visionId=positiveId(visionIdText);requireRoomAccess(service,user,roomId);const auto rows=service->database()->execSqlSync("UPDATE personal_visions SET is_read=TRUE WHERE id=$1 AND room_id=$2 AND user_id=$3 RETURNING row_to_json(personal_visions)::text AS json_text",visionId,roomId,user.id);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到私人視野");Json::Value out;out["ok"]=true;out["vision"]=rowJson(rows);reply(cb,std::move(out));},"閱讀私人視野失敗");});

  reg2("/api/rooms/{1}/visions/{2}/share",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& visionIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto visionId=positiveId(visionIdText);requireRoomAccess(service,user,roomId);const auto visionRows=service->database()->execSqlSync("SELECT title,content,shareable FROM personal_visions WHERE id=$1 AND room_id=$2 AND user_id=$3",visionId,roomId,user.id);if(visionRows.empty())throw ApiError(drogon::k404NotFound,"找不到私人視野");if(!visionRows[0]["shareable"].as<bool>())throw ApiError(drogon::k403Forbidden,"這個視野不能分享");auto teamId=intValue(bodyOrEmpty(req)["team_id"]);if(teamId<=0){const auto teams=service->database()->execSqlSync("SELECT team_id FROM room_team_members WHERE room_id=$1 AND user_id=$2",roomId,user.id);if(teams.empty())throw ApiError(drogon::k400BadRequest,"你目前沒有隊伍");teamId=teams[0]["team_id"].as<std::int64_t>();}service->database()->execSqlSync("INSERT INTO team_shared_notes(team_id,shared_by_user_id,title,content,note_type,source_vision_id) VALUES($1,$2,$3,$4,'vision',$5)",teamId,user.id,visionRows[0]["title"].as<std::string>(),visionRows[0]["content"].as<std::string>(),visionId);Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"分享視野失敗");});

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

  reg1("/api/admin/rooms/{1}/scheduled-events",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{requireUser(service,req,true);Json::Value extra;extra["room_id"]=Json::Int64(positiveId(roomIdText));Json::Value out;out["ok"]=true;out["event"]=genericInsert(service->database(),"scheduled_world_events",bodyOrEmpty(req),extra);reply(cb,std::move(out));},"建立排程事件失敗");});
  reg2("/api/admin/rooms/{1}/scheduled-events/{2}",drogon::Delete,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& eventIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto rows=service->database()->execSqlSync("DELETE FROM scheduled_world_events WHERE id=$1 AND room_id=$2 RETURNING id",positiveId(eventIdText),positiveId(roomIdText));if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到排程事件");Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"刪除排程事件失敗");});

  reg1("/api/admin/rooms/{1}/chases",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto roomId=positiveId(roomIdText);Json::Value extra;extra["room_id"]=Json::Int64(roomId);Json::Value out;out["ok"]=true;out["chase"]=genericInsert(service->database(),"room_chases",bodyOrEmpty(req),extra);reply(cb,std::move(out));},"建立追逐失敗");});

  reg2("/api/rooms/{1}/chases/{2}/action",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& chaseIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto chaseId=positiveId(chaseIdText);requireRoomAccess(service,user,roomId);const auto rows=service->database()->execSqlSync("SELECT row_to_json(c)::text AS json_text FROM room_chases c WHERE id=$1 AND room_id=$2 AND status='active'",chaseId,roomId);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到進行中的追逐");auto chase=rowJson(rows);const auto body=bodyOrEmpty(req);const auto action=stringValue(body["action"],"run");const auto character=characterRaw(service->database(),user.id);const auto stat=action=="hide"?intValue(character["spirit"]):intValue(character["agility"]);const auto roll=randomInt(1,100);const bool success=roll<=std::max<std::int64_t>(1,stat);auto distance=intValue(chase["distance"],3);distance+=success?1:-1;distance=std::max<std::int64_t>(0,distance);auto log=chase["log"].isArray()?chase["log"]:Json::Value{Json::arrayValue};Json::Value entry;entry["user_id"]=Json::Int64(user.id);entry["action"]=action;entry["roll"]=roll;entry["target"]=Json::Int64(stat);entry["success"]=success;entry["distance"]=Json::Int64(distance);log.append(entry);auto status=distance>=intValue(chase["escape_distance"],6)?"escaped":distance<=0?"caught":"active";service->database()->execSqlSync("UPDATE room_chases SET distance=$1,status=$2,round=round+1,last_actor_user_id=$3,log=$4::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=$5",distance,status,user.id,compactJson(log),chaseId);Json::Value out;out["ok"]=true;out["success"]=success;out["status"]=status;out["distance"]=Json::Int64(distance);out["roll"]=roll;reply(cb,std::move(out));},"追逐行動失敗");});

  reg2("/api/admin/rooms/{1}/chases/{2}/stop",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& chaseIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto rows=service->database()->execSqlSync("UPDATE room_chases SET status='stopped',updated_at=CURRENT_TIMESTAMP WHERE id=$1 AND room_id=$2 RETURNING id",positiveId(chaseIdText),positiveId(roomIdText));if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到追逐");Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"停止追逐失敗");});

  reg2("/api/admin/rooms/{1}/monsters/{2}/boss-next",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& monsterIdText){runHandler(cb,[&]{const auto user=requireUser(service,req,true);const auto roomId=positiveId(roomIdText);const auto monsterId=positiveId(monsterIdText);const auto body=bodyOrEmpty(req);const auto name=stringValue(body["name"],stringValue(body["phase_name"],"下一階段"));const auto rows=service->database()->execSqlSync("UPDATE room_monsters SET boss_phase_index=boss_phase_index+1,boss_phase_name=$1 WHERE id=$2 AND room_id=$3 RETURNING boss_phase_index",name,monsterId,roomId);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到怪物");Json::Value payload;payload["text"]="Boss 進入「"+name+"」";payload["monster_id"]=Json::Int64(monsterId);payload["phase_index"]=Json::Int64(rows[0]["boss_phase_index"].as<std::int64_t>());service->addEvent(roomId,user.id,"boss_phase",payload);Json::Value out;out["ok"]=true;out["room"]=service->roomSnapshot(roomId);reply(cb,std::move(out));},"切換 Boss 階段失敗");});

  // Search containers.
  reg1("/api/rooms/{1}/containers",drogon::Get,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);requireRoomAccess(service,user,roomId);Json::Value out;out["containers"]=selectRows1(service->database(),user.isAdmin?"SELECT row_to_json(c)::text AS json_text FROM search_containers c WHERE room_id=$1 AND active=TRUE ORDER BY id":"SELECT row_to_json(c)::text AS json_text FROM search_containers c WHERE room_id=$1 AND active=TRUE AND hidden=FALSE ORDER BY id",roomId);reply(cb,std::move(out));},"讀取容器失敗");});
  reg1("/api/admin/rooms/{1}/containers",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{requireUser(service,req,true);Json::Value extra;extra["room_id"]=Json::Int64(positiveId(roomIdText));Json::Value out;out["ok"]=true;out["container"]=genericInsert(service->database(),"search_containers",bodyOrEmpty(req),extra);reply(cb,std::move(out));},"建立容器失敗");});
  reg2("/api/admin/rooms/{1}/containers/{2}",drogon::Delete,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& containerIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto rows=service->database()->execSqlSync("DELETE FROM search_containers WHERE id=$1 AND room_id=$2 RETURNING id",positiveId(containerIdText),positiveId(roomIdText));if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到容器");Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"刪除容器失敗");});

  reg2("/api/rooms/{1}/containers/{2}/search",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& containerIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);const auto containerId=positiveId(containerIdText);requireRoomAccess(service,user,roomId);const auto rows=service->database()->execSqlSync("SELECT row_to_json(c)::text AS json_text FROM search_containers c WHERE id=$1 AND room_id=$2 AND active=TRUE",containerId,roomId);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到容器");const auto container=rowJson(rows);const auto character=characterRaw(service->database(),user.id);const auto attr=stringValue(container["search_attribute"],"spirit");const auto target=intValue(character[attr]);const auto dc=intValue(container["search_dc"],50);const auto roll=randomInt(1,100);const bool success=roll<=std::clamp<std::int64_t>(target-dc+50,1,99);auto found=success&&container["loot"].isArray()?container["loot"]:Json::Value{Json::arrayValue};const auto existing=service->database()->execSqlSync("SELECT claimed FROM search_records WHERE container_id=$1 AND user_id=$2",containerId,user.id);if(!existing.empty()&&existing[0]["claimed"].as<bool>()&&container.get("one_time",true).asBool())found=Json::arrayValue;auto claimed=false;if(success&&!found.empty()){auto inventory=inventoryOf(character);for(const auto& item:found){const auto name=item.isString()?item.asString():stringValue(item["name"]);const auto category=item.isObject()?stringValue(item["category"]):std::string{};const auto qty=item.isObject()?std::max<std::int64_t>(1,intValue(item["quantity"],1)):1;if(!name.empty())inventory=changeInventory(std::move(inventory),name,qty,category,item);}service->database()->execSqlSync("UPDATE character_cards SET inventory=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",compactJson(inventory),user.id);claimed=true;}service->database()->execSqlSync("INSERT INTO search_records(container_id,user_id,attempts,success,claimed,last_roll,last_target,found) VALUES($1,$2,1,$3,$4,$5,$6,$7::jsonb) ON CONFLICT(container_id,user_id) DO UPDATE SET attempts=search_records.attempts+1,success=search_records.success OR EXCLUDED.success,claimed=search_records.claimed OR EXCLUDED.claimed,last_roll=EXCLUDED.last_roll,last_target=EXCLUDED.last_target,found=EXCLUDED.found,updated_at=CURRENT_TIMESTAMP",containerId,user.id,success,claimed,roll,target,compactJson(found));Json::Value out;out["ok"]=true;out["success"]=success;out["roll"]=roll;out["target"]=Json::Int64(target);out["found"]=found;out["claimed"]=claimed;out["character"]=characterRaw(service->database(),user.id);reply(cb,std::move(out));},"搜索容器失敗");});

  // NPC relationship table.
  reg1("/api/rooms/{1}/npc-relations",drogon::Get,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);requireRoomAccess(service,user,roomId);Json::Value out;out["relations"]=selectRows2(service->database(),"SELECT row_to_json(r)::text AS json_text FROM npc_relationships r WHERE room_id=$1 AND user_id=$2 ORDER BY npc_template_id",roomId,user.id);reply(cb,std::move(out));},"讀取 NPC 關係失敗");});

  reg1("/api/admin/rooms/{1}/npc-relations",drogon::Patch,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto roomId=positiveId(roomIdText);const auto body=bodyOrEmpty(req);const auto npcId=intValue(body["npc_template_id"],intValue(body["npc_id"]));const auto userId=intValue(body["user_id"]);if(npcId<=0||userId<=0)throw ApiError(drogon::k400BadRequest,"請指定 NPC 與玩家");const auto affinity=intValue(body["affinity"]);const auto trust=intValue(body["trust"]);const auto fear=intValue(body["fear"]);const auto hostility=intValue(body["hostility"]);const auto memories=body["memories"].isArray()?body["memories"]:Json::Value{Json::arrayValue};service->database()->execSqlSync("INSERT INTO npc_relationships(room_id,npc_template_id,user_id,affinity,trust,fear,hostility,memories) VALUES($1,$2,$3,$4,$5,$6,$7,$8::jsonb) ON CONFLICT(room_id,npc_template_id,user_id) DO UPDATE SET affinity=EXCLUDED.affinity,trust=EXCLUDED.trust,fear=EXCLUDED.fear,hostility=EXCLUDED.hostility,memories=EXCLUDED.memories,updated_at=CURRENT_TIMESTAMP",roomId,npcId,userId,affinity,trust,fear,hostility,compactJson(memories));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"更新 NPC 關係失敗");});

  reg1("/api/admin/rooms/{1}/control",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{const auto user=requireUser(service,req,true);const auto roomId=positiveId(roomIdText);const auto body=bodyOrEmpty(req);Json::Value payload=body;payload["text"]=stringValue(body["text"],stringValue(body["action"],"DM 控制指令"));service->addEvent(roomId,user.id,"control",payload);Json::Value out;out["ok"]=true;out["room"]=service->roomSnapshot(roomId);reply(cb,std::move(out));},"執行 DM 控制失敗");});

  // Room savepoints.
  reg1("/api/admin/rooms/{1}/savepoints",drogon::Get,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{requireUser(service,req,true);Json::Value out;out["savepoints"]=selectRows1(service->database(),"SELECT row_to_json(s)::text AS json_text FROM room_savepoints s WHERE room_id=$1 ORDER BY created_at DESC LIMIT 100",positiveId(roomIdText));reply(cb,std::move(out));},"讀取房間存檔點失敗");});

  reg1("/api/admin/rooms/{1}/savepoints",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{const auto user=requireUser(service,req,true);const auto roomId=positiveId(roomIdText);const auto body=bodyOrEmpty(req);Json::Value snapshot=service->roomSnapshot(roomId);snapshot["map_nodes"]=selectRows1(service->database(),"SELECT row_to_json(n)::text AS json_text FROM room_map_nodes n WHERE room_id=$1 ORDER BY id",roomId);snapshot["dungeons"]=selectRows1(service->database(),"SELECT row_to_json(d)::text AS json_text FROM room_dungeons d WHERE room_id=$1 ORDER BY dungeon_template_id",roomId);snapshot["teams"]=selectRows1(service->database(),"SELECT row_to_json(t)::text AS json_text FROM room_teams t WHERE room_id=$1 ORDER BY id",roomId);snapshot["containers"]=selectRows1(service->database(),"SELECT row_to_json(c)::text AS json_text FROM search_containers c WHERE room_id=$1 ORDER BY id",roomId);const auto inserted=service->database()->execSqlSync("INSERT INTO room_savepoints(room_id,label,snapshot,created_by) VALUES($1,$2,$3::jsonb,$4) RETURNING row_to_json(room_savepoints)::text AS json_text",roomId,stringValue(body["label"],"手動存檔"),compactJson(snapshot),user.id);Json::Value out;out["ok"]=true;out["savepoint"]=rowJson(inserted);reply(cb,std::move(out));},"建立房間存檔點失敗");});

  reg2("/api/admin/rooms/{1}/savepoints/{2}/restore",drogon::Post,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& savepointIdText){runHandler(cb,[&]{const auto user=requireUser(service,req,true);const auto roomId=positiveId(roomIdText);const auto savepointId=positiveId(savepointIdText);const auto rows=service->database()->execSqlSync("SELECT snapshot::text AS snapshot_text FROM room_savepoints WHERE id=$1 AND room_id=$2",savepointId,roomId);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到房間存檔點");const auto snapshot=parseJson(rows[0]["snapshot_text"].as<std::string>());const auto room=snapshot["room"];if(room.isObject()){service->database()->execSqlSync("UPDATE rooms SET name=$1,status=$2,round=$3,current_actor_id=$4,saved=$5,map_name=$6,map_image_url=$7,map_description=$8,game_day=$9,game_hour=$10,game_minute=$11,battle_active=$12,current_actor_type=$13,current_actor_ref_id=$14,combat_order=$15::jsonb,combat_index=$16,updated_at=CURRENT_TIMESTAMP WHERE id=$17",stringValue(room["name"],"房間"),stringValue(room["status"],"lobby"),std::max<std::int64_t>(1,intValue(room["round"],1)),intValue(room["current_actor_id"]),room.get("saved",false).asBool(),stringValue(room["map_name"],"未設定地點"),stringValue(room["map_image_url"]),stringValue(room["map_description"]),std::max<std::int64_t>(1,intValue(room["game_day"],1)),std::clamp<std::int64_t>(intValue(room["game_hour"],8),0,23),std::clamp<std::int64_t>(intValue(room["game_minute"]),0,59),room.get("battle_active",false).asBool(),stringValue(room["current_actor_type"],"player"),intValue(room["current_actor_ref_id"]),compactJson(room["combat_order"].isArray()?room["combat_order"]:Json::Value{Json::arrayValue}),std::max<std::int64_t>(0,intValue(room["combat_index"])),roomId);}if(snapshot["members"].isArray())for(const auto&m:snapshot["members"]){const auto uid=intValue(m["user_id"],intValue(m["id"]));if(uid>0)service->database()->execSqlSync("UPDATE room_members SET display_name=$1,hp=$2,max_hp=$3,spirit=$4,max_spirit=$5,current_sanity=$6,max_sanity=$7,combat_state=$8,combat_reaction=$9,turn_actions_remaining=$10 WHERE room_id=$11 AND user_id=$12",stringValue(m["display_name"],"玩家"),std::max<std::int64_t>(0,intValue(m["hp"])),std::max<std::int64_t>(1,intValue(m["max_hp"],1)),std::max<std::int64_t>(0,intValue(m["spirit"])),std::max<std::int64_t>(0,intValue(m["max_spirit"])),std::max<std::int64_t>(0,intValue(m["current_sanity"])),std::max<std::int64_t>(0,intValue(m["max_sanity"])),stringValue(m["combat_state"],"active"),stringValue(m["combat_reaction"],"dodge"),std::max<std::int64_t>(0,intValue(m["turn_actions_remaining"])),roomId,uid);}Json::Value payload;payload["text"]="DM 還原房間存檔點";payload["savepoint_id"]=Json::Int64(savepointId);service->addEvent(roomId,user.id,"restore",payload);Json::Value out;out["ok"]=true;out["room"]=service->roomSnapshot(roomId);reply(cb,std::move(out));},"還原房間存檔點失敗");});

  reg2("/api/admin/rooms/{1}/savepoints/{2}",drogon::Delete,
       [service](const Request& req,Callback& cb,const std::string& roomIdText,const std::string& savepointIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto rows=service->database()->execSqlSync("DELETE FROM room_savepoints WHERE id=$1 AND room_id=$2 RETURNING id",positiveId(savepointIdText),positiveId(roomIdText));if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到房間存檔點");Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"刪除房間存檔點失敗");});

  reg1("/api/admin/rooms/{1}/timeline",drogon::Get,
       [service](const Request& req,Callback& cb,const std::string& roomIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto roomId=positiveId(roomIdText);Json::Value out;out["events"]=selectRows1(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT e.*,u.username FROM events e LEFT JOIN users u ON u.id=e.user_id WHERE e.room_id=$1 ORDER BY e.id DESC LIMIT 500) x",roomId);reply(cb,std::move(out));},"讀取時間線失敗");});

  // V39-compatible backup envelope. Credentials and runtime secrets are never exported.
  reg0("/api/admin/backups/site/export", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service, req, true);
      Json::Value tables, counts;
      tables["users"] = selectRows(service->database(), "SELECT row_to_json(x)::text AS json_text FROM (SELECT id,username,is_admin,created_at FROM users ORDER BY id) x");
      counts["users"] = static_cast<Json::UInt64>(tables["users"].size());
      const std::array<const char*,65> names={"rooms","room_members","events","character_cards","character_room_bindings","extra_skills","bloodline_templates","summon_templates","summon_instances","recipes","profession_templates","skill_templates","shop_items","monster_templates","npc_templates","dungeon_templates","room_monsters","monster_discoveries","room_npcs","room_dungeons","combat_entity_statuses","clue_templates","character_clues","status_templates","character_statuses","room_map_nodes","hidden_rolls","item_templates","horror_rules","rule_discoveries","room_teams","room_team_members","team_shared_clues","team_shared_rules","personal_visions","team_shared_notes","team_messages","scheduled_world_events","room_chases","npc_relationships","search_containers","search_records","room_savepoints","character_savepoints","audit_logs","map_node_discoveries","npc_discoveries","item_identifications","clue_combinations","clue_combination_unlocks","great_way_templates","faith_deities","character_great_ways","character_faiths","custom_wheels","wheel_spins","faction_templates","character_reputations","npc_dialogue_entries","npc_dialogue_history","npc_shop_items","npc_shop_stock","faction_relations","tasks","task_participants"};
      for(const auto* table:names){tables[table]=genericList(service->database(),table,"");counts[table]=static_cast<Json::UInt64>(tables[table].size());}
      Json::Value target; target["id"]="site"; target["name"]="全站資料封存";
      Json::Value data; data["mode"]="export_only"; data["credential_policy"]="passwords, tokens and server secrets are never exported"; data["restore_policy"]="site archives require a database administrator and are not restorable from the web interface"; data["counts"]=counts; data["tables"]=tables;
      reply(cb, createBackupEnvelope("site", target, data));
    }, "匯出全站備份失敗");
  });

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

  reg0("/api/admin/backups/character-savepoints", drogon::Get, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      requireUser(service,req,true); Json::Value out;
      out["savepoints"]=selectRows(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT cs.id,cs.user_id,cs.label,cs.created_by,cs.created_at,u.username,c.name AS character_name,creator.username AS created_by_username FROM character_savepoints cs LEFT JOIN users u ON u.id=cs.user_id LEFT JOIN character_cards c ON c.user_id=cs.user_id LEFT JOIN users creator ON creator.id=cs.created_by ORDER BY cs.id DESC LIMIT 200) x");
      out["limit"]=200; reply(cb,std::move(out));
    },"讀取角色安全存檔失敗");
  });

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

  reg0("/api/admin/backups/preview", drogon::Post, [service](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user=requireUser(service,req,true); const auto body=bodyOrEmpty(req);
      if(!body["backup"].isObject())throw ApiError(drogon::k400BadRequest,"請提供備份檔");
      Json::Value out; out["preview"]=inspectBackupForRestore(service,body["backup"],user.id,true); reply(cb,std::move(out));
    },"備份檔預覽失敗");
  });

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

  reg0("/api/admin/audit-logs",drogon::Get,[service](const Request& req,Callback& cb){runHandler(cb,[&]{requireUser(service,req,true);Json::Value out;out["logs"]=selectRows(service->database(),"SELECT row_to_json(a)::text AS json_text FROM audit_logs a ORDER BY created_at DESC LIMIT 500");reply(cb,std::move(out));},"讀取稽核紀錄失敗");});

  // ---------------------------------------------------------------------------
  // V27 exploration, clue inference and time-flow reset.
  // ---------------------------------------------------------------------------
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

  reg0("/api/admin/clue-combinations", drogon::Get, [service](const Request& req, Callback& cb){
    runHandler(cb,[&]{requireUser(service,req,true);const auto raw=trimmed(req->getParameter("room_id"));Json::Value out;if(raw.empty()||raw=="0")out["combinations"]=genericList(service->database(),"clue_combinations","id DESC");else out["combinations"]=selectRows1(service->database(),"SELECT row_to_json(c)::text AS json_text FROM clue_combinations c WHERE room_id=$1 ORDER BY id DESC",positiveId(raw));reply(cb,std::move(out));},"讀取線索組合失敗");
  });
  reg0("/api/admin/clue-combinations", drogon::Post, [service](const Request& req, Callback& cb){
    runHandler(cb,[&]{requireUser(service,req,true);auto body=bodyOrEmpty(req);const auto roomId=intValue(body["room_id"]);requireRoomExists(service->database(),roomId);if(!body["required_clue_ids"].isArray()||body["required_clue_ids"].size()<2)throw ApiError(drogon::k400BadRequest,"線索組合至少需要 2 條線索");Json::Value out;out["ok"]=true;out["combination"]=genericInsert(service->database(),"clue_combinations",body);reply(cb,std::move(out));},"建立線索組合失敗");
  });
  reg1("/api/admin/clue-combinations/{1}",drogon::Delete,[service](const Request& req,Callback& cb,const std::string&idText){runHandler(cb,[&]{requireUser(service,req,true);genericDelete(service->database(),"clue_combinations",positiveId(idText));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"刪除線索組合失敗");});
  reg1("/api/rooms/{1}/inference",drogon::Post,[service](const Request& req,Callback& cb,const std::string&roomIdText){
    runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText);requireRoomAccess(service,user,roomId);std::set<std::int64_t> known;for(const auto&r:service->database()->execSqlSync("SELECT clue_id FROM character_clues WHERE user_id=$1",user.id))known.insert(r["clue_id"].as<std::int64_t>());const auto shared=service->database()->execSqlSync("SELECT s.clue_id FROM team_shared_clues s JOIN room_team_members m ON m.team_id=s.team_id WHERE m.room_id=$1 AND m.user_id=$2",roomId,user.id);for(const auto&r:shared)known.insert(r["clue_id"].as<std::int64_t>());Json::Value unlocked{Json::arrayValue};const auto rows=service->database()->execSqlSync("SELECT row_to_json(c)::text AS json_text FROM clue_combinations c WHERE room_id=$1 AND active=TRUE ORDER BY id",roomId);for(std::size_t i=0;i<rows.size();++i){auto combo=rowJson(rows,i);const auto comboId=intValue(combo["id"]);if(!service->database()->execSqlSync("SELECT 1 FROM clue_combination_unlocks WHERE combination_id=$1 AND user_id=$2",comboId,user.id).empty())continue;auto need=combo["required_clue_ids"].isArray()?combo["required_clue_ids"]:Json::Value{Json::arrayValue};bool all=!need.empty();for(const auto&id:need)if(!known.contains(intValue(id)))all=false;if(!all)continue;service->database()->execSqlSync("INSERT INTO clue_combination_unlocks(combination_id,user_id) VALUES($1,$2) ON CONFLICT DO NOTHING",comboId,user.id);const auto type=stringValue(combo["result_type"],"vision");const auto resultId=intValue(combo["result_id"]);if(type=="rule"&&resultId>0)service->database()->execSqlSync("INSERT INTO rule_discoveries(rule_id,user_id,knowledge_level) VALUES($1,$2,3) ON CONFLICT(rule_id,user_id) DO UPDATE SET knowledge_level=3,updated_at=CURRENT_TIMESTAMP",resultId,user.id);else if(type=="clue"&&resultId>0)service->database()->execSqlSync("INSERT INTO character_clues(clue_id,user_id,note) VALUES($1,$2,'推理結果') ON CONFLICT DO NOTHING",resultId,user.id);else if(type=="map_node"&&resultId>0)service->database()->execSqlSync("INSERT INTO map_node_discoveries(node_id,user_id) VALUES($1,$2) ON CONFLICT DO NOTHING",resultId,user.id);else service->database()->execSqlSync("INSERT INTO personal_visions(room_id,user_id,title,content,vision_type,shareable) VALUES($1,$2,$3,$4,'memory',TRUE)",roomId,user.id,stringValue(combo["result_title"],stringValue(combo["name"],"推理結果")),stringValue(combo["result_content"]));unlocked.append(combo);Json::Value ev;ev["text"]="完成推理「"+stringValue(combo["name"],"推理")+"」";ev["combination_id"]=Json::Int64(comboId);service->addEvent(roomId,user.id,"inference",ev);}Json::Value out;out["ok"]=true;out["unlocked"]=unlocked;reply(cb,std::move(out));},"整理線索失敗");
  });
  reg1("/api/admin/rooms/{1}/time-flow/reset",drogon::Post,[service](const Request& req,Callback& cb,const std::string&roomIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto roomId=positiveId(roomIdText);service->database()->execSqlSync("UPDATE rooms SET time_flow_offset_minutes=0,updated_at=CURRENT_TIMESTAMP WHERE id=$1",roomId);service->database()->execSqlSync("UPDATE room_map_nodes SET time_flow_offset_minutes=0,updated_at=CURRENT_TIMESTAMP WHERE room_id=$1",roomId);Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"重設時間流失敗");});

  // ---------------------------------------------------------------------------
  // Great Way / Faith identity systems.
  // ---------------------------------------------------------------------------
  reg0("/api/great-ways",drogon::Get,[service](const Request& req,Callback& cb){runHandler(cb,[&]{const auto user=requireUser(service,req);Json::Value out;out["great_ways"]=user.isAdmin?genericList(service->database(),"great_way_templates"):genericList(service->database(),"great_way_templates","id ASC","active=TRUE");out["my_great_ways"]=selectRows1(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT c.*,g.name,g.principle,g.description,g.max_rank,g.effects,g.granted_skill_template_ids FROM character_great_ways c JOIN great_way_templates g ON g.id=c.great_way_id WHERE c.user_id=$1 ORDER BY c.created_at,g.id) x",user.id);reply(cb,std::move(out));},"讀取大道系統失敗");});
  reg0("/api/pantheon",drogon::Get,[service](const Request& req,Callback& cb){runHandler(cb,[&]{const auto user=requireUser(service,req);Json::Value out;out["deities"]=user.isAdmin?genericList(service->database(),"faith_deities"):genericList(service->database(),"faith_deities","id ASC","active=TRUE");out["my_faiths"]=selectRows1(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT c.*,d.name,d.title,d.domains,d.doctrine,d.description,d.max_rank,d.effects,d.granted_skill_template_ids FROM character_faiths c JOIN faith_deities d ON d.id=c.deity_id WHERE c.user_id=$1 ORDER BY c.created_at,d.id) x",user.id);reply(cb,std::move(out));},"讀取諸神殿失敗");});
  registerAdminCrud0(service,"/api/admin/great-ways","great_way_templates","great_ways");
  registerAdminPatchDelete1(service,"/api/admin/great-ways/{1}","great_way_templates","great_way");
  registerAdminCrud0(service,"/api/admin/deities","faith_deities","deities");
  registerAdminPatchDelete1(service,"/api/admin/deities/{1}","faith_deities","deity");
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
  reg2("/api/admin/players/{1}/great-ways/{2}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& userIdText, const std::string& idText) {
    runHandler(cb, [&] {
      requireUser(service, req, true); const auto userId=positiveId(userIdText);
      service->database()->execSqlSync("DELETE FROM character_great_ways WHERE user_id=$1 AND great_way_id=$2", userId, positiveId(idText));
      syncIdentitySourceEffects(service->database(), userId); Json::Value response; response["ok"]=true; reply(cb, std::move(response));
    }, "移除大道失敗");
  });
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
  reg2("/api/admin/players/{1}/faiths/{2}", drogon::Delete,
       [service](const Request& req, Callback& cb, const std::string& userIdText, const std::string& idText) {
    runHandler(cb, [&] {
      requireUser(service, req, true); const auto userId=positiveId(userIdText);
      service->database()->execSqlSync("DELETE FROM character_faiths WHERE user_id=$1 AND deity_id=$2", userId, positiveId(idText));
      syncIdentitySourceEffects(service->database(), userId); Json::Value response; response["ok"]=true; reply(cb, std::move(response));
    }, "移除信仰失敗");
  });

  // Wheels.
  reg0("/api/wheels",drogon::Get,[service](const Request& req,Callback& cb){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomParam=trimmed(req->getParameter("room_id"));Json::Value out;if(user.isAdmin)out["wheels"]=genericList(service->database(),"custom_wheels","id DESC");else if(!roomParam.empty()&&roomParam!="0")out["wheels"]=selectRows1(service->database(),"SELECT row_to_json(w)::text AS json_text FROM custom_wheels w WHERE active=TRUE AND audience<>'dm' AND (room_id IS NULL OR room_id=$1) ORDER BY id DESC",positiveId(roomParam));else out["wheels"]=genericList(service->database(),"custom_wheels","id DESC","active=TRUE AND audience<>'dm' AND room_id IS NULL");reply(cb,std::move(out));},"讀取輪盤失敗");});
  registerAdminCrud0(service,"/api/admin/wheels","custom_wheels","wheels");
  registerAdminPatchDelete1(service,"/api/admin/wheels/{1}","custom_wheels","wheel");
  reg1("/api/wheels/{1}/spin",drogon::Post,[service](const Request& req,Callback& cb,const std::string&idText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto id=positiveId(idText);const auto rows=service->database()->execSqlSync("SELECT row_to_json(w)::text AS json_text FROM custom_wheels w WHERE id=$1 AND active=TRUE",id);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到可用輪盤");const auto wheel=rowJson(rows);if(stringValue(wheel["audience"])=="dm"&&!user.isAdmin)throw ApiError(drogon::k403Forbidden,"這個輪盤只有 DM 可以使用");const auto roomId=intValue(wheel["room_id"]);if(roomId>0)requireRoomAccess(service,user,roomId);const auto options=wheel["options"].isArray()?wheel["options"]:Json::Value{Json::arrayValue};if(options.size()<2)throw ApiError(drogon::k400BadRequest,"輪盤選項不足");std::vector<int> weights;int total=0;for(const auto&o:options){const auto w=static_cast<int>(std::max<std::int64_t>(1,o.isObject()?intValue(o["weight"],1):1));weights.push_back(w);total+=w;}auto pick=randomInt(0,total-1);Json::ArrayIndex index=0;for(Json::ArrayIndex i=0;i<options.size();++i){if(pick<weights[i]){index=i;break;}pick-=weights[i];}const auto result=options[index];const auto label=result.isString()?result.asString():stringValue(result["label"],stringValue(result["name"],"結果"));service->database()->execSqlSync("INSERT INTO wheel_spins(wheel_id,room_id,user_id,result_index,result_label) VALUES($1,NULLIF($2,0),$3,$4,$5)",id,roomId,user.id,static_cast<int>(index),label);if(roomId>0){Json::Value ev;ev["text"]="轉動「"+stringValue(wheel["name"],"輪盤")+"」→ "+label;ev["wheel_id"]=Json::Int64(id);ev["result"]=label;service->addEvent(roomId,user.id,"wheel",ev);}Json::Value out;out["ok"]=true;out["result"]["index"]=index;out["result"]["label"]=label;out["wheel"]=wheel;reply(cb,std::move(out));},"轉動輪盤失敗");});

  // ---------------------------------------------------------------------------
  // Factions, reputation, NPC dialogue and NPC shops.
  // ---------------------------------------------------------------------------
  reg0("/api/factions",drogon::Get,[service](const Request& req,Callback& cb){runHandler(cb,[&]{const auto user=requireUser(service,req);Json::Value out;out["factions"]=genericList(service->database(),"faction_templates","name ASC,id ASC","active=TRUE");out["reputations"]=selectRows1(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT r.*,f.name,f.description,f.alignment FROM character_reputations r JOIN faction_templates f ON f.id=r.faction_id WHERE r.user_id=$1 ORDER BY r.reputation DESC,f.name) x",user.id);out["relations"]=selectRows(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT r.*,a.name AS source_name,b.name AS target_name FROM faction_relations r JOIN faction_templates a ON a.id=r.source_faction_id JOIN faction_templates b ON b.id=r.target_faction_id WHERE a.active=TRUE AND b.active=TRUE ORDER BY a.name,b.name) x");reply(cb,std::move(out));},"讀取勢力／聲望失敗");});
  reg0("/api/admin/factions",drogon::Get,[service](const Request& req,Callback& cb){runHandler(cb,[&]{const auto user=requireUser(service,req,true);Json::Value out;out["factions"]=genericList(service->database(),"faction_templates");out["reputations"]=selectRows1(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT r.*,f.name,f.description,f.alignment FROM character_reputations r JOIN faction_templates f ON f.id=r.faction_id WHERE r.user_id=$1 ORDER BY r.reputation DESC,f.name) x",user.id);out["relations"]=selectRows(service->database(),"SELECT row_to_json(x)::text AS json_text FROM (SELECT r.*,a.name AS source_name,b.name AS target_name FROM faction_relations r JOIN faction_templates a ON a.id=r.source_faction_id JOIN faction_templates b ON b.id=r.target_faction_id ORDER BY a.name,b.name) x");reply(cb,std::move(out));},"讀取勢力失敗");});
  reg0("/api/admin/factions",drogon::Post,[service](const Request& req,Callback& cb){runHandler(cb,[&]{requireUser(service,req,true);auto body=bodyOrEmpty(req);const auto name=trimmed(stringValue(body["name"]));if(name.empty())throw ApiError(drogon::k400BadRequest,"請輸入勢力名稱");Json::Value out;out["ok"]=true;out["faction"]=genericInsert(service->database(),"faction_templates",body);reply(cb,std::move(out));},"建立勢力失敗");});
  registerAdminPatchDelete1(service,"/api/admin/factions/{1}","faction_templates","faction");
  reg0("/api/admin/faction-relations",drogon::Put,[service](const Request& req,Callback& cb){runHandler(cb,[&]{requireUser(service,req,true);const auto body=bodyOrEmpty(req);const auto source=intValue(body["source_faction_id"]),target=intValue(body["target_faction_id"]);if(source<=0||target<=0||source==target)throw ApiError(drogon::k400BadRequest,"請選擇兩個不同勢力");const auto score=std::clamp<std::int64_t>(intValue(body["relation_score"]),-100,100);const auto rows=service->database()->execSqlSync("INSERT INTO faction_relations(source_faction_id,target_faction_id,relation_score,label,notes) VALUES($1,$2,$3,$4,$5) ON CONFLICT(source_faction_id,target_faction_id) DO UPDATE SET relation_score=EXCLUDED.relation_score,label=EXCLUDED.label,notes=EXCLUDED.notes,updated_at=CURRENT_TIMESTAMP RETURNING row_to_json(faction_relations)::text AS json_text",source,target,score,stringValue(body["label"]),stringValue(body["notes"]));Json::Value out;out["ok"]=true;out["relation"]=rowJson(rows);reply(cb,std::move(out));},"儲存勢力關係失敗");});
  reg2("/api/admin/faction-relations/{1}/{2}",drogon::Delete,[service](const Request& req,Callback& cb,const std::string&sourceText,const std::string&targetText){runHandler(cb,[&]{requireUser(service,req,true);service->database()->execSqlSync("DELETE FROM faction_relations WHERE source_faction_id=$1 AND target_faction_id=$2",positiveId(sourceText),positiveId(targetText));Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"刪除勢力關係失敗");});
  reg2("/api/admin/players/{1}/reputations/{2}",drogon::Patch,[service](const Request& req,Callback& cb,const std::string&userIdText,const std::string&factionIdText){runHandler(cb,[&]{requireUser(service,req,true);const auto userId=positiveId(userIdText),factionId=positiveId(factionIdText);const auto body=bodyOrEmpty(req);auto rep=intValue(body["reputation"]);if(body.isMember("delta")){const auto rows=service->database()->execSqlSync("SELECT reputation FROM character_reputations WHERE user_id=$1 AND faction_id=$2",userId,factionId);rep=(rows.empty()?0:rows[0]["reputation"].as<std::int64_t>())+intValue(body["delta"]);}rep=std::clamp<std::int64_t>(rep,-10000,10000);service->database()->execSqlSync("INSERT INTO character_reputations(user_id,faction_id,reputation,notes) VALUES($1,$2,$3,$4) ON CONFLICT(user_id,faction_id) DO UPDATE SET reputation=EXCLUDED.reputation,notes=CASE WHEN EXCLUDED.notes<>'' THEN EXCLUDED.notes ELSE character_reputations.notes END,updated_at=CURRENT_TIMESTAMP",userId,factionId,rep,stringValue(body["notes"]));Json::Value out;out["ok"]=true;out["reputation"]=Json::Int64(rep);reply(cb,std::move(out));},"更新勢力聲望失敗");});

  reg0("/api/admin/npc-social",drogon::Get,[service](const Request& req,Callback& cb){runHandler(cb,[&]{requireUser(service,req,true);Json::Value out;out["dialogues"]=genericList(service->database(),"npc_dialogue_entries","npc_template_id ASC,sort_order ASC,id ASC");out["shop_items"]=genericList(service->database(),"npc_shop_items","npc_template_id ASC,id ASC");out["relationships"]=genericList(service->database(),"npc_relationships","updated_at DESC");reply(cb,std::move(out));},"讀取 NPC 社交系統失敗");});
  registerAdminCrud0(service,"/api/admin/npc-dialogues","npc_dialogue_entries","dialogues");
  registerAdminPatchDelete1(service,"/api/admin/npc-dialogues/{1}","npc_dialogue_entries","dialogue");
  registerAdminCrud0(service,"/api/admin/npc-shop-items","npc_shop_items","items");
  registerAdminPatchDelete1(service,"/api/admin/npc-shop-items/{1}","npc_shop_items","item");

  reg2("/api/rooms/{1}/npcs/{2}/interact",drogon::Get,[service](const Request& req,Callback& cb,const std::string&roomIdText,const std::string&npcIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText),npcId=positiveId(npcIdText);requireRoomAccess(service,user,roomId);const auto rows=service->database()->execSqlSync("SELECT row_to_json(x)::text AS json_text FROM (SELECT rn.map_node_id,rn.status AS room_status,nt.* FROM room_npcs rn JOIN npc_templates nt ON nt.id=rn.npc_template_id WHERE rn.room_id=$1 AND rn.npc_template_id=$2) x",roomId,npcId);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到房間 NPC");const auto npc=rowJson(rows);Json::Value relation;const auto relRows=service->database()->execSqlSync("SELECT row_to_json(r)::text AS json_text FROM npc_relationships r WHERE room_id=$1 AND npc_template_id=$2 AND user_id=$3",roomId,npcId,user.id);if(relRows.empty()){relation["room_id"]=Json::Int64(roomId);relation["npc_template_id"]=Json::Int64(npcId);relation["user_id"]=Json::Int64(user.id);relation["affinity"]=0;relation["trust"]=0;relation["fear"]=0;relation["hostility"]=0;relation["memories"]=Json::arrayValue;}else relation=rowJson(relRows);const auto config=npc["config"].isObject()?npc["config"]:Json::Value{Json::objectValue};const auto factionId=intValue(config["faction_id"]);std::int64_t reputation=0;if(factionId>0){const auto rr=service->database()->execSqlSync("SELECT reputation FROM character_reputations WHERE user_id=$1 AND faction_id=$2",user.id,factionId);if(!rr.empty())reputation=rr[0]["reputation"].as<std::int64_t>();}const auto score=static_cast<int>(std::lround(intValue(relation["affinity"])*0.42+intValue(relation["trust"])*0.30-intValue(relation["hostility"])*0.62-intValue(relation["fear"])*0.12+std::clamp<double>(reputation/10.0,-30,30)));std::string key=score>=35?"friendly":score>=15?"warm":score>-15?"neutral":score>-35?"wary":"hostile";const std::unordered_map<std::string,std::string> labels={{"friendly","友善"},{"warm","親近"},{"neutral","中立"},{"wary","戒備"},{"hostile","敵對"}};Json::Value attitude;attitude["key"]=key;attitude["label"]=labels.at(key);attitude["score"]=score;Json::Value dialogues{Json::arrayValue};const auto drows=service->database()->execSqlSync("SELECT row_to_json(d)::text AS json_text FROM npc_dialogue_entries d WHERE npc_template_id=$1 AND active=TRUE ORDER BY sort_order,id",npcId);for(std::size_t i=0;i<drows.size();++i){auto d=rowJson(drows,i);if(intValue(relation["affinity"])<intValue(d["min_affinity"],-100)||intValue(relation["trust"])<intValue(d["min_trust"],-100)||intValue(relation["hostility"])>intValue(d["max_hostility"],100))continue;if(d.get("once_per_user",false).asBool()&&!service->database()->execSqlSync("SELECT 1 FROM npc_dialogue_history WHERE room_id=$1 AND user_id=$2 AND dialogue_entry_id=$3",roomId,user.id,intValue(d["id"])).empty())continue;dialogues.append(d);}Json::Value shop{Json::arrayValue};const auto srows=service->database()->execSqlSync("SELECT row_to_json(s)::text AS json_text FROM npc_shop_items s WHERE npc_template_id=$1 AND active=TRUE ORDER BY id",npcId);for(std::size_t i=0;i<srows.size();++i){auto item=rowJson(srows,i);const auto stock=intValue(item["stock"],-1);auto remaining=stock;if(stock>=0){const auto sr=service->database()->execSqlSync("SELECT remaining FROM npc_shop_stock WHERE room_id=$1 AND shop_item_id=$2",roomId,intValue(item["id"]));if(!sr.empty())remaining=sr[0]["remaining"].as<std::int64_t>();}item["remaining"]=Json::Int64(remaining);item["can_buy"]=(remaining!=0&&key!="hostile"&&intValue(relation["affinity"])>=intValue(item["min_affinity"],-100)&&intValue(relation["hostility"])<=intValue(item["max_hostility"],100));shop.append(item);}Json::Value out;out["npc"]=npc;out["reputation"]=Json::Int64(reputation);out["relationship"]=relation;out["attitude"]=attitude;out["dialogues"]=dialogues;out["shop_items"]=shop;reply(cb,std::move(out));},"讀取 NPC 互動失敗");});

  reg3("/api/rooms/{1}/npcs/{2}/dialogue/{3}",drogon::Post,[service](const Request& req,Callback& cb,const std::string&roomIdText,const std::string&npcIdText,const std::string&entryIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText),npcId=positiveId(npcIdText),entryId=positiveId(entryIdText);requireRoomAccess(service,user,roomId);if(service->database()->execSqlSync("SELECT 1 FROM room_npcs WHERE room_id=$1 AND npc_template_id=$2",roomId,npcId).empty())throw ApiError(drogon::k404NotFound,"找不到房間 NPC");const auto erows=service->database()->execSqlSync("SELECT row_to_json(d)::text AS json_text FROM npc_dialogue_entries d WHERE id=$1 AND npc_template_id=$2 AND active=TRUE",entryId,npcId);if(erows.empty())throw ApiError(drogon::k404NotFound,"找不到可用對話");const auto entry=rowJson(erows);Json::Value relation;const auto rr=service->database()->execSqlSync("SELECT row_to_json(r)::text AS json_text FROM npc_relationships r WHERE room_id=$1 AND npc_template_id=$2 AND user_id=$3",roomId,npcId,user.id);if(rr.empty()){relation["affinity"]=0;relation["trust"]=0;relation["fear"]=0;relation["hostility"]=0;relation["memories"]=Json::arrayValue;}else relation=rowJson(rr);if(intValue(relation["affinity"])<intValue(entry["min_affinity"],-100)||intValue(relation["trust"])<intValue(entry["min_trust"],-100)||intValue(relation["hostility"])>intValue(entry["max_hostility"],100))throw ApiError(drogon::k403Forbidden,"目前關係條件不足");if(entry.get("once_per_user",false).asBool()&&!service->database()->execSqlSync("SELECT 1 FROM npc_dialogue_history WHERE room_id=$1 AND user_id=$2 AND dialogue_entry_id=$3",roomId,user.id,entryId).empty())throw ApiError(drogon::k403Forbidden,"這段對話已經使用過");const auto delta=entry["relation_delta"].isObject()?entry["relation_delta"]:Json::Value{Json::objectValue};const auto affinity=std::clamp<std::int64_t>(intValue(relation["affinity"])+intValue(delta["affinity"]),-100,100);const auto trust=std::clamp<std::int64_t>(intValue(relation["trust"])+intValue(delta["trust"]),-100,100);const auto fear=std::clamp<std::int64_t>(intValue(relation["fear"])+intValue(delta["fear"]),0,100);const auto hostility=std::clamp<std::int64_t>(intValue(relation["hostility"])+intValue(delta["hostility"]),0,100);auto memories=relation["memories"].isArray()?relation["memories"]:Json::Value{Json::arrayValue};Json::Value memory;memory["text"]="玩家選擇「"+stringValue(entry["choice_text"])+"」；NPC 回應「"+stringValue(entry["npc_text"])+"」";memory["type"]="dialogue";memories.append(memory);while(memories.size()>100){Json::Value cut{Json::arrayValue};for(Json::ArrayIndex i=1;i<memories.size();++i)cut.append(memories[i]);memories=std::move(cut);}service->database()->execSqlSync("INSERT INTO npc_relationships(room_id,npc_template_id,user_id,affinity,trust,fear,hostility,memories) VALUES($1,$2,$3,$4,$5,$6,$7,$8::jsonb) ON CONFLICT(room_id,npc_template_id,user_id) DO UPDATE SET affinity=EXCLUDED.affinity,trust=EXCLUDED.trust,fear=EXCLUDED.fear,hostility=EXCLUDED.hostility,memories=EXCLUDED.memories,updated_at=CURRENT_TIMESTAMP",roomId,npcId,user.id,affinity,trust,fear,hostility,compactJson(memories));if(entry.get("once_per_user",false).asBool())service->database()->execSqlSync("INSERT INTO npc_dialogue_history(room_id,user_id,npc_template_id,dialogue_entry_id) VALUES($1,$2,$3,$4) ON CONFLICT DO NOTHING",roomId,user.id,npcId,entryId);Json::Value ev;ev["text"]="與 NPC 交談";ev["npc_template_id"]=Json::Int64(npcId);ev["dialogue_entry_id"]=Json::Int64(entryId);service->addEvent(roomId,user.id,"npc_dialogue",ev);Json::Value out;out["ok"]=true;out["reply"]=stringValue(entry["npc_text"]);out["relationship"]["affinity"]=Json::Int64(affinity);out["relationship"]["trust"]=Json::Int64(trust);out["relationship"]["fear"]=Json::Int64(fear);out["relationship"]["hostility"]=Json::Int64(hostility);out["granted_task"]=Json::nullValue;reply(cb,std::move(out));},"NPC 對話失敗");});

  reg3("/api/rooms/{1}/npcs/{2}/shop/{3}/buy",drogon::Post,[service](const Request& req,Callback& cb,const std::string&roomIdText,const std::string&npcIdText,const std::string&itemIdText){runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomIdText),npcId=positiveId(npcIdText),itemId=positiveId(itemIdText);requireRoomAccess(service,user,roomId);if(service->database()->execSqlSync("SELECT 1 FROM room_npcs WHERE room_id=$1 AND npc_template_id=$2",roomId,npcId).empty())throw ApiError(drogon::k404NotFound,"找不到房間 NPC");const auto rows=service->database()->execSqlSync("SELECT row_to_json(s)::text AS json_text FROM npc_shop_items s WHERE id=$1 AND npc_template_id=$2 AND active=TRUE",itemId,npcId);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到 NPC 商品");auto item=rowJson(rows);const auto relRows=service->database()->execSqlSync("SELECT affinity,hostility FROM npc_relationships WHERE room_id=$1 AND npc_template_id=$2 AND user_id=$3",roomId,npcId,user.id);const auto affinity=relRows.empty()?0:relRows[0]["affinity"].as<std::int64_t>();const auto hostility=relRows.empty()?0:relRows[0]["hostility"].as<std::int64_t>();if(affinity<intValue(item["min_affinity"],-100)||hostility>intValue(item["max_hostility"],100))throw ApiError(drogon::k403Forbidden,"NPC 目前拒絕交易");const auto stock=intValue(item["stock"],-1);
        auto character=characterRaw(service->database(),user.id);const auto currency=stringValue(item["currency"],"weird");auto basePrice=std::max<std::int64_t>(0,intValue(item["price"]));double discount=0.0;const auto discountFaction=intValue(item["discount_faction_id"]);if(discountFaction>0){const auto repRows=service->database()->execSqlSync("SELECT reputation FROM character_reputations WHERE user_id=$1 AND faction_id=$2",user.id,discountFaction);const auto rep=repRows.empty()?0:repRows[0]["reputation"].as<std::int64_t>();discount=std::min<double>(static_cast<double>(intValue(item["max_discount_percent"])),std::max<double>(0.0,rep/100.0*item["discount_per_100"].asDouble()));}const auto price=static_cast<std::int64_t>(std::ceil(basePrice*(1.0-discount/100.0)));const char* coinField=currency=="game"?"game_coins":"weird_coins";const auto coins=intValue(character[coinField]);if(currency=="game"&&!character.get("game_coin_unlocked",false).asBool())throw ApiError(drogon::k403Forbidden,"你尚未解鎖遊戲幣");if(coins<price)throw ApiError(drogon::k400BadRequest,(currency=="game"?"遊戲幣不足":"詭幣不足"));if(stock>=0){service->database()->execSqlSync("INSERT INTO npc_shop_stock(room_id,shop_item_id,remaining) VALUES($1,$2,$3) ON CONFLICT(room_id,shop_item_id) DO NOTHING",roomId,itemId,stock);const auto dec=service->database()->execSqlSync("UPDATE npc_shop_stock SET remaining=remaining-1,updated_at=CURRENT_TIMESTAMP WHERE room_id=$1 AND shop_item_id=$2 AND remaining>0 RETURNING remaining",roomId,itemId);if(dec.empty())throw ApiError(drogon::k400BadRequest,"商品已售罄");}auto inventory=inventoryOf(character);auto equipment=character["equipment"].isArray()?character["equipment"]:Json::Value{Json::arrayValue};auto weapons=character["weapons"].isArray()?character["weapons"]:Json::Value{Json::arrayValue};const auto qty=std::max<std::int64_t>(1,intValue(item["quantity"],1));if(stringValue(item["item_type"])=="equipment")for(std::int64_t i=0;i<qty;++i)equipment.append(stringValue(item["name"]));else if(stringValue(item["item_type"])=="weapon")for(std::int64_t i=0;i<qty;++i)weapons.append(stringValue(item["name"]));else inventory=changeInventory(std::move(inventory),stringValue(item["name"]),qty,stringValue(item["item_category"]));service->database()->execSqlSync(std::string("UPDATE character_cards SET ")+coinField+"=$1,inventory=$2::jsonb,equipment=$3::jsonb,weapons=$4::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$5",coins-price,compactJson(inventory),compactJson(equipment),compactJson(weapons),user.id);Json::Value ev;ev["text"]="向 NPC 購買「"+stringValue(item["name"])+"」";ev["npc_template_id"]=Json::Int64(npcId);ev["shop_item_id"]=Json::Int64(itemId);ev["price"]=Json::Int64(price);service->addEvent(roomId,user.id,"npc_shop",ev);item["effective_price"]=Json::Int64(price);item["discount_percent"]=discount;Json::Value out;out["ok"]=true;out["item"]=item;out["character"]=characterRaw(service->database(),user.id);reply(cb,std::move(out));},"NPC 商店購買失敗");});
}

}  // namespace trpg
