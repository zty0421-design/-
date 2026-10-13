#pragma once

#include <json/json.h>

#include <cstdint>
#include <memory>
#include <string>

namespace trpg {

class CoreService;

// [功能備註｜62-cpp.23 事件型技能 migration]
// 在 legacy schema 套用後補齊 status_templates.triggers 與通用戰鬥狀態實例表。
void applyEventSkillMigrations(const std::shared_ptr<CoreService>& service);

// [功能備註｜62-cpp.23 事件型技能核心]
// 將玩家技能、怪物被動、大道技能與戰鬥狀態統一成 Trigger -> Condition -> Effect -> Status。
// eventType 使用既有 COMBAT_* 事件；trigger 亦接受 ON_HIT / ON_DAMAGE_DEALT 等友善別名。
Json::Value dispatchEventSkillEvent(const std::shared_ptr<CoreService>& service,
                                    std::int64_t roomId,
                                    const std::string& actorType,
                                    std::int64_t actorId,
                                    const std::string& eventType,
                                    const Json::Value& context,
                                    Json::Value* procState = nullptr);

// [功能備註｜62-cpp.23 多段攻擊傷害修正]
// 讀取攻擊者／目標身上的泛型戰鬥狀態，套用每層傷害增減。
// 例如【畫像標記•攻】使用 damage_taken_from_source_percent_per_stack=10，
// 只提高「施加標記者」對該目標造成的傷害，不會讓其他角色一起吃到增傷。
std::int64_t applyEventSkillDamageModifiers(const std::shared_ptr<CoreService>& service,
                                            std::int64_t roomId,
                                            const std::string& attackerType,
                                            std::int64_t attackerId,
                                            const std::string& targetType,
                                            std::int64_t targetId,
                                            std::int64_t baseDamage,
                                            Json::Value* breakdown = nullptr);

// [功能備註｜62-cpp.23 治療效率]
// 治療先依「目前已有」的狀態計算，再送 ON_HEAL_RECEIVED；因此本次治療觸發的新層數
// 不會反向放大同一次治療，避免事件循環與判定順序歧義。
std::int64_t applyEventSkillHealingModifiers(const std::shared_ptr<CoreService>& service,
                                             std::int64_t roomId,
                                             const std::string& healerType,
                                             std::int64_t healerId,
                                             const std::string& targetType,
                                             std::int64_t targetId,
                                             std::int64_t baseHealing,
                                             Json::Value* breakdown = nullptr);

// [功能備註｜62-cpp.23.1 場地技能]
// 讀取目前戰鬥中的場地效果，會提供給房間快照與前端戰鬥面板。
Json::Value activeCombatFieldEffects(const CoreService& service,
                                     std::int64_t roomId);

// [功能備註｜62-cpp.23.1 戰鬥結束場地清理]
// 持續到戰鬥結束的 FieldEffect 在戰鬥結束時統一失效。
void clearCombatFieldEffects(const std::shared_ptr<CoreService>& service,
                             std::int64_t roomId);

// [功能備註｜62-cpp.23 狀態回合]
// 每進入新戰鬥回合時遞減有期限的 combat_status_instances；0 回合代表持續型狀態。
// 62-cpp.23.1 同時遞減有期限的 combat_field_effects。
void tickEventSkillStatuses(const std::shared_ptr<CoreService>& service,
                            std::int64_t roomId,
                            std::int64_t round);

// [功能備註｜63-cpp.24 儀式效果共用技能核心]
// 讓儀式成功／失敗效果直接使用既有 Effect 執行器（狀態、傷害、治療、場地、事件等），避免另寫一套效果語言。
Json::Value applyConfiguredEventSkillEffects(const std::shared_ptr<CoreService>& service,
                                             std::int64_t roomId,
                                             const std::string& actorType,
                                             std::int64_t actorId,
                                             const Json::Value& effects,
                                             const Json::Value& context = Json::Value{Json::objectValue});

}  // namespace trpg
