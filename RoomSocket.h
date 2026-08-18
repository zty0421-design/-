#pragma once

#include "trpg/Security.h"

#include <drogon/WebSocketController.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace trpg {

class RoomSocket : public drogon::WebSocketController<RoomSocket> {
 public:
  void handleNewMessage(const drogon::WebSocketConnectionPtr& connection,
                        std::string&& message,
                        const drogon::WebSocketMessageType& type) override;
  void handleNewConnection(const drogon::HttpRequestPtr& request,
                           const drogon::WebSocketConnectionPtr& connection) override;
  void handleConnectionClosed(
      const drogon::WebSocketConnectionPtr& connection) override;

  static void broadcastSnapshotForRoom(std::int64_t roomId);

  WS_PATH_LIST_BEGIN
  WS_PATH_ADD("/ws");
  WS_PATH_LIST_END

 private:
  struct Session {
    JwtClaims claims;
    std::optional<std::int64_t> roomId;
  };

  static std::mutex mutex_;
  static std::map<const drogon::WebSocketConnection*, Session> sessions_;
  static std::map<std::int64_t,
                  std::vector<std::weak_ptr<drogon::WebSocketConnection>>>
      roomConnections_;

  static void sendEvent(const drogon::WebSocketConnectionPtr& connection,
                        const std::string& event, const Json::Value& data);
  static void sendError(const drogon::WebSocketConnectionPtr& connection,
                        const std::string& message);
  static void broadcastEvent(std::int64_t roomId, const std::string& event,
                             const Json::Value& data);
  static void broadcastPresence(std::int64_t roomId);
};

}  // namespace trpg
