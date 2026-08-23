#pragma once

#include <memory>

namespace trpg {

class CoreService;

void applyLegacyV39Migrations(const std::shared_ptr<CoreService>& service);
void registerLegacyV39Routes(const std::shared_ptr<CoreService>& service);

}  // namespace trpg
