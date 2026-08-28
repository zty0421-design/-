#pragma once

#include <memory>
#include <cstdint>
#include <string>
#include <json/json.h>

namespace trpg {

class CoreService;

void applyLegacyV39Migrations(const std::shared_ptr<CoreService>& service);
void registerLegacyV39Routes(const std::shared_ptr<CoreService>& service);
Json::Value advanceStructuredTaskStepsForEvent(const std::shared_ptr<CoreService>& service,
                                                std::int64_t roomId,
                                                std::int64_t userId,
                                                const std::string& eventName,
                                                const Json::Value& payload);

}  // namespace trpg
