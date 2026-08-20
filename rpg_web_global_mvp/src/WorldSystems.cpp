#include "trpg/WorldSystems.h"

#include "trpg/CoreService.h"
#include "trpg/RoomSocket.h"
#include "trpg/RuleEngine.h"

#include <drogon/drogon.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <limits>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace trpg {
namespace {

using Db = drogon::orm::DbClientPtr;
using Request = drogon::HttpRequestPtr;
using Callback = std::function<void(const drogon::HttpResponsePtr&)>;

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

constexpr std::array<const char*, 7> kElements = {"暗", "光", "金", "木", "水", "火", "土"};

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

std::int64_t intValue(const Json::Value& value, std::int64_t fallback = 0) {
  try {
    if (value.isInt64()) return value.asInt64();
    if (value.isUInt64()) return static_cast<std::int64_t>(value.asUInt64());
    if (value.isDouble()) return static_cast<std::int64_t>(value.asDouble());
    if (value.isString()) return std::stoll(value.asString());
  } catch (...) {
  }
  return fallback;
}

std::string strValue(const Json::Value& value, std::string fallback = {}) {
  return value.isString() ? value.asString() : fallback;
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


// [功能備註｜頭像圖片上傳] 前端直接選 JPG/PNG/WebP 後送 base64；C++ 解碼、驗證真實檔頭，再存 PostgreSQL。
constexpr std::size_t kAvatarMaxBytes = 5U * 1024U * 1024U;

std::vector<unsigned char> decodeBase64(std::string input) {
  input.erase(std::remove_if(input.begin(), input.end(), [](unsigned char c) {
    return c == '\r' || c == '\n' || c == ' ' || c == '\t';
  }), input.end());
  if (input.empty() || input.size() % 4 != 0 || input.size() > ((kAvatarMaxBytes + 2) / 3 * 4 + 8)) {
    throw ApiError(drogon::k400BadRequest, "頭像資料格式錯誤或檔案過大");
  }
  std::vector<unsigned char> output((input.size() / 4) * 3 + 3);
  const int decoded = EVP_DecodeBlock(output.data(),
      reinterpret_cast<const unsigned char*>(input.data()),
      static_cast<int>(input.size()));
  if (decoded < 0) throw ApiError(drogon::k400BadRequest, "頭像 base64 解碼失敗");
  std::size_t padding = 0;
  if (!input.empty() && input.back() == '=') ++padding;
  if (input.size() > 1 && input[input.size() - 2] == '=') ++padding;
  const auto finalSize = static_cast<std::size_t>(decoded) - padding;
  if (finalSize == 0 || finalSize > kAvatarMaxBytes) {
    throw ApiError(drogon::k400BadRequest, "頭像大小必須介於 1 byte 到 5 MB");
  }
  output.resize(finalSize);
  return output;
}

std::string encodeBase64(const std::vector<unsigned char>& bytes) {
  if (bytes.empty()) return {};
  std::string output(((bytes.size() + 2) / 3) * 4, '\0');
  const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()),
      bytes.data(), static_cast<int>(bytes.size()));
  if (written < 0) throw std::runtime_error("base64 encoding failed");
  output.resize(static_cast<std::size_t>(written));
  return output;
}

std::string detectedAvatarMime(const std::vector<unsigned char>& bytes) {
  if (bytes.size() >= 8 &&
      bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4e && bytes[3] == 0x47 &&
      bytes[4] == 0x0d && bytes[5] == 0x0a && bytes[6] == 0x1a && bytes[7] == 0x0a) {
    return "image/png";
  }
  if (bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff) {
    return "image/jpeg";
  }
  if (bytes.size() >= 12 &&
      bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F' &&
      bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P') {
    return "image/webp";
  }
  throw ApiError(drogon::k400BadRequest, "只支援 JPG、PNG、WebP 頭像圖片");
}

Json::Value storeAvatarUpload(const std::shared_ptr<CoreService>& service,
                              std::int64_t userId,
                              const Json::Value& body) {
  const auto base64 = strValue(body["image_base64"]);
  if (base64.empty()) throw ApiError(drogon::k400BadRequest, "請選擇頭像圖片");
  const auto bytes = decodeBase64(base64);
  const auto mime = detectedAvatarMime(bytes);
  const auto cards = service->database()->execSqlSync(
      "SELECT id FROM character_cards WHERE user_id=$1", userId);
  if (cards.empty()) throw ApiError(drogon::k404NotFound, "該玩家尚未建立角色卡");
  const auto normalizedBase64 = encodeBase64(bytes);
  service->database()->execSqlSync(R"SQL(
    INSERT INTO avatar_images(user_id,mime_type,image_base64,size_bytes,updated_at)
    VALUES($1,$2,$3,$4,CURRENT_TIMESTAMP)
    ON CONFLICT(user_id) DO UPDATE SET
      mime_type=EXCLUDED.mime_type,
      image_base64=EXCLUDED.image_base64,
      size_bytes=EXCLUDED.size_bytes,
      updated_at=CURRENT_TIMESTAMP
  )SQL", userId, mime, normalizedBase64, static_cast<std::int64_t>(bytes.size()));
  const auto url = "/api/avatar-images/" + std::to_string(userId);
  service->database()->execSqlSync(
      "UPDATE character_cards SET avatar_url=$1,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",
      url, userId);
  Json::Value out;
  out["ok"] = true;
  out["avatar_url"] = url;
  out["mime_type"] = mime;
  out["size_bytes"] = Json::Int64(static_cast<std::int64_t>(bytes.size()));
  out["character"] = service->characterByUser(userId);
  return out;
}


// [功能備註｜NPC 頭像上傳] 使用與玩家相同的檔頭驗證；保存後把 npc_templates.image_url 指到公開圖片路由。
Json::Value storeNpcAvatarUpload(const std::shared_ptr<CoreService>& service,
                                 std::int64_t npcId,const Json::Value& body){
  const auto base64=strValue(body["image_base64"]);if(base64.empty())throw ApiError(drogon::k400BadRequest,"請選擇 NPC 頭像圖片");
  const auto bytes=decodeBase64(base64);const auto mime=detectedAvatarMime(bytes);
  if(service->database()->execSqlSync("SELECT 1 FROM npc_templates WHERE id=$1",npcId).empty())throw ApiError(drogon::k404NotFound,"找不到 NPC");
  const auto normalized=encodeBase64(bytes);
  service->database()->execSqlSync(R"SQL(INSERT INTO npc_avatar_images(npc_template_id,mime_type,image_base64,size_bytes,updated_at) VALUES($1,$2,$3,$4,CURRENT_TIMESTAMP) ON CONFLICT(npc_template_id) DO UPDATE SET mime_type=EXCLUDED.mime_type,image_base64=EXCLUDED.image_base64,size_bytes=EXCLUDED.size_bytes,updated_at=CURRENT_TIMESTAMP)SQL",npcId,mime,normalized,static_cast<std::int64_t>(bytes.size()));
  const auto url="/api/npc-avatar-images/"+std::to_string(npcId);service->database()->execSqlSync("UPDATE npc_templates SET image_url=$1,updated_at=CURRENT_TIMESTAMP WHERE id=$2",url,npcId);
  Json::Value out;out["ok"]=true;out["image_url"]=url;out["mime_type"]=mime;out["size_bytes"]=Json::Int64(static_cast<std::int64_t>(bytes.size()));return out;
}
void discardStoredNpcAvatarIfExternal(const std::shared_ptr<CoreService>& service,std::int64_t npcId,const std::string&url){const auto internal="/api/npc-avatar-images/"+std::to_string(npcId);if(url!=internal)service->database()->execSqlSync("DELETE FROM npc_avatar_images WHERE npc_template_id=$1",npcId);}

void discardStoredAvatarIfExternal(const std::shared_ptr<CoreService>& service,
                                   std::int64_t userId,
                                   const std::string& url) {
  const auto internal = "/api/avatar-images/" + std::to_string(userId);
  if (url != internal) {
    service->database()->execSqlSync("DELETE FROM avatar_images WHERE user_id=$1", userId);
  }
}

template <typename Function>
void runHandler(Callback& callback, Function&& function,
                const char* genericMessage = "操作失敗") {
  try {
    function();
  } catch (const ApiError& error) {
    fail(callback, error.status, error.what());
  } catch (const std::exception& error) {
    LOG_ERROR << "world systems error: " << error.what();
    fail(callback, drogon::k500InternalServerError, genericMessage);
  }
}

Json::Value bodyOrEmpty(const Request& request) {
  const auto json = request->getJsonObject();
  return json && json->isObject() ? *json : Json::Value{Json::objectValue};
}

std::int64_t positiveId(const std::string& text) {
  try {
    const auto value = std::stoll(text);
    if (value <= 0) throw std::invalid_argument{"id"};
    return value;
  } catch (...) {
    throw ApiError(drogon::k400BadRequest, "ID 無效");
  }
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
  if (admin && !user.isAdmin) throw ApiError(drogon::k403Forbidden, "需要 DM 權限");
  return user;
}

void requireRoom(const std::shared_ptr<CoreService>& service,
                 const UserInfo& user, std::int64_t roomId) {
  if (!user.isAdmin && !service->userCanAccessRoom(user.id, roomId)) {
    throw ApiError(drogon::k403Forbidden, "你不在這個房間");
  }
}

Json::Value rowJson(const drogon::orm::Result& rows, std::size_t index = 0) {
  if (rows.empty() || index >= rows.size()) return Json::Value{};
  return parseJson(rows[index]["json_text"].as<std::string>());
}

std::optional<std::int64_t> effectiveNode(const Db& db, std::int64_t roomId,
                                          std::int64_t userId) {
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

bool nodeBlocksRemote(const Db& db, std::int64_t nodeId) {
  if (nodeId <= 0) return false;
  const auto rows = db->execSqlSync(
      "SELECT communication_blocked FROM room_map_nodes WHERE id=$1", nodeId);
  return !rows.empty() && rows[0]["communication_blocked"].as<bool>();
}

std::vector<std::pair<std::string,std::int64_t>> heldItems(const Json::Value& character) {
  std::vector<std::pair<std::string,std::int64_t>> held;
  auto collect = [&](const Json::Value& raw) {
    std::string name;
    std::int64_t qty = 1;
    if (raw.isString()) name = raw.asString();
    else if (raw.isObject()) {
      name = strValue(raw["name"], strValue(raw["title"]));
      qty = std::max<std::int64_t>(1, intValue(raw["quantity"], 1));
    }
    if (name.empty()) return;
    for (auto& entry : held) {
      if (entry.first == name) { entry.second += qty; return; }
    }
    held.emplace_back(name, qty);
  };
  for (const auto* key : {"inventory", "equipment", "weapons"}) {
    if (character[key].isArray()) for (const auto& raw : character[key]) collect(raw);
  }
  return held;
}

bool hasCommunicationDevice(const std::shared_ptr<CoreService>& service,
                            std::int64_t userId) {
  const auto character = service->characterByUser(userId);
  if (character.isNull()) return false;
  for (const auto& [name, qty] : heldItems(character)) {
    if (qty <= 0) continue;
    const auto rows = service->database()->execSqlSync(
        "SELECT effects::text AS effects_text FROM item_templates WHERE name=$1 LIMIT 1", name);
    if (rows.empty() || rows[0]["effects_text"].isNull()) continue;
    const auto effects = parseJson(rows[0]["effects_text"].as<std::string>());
    if (effects.get("communication_device", false).asBool() ||
        effects.get("cross_scene_communication", false).asBool()) return true;
  }
  return false;
}

Json::Value normalizedElementMap(const Json::Value& source) {
  Json::Value out{Json::objectValue};
  for (const auto* element : kElements) {
    out[element] = Json::Int64(std::max<std::int64_t>(0, intValue(source[element])));
  }
  return out;
}

void addGeneration(Json::Value& total, const Json::Value& source,
                   std::int64_t multiplier = 1) {
  if (!source.isObject() || multiplier <= 0) return;
  for (const auto* element : kElements) {
    const auto add = intValue(source[element]);
    if (add > 0) total[element] = Json::Int64(intValue(total[element]) + add * multiplier);
  }
}

Json::Value generationRates(const std::shared_ptr<CoreService>& service,
                            const Json::Value& character) {
  Json::Value rates = normalizedElementMap(Json::Value{Json::objectValue});
  std::set<std::int64_t> fallbackTemplates;
  if (character["skills"].isArray()) {
    for (const auto& skill : character["skills"]) {
      if (!skill.isObject()) continue;
      const auto& embedded = skill["data"]["element_storage_generation"];
      if (embedded.isObject()) {
        // 角色技能通常已複製模板 data；有內嵌資料時只計一次，避免模板再加一次。
        addGeneration(rates, embedded);
      } else {
        const auto tid = intValue(skill["template_id"]);
        if (tid > 0) fallbackTemplates.insert(tid);
      }
    }
  }
  for (const auto tid : fallbackTemplates) {
    const auto rows = service->database()->execSqlSync(
        "SELECT data::text AS data_text FROM skill_templates WHERE id=$1", tid);
    if (!rows.empty() && !rows[0]["data_text"].isNull()) {
      addGeneration(rates, parseJson(rows[0]["data_text"].as<std::string>())["element_storage_generation"]);
    }
  }
  for (const auto& [name, qty] : heldItems(character)) {
    const auto rows = service->database()->execSqlSync(
        "SELECT effects::text AS effects_text FROM item_templates WHERE name=$1 LIMIT 1", name);
    if (!rows.empty() && !rows[0]["effects_text"].isNull()) {
      addGeneration(rates, parseJson(rows[0]["effects_text"].as<std::string>())["element_storage_generation"], qty);
    }
  }
  return rates;
}

std::vector<std::int64_t> parseConnections(const Json::Value& raw) {
  std::vector<std::int64_t> ids;
  if (!raw.isArray()) return ids;
  for (const auto& value : raw) {
    const auto id = value.isObject() ? intValue(value["id"], intValue(value["node_id"])) : intValue(value);
    if (id > 0 && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
  }
  return ids;
}

std::int64_t randomChoice(const std::vector<std::int64_t>& values) {
  if (values.empty()) return 0;
  static thread_local std::mt19937_64 engine{std::random_device{}()};
  std::uniform_int_distribution<std::size_t> dist(0, values.size() - 1);
  return values[dist(engine)];
}

int randomD100() {
  static thread_local std::mt19937 engine{std::random_device{}()};
  return std::uniform_int_distribution<int>(1, 100)(engine);
}

void acceptInvite(const std::shared_ptr<CoreService>& service,
                  std::int64_t inviteId, std::int64_t roomId,
                  std::int64_t teamId, std::int64_t userId,
                  const char* source) {
  service->database()->execSqlSync(
      "DELETE FROM room_team_members WHERE room_id=$1 AND user_id=$2", roomId, userId);
  service->database()->execSqlSync(
      "INSERT INTO room_team_members(team_id,room_id,user_id) VALUES($1,$2,$3) "
      "ON CONFLICT(team_id,user_id) DO NOTHING", teamId, roomId, userId);
  service->database()->execSqlSync(
      "UPDATE room_team_invites SET status='accepted',responded_at=CURRENT_TIMESTAMP,"
      "response_source=$1 WHERE id=$2", source, inviteId);
  const auto nodeRows = service->database()->execSqlSync(
      "SELECT current_map_node_id FROM room_teams WHERE id=$1 AND room_id=$2", teamId, roomId);
  if (!nodeRows.empty() && !nodeRows[0]["current_map_node_id"].isNull()) {
    service->database()->execSqlSync(
        "UPDATE room_members SET current_map_node_id=$1 "
        "WHERE room_id=$2 AND user_id=$3",
        nodeRows[0]["current_map_node_id"].as<std::int64_t>(), roomId, userId);
  }
}

void exploreOffline(const std::shared_ptr<CoreService>& service,
                    std::int64_t roomId, std::int64_t userId,
                    std::int64_t nodeId) {
  if (nodeId <= 0) return;
  const auto nodeRows = service->database()->execSqlSync(
      "SELECT name,investigation_dc FROM room_map_nodes WHERE id=$1 AND room_id=$2", nodeId, roomId);
  if (nodeRows.empty()) return;
  const auto character = service->characterByUser(userId);
  const auto spirit = intValue(character["spirit"]);
  const auto dc = nodeRows[0]["investigation_dc"].as<std::int64_t>();
  const auto rate = std::clamp<std::int64_t>(40 + spirit / 10 - (std::max<std::int64_t>(1, dc) - 50), 5, 95);
  const auto roll = randomD100();
  Json::Value found{Json::arrayValue};
  if (roll <= rate) {
    const auto clues = service->database()->execSqlSync(
        "SELECT id,name,linked_rule_id,reveal_level FROM clue_templates "
        "WHERE auto_discover=TRUE AND (room_id IS NULL OR room_id=$1) "
        "AND (map_node_id IS NULL OR map_node_id=$2) AND investigation_dc<=$3",
        roomId, nodeId, rate);
    for (const auto& row : clues) {
      const auto clueId = row["id"].as<std::int64_t>();
      service->database()->execSqlSync(
          "INSERT INTO character_clues(clue_id,user_id,note) VALUES($1,$2,'離線 AI 探索發現') "
          "ON CONFLICT(clue_id,user_id) DO NOTHING", clueId, userId);
      if (!row["linked_rule_id"].isNull()) {
        service->database()->execSqlSync(
            "INSERT INTO rule_discoveries(rule_id,user_id,knowledge_level) VALUES($1,$2,$3) "
            "ON CONFLICT(rule_id,user_id) DO UPDATE SET knowledge_level=GREATEST(rule_discoveries.knowledge_level,EXCLUDED.knowledge_level),updated_at=CURRENT_TIMESTAMP",
            row["linked_rule_id"].as<std::int64_t>(), userId,
            std::clamp<std::int64_t>(row["reveal_level"].isNull() ? 1 : row["reveal_level"].as<std::int64_t>(), 1, 3));
      }
      found.append(row["name"].as<std::string>());
    }
  }
  Json::Value event;
  event["text"] = "離線 AI 自動探查「" + nodeRows[0]["name"].as<std::string>() + "」";
  event["roll"] = roll;
  event["rate"] = Json::Int64(rate);
  event["success"] = roll <= rate;
  event["found"] = found;
  service->addEvent(roomId, userId, "offline_ai_explore", event);
}

std::vector<std::int64_t> allRoomUsers(const Db& db, std::int64_t roomId) {
  std::vector<std::int64_t> ids;
  const auto rows = db->execSqlSync("SELECT user_id FROM room_members WHERE room_id=$1", roomId);
  for (const auto& row : rows) ids.push_back(row["user_id"].as<std::int64_t>());
  return ids;
}

bool userIsAdmin(const Db& db, std::int64_t userId) {
  const auto rows = db->execSqlSync("SELECT is_admin FROM users WHERE id=$1", userId);
  return !rows.empty() && rows[0]["is_admin"].as<bool>();
}

void appendAudience(Json::Value& payload, const std::vector<std::int64_t>& audience) {
  payload["audience_user_ids"] = Json::arrayValue;
  for (const auto id : audience) payload["audience_user_ids"].append(Json::Int64(id));
}

}  // namespace

bool canUsersCommunicate(const std::shared_ptr<CoreService>& service,
                         std::int64_t roomId,
                         std::int64_t senderUserId,
                         std::int64_t receiverUserId) {
  if (senderUserId == receiverUserId) return true;
  const auto db = service->database();
  if (userIsAdmin(db, senderUserId) || userIsAdmin(db, receiverUserId)) return true;
  const auto senderNode = effectiveNode(db, roomId, senderUserId);
  const auto receiverNode = effectiveNode(db, roomId, receiverUserId);
  if (senderNode && receiverNode && *senderNode == *receiverNode) return true;
  if ((senderNode && nodeBlocksRemote(db, *senderNode)) ||
      (receiverNode && nodeBlocksRemote(db, *receiverNode))) return false;
  return hasCommunicationDevice(service, senderUserId) &&
         hasCommunicationDevice(service, receiverUserId);
}

std::vector<std::int64_t> communicationAudience(
    const std::shared_ptr<CoreService>& service,
    std::int64_t roomId,
    std::int64_t senderUserId) {
  std::vector<std::int64_t> audience;
  for (const auto userId : allRoomUsers(service->database(), roomId)) {
    if (canUsersCommunicate(service, roomId, senderUserId, userId)) audience.push_back(userId);
  }
  if (std::find(audience.begin(), audience.end(), senderUserId) == audience.end()) {
    audience.push_back(senderUserId);
  }
  return audience;
}

Json::Value roomSnapshotForViewer(const std::shared_ptr<CoreService>& service,
                                  std::int64_t roomId,
                                  std::int64_t viewerUserId) {
  auto snapshot = service->roomSnapshot(roomId);
  if (userIsAdmin(service->database(), viewerUserId)) return snapshot;
  Json::Value filtered{Json::arrayValue};
  for (const auto& event : snapshot["events"]) {
    if (strValue(event["type"]) != "chat") {
      filtered.append(event);
      continue;
    }
    const auto& audience = event["payload"]["audience_user_ids"];
    if (!audience.isArray() || audience.empty()) {
      filtered.append(event);  // 舊聊天紀錄保持相容。
      continue;
    }
    bool allowed = false;
    for (const auto& id : audience) if (intValue(id) == viewerUserId) allowed = true;
    if (allowed) filtered.append(event);
  }
  snapshot["events"] = std::move(filtered);
  return snapshot;
}

Json::Value runElementGenerationTick(const std::shared_ptr<CoreService>& service,
                                     std::int64_t userId) {
  Json::Value result;
  result["updated"] = Json::arrayValue;
  std::string sql =
      "SELECT user_id,GREATEST(1,LEAST(1440,FLOOR(EXTRACT(EPOCH FROM (CURRENT_TIMESTAMP-element_generation_last_at))/60)::BIGINT)) AS minutes "
      "FROM character_cards WHERE CURRENT_TIMESTAMP-element_generation_last_at>=INTERVAL '1 minute'";
  const auto rows = [&]() {
    if (userId > 0) return service->database()->execSqlSync(sql + " AND user_id=$1", userId);
    return service->database()->execSqlSync(sql);
  }();
  for (const auto& row : rows) {
    const auto uid = row["user_id"].as<std::int64_t>();
    const auto minutes = row["minutes"].as<std::int64_t>();
    auto character = service->characterByUser(uid);
    if (character.isNull()) continue;
    const auto rates = generationRates(service, character);
    auto stored = normalizedElementMap(character["element_storage"]);
    const auto caps = normalizedElementMap(character["element_storage_caps"]);
    bool changed = false;
    for (const auto* element : kElements) {
      const auto rate = intValue(rates[element]);
      if (rate <= 0) continue;
      const auto before = intValue(stored[element]);
      const auto after = std::min<std::int64_t>(intValue(caps[element]), before + rate * minutes);
      stored[element] = Json::Int64(after);
      changed = changed || after != before;
    }
    service->database()->execSqlSync(
        "UPDATE character_cards SET element_storage=$1::jsonb,element_generation_last_at=CURRENT_TIMESTAMP,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",
        compactJson(stored), uid);
    if (changed) {
      Json::Value entry;
      entry["user_id"] = Json::Int64(uid);
      entry["minutes"] = Json::Int64(minutes);
      entry["rates"] = rates;
      entry["storage"] = stored;
      result["updated"].append(entry);
    }
  }
  result["ok"] = true;
  return result;
}

Json::Value runOfflineAiTick(const std::shared_ptr<CoreService>& service,
                             std::int64_t roomId) {
  Json::Value result;
  result["ok"] = true;
  result["actions"] = Json::arrayValue;
  std::string sql =
      "SELECT rm.room_id,rm.user_id,rm.current_map_node_id,rm.offline_ai_enabled,rm.ai_last_action_at "
      "FROM room_members rm JOIN rooms r ON r.id=rm.room_id "
      "WHERE rm.role='player' AND rm.offline_ai_enabled=TRUE AND r.status NOT IN ('closed','saved')";
  const auto members = [&]() {
    if (roomId > 0) return service->database()->execSqlSync(sql + " AND rm.room_id=$1", roomId);
    return service->database()->execSqlSync(sql);
  }();
  for (const auto& row : members) {
    const auto rid = row["room_id"].as<std::int64_t>();
    const auto uid = row["user_id"].as<std::int64_t>();
    if (RoomSocket::isUserOnline(uid, rid)) {
      service->database()->execSqlSync(
          "UPDATE room_members SET offline_ai_active=FALSE,last_seen_at=CURRENT_TIMESTAMP WHERE room_id=$1 AND user_id=$2",
          rid, uid);
      continue;
    }
    const auto due = service->database()->execSqlSync(
        "SELECT (ai_last_action_at IS NULL OR CURRENT_TIMESTAMP-ai_last_action_at>=INTERVAL '30 seconds') AS due "
        "FROM room_members WHERE room_id=$1 AND user_id=$2", rid, uid);
    if (due.empty() || !due[0]["due"].as<bool>()) continue;
    service->database()->execSqlSync(
        "UPDATE room_members SET offline_ai_active=TRUE,ai_last_action_at=CURRENT_TIMESTAMP WHERE room_id=$1 AND user_id=$2",
        rid, uid);

    const auto invites = service->database()->execSqlSync(
        "SELECT id,team_id FROM room_team_invites WHERE room_id=$1 AND target_user_id=$2 AND status='pending' ORDER BY created_at LIMIT 1",
        rid, uid);
    if (!invites.empty()) {
      const auto inviteId = invites[0]["id"].as<std::int64_t>();
      const auto teamId = invites[0]["team_id"].as<std::int64_t>();
      acceptInvite(service, inviteId, rid, teamId, uid, "offline_ai");
      Json::Value event;
      event["text"] = "離線 AI 自動接受隊伍邀請";
      event["team_id"] = Json::Int64(teamId);
      service->addEvent(rid, uid, "offline_ai_join", event);
      evaluateRuleEvent(service, rid, uid, "TEAM_JOINED", event, false);
    }

    const auto teamRows = service->database()->execSqlSync(
        "SELECT t.id,t.leader_user_id,t.current_map_node_id FROM room_team_members tm "
        "JOIN room_teams t ON t.id=tm.team_id WHERE tm.room_id=$1 AND tm.user_id=$2",
        rid, uid);
    std::int64_t leaderTeamId = 0;
    if (!teamRows.empty()) {
      const auto teamId = teamRows[0]["id"].as<std::int64_t>();
      const auto leaderId = teamRows[0]["leader_user_id"].as<std::int64_t>();
      if (leaderId != uid) {
        if (!teamRows[0]["current_map_node_id"].isNull()) {
          const auto nodeId = teamRows[0]["current_map_node_id"].as<std::int64_t>();
          auto moveEvent = ruleMapNodePayload(service, rid, nodeId);
          moveEvent["source"] = "offline_ai_follow";
          moveEvent["team_id"] = Json::Int64(teamId);
          auto attempt = evaluateRuleEvent(service, rid, uid, "PLAYER_MOVE_ATTEMPT", moveEvent, false);
          if (attempt["blocked"].asBool()) {
            Json::Value action; action["room_id"] = Json::Int64(rid); action["user_id"] = Json::Int64(uid);
            action["type"] = "rule_blocked"; action["team_id"] = Json::Int64(teamId); action["node_id"] = Json::Int64(nodeId);
            result["actions"].append(action);
            continue;
          }
          service->database()->execSqlSync(
              "UPDATE room_members SET current_map_node_id=$1 WHERE room_id=$2 AND user_id=$3",
              nodeId, rid, uid);
          evaluateRuleEvent(service, rid, uid, "PLAYER_MOVED", moveEvent, false);
        }
        Json::Value action;
        action["room_id"] = Json::Int64(rid);
        action["user_id"] = Json::Int64(uid);
        action["type"] = "follow_team";
        action["team_id"] = Json::Int64(teamId);
        result["actions"].append(action);
        continue;
      }
      // 隊長本人離線時，由 AI 依探索規則帶著整隊前進，而不是讓隊伍永遠停住。
      leaderTeamId = teamId;
    }

    auto current = effectiveNode(service->database(), rid, uid);
    std::int64_t currentId = current.value_or(0);
    if (currentId <= 0) {
      const auto starts = service->database()->execSqlSync(
          "SELECT id FROM room_map_nodes WHERE room_id=$1 AND hidden=FALSE ORDER BY id LIMIT 1", rid);
      if (!starts.empty()) currentId = starts[0]["id"].as<std::int64_t>();
    }
    std::vector<std::int64_t> candidates;
    if (currentId > 0) {
      const auto nodeRows = service->database()->execSqlSync(
          "SELECT connections::text AS connections_text FROM room_map_nodes WHERE id=$1 AND room_id=$2",
          currentId, rid);
      if (!nodeRows.empty() && !nodeRows[0]["connections_text"].isNull()) {
        candidates = parseConnections(parseJson(nodeRows[0]["connections_text"].as<std::string>(), Json::Value{Json::arrayValue}));
      }
    }
    if (candidates.empty()) {
      const auto nodes = service->database()->execSqlSync(
          "SELECT id FROM room_map_nodes WHERE room_id=$1 AND hidden=FALSE AND id<>$2 ORDER BY id",
          rid, currentId);
      for (const auto& n : nodes) candidates.push_back(n["id"].as<std::int64_t>());
    }
    auto nextId = randomChoice(candidates);
    if (nextId <= 0) nextId = currentId;
    if (nextId > 0) {
      auto moveEvent = ruleMapNodePayload(service, rid, nextId);
      moveEvent["source"] = leaderTeamId > 0 ? "offline_ai_team_explore" : "offline_ai_explore";
      moveEvent["from_node_id"] = Json::Int64(currentId);
      bool movementBlocked = false;
      std::int64_t blockedUserId = 0;
      std::vector<std::int64_t> movingUsers{uid};
      if (leaderTeamId > 0) {
        movingUsers.clear();
        const auto teamMembers = service->database()->execSqlSync("SELECT user_id FROM room_team_members WHERE team_id=$1 ORDER BY user_id", leaderTeamId);
        for (const auto& member : teamMembers) movingUsers.push_back(member["user_id"].as<std::int64_t>());
        moveEvent["team_id"] = Json::Int64(leaderTeamId);
      }
      for (const auto movingUserId : movingUsers) {
        auto attempt = evaluateRuleEvent(service, rid, movingUserId, "PLAYER_MOVE_ATTEMPT", moveEvent, false);
        if (attempt["blocked"].asBool()) { movementBlocked = true; blockedUserId = movingUserId; break; }
      }
      if (movementBlocked) {
        Json::Value action; action["room_id"] = Json::Int64(rid); action["user_id"] = Json::Int64(uid);
        action["blocked_user_id"] = Json::Int64(blockedUserId); action["type"] = "rule_blocked"; action["node_id"] = Json::Int64(nextId);
        result["actions"].append(action);
        continue;
      }
      if (leaderTeamId > 0) {
        service->database()->execSqlSync(
            "UPDATE room_teams SET current_map_node_id=$1,updated_at=CURRENT_TIMESTAMP WHERE id=$2 AND room_id=$3",
            nextId, leaderTeamId, rid);
        service->database()->execSqlSync(
            "UPDATE room_members SET current_map_node_id=$1 WHERE room_id=$2 AND user_id IN (SELECT user_id FROM room_team_members WHERE team_id=$3)",
            nextId, rid, leaderTeamId);
      } else {
        service->database()->execSqlSync(
            "UPDATE room_members SET current_map_node_id=$1 WHERE room_id=$2 AND user_id=$3",
            nextId, rid, uid);
      }
      service->database()->execSqlSync(
          "INSERT INTO map_node_discoveries(node_id,user_id) VALUES($1,$2) ON CONFLICT DO NOTHING",
          nextId, uid);
      Json::Value event = moveEvent;
      event["text"] = "離線 AI 自動探索並移動";
      service->addEvent(rid, uid, "offline_ai_move", event);
      for (const auto movingUserId : movingUsers) evaluateRuleEvent(service, rid, movingUserId, "PLAYER_MOVED", event, false);
      exploreOffline(service, rid, uid, nextId);
    }
    Json::Value action;
    action["room_id"] = Json::Int64(rid);
    action["user_id"] = Json::Int64(uid);
    action["type"] = leaderTeamId > 0 ? "explore_team" : "explore";
    if (leaderTeamId > 0) action["team_id"] = Json::Int64(leaderTeamId);
    action["node_id"] = Json::Int64(nextId);
    result["actions"].append(action);
  }
  return result;
}

void registerWorldExpansionRoutes(const std::shared_ptr<CoreService>& service) {
  auto reg0 = [](const std::string& path, drogon::HttpMethod method, auto handler) {
    drogon::app().registerHandler(path, std::move(handler), {method});
  };
  auto reg1 = [](const std::string& path, drogon::HttpMethod method, auto handler) {
    drogon::app().registerHandler(path, std::move(handler), {method});
  };
  auto reg2 = reg1;

  // [功能備註｜規則書] 玩家讀取公開規則書；DM 可讀全部。
  reg0("/api/rulebook", drogon::Get,
       [service](const Request& req, Callback&& cbRaw) {
    Callback cb = std::move(cbRaw);
    runHandler(cb, [&] {
      const auto user = requireUser(service, req);
      const auto rows = service->database()->execSqlSync(
          user.isAdmin
            ? "SELECT row_to_json(x)::text AS json_text FROM rulebook_entries x ORDER BY sort_order,id"
            : "SELECT row_to_json(x)::text AS json_text FROM rulebook_entries x WHERE active=TRUE AND public=TRUE ORDER BY sort_order,id");
      Json::Value out; out["entries"] = Json::arrayValue;
      for (std::size_t i=0;i<rows.size();++i) out["entries"].append(rowJson(rows,i));
      reply(cb, std::move(out));
    }, "讀取規則書失敗");
  });

  // [功能備註｜規則書｜DM] 建立規則書條目。
  reg0("/api/admin/rulebook", drogon::Post,
       [service](const Request& req, Callback&& cbRaw) {
    Callback cb = std::move(cbRaw);
    runHandler(cb, [&] {
      requireUser(service, req, true); const auto body=bodyOrEmpty(req);
      const auto title=strValue(body["title"]); if(title.empty()) throw ApiError(drogon::k400BadRequest,"請輸入規則標題");
      const auto rows=service->database()->execSqlSync(
          "INSERT INTO rulebook_entries(title,category,content,sort_order,public,active) VALUES($1,$2,$3,$4,$5,$6) RETURNING row_to_json(rulebook_entries)::text AS json_text",
          title,strValue(body["category"],"一般"),strValue(body["content"]),intValue(body["sort_order"],100),body.get("public",true).asBool(),body.get("active",true).asBool());
      Json::Value out;out["ok"]=true;out["entry"]=rowJson(rows);reply(cb,std::move(out));
    }, "建立規則書失敗");
  });

  reg1("/api/admin/rulebook/{1}", drogon::Patch,
       [service](const Request& req, Callback&& cbRaw, const std::string& idText) {
    Callback cb=std::move(cbRaw); runHandler(cb,[&]{requireUser(service,req,true);const auto id=positiveId(idText);const auto b=bodyOrEmpty(req);
      const auto rows=service->database()->execSqlSync(
        "UPDATE rulebook_entries SET title=COALESCE(NULLIF($1,''),title),category=COALESCE(NULLIF($2,''),category),content=$3,sort_order=$4,public=$5,active=$6,updated_at=CURRENT_TIMESTAMP WHERE id=$7 RETURNING row_to_json(rulebook_entries)::text AS json_text",
        strValue(b["title"]),strValue(b["category"]),strValue(b["content"]),intValue(b["sort_order"],100),b.get("public",true).asBool(),b.get("active",true).asBool(),id);
      if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到規則");Json::Value out;out["ok"]=true;out["entry"]=rowJson(rows);reply(cb,std::move(out));},"修改規則書失敗");
  });
  reg1("/api/admin/rulebook/{1}", drogon::Delete,
       [service](const Request& req, Callback&& cbRaw, const std::string& idText) {
    Callback cb=std::move(cbRaw);runHandler(cb,[&]{requireUser(service,req,true);const auto rows=service->database()->execSqlSync("DELETE FROM rulebook_entries WHERE id=$1 RETURNING id",positiveId(idText));if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到規則");Json::Value out;out["ok"]=true;reply(cb,std::move(out));},"刪除規則書失敗");
  });

  // [功能備註｜玩家頭像] 玩家可貼網址，也可直接選圖片上傳；圖片由 C++ 驗證後存 PostgreSQL。
  reg0("/api/me/avatar", drogon::Patch,
       [service](const Request& req, Callback&& cbRaw) {
    Callback cb=std::move(cbRaw);runHandler(cb,[&]{const auto user=requireUser(service,req);auto url=strValue(bodyOrEmpty(req)["avatar_url"]);if(url.size()>2048)throw ApiError(drogon::k400BadRequest,"頭像網址太長");discardStoredAvatarIfExternal(service,user.id,url);service->database()->execSqlSync("UPDATE character_cards SET avatar_url=$1,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",url,user.id);Json::Value out;out["ok"]=true;out["character"]=service->characterByUser(user.id);reply(cb,std::move(out));},"修改頭像失敗");
  });

  // [功能備註｜玩家頭像上傳｜POST /api/me/avatar/upload] 玩家直接上傳 JPG/PNG/WebP，最大 5 MB。
  reg0("/api/me/avatar/upload", drogon::Post,
       [service](const Request& req, Callback&& cbRaw) {
    Callback cb=std::move(cbRaw);runHandler(cb,[&]{const auto user=requireUser(service,req);auto out=storeAvatarUpload(service,user.id,bodyOrEmpty(req));reply(cb,std::move(out));},"上傳頭像失敗");
  });

  // [功能備註｜DM 玩家頭像管理] DM 可貼網址覆寫任意玩家頭像；若改成外部網址/清空會刪除該玩家舊的資料庫圖片。
  reg1("/api/admin/players/{1}/avatar", drogon::Patch,
       [service](const Request& req, Callback&& cbRaw, const std::string& userIdText) {
    Callback cb=std::move(cbRaw);runHandler(cb,[&]{
      const auto admin=requireUser(service,req,true);
      const auto userId=positiveId(userIdText);
      auto url=strValue(bodyOrEmpty(req)["avatar_url"]);
      if(url.size()>2048)throw ApiError(drogon::k400BadRequest,"頭像網址太長");
      discardStoredAvatarIfExternal(service,userId,url);
      const auto rows=service->database()->execSqlSync(
          "UPDATE character_cards SET avatar_url=$1,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2 RETURNING user_id",
          url,userId);
      if(rows.empty())throw ApiError(drogon::k404NotFound,"該玩家尚未建立角色卡");
      Json::Value audit;audit["text"]="DM 修改玩家頭像";audit["target_user_id"]=Json::Int64(userId);audit["avatar_url"]=url;
      const auto roomRows=service->database()->execSqlSync("SELECT room_id FROM room_members WHERE user_id=$1 ORDER BY joined_at DESC LIMIT 1",userId);
      if(!roomRows.empty())service->addEvent(roomRows[0]["room_id"].as<std::int64_t>(),admin.id,"admin_avatar_update",audit);
      Json::Value out;out["ok"]=true;out["character"]=service->characterByUser(userId);reply(cb,std::move(out));
    },"DM 修改玩家頭像失敗");
  });

  // [功能備註｜DM 玩家頭像上傳｜POST /api/admin/players/{1}/avatar/upload] DM 可直接替任意玩家上傳圖片。
  reg1("/api/admin/players/{1}/avatar/upload", drogon::Post,
       [service](const Request& req, Callback&& cbRaw, const std::string& userIdText) {
    Callback cb=std::move(cbRaw);runHandler(cb,[&]{
      const auto admin=requireUser(service,req,true);const auto userId=positiveId(userIdText);
      auto out=storeAvatarUpload(service,userId,bodyOrEmpty(req));
      Json::Value audit;audit["text"]="DM 上傳玩家頭像";audit["target_user_id"]=Json::Int64(userId);audit["avatar_url"]=out["avatar_url"];
      const auto roomRows=service->database()->execSqlSync("SELECT room_id FROM room_members WHERE user_id=$1 ORDER BY joined_at DESC LIMIT 1",userId);
      if(!roomRows.empty())service->addEvent(roomRows[0]["room_id"].as<std::int64_t>(),admin.id,"admin_avatar_upload",audit);
      reply(cb,std::move(out));
    },"DM 上傳玩家頭像失敗");
  });

  // [功能備註｜頭像圖片讀取｜GET /api/avatar-images/{1}] <img> 無法附 Bearer token，因此只公開回傳該玩家目前的頭像圖片內容，不回傳其他玩家資料。
  reg1("/api/avatar-images/{1}", drogon::Get,
       [service](const Request&, Callback&& cbRaw, const std::string& userIdText) {
    Callback cb=std::move(cbRaw);
    try {
      const auto userId=positiveId(userIdText);
      const auto rows=service->database()->execSqlSync(
          "SELECT mime_type,image_base64 FROM avatar_images WHERE user_id=$1",userId);
      if(rows.empty()){fail(cb,drogon::k404NotFound,"找不到頭像圖片");return;}
      const auto mime=rows[0]["mime_type"].as<std::string>();
      const auto bytes=decodeBase64(rows[0]["image_base64"].as<std::string>());
      auto response=drogon::HttpResponse::newHttpResponse();
      response->setStatusCode(drogon::k200OK);
      response->addHeader("Content-Type",mime);
      response->addHeader("Cache-Control","no-store, max-age=0");
      response->addHeader("Content-Disposition","inline");
      response->setBody(std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
      cb(response);
    } catch(const ApiError& error){fail(cb,error.status,error.what());}
      catch(const std::exception& error){LOG_ERROR<<"avatar image read error: "<<error.what();fail(cb,drogon::k500InternalServerError,"讀取頭像失敗");}
  });


  // [功能備註｜NPC 頭像上傳｜POST /api/admin/npc-templates/{1}/avatar/upload] DM 直接選 JPG/PNG/WebP。
  reg1("/api/admin/npc-templates/{1}/avatar/upload",drogon::Post,[service](const Request&req,Callback&&cbRaw,const std::string&idText){Callback cb=std::move(cbRaw);runHandler(cb,[&]{requireUser(service,req,true);auto out=storeNpcAvatarUpload(service,positiveId(idText),bodyOrEmpty(req));reply(cb,std::move(out));},"上傳 NPC 頭像失敗");});
  // [功能備註｜NPC 頭像網址｜PATCH /api/admin/npc-templates/{1}/avatar] 保留貼網址；切到外部網址會清掉舊 DB 圖片。
  reg1("/api/admin/npc-templates/{1}/avatar",drogon::Patch,[service](const Request&req,Callback&&cbRaw,const std::string&idText){Callback cb=std::move(cbRaw);runHandler(cb,[&]{requireUser(service,req,true);const auto id=positiveId(idText);auto url=strValue(bodyOrEmpty(req)["image_url"]);if(url.size()>2048)throw ApiError(drogon::k400BadRequest,"頭像網址太長");discardStoredNpcAvatarIfExternal(service,id,url);const auto rows=service->database()->execSqlSync("UPDATE npc_templates SET image_url=$1,updated_at=CURRENT_TIMESTAMP WHERE id=$2 RETURNING id",url,id);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到 NPC");Json::Value out;out["ok"]=true;out["image_url"]=url;reply(cb,std::move(out));},"修改 NPC 頭像失敗");});
  // [功能備註｜NPC 頭像圖片讀取｜GET /api/npc-avatar-images/{1}] 圖片路由供 <img> 直接顯示。
  reg1("/api/npc-avatar-images/{1}",drogon::Get,[service](const Request&,Callback&&cbRaw,const std::string&idText){Callback cb=std::move(cbRaw);try{const auto id=positiveId(idText);const auto rows=service->database()->execSqlSync("SELECT mime_type,image_base64 FROM npc_avatar_images WHERE npc_template_id=$1",id);if(rows.empty()){fail(cb,drogon::k404NotFound,"找不到 NPC 頭像");return;}const auto mime=rows[0]["mime_type"].as<std::string>();const auto bytes=decodeBase64(rows[0]["image_base64"].as<std::string>());auto response=drogon::HttpResponse::newHttpResponse();response->setStatusCode(drogon::k200OK);response->addHeader("Content-Type",mime);response->addHeader("Cache-Control","no-store, max-age=0");response->setBody(std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size()));cb(response);}catch(const ApiError&e){fail(cb,e.status,e.what());}catch(const std::exception&e){LOG_ERROR<<"npc avatar read error: "<<e.what();fail(cb,drogon::k500InternalServerError,"讀取 NPC 頭像失敗");}});

  // [功能備註｜程序場景地圖] 不依賴外部 AI，依主題自動生成節點、描述與雙向連線。
  reg1("/api/admin/rooms/{1}/map/generate", drogon::Post,
       [service](const Request& req, Callback&& cbRaw, const std::string& roomText) {
    Callback cb=std::move(cbRaw);runHandler(cb,[&]{requireUser(service,req,true);const auto roomId=positiveId(roomText);const auto b=bodyOrEmpty(req);const auto count=std::clamp<std::int64_t>(intValue(b["count"],8),3,40);const auto theme=strValue(b["theme"],"未知區域");const bool replace=b.get("replace",false).asBool();if(replace){service->database()->execSqlSync("UPDATE rooms SET current_map_node_id=NULL WHERE id=$1",roomId);service->database()->execSqlSync("DELETE FROM room_map_nodes WHERE room_id=$1 AND auto_generated=TRUE",roomId);}std::vector<std::int64_t> ids;const std::array<const char*,12> labels={"入口","前廳","長廊","側室","中庭","階梯間","封閉室","觀測間","倉庫","交叉口","深層區","核心區"};for(std::int64_t i=0;i<count;++i){const auto name=theme+"・"+labels[static_cast<std::size_t>(i)%labels.size()]+(i>=static_cast<std::int64_t>(labels.size())?std::to_string(i+1):"");const auto desc="自動生成場景："+theme+"。此區域可由 DM 直接修改名稱、描述、通訊限制與連線。";const auto rows=service->database()->execSqlSync("INSERT INTO room_map_nodes(room_id,name,description,connections,auto_generated) VALUES($1,$2,$3,'[]'::jsonb,TRUE) RETURNING id",roomId,name,desc);ids.push_back(rows[0]["id"].as<std::int64_t>());}for(std::size_t i=0;i<ids.size();++i){Json::Value links{Json::arrayValue};if(i>0)links.append(Json::Int64(ids[i-1]));if(i+1<ids.size())links.append(Json::Int64(ids[i+1]));if(i>1&&i%3==0)links.append(Json::Int64(ids[i-2]));service->database()->execSqlSync("UPDATE room_map_nodes SET connections=$1::jsonb WHERE id=$2",compactJson(links),ids[i]);}if(!ids.empty())service->database()->execSqlSync("UPDATE rooms SET current_map_node_id=COALESCE(current_map_node_id,$1),updated_at=CURRENT_TIMESTAMP WHERE id=$2",ids.front(),roomId);Json::Value out;out["ok"]=true;out["generated"]=Json::Int64(ids.size());out["node_ids"]=Json::arrayValue;for(auto id:ids)out["node_ids"].append(Json::Int64(id));reply(cb,std::move(out));},"生成場景地圖失敗");
  });

  // [功能備註｜玩家位置] 無隊伍玩家可自行在已存在節點移動；有隊伍時由隊伍位置接管。
  reg1("/api/rooms/{1}/location", drogon::Patch,
       [service](const Request& req, Callback&& cbRaw, const std::string& roomText) {
    Callback cb=std::move(cbRaw);runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomText);requireRoom(service,user,roomId);const auto nodeId=intValue(bodyOrEmpty(req)["node_id"]);if(nodeId<=0)throw ApiError(drogon::k400BadRequest,"請指定地點");if(!service->database()->execSqlSync("SELECT 1 FROM room_team_members WHERE room_id=$1 AND user_id=$2",roomId,user.id).empty()&&!user.isAdmin)throw ApiError(drogon::k400BadRequest,"你目前有隊伍，位置由隊長帶隊移動");const auto targetNodeRows=service->database()->execSqlSync("SELECT 1 FROM room_map_nodes WHERE id=$1 AND room_id=$2 AND hidden=FALSE",nodeId,roomId);if(targetNodeRows.empty())throw ApiError(drogon::k404NotFound,"找不到可前往地點");Json::Value ev=ruleMapNodePayload(service,roomId,nodeId);ev["text"]="玩家移動到新的場景";auto attempt=evaluateRuleEvent(service,roomId,user.id,"PLAYER_MOVE_ATTEMPT",ev,false);if(attempt["blocked"].asBool())throw ApiError(drogon::k403Forbidden,"前往此地點的行動被規則阻止");service->database()->execSqlSync("UPDATE room_members SET current_map_node_id=$1 WHERE room_id=$2 AND user_id=$3",nodeId,roomId,user.id);service->addEvent(roomId,user.id,"player_move",ev);evaluateRuleEvent(service,roomId,user.id,"PLAYER_MOVED",ev,false);Json::Value out;out["ok"]=true;out["node_id"]=Json::Int64(nodeId);reply(cb,std::move(out));},"移動失敗");
  });

  // [功能備註｜通訊規則] 回傳目前場景、禁訊區與跨場景通訊道具狀態。
  reg1("/api/rooms/{1}/communication", drogon::Get,
       [service](const Request& req, Callback&& cbRaw, const std::string& roomText) {
    Callback cb=std::move(cbRaw);runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomText);requireRoom(service,user,roomId);Json::Value out;const auto node=effectiveNode(service->database(),roomId,user.id);if(node) out["node_id"]=Json::Int64(*node); else out["node_id"]=Json::nullValue;out["communication_device"]=hasCommunicationDevice(service,user.id);out["remote_blocked"]=node&&nodeBlocksRemote(service->database(),*node);out["audience"]=Json::arrayValue;for(auto id:communicationAudience(service,roomId,user.id))out["audience"].append(Json::Int64(id));reply(cb,std::move(out));},"讀取通訊狀態失敗");
  });

  // [功能備註｜隊伍邀請] 在線玩家可回應；離線玩家依需求自動接受。
  reg1("/api/rooms/{1}/team-invites", drogon::Get,
       [service](const Request& req, Callback&& cbRaw, const std::string& roomText) {
    Callback cb=std::move(cbRaw);runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomText);requireRoom(service,user,roomId);const auto rows=service->database()->execSqlSync("SELECT row_to_json(x)::text AS json_text FROM (SELECT i.*,t.name AS team_name,u.username AS invited_by_username FROM room_team_invites i JOIN room_teams t ON t.id=i.team_id LEFT JOIN users u ON u.id=i.invited_by_user_id WHERE i.room_id=$1 AND i.target_user_id=$2 AND i.status='pending' ORDER BY i.created_at DESC) x",roomId,user.id);Json::Value out;out["invites"]=Json::arrayValue;for(std::size_t i=0;i<rows.size();++i)out["invites"].append(rowJson(rows,i));reply(cb,std::move(out));},"讀取邀請失敗");
  });
  reg1("/api/rooms/{1}/team-invites", drogon::Post,
       [service](const Request& req, Callback&& cbRaw, const std::string& roomText) {
    Callback cb=std::move(cbRaw);runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomText);requireRoom(service,user,roomId);const auto b=bodyOrEmpty(req);const auto teamId=intValue(b["team_id"]);const auto target=intValue(b["target_user_id"]);if(teamId<=0||target<=0)throw ApiError(drogon::k400BadRequest,"請指定隊伍與玩家");const auto team=service->database()->execSqlSync("SELECT leader_user_id FROM room_teams WHERE id=$1 AND room_id=$2",teamId,roomId);if(team.empty())throw ApiError(drogon::k404NotFound,"找不到隊伍");if(!user.isAdmin&&team[0]["leader_user_id"].as<std::int64_t>()!=user.id)throw ApiError(drogon::k403Forbidden,"只有隊長或 DM 可邀請");if(service->database()->execSqlSync("SELECT 1 FROM room_members WHERE room_id=$1 AND user_id=$2",roomId,target).empty())throw ApiError(drogon::k404NotFound,"玩家不在房間");const auto rows=service->database()->execSqlSync("INSERT INTO room_team_invites(room_id,team_id,target_user_id,invited_by_user_id,status) VALUES($1,$2,$3,$4,'pending') ON CONFLICT(room_id,team_id,target_user_id) WHERE status='pending' DO UPDATE SET invited_by_user_id=EXCLUDED.invited_by_user_id,created_at=CURRENT_TIMESTAMP RETURNING id",roomId,teamId,target,user.id);const auto inviteId=rows[0]["id"].as<std::int64_t>();const bool offline=!RoomSocket::isUserOnline(target,roomId);if(offline){acceptInvite(service,inviteId,roomId,teamId,target,"offline_auto_accept");Json::Value ev;ev["text"]="離線玩家自動接受隊伍邀請";ev["team_id"]=Json::Int64(teamId);service->addEvent(roomId,target,"offline_ai_join",ev);evaluateRuleEvent(service,roomId,target,"TEAM_JOINED",ev,false);}Json::Value out;out["ok"]=true;out["invite_id"]=Json::Int64(inviteId);out["auto_accepted"]=offline;reply(cb,std::move(out));},"發送隊伍邀請失敗");
  });
  reg2("/api/rooms/{1}/team-invites/{2}/respond", drogon::Post,
       [service](const Request& req, Callback&& cbRaw, const std::string& roomText,const std::string& inviteText) {
    Callback cb=std::move(cbRaw);runHandler(cb,[&]{const auto user=requireUser(service,req);const auto roomId=positiveId(roomText),inviteId=positiveId(inviteText);requireRoom(service,user,roomId);const auto rows=service->database()->execSqlSync("SELECT team_id,status FROM room_team_invites WHERE id=$1 AND room_id=$2 AND target_user_id=$3",inviteId,roomId,user.id);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到邀請");if(rows[0]["status"].as<std::string>()!="pending")throw ApiError(drogon::k400BadRequest,"邀請已處理");const auto accepted=bodyOrEmpty(req).get("accept",true).asBool();if(accepted){const auto teamId=rows[0]["team_id"].as<std::int64_t>();acceptInvite(service,inviteId,roomId,teamId,user.id,"player");Json::Value ruleEv;ruleEv["team_id"]=Json::Int64(teamId);ruleEv["invite_id"]=Json::Int64(inviteId);evaluateRuleEvent(service,roomId,user.id,"TEAM_JOINED",ruleEv,false);}else service->database()->execSqlSync("UPDATE room_team_invites SET status='declined',responded_at=CURRENT_TIMESTAMP,response_source='player' WHERE id=$1",inviteId);Json::Value out;out["ok"]=true;out["accepted"]=accepted;reply(cb,std::move(out));},"回應邀請失敗");
  });

  // [功能備註｜離線 AI｜DM] 查看／設定玩家離線接管，或手動執行一次。
  reg1("/api/admin/rooms/{1}/offline-ai", drogon::Get,
       [service](const Request& req, Callback&& cbRaw,const std::string& roomText){Callback cb=std::move(cbRaw);runHandler(cb,[&]{requireUser(service,req,true);const auto roomId=positiveId(roomText);const auto rows=service->database()->execSqlSync("SELECT jsonb_build_object('user_id',rm.user_id,'username',u.username,'display_name',rm.display_name,'enabled',rm.offline_ai_enabled,'active',rm.offline_ai_active,'last_seen_at',rm.last_seen_at,'ai_last_action_at',rm.ai_last_action_at,'current_map_node_id',rm.current_map_node_id)::text AS json_text FROM room_members rm JOIN users u ON u.id=rm.user_id WHERE rm.room_id=$1 AND rm.role='player' ORDER BY rm.joined_at",roomId);Json::Value out;out["players"]=Json::arrayValue;for(std::size_t i=0;i<rows.size();++i)out["players"].append(rowJson(rows,i));reply(cb,std::move(out));},"讀取離線 AI 失敗");});
  reg2("/api/admin/rooms/{1}/offline-ai/{2}", drogon::Patch,
       [service](const Request& req, Callback&& cbRaw,const std::string& roomText,const std::string& userText){Callback cb=std::move(cbRaw);runHandler(cb,[&]{requireUser(service,req,true);const auto roomId=positiveId(roomText),uid=positiveId(userText);const auto enabled=bodyOrEmpty(req).get("enabled",true).asBool();const auto rows=service->database()->execSqlSync("UPDATE room_members SET offline_ai_enabled=$1,offline_ai_active=CASE WHEN $1 THEN offline_ai_active ELSE FALSE END WHERE room_id=$2 AND user_id=$3 RETURNING user_id",enabled,roomId,uid);if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到玩家");Json::Value out;out["ok"]=true;out["enabled"]=enabled;reply(cb,std::move(out));},"設定離線 AI 失敗");});
  reg1("/api/admin/rooms/{1}/offline-ai/tick", drogon::Post,
       [service](const Request& req, Callback&& cbRaw,const std::string& roomText){Callback cb=std::move(cbRaw);runHandler(cb,[&]{requireUser(service,req,true);auto out=runOfflineAiTick(service,positiveId(roomText));reply(cb,std::move(out));},"執行離線 AI 失敗");});

  // [功能備註｜元素自生] 玩家可查目前自生速度；DM 可手動觸發一次全站結算。
  reg0("/api/me/elements/generation", drogon::Get,
       [service](const Request& req, Callback&& cbRaw){Callback cb=std::move(cbRaw);runHandler(cb,[&]{const auto user=requireUser(service,req);const auto character=service->characterByUser(user.id);Json::Value out;out["rates"]=generationRates(service,character);out["storage"]=character["element_storage"];out["caps"]=character["element_storage_caps"];reply(cb,std::move(out));},"讀取元素自生失敗");});
  reg0("/api/admin/elements/generation/tick", drogon::Post,
       [service](const Request& req, Callback&& cbRaw){Callback cb=std::move(cbRaw);runHandler(cb,[&]{requireUser(service,req,true);auto out=runElementGenerationTick(service);reply(cb,std::move(out));},"結算元素自生失敗");});
}

void startWorldAutomation(const std::shared_ptr<CoreService>& service) {
  static std::jthread worker;
  if (worker.joinable()) return;
  std::weak_ptr<CoreService> weak = service;
  worker = std::jthread([weak](std::stop_token stop) {
    using namespace std::chrono_literals;
    while (!stop.stop_requested()) {
      for (int i=0;i<30 && !stop.stop_requested();++i) std::this_thread::sleep_for(1s);
      if (stop.stop_requested()) break;
      if (auto locked = weak.lock()) {
        try { runOfflineAiTick(locked); } catch (const std::exception& e) { LOG_ERROR << "offline AI tick failed: " << e.what(); }
        try { runElementGenerationTick(locked); } catch (const std::exception& e) { LOG_ERROR << "element generation tick failed: " << e.what(); }
        try { runRuleEngineTick(locked); } catch (const std::exception& e) { LOG_ERROR << "rule engine tick failed: " << e.what(); }
      }
    }
  });
}

}  // namespace trpg
