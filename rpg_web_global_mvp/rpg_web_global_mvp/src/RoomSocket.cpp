#include "trpg/RoomSocket.h"

#include "trpg/CoreService.h"
#include "trpg/WorldSystems.h"
#include "trpg/RuleEngine.h"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace trpg {
namespace {

Json::Value parseMessage(const std::string& source) {
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value value;
  std::string errors;
  if (!reader->parse(source.data(), source.data() + source.size(), &value, &errors) ||
      !value.isObject()) {
    return Json::nullValue;
  }
  return value;
}

std::string compactJson(const Json::Value& value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value);
}

std::optional<std::int64_t> positiveId(const Json::Value& value) {
  try {
    std::int64_t id = 0;
    if (value.isInt64()) id = value.asInt64();
    else if (value.isUInt64() &&
             value.asUInt64() <=
                 static_cast<Json::UInt64>(std::numeric_limits<std::int64_t>::max()))
      id = static_cast<std::int64_t>(value.asUInt64());
    else if (value.isString()) {
      std::size_t used = 0;
      id = std::stoll(value.asString(), &used, 10);
      if (used != value.asString().size()) return std::nullopt;
    }
    return id > 0 ? std::optional<std::int64_t>{id} : std::nullopt;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::mutex snapshotDebounceMutex;
std::set<std::int64_t> pendingSnapshotRooms;
std::map<std::int64_t, std::set<std::int64_t>> presenceCache;

std::uint32_t uniformRandom(std::uint32_t upperExclusive) {
  if (upperExclusive == 0) return 0;
  const auto limit = std::numeric_limits<std::uint32_t>::max() -
                     (std::numeric_limits<std::uint32_t>::max() % upperExclusive);
  std::uint32_t value = 0;
  do {
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&value), sizeof(value)) != 1) {
      throw std::runtime_error("secure random generation failed");
    }
  } while (value >= limit);
  return value % upperExclusive;
}

}  // namespace

std::mutex RoomSocket::mutex_;
std::map<const drogon::WebSocketConnection*, RoomSocket::Session>
    RoomSocket::sessions_;
std::map<std::int64_t, std::vector<std::weak_ptr<drogon::WebSocketConnection>>>
    RoomSocket::roomConnections_;

void RoomSocket::sendEvent(const drogon::WebSocketConnectionPtr& connection,
                           const std::string& event, const Json::Value& data) {
  Json::Value envelope;
  envelope["event"] = event;
  envelope["data"] = data;
  connection->send(compactJson(envelope));
}

void RoomSocket::sendError(const drogon::WebSocketConnectionPtr& connection,
                           const std::string& message) {
  sendEvent(connection, "error:message", Json::Value{message});
}

void RoomSocket::handleNewConnection(
    const drogon::HttpRequestPtr& request,
    const drogon::WebSocketConnectionPtr& connection) {
  const auto service = CoreService::instance();
  std::optional<JwtClaims> claims;
  if (service) {
    const auto ticket = request->getParameter("ticket");
    if (!ticket.empty()) {
      try {
        // 一次性票證可避免 Render／反向代理在 WebSocket URL 中處理長 JWT 時造成握手不穩。
        const auto rows = service->database()->execSqlSync(
            "WITH taken AS ("
            " DELETE FROM realtime_tickets "
            " WHERE ticket=$1 AND expires_at>CURRENT_TIMESTAMP "
            " RETURNING user_id"
            ") "
            "SELECT u.id,u.username,u.is_admin FROM users u "
            "JOIN taken t ON t.user_id=u.id",
            ticket);
        if (!rows.empty()) {
          claims = JwtClaims{rows[0]["id"].as<std::int64_t>(),
                             rows[0]["username"].as<std::string>(),
                             rows[0]["is_admin"].as<bool>(), 0, 0};
        }
      } catch (const std::exception& error) {
        LOG_WARN << "websocket ticket validation failed: " << error.what();
      }
    }
    // 舊客戶端相容：如果沒有票證，仍接受原本的 token query。
    if (!claims) {
      const auto token = request->getParameter("token");
      if (!token.empty()) claims = service->authenticateToken(token);
    }
  }
  if (!claims) {
    LOG_WARN << "websocket handshake rejected: invalid/expired credentials";
    sendError(connection, "即時連線驗證失敗，請重新登入");
    connection->forceClose();
    return;
  }
  {
    std::lock_guard lock(mutex_);
    sessions_[connection.get()] = Session{*claims, std::nullopt};
  }
  Json::Value connected;
  connected["user_id"] = Json::Int64(claims->id);
  sendEvent(connection, "connect", connected);
}

void RoomSocket::handleNewMessage(
    const drogon::WebSocketConnectionPtr& connection, std::string&& message,
    const drogon::WebSocketMessageType& type) {
  if (type != drogon::WebSocketMessageType::Text) return;
  const auto service = CoreService::instance();
  if (!service) {
    sendError(connection, "伺服器尚未完成初始化");
    return;
  }
  const auto envelope = parseMessage(message);
  if (envelope.isNull() || !envelope["event"].isString()) {
    sendError(connection, "即時訊息格式錯誤");
    return;
  }
  const auto event = envelope["event"].asString();
  const auto data = envelope.get("data", Json::Value{Json::objectValue});

  Session session;
  {
    std::lock_guard lock(mutex_);
    const auto found = sessions_.find(connection.get());
    if (found == sessions_.end()) {
      sendError(connection, "即時連線已失效");
      return;
    }
    session = found->second;
  }

  try {
    // 輕量應用層心跳：不查資料庫，讓 Render／反向代理上的閒置 WebSocket 保持活性並提供延遲測量。
    if (event == "client:ping") {
      Json::Value pong{Json::objectValue};
      pong["seq"] = data.get("seq", 0);
      pong["client_ts"] = data.get("client_ts", Json::Int64(0));
      sendEvent(connection, "server:pong", pong);
      return;
    }

    // [WebSocket功能備註｜room:enter] 玩家進入房間後加入同步群組，並更新在線名單／房間快照。
    if (event == "room:enter") {
      const auto roomId = positiveId(data["roomId"]);
      if (!roomId) {
        sendError(connection, "房間編號無效");
        return;
      }
      // 同一 session 重複 room:enter 直接返回，連權限 DB 查詢都不重做。
      if (session.roomId && *session.roomId == *roomId) return;
      if (!service->userCanAccessRoom(session.claims.id, *roomId)) {
        sendError(connection, "你不能進入這個房間");
        return;
      }
      std::optional<std::int64_t> oldRoom;
      bool sameRoom = false;
      {
        std::lock_guard lock(mutex_);
        auto& stored = sessions_[connection.get()];
        oldRoom = stored.roomId;
        sameRoom = oldRoom && *oldRoom == *roomId;
        if (!sameRoom && oldRoom) {
          auto oldIt = roomConnections_.find(*oldRoom);
          if (oldIt != roomConnections_.end()) {
            auto& oldConnections = oldIt->second;
            oldConnections.erase(
                std::remove_if(oldConnections.begin(), oldConnections.end(), [&](const auto& weak) {
                  const auto current = weak.lock();
                  return !current || current.get() == connection.get();
                }),
                oldConnections.end());
            if (oldConnections.empty()) roomConnections_.erase(oldIt);
          }
        }
        stored.roomId = *roomId;
        auto& connections = roomConnections_[*roomId];
        const bool alreadyTracked = std::any_of(
            connections.begin(), connections.end(), [&](const auto& weak) {
              const auto current = weak.lock();
              return current && current.get() == connection.get();
            });
        if (!alreadyTracked) connections.emplace_back(connection);
      }
      // 同一條 WebSocket 重複送 room:enter 時直接視為冪等操作。
      // 這可避免重複觸發 PLAYER_ROOM_JOINED、DB last_seen 更新與完整快照。
      if (sameRoom) return;
      if (oldRoom && *oldRoom != *roomId) broadcastPresence(*oldRoom);
      broadcastPresence(*roomId);
      service->database()->execSqlSync(
          "UPDATE room_members SET offline_ai_active=FALSE,last_seen_at=CURRENT_TIMESTAMP WHERE room_id=$1 AND user_id=$2",
          *roomId, session.claims.id);
      Json::Value joinRuleEvent; joinRuleEvent["room_id"] = Json::Int64(*roomId);
      evaluateRuleEvent(service, *roomId, session.claims.id, "PLAYER_ROOM_JOINED", joinRuleEvent, false);
      sendEvent(connection, "room:snapshot", roomSnapshotForViewer(service, *roomId, session.claims.id));
      return;
    }

    // [WebSocket功能備註｜64-cpp.25.4 舊 chat:send 相容] 舊客戶端的 chat 一律視為「普通音量角色說話」，不再存在場外聊天語義。
    if (event == "chat:send") {
      const auto roomId = positiveId(data["roomId"]);
      if (!roomId || !session.roomId || *roomId != *session.roomId ||
          !service->userCanAccessRoom(session.claims.id, *roomId)) {
        sendError(connection, "請先進入房間");
        return;
      }
      auto text = data["text"].isString() ? data["text"].asString() : std::string{};
      const auto first = text.find_first_not_of(" \t\r\n");
      const auto last = text.find_last_not_of(" \t\r\n");
      text = first == std::string::npos ? std::string{} : text.substr(first, last - first + 1);
      if (text.empty()) return;
      if (text.size() > 1000) text.resize(1000);
      Json::Value payload;
      payload["text"] = text;
      payload["action"] = text;
      payload["interaction_type"] = "speech";
      payload["volume"] = "normal";
      payload["noise"] = Json::Int64(10);
      payload["sender_node_id"] = Json::nullValue;
      const auto audience = communicationAudience(service, *roomId, session.claims.id);
      payload["audience_user_ids"] = Json::arrayValue;
      for (const auto userId : audience) payload["audience_user_ids"].append(Json::Int64(userId));
      payload["communication_mode"] = audience.size() > 1 ? "scene_or_device" : "self_only";
      auto speechRule = evaluateRuleEvent(service, *roomId, session.claims.id, "CHARACTER_SPOKE", payload, false);
      if (speechRule["blocked"].asBool()) { sendError(connection, "這句話被規則阻止"); return; }
      auto soundRule = evaluateRuleEvent(service, *roomId, session.claims.id, "SOUND_EMITTED", payload, false);
      if (soundRule["blocked"].asBool()) { sendError(connection, "聲音被規則阻止"); return; }
      service->addEvent(*roomId, session.claims.id, "speech", payload);
      broadcastSnapshotForRoom(*roomId);
      return;
    }

    // [WebSocket功能備註｜dice:roll] 多人房間擲骰；解析骰式、記錄事件並同步結果。
    if (event == "dice:roll") {
      const auto roomId = positiveId(data["roomId"]);
      if (!roomId || !session.roomId || *roomId != *session.roomId ||
          !service->userCanAccessRoom(session.claims.id, *roomId)) {
        sendError(connection, "請先進入房間");
        return;
      }
      auto notation = data["notation"].isString() ? data["notation"].asString() : "1d20";
      static const std::regex pattern{R"(^([0-9]{1,2})d([0-9]{1,4})([+-][0-9]{1,4})?$)",
                                      std::regex::icase};
      std::smatch match;
      if (!std::regex_match(notation, match, pattern)) {
        sendError(connection, "骰式格式例如 1d20 或 2d6+3");
        return;
      }
      const auto count = std::clamp(std::stoi(match[1].str()), 1, 20);
      const auto sides = std::clamp(std::stoi(match[2].str()), 1, 1000);
      const auto modifier = match[3].matched ? std::stoi(match[3].str()) : 0;
      Json::Value rolls{Json::arrayValue};
      std::int64_t total = modifier;
      for (int index = 0; index < count; ++index) {
        const auto roll = 1 + static_cast<int>(uniformRandom(static_cast<std::uint32_t>(sides)));
        rolls.append(roll);
        total += roll;
      }
      auto label = data["label"].isString() ? data["label"].asString() : "擲骰";
      if (label.size() > 80) label.resize(80);
      Json::Value payload;
      payload["notation"] = notation;
      payload["label"] = label;
      payload["rolls"] = std::move(rolls);
      payload["modifier"] = modifier;
      payload["total"] = Json::Int64(total);
      service->addEvent(*roomId, session.claims.id, "dice", payload);
      broadcastSnapshotForRoom(*roomId);
      return;
    }

    sendError(connection, "這個即時功能尚未移植到 C++ 第一階段");
  } catch (const std::exception& error) {
    LOG_ERROR << "websocket event failed: " << error.what();
    sendError(connection, "即時操作失敗");
  }
}

void RoomSocket::handleConnectionClosed(
    const drogon::WebSocketConnectionPtr& connection) {
  std::optional<std::int64_t> oldRoom;
  std::optional<std::int64_t> oldUser;
  {
    std::lock_guard lock(mutex_);
    const auto found = sessions_.find(connection.get());
    if (found != sessions_.end()) {
      oldRoom = found->second.roomId;
      oldUser = found->second.claims.id;
      sessions_.erase(found);
    }
  }
  if (oldRoom) {
    broadcastPresence(*oldRoom);
    const auto service = CoreService::instance();
    if (service && oldUser) {
      try {
        service->database()->execSqlSync(
            "UPDATE room_members SET last_seen_at=CURRENT_TIMESTAMP WHERE room_id=$1 AND user_id=$2",
            *oldRoom, *oldUser);
      } catch (const std::exception& error) {
        LOG_WARN << "failed to store disconnect time: " << error.what();
      }
    }
  }
}

void RoomSocket::broadcastEvent(std::int64_t roomId, const std::string& event,
                                const Json::Value& data) {
  std::vector<drogon::WebSocketConnectionPtr> recipients;
  {
    std::lock_guard lock(mutex_);
    auto& connections = roomConnections_[roomId];
    connections.erase(
        std::remove_if(connections.begin(), connections.end(), [&](const auto& weak) {
          const auto connection = weak.lock();
          if (!connection) return true;
          const auto session = sessions_.find(connection.get());
          if (session == sessions_.end() || !session->second.roomId ||
              *session->second.roomId != roomId) {
            return true;
          }
          recipients.push_back(connection);
          return false;
        }),
        connections.end());
    if (connections.empty()) roomConnections_.erase(roomId);
  }
  for (const auto& connection : recipients) sendEvent(connection, event, data);
}

void RoomSocket::broadcastPresence(std::int64_t roomId) {
  std::set<std::int64_t> users;
  bool changed = false;
  {
    std::lock_guard lock(mutex_);
    auto roomIt = roomConnections_.find(roomId);
    if (roomIt != roomConnections_.end()) {
      auto& connections = roomIt->second;
      connections.erase(
          std::remove_if(connections.begin(), connections.end(), [&](const auto& weak) {
            const auto connection = weak.lock();
            if (!connection) return true;
            const auto session = sessions_.find(connection.get());
            if (session == sessions_.end() || !session->second.roomId ||
                *session->second.roomId != roomId) return true;
            users.insert(session->second.claims.id);
            return false;
          }),
          connections.end());
      if (connections.empty()) roomConnections_.erase(roomIt);
    }
    const auto cached = presenceCache.find(roomId);
    changed = cached == presenceCache.end() || cached->second != users;
    if (users.empty()) presenceCache.erase(roomId);
    else presenceCache[roomId] = users;
  }
  if (!changed) return;
  Json::Value data;
  data["online"] = Json::arrayValue;
  for (const auto userId : users) data["online"].append(Json::Int64(userId));
  broadcastEvent(roomId, "presence", data);
}

bool RoomSocket::isUserOnline(std::int64_t userId, std::int64_t roomId) {
  std::lock_guard lock(mutex_);
  for (const auto& [_, session] : sessions_) {
    if (session.claims.id != userId) continue;
    if (roomId <= 0 || (session.roomId && *session.roomId == roomId)) return true;
  }
  return false;
}

void RoomSocket::broadcastSnapshotNow(std::int64_t roomId) {
  const auto service = CoreService::instance();
  if (!service) return;
  // 同一玩家可能同時開手機／電腦或多個分頁。viewer snapshot 對同一 user 完全相同，
  // 因此每位 user 只查一次 DB，再送給該 user 的所有 WebSocket。
  std::map<std::int64_t, std::vector<drogon::WebSocketConnectionPtr>> recipientsByUser;
  {
    std::lock_guard lock(mutex_);
    auto& connections = roomConnections_[roomId];
    connections.erase(
        std::remove_if(connections.begin(), connections.end(), [&](const auto& weak) {
          const auto connection = weak.lock();
          if (!connection) return true;
          const auto found = sessions_.find(connection.get());
          if (found == sessions_.end() || !found->second.roomId || *found->second.roomId != roomId) return true;
          recipientsByUser[found->second.claims.id].push_back(connection);
          return false;
        }),
        connections.end());
    if (connections.empty()) roomConnections_.erase(roomId);
  }
  for (const auto& [userId, connections] : recipientsByUser) {
    try {
      const auto snapshot = roomSnapshotForViewer(service, roomId, userId);
      for (const auto& connection : connections) sendEvent(connection, "room:snapshot", snapshot);
    } catch (const std::exception& error) {
      LOG_ERROR << "snapshot broadcast failed for user " << userId << ": " << error.what();
    }
  }
}

void RoomSocket::broadcastSnapshotForRoom(std::int64_t roomId) {
  if (roomId <= 0) return;
  {
    std::lock_guard lock(snapshotDebounceMutex);
    if (!pendingSnapshotRooms.insert(roomId).second) return;
  }
  // 同一個動作可能連續觸發規則、NPC、戰鬥等多次廣播；35ms 內合併為一份 viewer snapshot，降低 DB 與 JSON 壓力。
  drogon::app().getLoop()->runAfter(0.035, [roomId] {
    {
      std::lock_guard lock(snapshotDebounceMutex);
      pendingSnapshotRooms.erase(roomId);
    }
    broadcastSnapshotNow(roomId);
  });
}

}  // namespace trpg
