#include "trpg/GearAffixSystem.h"
#include "trpg/CoreService.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <ctime>
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

void fail(Callback& callback, drogon::HttpStatusCode status, const std::string& message) {
  Json::Value body;
  body["error"] = message;
  reply(callback, std::move(body), status);
}

template <typename Fn>
void runHandler(Callback& callback, Fn&& fn, const std::string& fallback = "操作失敗") {
  try {
    fn();
  } catch (const ApiError& error) {
    fail(callback, error.status, error.what());
  } catch (const drogon::orm::DrogonDbException& error) {
    LOG_ERROR << "gear/affix database error: " << error.base().what();
    fail(callback, drogon::k500InternalServerError, fallback);
  } catch (const std::exception& error) {
    LOG_ERROR << "gear/affix request error: " << error.what();
    fail(callback, drogon::k500InternalServerError, fallback);
  }
}

Json::Value bodyOrEmpty(const Request& request) {
  const auto json = request->getJsonObject();
  return json && json->isObject() ? *json : Json::Value{Json::objectValue};
}

std::string str(const Json::Value& value, std::string fallback = {}) {
  if (value.isString()) return value.asString();
  if (value.isBool()) return value.asBool() ? "true" : "false";
  if (value.isInt64()) return std::to_string(value.asInt64());
  if (value.isUInt64()) return std::to_string(value.asUInt64());
  if (value.isInt()) return std::to_string(value.asInt());
  if (value.isUInt()) return std::to_string(value.asUInt());
  return fallback;
}

std::int64_t integer(const Json::Value& value, std::int64_t fallback = 0) {
  try {
    if (value.isInt64()) return value.asInt64();
    if (value.isUInt64()) {
      const auto raw = value.asUInt64();
      return raw <= static_cast<Json::UInt64>(std::numeric_limits<std::int64_t>::max())
                 ? static_cast<std::int64_t>(raw)
                 : fallback;
    }
    if (value.isInt()) return value.asInt();
    if (value.isUInt()) return value.asUInt();
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

std::string trimmed(std::string value) {
  const auto nonSpace = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), nonSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), nonSpace).base(), value.end());
  return value;
}

std::string normalizeRank(std::string rank) {
  rank = trimmed(rank);
  std::transform(rank.begin(), rank.end(), rank.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  if (rank.empty()) rank = "G";
  static const std::array<std::string, 10> ranks = {
      "G", "F", "E", "D", "C", "B", "A", "S", "SS", "SSS"};
  if (std::find(ranks.begin(), ranks.end(), rank) == ranks.end()) {
    throw ApiError(drogon::k400BadRequest, "品階只能是 G/F/E/D/C/B/A/S/SS/SSS");
  }
  return rank;
}

std::string normalizeAffixKind(std::string kind) {
  kind = trimmed(kind);
  std::transform(kind.begin(), kind.end(), kind.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (kind.empty()) kind = "normal";
  if (kind != "normal" && kind != "special") {
    throw ApiError(drogon::k400BadRequest, "詞條類型只能是 normal 或 special");
  }
  return kind;
}

Json::Value objectOrEmpty(const Json::Value& value) {
  return value.isObject() ? value : Json::Value{Json::objectValue};
}

Json::Value arrayOrEmpty(const Json::Value& value) {
  return value.isArray() ? value : Json::Value{Json::arrayValue};
}

double clampedPercent(const Json::Value& value, double fallback = 0.0) {
  const auto raw = value.isNumeric() ? value.asDouble() : fallback;
  return std::clamp(raw, 0.0, 100.0);
}

bool rollPercent(double percent) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_real_distribution<double> dist(0.0, 100.0);
  return dist(rng) < std::clamp(percent, 0.0, 100.0);
}

std::int64_t positiveId(const std::string& text) {
  try {
    std::size_t used = 0;
    const auto id = std::stoll(text, &used);
    if (used != text.size() || id <= 0) throw std::invalid_argument{"id"};
    return id;
  } catch (...) {
    throw ApiError(drogon::k400BadRequest, "ID 無效");
  }
}

UserInfo requireUser(const std::shared_ptr<CoreService>& service,
                     const Request& request, bool admin = false) {
  const auto claims = service->authenticateRequest(request);
  if (!claims) throw ApiError(drogon::k401Unauthorized, "請先登入");
  const auto rows = service->database()->execSqlSync(
      "SELECT id,is_admin FROM users WHERE id=$1", claims->id);
  if (rows.empty()) throw ApiError(drogon::k401Unauthorized, "帳號不存在");
  UserInfo out{rows[0]["id"].as<std::int64_t>(), rows[0]["is_admin"].as<bool>()};
  if (admin && !out.isAdmin) throw ApiError(drogon::k403Forbidden, "需要全站 DM 權限");
  return out;
}

Json::Value rowJson(const drogon::orm::Result& rows, std::size_t index = 0) {
  if (rows.empty() || index >= rows.size()) return Json::Value{};
  if (rows[index]["json_text"].isNull()) return Json::Value{};
  return parseJson(rows[index]["json_text"].as<std::string>());
}

Json::Value listJson(const drogon::orm::Result& rows) {
  Json::Value out{Json::arrayValue};
  for (std::size_t i = 0; i < rows.size(); ++i) {
    auto value = rowJson(rows, i);
    if (!value.isNull()) out.append(std::move(value));
  }
  return out;
}

std::string gearName(const Json::Value& raw) {
  if (raw.isString()) return trimmed(raw.asString());
  if (raw.isObject()) {
    for (const auto* key : {"name", "title", "item_name"}) {
      const auto value = trimmed(str(raw[key]));
      if (!value.empty()) return value;
    }
  }
  return {};
}

Json::Value gearData(const Json::Value& raw, const std::string& type) {
  Json::Value data{Json::objectValue};
  if (raw.isObject()) data = raw;
  data["name"] = gearName(raw);
  data["rank"] = normalizeRank(str(data["rank"], "G"));
  if (type == "weapon") {
    const auto hands = std::clamp<std::int64_t>(integer(data["weapon_hands"], 1), 1, 2);
    data["weapon_hands"] = Json::Int64(hands);
  }
  if (type == "equipment") data["equip_slot_code"] = trimmed(str(data["equip_slot_code"]));
  return data;
}

Json::Value profileFor(const Db& db, const std::string& targetType, const std::string& rank) {
  const auto rows = db->execSqlSync(
      "SELECT row_to_json(x)::text AS json_text FROM ("
      "SELECT * FROM reforge_profiles WHERE active=TRUE AND rank=$1 "
      "AND target_type IN ($2,'*') ORDER BY CASE WHEN target_type=$2 THEN 0 ELSE 1 END,id LIMIT 1) x",
      rank, targetType);
  return rowJson(rows);
}

std::optional<std::int64_t> poolFor(const Db& db, const Json::Value& profile,
                                    const std::string& targetType, const std::string& rank,
                                    const Json::Value& data) {
  const auto explicitPool = integer(data["affix_pool_id"]);
  if (explicitPool > 0) return explicitPool;
  const auto profilePool = integer(profile["pool_id"]);
  if (profilePool > 0) return profilePool;
  const auto rows = db->execSqlSync(
      "SELECT id FROM affix_pools WHERE active=TRUE AND rank=$1 AND target_type IN ($2,'*') "
      "ORDER BY CASE WHEN target_type=$2 THEN 0 ELSE 1 END,id LIMIT 1",
      rank, targetType);
  if (rows.empty()) return std::nullopt;
  return rows[0]["id"].as<std::int64_t>();
}

struct PoolEntry {
  std::int64_t id{};
  double weight{};
  std::string rank;
  std::string kind;
};

std::vector<PoolEntry> poolEntriesDetailed(
    const Db& db, std::int64_t poolId, const std::set<std::int64_t>& excluded = {}) {
  const auto rows = db->execSqlSync(
      "SELECT e.affix_id,e.weight,a.rank,a.affix_kind FROM affix_pool_entries e "
      "JOIN affix_definitions a ON a.id=e.affix_id "
      "WHERE e.pool_id=$1 AND e.active=TRUE AND a.active=TRUE AND e.weight>0 ORDER BY e.affix_id",
      poolId);
  std::vector<PoolEntry> out;
  for (const auto& row : rows) {
    const auto id = row["affix_id"].as<std::int64_t>();
    if (excluded.count(id)) continue;
    out.push_back(PoolEntry{id, row["weight"].as<double>(), row["rank"].as<std::string>(),
                            row["affix_kind"].as<std::string>()});
  }
  return out;
}

std::int64_t weightedPick(const std::vector<std::pair<std::int64_t, double>>& entries) {
  double total = 0.0;
  for (const auto& [id, weight] : entries) {
    (void)id;
    total += std::max(0.0, weight);
  }
  if (total <= 0.0) throw ApiError(drogon::k400BadRequest, "目前詞條池沒有可抽取的詞條");
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_real_distribution<double> dist(0.0, total);
  auto roll = dist(rng);
  for (const auto& [id, weight] : entries) {
    roll -= std::max(0.0, weight);
    if (roll <= 0.0) return id;
  }
  return entries.back().first;
}

std::string weightedRankPick(const std::vector<std::pair<std::string, double>>& entries) {
  double total = 0.0;
  for (const auto& [rank, weight] : entries) {
    (void)rank;
    total += std::max(0.0, weight);
  }
  if (total <= 0.0) throw ApiError(drogon::k400BadRequest, "混合品階詞條池沒有可抽取的品階");
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_real_distribution<double> dist(0.0, total);
  auto roll = dist(rng);
  for (const auto& [rank, weight] : entries) {
    roll -= std::max(0.0, weight);
    if (roll <= 0.0) return rank;
  }
  return entries.back().first;
}

// [功能備註｜59-cpp.20 混合品階詞條池]
// mixed_ranks=true 時先依 rank_weights 抽品階，再於該品階依詞條權重抽詞條；
// 一般池與特殊池仍各自獨立，避免特殊詞條混入一般槽。
std::int64_t pickAffixFromPool(const Db& db, std::int64_t poolId,
                               const std::set<std::int64_t>& excluded = {},
                               const std::string& expectedKind = "normal") {
  const auto poolRows = db->execSqlSync(
      "SELECT row_to_json(p)::text AS json_text FROM affix_pools p WHERE id=$1 AND active=TRUE",
      poolId);
  if (poolRows.empty()) throw ApiError(drogon::k400BadRequest, "找不到可用詞條池");
  const auto pool = rowJson(poolRows);
  const auto poolKind = normalizeAffixKind(str(pool["pool_kind"], "normal"));
  if (poolKind != expectedKind) {
    throw ApiError(drogon::k400BadRequest,
                   expectedKind == "special" ? "特殊詞條必須使用特殊詞條池"
                                             : "一般詞條必須使用一般詞條池");
  }

  auto entries = poolEntriesDetailed(db, poolId, excluded);
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [&](const PoolEntry& entry) { return entry.kind != expectedKind; }),
                entries.end());
  if (entries.empty()) throw ApiError(drogon::k400BadRequest, "目前詞條池沒有可抽取的詞條");

  const bool mixedRanks = pool.get("mixed_ranks", false).asBool();
  if (!mixedRanks) {
    std::vector<std::pair<std::int64_t, double>> weighted;
    weighted.reserve(entries.size());
    for (const auto& entry : entries) weighted.emplace_back(entry.id, entry.weight);
    return weightedPick(weighted);
  }

  const auto rankWeights = objectOrEmpty(pool["rank_weights"]);
  std::map<std::string, bool> availableRanks;
  for (const auto& entry : entries) availableRanks[entry.rank] = true;
  std::vector<std::pair<std::string, double>> rankCandidates;
  for (const auto& [rank, _] : availableRanks) {
    (void)_;
    const auto configured = rankWeights.isMember(rank) && rankWeights[rank].isNumeric()
                                ? rankWeights[rank].asDouble()
                                : 1.0;
    if (configured > 0.0) rankCandidates.emplace_back(rank, configured);
  }
  const auto selectedRank = weightedRankPick(rankCandidates);
  std::vector<std::pair<std::int64_t, double>> weighted;
  for (const auto& entry : entries) {
    if (entry.rank == selectedRank) weighted.emplace_back(entry.id, entry.weight);
  }
  return weightedPick(weighted);
}

void spendCurrency(const Db& db, std::int64_t userId, const std::string& code,
                   std::int64_t amount) {
  if (amount <= 0) return;
  if (code.empty()) throw ApiError(drogon::k400BadRequest, "洗煉貨幣未設定");
  const auto rows = db->execSqlSync(
      "UPDATE character_special_currencies SET amount=amount-$1,updated_at=CURRENT_TIMESTAMP "
      "WHERE user_id=$2 AND currency_code=$3 AND amount>=$1 RETURNING amount",
      amount, userId, code);
  if (rows.empty()) throw ApiError(drogon::k400BadRequest, "特殊貨幣不足");
}

void refundCurrency(const Db& db, std::int64_t userId, const std::string& code,
                    std::int64_t amount) {
  if (amount <= 0 || code.empty()) return;
  db->execSqlSync(
      "INSERT INTO character_special_currencies(user_id,currency_code,amount) VALUES($1,$2,$3) "
      "ON CONFLICT(user_id,currency_code) DO UPDATE SET amount=character_special_currencies.amount+EXCLUDED.amount,updated_at=CURRENT_TIMESTAMP",
      userId, code, amount);
}

Json::Value poolWithEntries(const Db& db, std::int64_t poolId);

Json::Value expandedAffixList(const Db& db, const Json::Value& ids) {
  Json::Value expanded{Json::arrayValue};
  if (!ids.isArray()) return expanded;
  for (const auto& raw : ids) {
    const auto id = integer(raw);
    if (id <= 0) continue;
    const auto rows = db->execSqlSync(
        "SELECT row_to_json(a)::text AS json_text FROM affix_definitions a WHERE id=$1", id);
    if (!rows.empty()) expanded.append(rowJson(rows));
  }
  return expanded;
}

// [功能備註｜59-cpp.20 三層詞條資料]
// affixes=一般詞條；special_affix_details=額外特殊詞條；intrinsic_effects=固有效果。
// 特殊詞條只讀 special_affixes 欄位，永遠不計入 current_affixes / max_affixes。
Json::Value expandTarget(const Db& db, Json::Value target) {
  target["affixes"] = expandedAffixList(db, target["current_affixes"]);
  target["special_affix_details"] = expandedAffixList(db, target["special_affixes"]);
  if (!target["intrinsic_effects"].isArray()) target["intrinsic_effects"] = Json::arrayValue;

  auto profile = profileFor(db, str(target["target_type"]), normalizeRank(str(target["rank"], "G")));
  target["reforge_profile"] = profile;
  if (integer(target["pool_id"]) <= 0) {
    const auto pool =
        poolFor(db, profile, str(target["target_type"]), str(target["rank"], "G"), target["data"]);
    if (pool) target["pool_id"] = Json::Int64(*pool);
  }
  const auto poolId = integer(target["pool_id"]);
  if (poolId > 0)
    target["pool_entries"] = poolWithEntries(db, poolId)["entries"];
  else
    target["pool_entries"] = Json::arrayValue;

  // [功能備註｜59-cpp.20 裝備 → 人偶特殊詞條]
  // 回傳此來源目前可用的特殊詞條規則，以及已對哪些接收目標判定過。
  const auto ruleRows = db->execSqlSync(
      "SELECT row_to_json(r)::text AS json_text FROM special_affix_rules r "
      "WHERE r.active=TRUE AND r.source_target_type IN ($1,'*') "
      "AND r.source_rank IN ($2,'*') "
      "AND (r.source_name='' OR LOWER(r.source_name)=LOWER($3)) ORDER BY r.id",
      str(target["target_type"]), str(target["rank"], "G"), str(target["display_name"]));
  Json::Value rules{Json::arrayValue};
  for (std::size_t i = 0; i < ruleRows.size(); ++i) {
    auto rule = rowJson(ruleRows, i);
    const auto historyRows = db->execSqlSync(
        "SELECT recipient_target_id FROM special_affix_roll_history "
        "WHERE rule_id=$1 AND source_target_id=$2 ORDER BY id",
        integer(rule["id"]), integer(target["id"]));
    Json::Value attempted{Json::arrayValue};
    for (const auto& row : historyRows)
      attempted.append(Json::Int64(row["recipient_target_id"].as<std::int64_t>()));
    rule["attempted_recipient_ids"] = std::move(attempted);
    rules.append(std::move(rule));
  }
  target["special_grant_rules"] = std::move(rules);
  return target;
}

Json::Value createTarget(const Db& db, std::int64_t ownerUserId, const std::string& targetType,
                         const std::string& targetKey, const std::string& displayName,
                         const std::string& rank, const Json::Value& data) {
  const auto normalizedRank = normalizeRank(rank);
  auto profile = profileFor(db, targetType, normalizedRank);
  const auto pool = poolFor(db, profile, targetType, normalizedRank, data);
  const auto maxAffixes = std::max<std::int64_t>(0, integer(profile["max_affixes"], 0));
  const auto maxSpecialAffixes = std::max<std::int64_t>(0, integer(data["max_special_affixes"], 0));
  const auto intrinsicEffects = arrayOrEmpty(data["intrinsic_effects"]);
  const auto rows = db->execSqlSync(
      "INSERT INTO affix_targets(owner_user_id,target_type,target_key,display_name,rank,pool_id,"
      "max_affixes,current_affixes,max_special_affixes,special_affixes,intrinsic_effects,data) "
      "VALUES($1,$2,$3,$4,$5,NULLIF($6,0),$7,'[]'::jsonb,$8,'[]'::jsonb,$9::jsonb,$10::jsonb) "
      "ON CONFLICT(owner_user_id,target_type,target_key) DO UPDATE SET "
      "display_name=EXCLUDED.display_name,rank=EXCLUDED.rank,"
      "pool_id=COALESCE(affix_targets.pool_id,EXCLUDED.pool_id),"
      "max_affixes=CASE WHEN affix_targets.max_affixes>0 THEN affix_targets.max_affixes ELSE EXCLUDED.max_affixes END,"
      "max_special_affixes=CASE WHEN affix_targets.max_special_affixes>0 THEN affix_targets.max_special_affixes ELSE EXCLUDED.max_special_affixes END,"
      "intrinsic_effects=EXCLUDED.intrinsic_effects,data=EXCLUDED.data,updated_at=CURRENT_TIMESTAMP "
      "RETURNING row_to_json(affix_targets)::text AS json_text",
      ownerUserId, targetType, targetKey, displayName, normalizedRank,
      pool ? static_cast<std::int64_t>(*pool) : static_cast<std::int64_t>(0), maxAffixes,
      maxSpecialAffixes, compactJson(intrinsicEffects), compactJson(data));
  auto target = rowJson(rows);
  if (pool && integer(profile["initial_affixes"]) > 0 && target["current_affixes"].isArray() &&
      target["current_affixes"].empty()) {
    const auto initial = std::min<std::int64_t>(
        integer(profile["initial_affixes"]), std::max<std::int64_t>(0, integer(target["max_affixes"])));
    Json::Value current{Json::arrayValue};
    std::set<std::int64_t> used;
    for (std::int64_t i = 0; i < initial; ++i) {
      try {
        const auto affixId = pickAffixFromPool(db, *pool, used, "normal");
        used.insert(affixId);
        current.append(Json::Int64(affixId));
      } catch (const ApiError&) {
        break;
      }
    }
    db->execSqlSync(
        "UPDATE affix_targets SET current_affixes=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE id=$2",
        compactJson(current), integer(target["id"]));
    target["current_affixes"] = current;
  }
  return expandTarget(db, std::move(target));
}

void syncHeldTargets(const std::shared_ptr<CoreService>& service, std::int64_t userId) {
  auto character = service->characterByUser(userId);
  if (!character.isObject()) return;
  const auto db = service->database();
  const std::array<std::pair<const char*, const char*>, 4> sources = {{
      {"weapons", "weapon"}, {"equipment", "equipment"}, {"inventory", "item"},
      {"summons", "summon"}}};
  for (const auto& [field, type] : sources) {
    if (!character[field].isArray()) continue;
    std::map<std::string, int> occurrence;
    for (const auto& raw : character[field]) {
      const auto name = gearName(raw);
      if (name.empty()) continue;
      const auto n = ++occurrence[name];
      const auto key = std::string(type) + ":" + name + ":" + std::to_string(n);
      auto data = gearData(raw, type);
      const auto rank = str(data["rank"], "G");
      const auto profile = profileFor(db, type, rank);
      const bool enabled = data.get("affix_enabled", false).asBool() ||
                           integer(data["affix_pool_id"]) > 0 || !profile.isNull();
      if (!enabled) continue;
      createTarget(db, userId, type, key, name, rank, data);
    }
  }
}

Json::Value currenciesFor(const Db& db, std::int64_t userId) {
  const auto rows = db->execSqlSync(
      "SELECT row_to_json(x)::text AS json_text FROM ("
      "SELECT t.code,t.name,t.description,COALESCE(c.amount,0)::bigint AS amount,t.active "
      "FROM special_currency_types t LEFT JOIN character_special_currencies c "
      "ON c.currency_code=t.code AND c.user_id=$1 WHERE t.active=TRUE ORDER BY t.sort_order,t.code) x",
      userId);
  return listJson(rows);
}

Json::Value poolWithEntries(const Db& db, std::int64_t poolId) {
  const auto rows = db->execSqlSync(
      "SELECT row_to_json(p)::text AS json_text FROM affix_pools p WHERE id=$1", poolId);
  auto pool = rowJson(rows);
  if (pool.isNull()) return pool;
  const auto entries = db->execSqlSync(
      "SELECT row_to_json(x)::text AS json_text FROM ("
      "SELECT e.pool_id,e.affix_id,e.weight,e.active,a.name,a.rank,a.affix_kind,a.category,a.description "
      "FROM affix_pool_entries e JOIN affix_definitions a ON a.id=e.affix_id "
      "WHERE e.pool_id=$1 ORDER BY a.rank,a.name,a.id) x",
      poolId);
  auto list = listJson(entries);
  const auto poolKind = normalizeAffixKind(str(pool["pool_kind"], "normal"));
  const bool mixedRanks = pool.get("mixed_ranks", false).asBool();

  if (!mixedRanks) {
    double total = 0.0;
    for (const auto& item : list) {
      if (item.get("active", true).asBool() && str(item["affix_kind"], "normal") == poolKind)
        total += std::max(0.0, item["weight"].asDouble());
    }
    for (Json::ArrayIndex i = 0; i < list.size(); ++i) {
      const auto w = std::max(0.0, list[i]["weight"].asDouble());
      list[i]["probability"] =
          total > 0.0 && str(list[i]["affix_kind"], "normal") == poolKind ? (w / total * 100.0)
                                                                          : 0.0;
      list[i]["rank_probability"] = 100.0;
      list[i]["within_rank_probability"] = list[i]["probability"];
    }
  } else {
    // [功能備註｜59-cpp.20 混合品階機率顯示]
    // 顯示最終機率 = 品階機率 × 同品階詞條內權重機率。
    const auto rankWeights = objectOrEmpty(pool["rank_weights"]);
    std::map<std::string, double> entryTotals;
    for (const auto& item : list) {
      if (!item.get("active", true).asBool() || str(item["affix_kind"], "normal") != poolKind) continue;
      entryTotals[str(item["rank"], "G")] += std::max(0.0, item["weight"].asDouble());
    }
    std::map<std::string, double> selectionWeights;
    double totalRankWeight = 0.0;
    for (const auto& [rank, entryTotal] : entryTotals) {
      if (entryTotal <= 0.0) continue;
      const auto configured = rankWeights.isMember(rank) && rankWeights[rank].isNumeric()
                                  ? std::max(0.0, rankWeights[rank].asDouble())
                                  : 1.0;
      selectionWeights[rank] = configured;
      totalRankWeight += configured;
    }
    for (Json::ArrayIndex i = 0; i < list.size(); ++i) {
      const auto rank = str(list[i]["rank"], "G");
      const auto w = std::max(0.0, list[i]["weight"].asDouble());
      const auto sameRankTotal = entryTotals[rank];
      const auto rankChance =
          totalRankWeight > 0.0 ? selectionWeights[rank] / totalRankWeight * 100.0 : 0.0;
      const auto withinChance = sameRankTotal > 0.0 ? w / sameRankTotal * 100.0 : 0.0;
      const bool eligible = list[i].get("active", true).asBool() &&
                            str(list[i]["affix_kind"], "normal") == poolKind;
      list[i]["rank_probability"] = eligible ? rankChance : 0.0;
      list[i]["within_rank_probability"] = eligible ? withinChance : 0.0;
      list[i]["probability"] = eligible ? rankChance * withinChance / 100.0 : 0.0;
    }
  }
  pool["entries"] = std::move(list);
  return pool;
}

void register0(const std::string& path, drogon::HttpMethod method,
               std::function<void(const Request&, Callback&)> handler) {
  drogon::app().registerHandler(path,
      [handler=std::move(handler)](const Request& request, Callback&& callback) mutable {
        handler(request, callback);
      }, {method});
}

void register1(const std::string& path, drogon::HttpMethod method,
               std::function<void(const Request&, Callback&, const std::string&)> handler) {
  drogon::app().registerHandler(path,
      [handler=std::move(handler)](const Request& request, Callback&& callback,
                                   const std::string& p1) mutable {
        handler(request, callback, p1);
      }, {method});
}

void register2(const std::string& path, drogon::HttpMethod method,
               std::function<void(const Request&, Callback&, const std::string&, const std::string&)> handler) {
  drogon::app().registerHandler(path,
      [handler=std::move(handler)](const Request& request, Callback&& callback,
                                   const std::string& p1, const std::string& p2) mutable {
        handler(request, callback, p1, p2);
      }, {method});
}

}  // namespace

void applyGearAffixMigrations(const std::shared_ptr<CoreService>& service) {
  const auto db = service->database();
  const std::vector<std::string> ddl = {
      R"SQL(CREATE TABLE IF NOT EXISTS special_currency_types(
        code TEXT PRIMARY KEY,name TEXT NOT NULL,description TEXT NOT NULL DEFAULT '',sort_order INTEGER NOT NULL DEFAULT 0,
        active BOOLEAN NOT NULL DEFAULT TRUE,created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP))SQL",
      R"SQL(CREATE TABLE IF NOT EXISTS character_special_currencies(
        user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,currency_code TEXT NOT NULL REFERENCES special_currency_types(code) ON DELETE CASCADE,
        amount BIGINT NOT NULL DEFAULT 0 CHECK(amount>=0),updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,PRIMARY KEY(user_id,currency_code)))SQL",
      R"SQL(CREATE TABLE IF NOT EXISTS equipment_slot_types(
        code TEXT PRIMARY KEY,name TEXT NOT NULL,sort_order INTEGER NOT NULL DEFAULT 0,active BOOLEAN NOT NULL DEFAULT TRUE,
        config JSONB NOT NULL DEFAULT '{}'::jsonb,created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP))SQL",
      R"SQL(CREATE TABLE IF NOT EXISTS affix_definitions(
        id BIGSERIAL PRIMARY KEY,name TEXT NOT NULL,rank TEXT NOT NULL DEFAULT 'G',category TEXT NOT NULL DEFAULT '特殊',description TEXT NOT NULL DEFAULT '',
        effects JSONB NOT NULL DEFAULT '{}'::jsonb,tags JSONB NOT NULL DEFAULT '[]'::jsonb,active BOOLEAN NOT NULL DEFAULT TRUE,
        created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP))SQL",
      R"SQL(CREATE TABLE IF NOT EXISTS affix_pools(
        id BIGSERIAL PRIMARY KEY,name TEXT NOT NULL,target_type TEXT NOT NULL DEFAULT '*',rank TEXT NOT NULL DEFAULT 'G',description TEXT NOT NULL DEFAULT '',
        active BOOLEAN NOT NULL DEFAULT TRUE,created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP))SQL",
      R"SQL(CREATE TABLE IF NOT EXISTS affix_pool_entries(
        pool_id BIGINT NOT NULL REFERENCES affix_pools(id) ON DELETE CASCADE,affix_id BIGINT NOT NULL REFERENCES affix_definitions(id) ON DELETE CASCADE,
        weight DOUBLE PRECISION NOT NULL DEFAULT 1 CHECK(weight>=0),active BOOLEAN NOT NULL DEFAULT TRUE,updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
        PRIMARY KEY(pool_id,affix_id)))SQL",
      R"SQL(CREATE TABLE IF NOT EXISTS reforge_profiles(
        id BIGSERIAL PRIMARY KEY,target_type TEXT NOT NULL DEFAULT '*',rank TEXT NOT NULL DEFAULT 'G',pool_id BIGINT REFERENCES affix_pools(id) ON DELETE SET NULL,
        initial_affixes INTEGER NOT NULL DEFAULT 0,max_affixes INTEGER NOT NULL DEFAULT 0,random_currency_code TEXT NOT NULL DEFAULT 'trait_core',random_cost BIGINT NOT NULL DEFAULT 1,
        targeted_currency_code TEXT NOT NULL DEFAULT 'trait_core',targeted_cost BIGINT NOT NULL DEFAULT 5,direct_grant BOOLEAN NOT NULL DEFAULT FALSE,active BOOLEAN NOT NULL DEFAULT TRUE,
        created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,UNIQUE(target_type,rank)))SQL",
      R"SQL(CREATE TABLE IF NOT EXISTS affix_targets(
        id BIGSERIAL PRIMARY KEY,owner_user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,target_type TEXT NOT NULL,target_key TEXT NOT NULL,display_name TEXT NOT NULL,
        rank TEXT NOT NULL DEFAULT 'G',pool_id BIGINT REFERENCES affix_pools(id) ON DELETE SET NULL,max_affixes INTEGER NOT NULL DEFAULT 0,current_affixes JSONB NOT NULL DEFAULT '[]'::jsonb,
        data JSONB NOT NULL DEFAULT '{}'::jsonb,created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
        UNIQUE(owner_user_id,target_type,target_key)))SQL",
      "CREATE INDEX IF NOT EXISTS idx_affix_targets_owner ON affix_targets(owner_user_id,target_type)",
      "CREATE INDEX IF NOT EXISTS idx_affix_pool_entries_pool ON affix_pool_entries(pool_id)",
      "ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS equip_slot_code TEXT NOT NULL DEFAULT ''",
      "ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS weapon_hands INTEGER NOT NULL DEFAULT 1",
      "ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS affix_pool_id BIGINT",
      "ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS affix_enabled BOOLEAN NOT NULL DEFAULT FALSE",
      // [功能備註｜59-cpp.20 詞條分層] 一般／特殊詞條與固有效果分開儲存。
      "ALTER TABLE affix_definitions ADD COLUMN IF NOT EXISTS affix_kind TEXT NOT NULL DEFAULT 'normal'",
      "ALTER TABLE affix_pools ADD COLUMN IF NOT EXISTS pool_kind TEXT NOT NULL DEFAULT 'normal'",
      "ALTER TABLE affix_pools ADD COLUMN IF NOT EXISTS mixed_ranks BOOLEAN NOT NULL DEFAULT FALSE",
      "ALTER TABLE affix_pools ADD COLUMN IF NOT EXISTS rank_weights JSONB NOT NULL DEFAULT '{}'::jsonb",
      "ALTER TABLE affix_targets ADD COLUMN IF NOT EXISTS special_affixes JSONB NOT NULL DEFAULT '[]'::jsonb",
      "ALTER TABLE affix_targets ADD COLUMN IF NOT EXISTS intrinsic_effects JSONB NOT NULL DEFAULT '[]'::jsonb",
      "ALTER TABLE affix_targets ADD COLUMN IF NOT EXISTS max_special_affixes INTEGER NOT NULL DEFAULT 0",
      // [功能備註｜59-cpp.20 額外特殊詞條規則] 裝備等來源可按機率對指定類型目標額外授予特殊詞條。
      R"SQL(CREATE TABLE IF NOT EXISTS special_affix_rules(
        id BIGSERIAL PRIMARY KEY,name TEXT NOT NULL,source_target_type TEXT NOT NULL DEFAULT 'equipment',
        source_name TEXT NOT NULL DEFAULT '',source_rank TEXT NOT NULL DEFAULT '*',recipient_target_type TEXT NOT NULL DEFAULT 'puppet',
        pool_id BIGINT NOT NULL REFERENCES affix_pools(id) ON DELETE CASCADE,chance_percent DOUBLE PRECISION NOT NULL DEFAULT 0,
        grant_count INTEGER NOT NULL DEFAULT 1,max_special_affixes INTEGER NOT NULL DEFAULT 1,once_per_pair BOOLEAN NOT NULL DEFAULT TRUE,
        active BOOLEAN NOT NULL DEFAULT TRUE,created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP))SQL",
      R"SQL(CREATE TABLE IF NOT EXISTS special_affix_roll_history(
        id BIGSERIAL PRIMARY KEY,rule_id BIGINT NOT NULL REFERENCES special_affix_rules(id) ON DELETE CASCADE,
        source_target_id BIGINT NOT NULL REFERENCES affix_targets(id) ON DELETE CASCADE,
        recipient_target_id BIGINT NOT NULL REFERENCES affix_targets(id) ON DELETE CASCADE,
        success BOOLEAN NOT NULL DEFAULT FALSE,granted_affixes JSONB NOT NULL DEFAULT '[]'::jsonb,
        rolled_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP))SQL",
      "CREATE INDEX IF NOT EXISTS idx_special_affix_rules_source ON special_affix_rules(source_target_type,source_rank,active)",
      "CREATE INDEX IF NOT EXISTS idx_special_affix_roll_history_pair ON special_affix_roll_history(rule_id,source_target_id,recipient_target_id)"};
  for (const auto& sql : ddl) db->execSqlSync(sql);

  db->execSqlSync(
      "INSERT INTO special_currency_types(code,name,description,sort_order,active) VALUES('trait_core','詞條核心','通用洗煉特殊貨幣；可由合成配方產出。',10,TRUE) "
      "ON CONFLICT(code) DO UPDATE SET name=EXCLUDED.name,description=EXCLUDED.description,active=TRUE");
  // Render/PostgreSQL startup hotfix:
  // These are trusted compile-time constants, so seed them as literal SQL instead of
  // PostgreSQL binary bind parameters.  Older Drogon/libpq combinations can infer
  // parameter $3 (sort_order) with a binary width that disagrees with INTEGER and
  // abort startup with: "insufficient data left in message / portal parameter $3".
  // Keeping startup seed SQL parameter-free removes that protocol ambiguity.
  const std::array<const char*, 8> defaultSlotSeedSql = {{
      "INSERT INTO equipment_slot_types(code,name,sort_order,active) VALUES('head','頭部',10,TRUE) ON CONFLICT(code) DO NOTHING",
      "INSERT INTO equipment_slot_types(code,name,sort_order,active) VALUES('body','身體',20,TRUE) ON CONFLICT(code) DO NOTHING",
      "INSERT INTO equipment_slot_types(code,name,sort_order,active) VALUES('hands','手部',30,TRUE) ON CONFLICT(code) DO NOTHING",
      "INSERT INTO equipment_slot_types(code,name,sort_order,active) VALUES('legs','腿部',40,TRUE) ON CONFLICT(code) DO NOTHING",
      "INSERT INTO equipment_slot_types(code,name,sort_order,active) VALUES('feet','腳部',50,TRUE) ON CONFLICT(code) DO NOTHING",
      "INSERT INTO equipment_slot_types(code,name,sort_order,active) VALUES('accessory_1','飾品 1',60,TRUE) ON CONFLICT(code) DO NOTHING",
      "INSERT INTO equipment_slot_types(code,name,sort_order,active) VALUES('accessory_2','飾品 2',70,TRUE) ON CONFLICT(code) DO NOTHING",
      "INSERT INTO equipment_slot_types(code,name,sort_order,active) VALUES('special','特殊裝備',80,TRUE) ON CONFLICT(code) DO NOTHING"}};
  for (const auto* sql : defaultSlotSeedSql) {
    try {
      db->execSqlSync(sql);
    } catch (const drogon::orm::DrogonDbException& error) {
      LOG_ERROR << "gear/affix startup slot seed failed: " << error.base().what()
                << "\nSQL: " << sql;
      throw;
    }
  }
  // G 級人偶只是預設範例；底層支援任何 target_type，不鎖死人偶。
  db->execSqlSync(
      "INSERT INTO reforge_profiles(target_type,rank,initial_affixes,max_affixes,random_currency_code,random_cost,targeted_currency_code,targeted_cost,direct_grant,active) "
      "VALUES('puppet','G',1,2,'trait_core',1,'trait_core',5,FALSE,TRUE) ON CONFLICT(target_type,rank) DO NOTHING");
}

void registerGearAffixRoutes(const std::shared_ptr<CoreService>& service) {
  const auto db = service->database();

  // [功能備註｜特殊貨幣] 玩家查看詞條核心等洗煉貨幣。
  register0("/api/me/special-currencies", drogon::Get, [service, db](const Request& req, Callback& cb) {
    runHandler(cb, [&] { const auto user=requireUser(service,req); Json::Value out; out["currencies"]=currenciesFor(db,user.id); reply(cb,std::move(out)); }, "讀取特殊貨幣失敗");
  });

  register0("/api/admin/special-currencies", drogon::Get, [service, db](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service,req,true); const auto rows=db->execSqlSync("SELECT row_to_json(t)::text AS json_text FROM special_currency_types t ORDER BY sort_order,code"); Json::Value out; out["currencies"]=listJson(rows); reply(cb,std::move(out)); });
  });
  register0("/api/admin/special-currencies", drogon::Post, [service, db](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service,req,true); const auto body=bodyOrEmpty(req); auto code=trimmed(str(body["code"])); auto name=trimmed(str(body["name"])); if(code.empty()||name.empty())throw ApiError(drogon::k400BadRequest,"代碼與名稱不能空白"); db->execSqlSync("INSERT INTO special_currency_types(code,name,description,sort_order,active) VALUES($1,$2,$3,$4,$5) ON CONFLICT(code) DO UPDATE SET name=EXCLUDED.name,description=EXCLUDED.description,sort_order=EXCLUDED.sort_order,active=EXCLUDED.active,updated_at=CURRENT_TIMESTAMP",code,name,str(body["description"]),integer(body["sort_order"]),body.get("active",true).asBool()); Json::Value out;out["ok"]=true;reply(cb,std::move(out)); });
  });
  register1("/api/admin/players/{1}/special-currencies", drogon::Patch, [service, db](const Request& req, Callback& cb, const std::string& idText) {
    runHandler(cb, [&] { requireUser(service,req,true); const auto userId=positiveId(idText); const auto body=bodyOrEmpty(req); const auto code=trimmed(str(body["code"])); const auto delta=integer(body["delta"]); if(code.empty())throw ApiError(drogon::k400BadRequest,"缺少貨幣代碼"); if(db->execSqlSync("SELECT 1 FROM special_currency_types WHERE code=$1",code).empty())throw ApiError(drogon::k404NotFound,"找不到特殊貨幣"); db->execSqlSync("INSERT INTO character_special_currencies(user_id,currency_code,amount) VALUES($1,$2,GREATEST(0,$3)) ON CONFLICT(user_id,currency_code) DO UPDATE SET amount=GREATEST(0,character_special_currencies.amount+$3),updated_at=CURRENT_TIMESTAMP",userId,code,delta); Json::Value out;out["ok"]=true;out["currencies"]=currenciesFor(db,userId);reply(cb,std::move(out)); });
  });

  // [功能備註｜裝備槽] 玩家讀取 DM 自訂裝備槽；武器固定主／副兩槽。
  register0("/api/equipment-slots", drogon::Get, [service, db](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service,req); const auto rows=db->execSqlSync("SELECT row_to_json(s)::text AS json_text FROM equipment_slot_types s WHERE active=TRUE ORDER BY sort_order,code"); Json::Value out;out["slots"]=listJson(rows);reply(cb,std::move(out)); });
  });
  register0("/api/admin/equipment-slots", drogon::Get, [service, db](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service,req,true); const auto rows=db->execSqlSync("SELECT row_to_json(s)::text AS json_text FROM equipment_slot_types s ORDER BY sort_order,code"); Json::Value out;out["slots"]=listJson(rows);reply(cb,std::move(out)); });
  });
  register0("/api/admin/equipment-slots", drogon::Post, [service, db](const Request& req, Callback& cb) {
    runHandler(cb, [&] { requireUser(service,req,true); const auto body=bodyOrEmpty(req); auto code=trimmed(str(body["code"])); auto name=trimmed(str(body["name"])); if(code.empty()||name.empty())throw ApiError(drogon::k400BadRequest,"槽位代碼與名稱不能空白"); if(code=="main_weapon"||code=="sub_weapon")throw ApiError(drogon::k400BadRequest,"主／副武器槽為系統固定槽位"); db->execSqlSync("INSERT INTO equipment_slot_types(code,name,sort_order,active,config) VALUES($1,$2,$3,$4,$5::jsonb) ON CONFLICT(code) DO UPDATE SET name=EXCLUDED.name,sort_order=EXCLUDED.sort_order,active=EXCLUDED.active,config=EXCLUDED.config,updated_at=CURRENT_TIMESTAMP",code,name,integer(body["sort_order"]),body.get("active",true).asBool(),compactJson(body["config"].isObject()?body["config"]:Json::Value{Json::objectValue}));Json::Value out;out["ok"]=true;reply(cb,std::move(out)); });
  });
  register1("/api/admin/equipment-slots/{1}", drogon::Patch, [service, db](const Request& req, Callback& cb, const std::string& code) {
    runHandler(cb, [&] { requireUser(service,req,true); const auto body=bodyOrEmpty(req); if(db->execSqlSync("SELECT 1 FROM equipment_slot_types WHERE code=$1",code).empty())throw ApiError(drogon::k404NotFound,"找不到裝備槽"); db->execSqlSync("UPDATE equipment_slot_types SET name=COALESCE(NULLIF($1,''),name),sort_order=$2,active=$3,config=$4::jsonb,updated_at=CURRENT_TIMESTAMP WHERE code=$5",trimmed(str(body["name"])),integer(body["sort_order"]),body.get("active",true).asBool(),compactJson(body["config"].isObject()?body["config"]:Json::Value{Json::objectValue}),code);Json::Value out;out["ok"]=true;reply(cb,std::move(out)); });
  });
  register1("/api/admin/equipment-slots/{1}", drogon::Delete, [service, db](const Request& req, Callback& cb, const std::string& code) {
    runHandler(cb, [&] { requireUser(service,req,true); db->execSqlSync("DELETE FROM equipment_slot_types WHERE code=$1",code);Json::Value out;out["ok"]=true;reply(cb,std::move(out)); });
  });

  // [功能備註｜裝備配置] 主武器／副武器固定，裝備槽由 DM 自訂；雙手武器會鎖住副手。
  register0("/api/character/loadout", drogon::Patch, [service, db](const Request& req, Callback& cb) {
    runHandler(cb, [&] {
      const auto user=requireUser(service,req); syncHeldTargets(service,user.id); const auto body=bodyOrEmpty(req);
      auto weapons=body["weapon_slots"].isObject()?body["weapon_slots"]:Json::Value{Json::objectValue};
      auto equipment=body["equipment_slots"].isObject()?body["equipment_slots"]:Json::Value{Json::objectValue};
      auto validateTarget=[&](std::int64_t id,const std::string& type)->Json::Value{if(id<=0)return Json::Value{};const auto rows=db->execSqlSync("SELECT row_to_json(t)::text AS json_text FROM affix_targets t WHERE id=$1 AND owner_user_id=$2 AND target_type=$3",id,user.id,type);if(rows.empty())throw ApiError(drogon::k400BadRequest,"裝備不是你的持有物");return rowJson(rows);};
      const auto mainId=integer(weapons["main"]),subId=integer(weapons["sub"]); if(mainId>0&&mainId==subId)throw ApiError(drogon::k400BadRequest,"主武器與副武器不能是同一件");
      auto mainTarget=validateTarget(mainId,"weapon"); auto subTarget=validateTarget(subId,"weapon");
      if(!mainTarget.isNull()&&integer(mainTarget["data"]["weapon_hands"],1)>=2&&subId>0)throw ApiError(drogon::k400BadRequest,"雙手武器會同時占用主／副武器槽");
      if(!subTarget.isNull()&&integer(subTarget["data"]["weapon_hands"],1)>=2)throw ApiError(drogon::k400BadRequest,"雙手武器只能裝在主武器槽");
      std::set<std::int64_t> used; if(mainId>0)used.insert(mainId);if(subId>0)used.insert(subId);
      Json::Value cleanEquip{Json::objectValue};
      const auto allowedRows=db->execSqlSync("SELECT code FROM equipment_slot_types WHERE active=TRUE"); std::set<std::string> allowed;for(const auto& row:allowedRows)allowed.insert(row["code"].as<std::string>());
      for(const auto& key:equipment.getMemberNames()){
        if(!allowed.count(key))continue;const auto id=integer(equipment[key]);if(id<=0){cleanEquip[key]=Json::Int64(0);continue;}if(used.count(id))throw ApiError(drogon::k400BadRequest,"同一件裝備不能放入多個槽位");auto target=validateTarget(id,"equipment");const auto required=trimmed(str(target["data"]["equip_slot_code"]));if(!required.empty()&&required!=key)throw ApiError(drogon::k400BadRequest,"此裝備只能放在「"+required+"」槽位");used.insert(id);cleanEquip[key]=Json::Int64(id);
      }
      Json::Value loadout{Json::objectValue};loadout["configured"]=true;loadout["weapon_slots"]=Json::Value{Json::objectValue};loadout["weapon_slots"]["main"]=Json::Int64(mainId);loadout["weapon_slots"]["sub"]=Json::Int64(subId);loadout["equipment_slots"]=cleanEquip;
      db->execSqlSync("UPDATE character_cards SET gear_loadout=$1::jsonb,updated_at=CURRENT_TIMESTAMP WHERE user_id=$2",compactJson(loadout),user.id);
      Json::Value out;out["ok"]=true;out["character"]=service->characterByUser(user.id);reply(cb,std::move(out));
    },"儲存裝備配置失敗");
  });

  // [功能備註｜詞條庫] 詞條內容由 DM 自行設計；系統只管理通用資料與機率。
  register0("/api/admin/affixes", drogon::Get, [service, db](const Request& req, Callback& cb) {
    runHandler(cb,[&]{requireUser(service,req,true);const auto rows=db->execSqlSync("SELECT row_to_json(a)::text AS json_text FROM affix_definitions a ORDER BY rank,name,id");Json::Value out;out["affixes"]=listJson(rows);reply(cb,std::move(out));});
  });
  register0("/api/admin/affixes", drogon::Post, [service, db](const Request& req, Callback& cb) {
    runHandler(cb,[&]{
      requireUser(service,req,true);auto body=bodyOrEmpty(req);
      const auto name=trimmed(str(body["name"]));if(name.empty())throw ApiError(drogon::k400BadRequest,"詞條名稱不能空白");
      const auto rank=normalizeRank(str(body["rank"],"G"));const auto kind=normalizeAffixKind(str(body["affix_kind"],"normal"));
      const auto rows=db->execSqlSync(
          "INSERT INTO affix_definitions(name,rank,affix_kind,category,description,effects,tags,active) "
          "VALUES($1,$2,$3,$4,$5,$6::jsonb,$7::jsonb,$8) RETURNING row_to_json(affix_definitions)::text AS json_text",
          name,rank,kind,trimmed(str(body["category"],kind=="special"?"特殊":"一般")),str(body["description"]),
          compactJson(objectOrEmpty(body["effects"])),compactJson(arrayOrEmpty(body["tags"])),body.get("active",true).asBool());
      Json::Value out;out["affix"]=rowJson(rows);reply(cb,std::move(out));
    });
  });
  register1("/api/admin/affixes/{1}", drogon::Patch, [service, db](const Request& req, Callback& cb,const std::string&idText){
    runHandler(cb,[&]{
      requireUser(service,req,true);const auto id=positiveId(idText);auto body=bodyOrEmpty(req);
      const auto existingRows=db->execSqlSync("SELECT row_to_json(a)::text AS json_text FROM affix_definitions a WHERE id=$1",id);
      if(existingRows.empty())throw ApiError(drogon::k404NotFound,"找不到詞條");const auto existing=rowJson(existingRows);
      const auto name=trimmed(str(body["name"],str(existing["name"])));if(name.empty())throw ApiError(drogon::k400BadRequest,"詞條名稱不能空白");
      const auto rank=normalizeRank(str(body["rank"],str(existing["rank"],"G")));
      const auto kind=normalizeAffixKind(str(body["affix_kind"],str(existing["affix_kind"],"normal")));
      const auto effects=body.isMember("effects")?objectOrEmpty(body["effects"]):objectOrEmpty(existing["effects"]);
      const auto tags=body.isMember("tags")?arrayOrEmpty(body["tags"]):arrayOrEmpty(existing["tags"]);
      db->execSqlSync(
          "UPDATE affix_definitions SET name=$1,rank=$2,affix_kind=$3,category=$4,description=$5,effects=$6::jsonb,tags=$7::jsonb,active=$8,updated_at=CURRENT_TIMESTAMP WHERE id=$9",
          name,rank,kind,trimmed(str(body["category"],str(existing["category"],kind=="special"?"特殊":"一般"))),
          str(body["description"],str(existing["description"])),compactJson(effects),compactJson(tags),
          body.isMember("active")?body["active"].asBool():existing.get("active",true).asBool(),id);
      Json::Value out;out["ok"]=true;reply(cb,std::move(out));
    });
  });
  register1("/api/admin/affixes/{1}", drogon::Delete, [service, db](const Request& req, Callback& cb,const std::string&idText){runHandler(cb,[&]{requireUser(service,req,true);db->execSqlSync("DELETE FROM affix_definitions WHERE id=$1",positiveId(idText));Json::Value out;out["ok"]=true;reply(cb,std::move(out));});});

  // [功能備註｜詞條池／機率] DM 在遊戲內改權重；百分比由所有啟用權重自動換算。
  register0("/api/admin/affix-pools", drogon::Get, [service, db](const Request& req, Callback& cb){runHandler(cb,[&]{requireUser(service,req,true);const auto rows=db->execSqlSync("SELECT id FROM affix_pools ORDER BY rank,name,id");Json::Value pools{Json::arrayValue};for(const auto& row:rows)pools.append(poolWithEntries(db,row["id"].as<std::int64_t>()));Json::Value out;out["pools"]=std::move(pools);reply(cb,std::move(out));});});
  register0("/api/admin/affix-pools", drogon::Post, [service, db](const Request& req, Callback& cb){
    runHandler(cb,[&]{
      requireUser(service,req,true);auto body=bodyOrEmpty(req);auto name=trimmed(str(body["name"]));
      if(name.empty())throw ApiError(drogon::k400BadRequest,"詞條池名稱不能空白");
      auto type=trimmed(str(body["target_type"],"*"));auto rank=normalizeRank(str(body["rank"],"G"));
      const auto poolKind=normalizeAffixKind(str(body["pool_kind"],"normal"));
      const bool mixed=body.get("mixed_ranks",false).asBool();const auto rankWeights=objectOrEmpty(body["rank_weights"]);
      const auto rows=db->execSqlSync(
          "INSERT INTO affix_pools(name,target_type,rank,pool_kind,mixed_ranks,rank_weights,description,active) "
          "VALUES($1,$2,$3,$4,$5,$6::jsonb,$7,$8) RETURNING id",
          name,type,rank,poolKind,mixed,compactJson(rankWeights),str(body["description"]),body.get("active",true).asBool());
      Json::Value out;out["pool"]=poolWithEntries(db,rows[0]["id"].as<std::int64_t>());reply(cb,std::move(out));
    });
  });
  register1("/api/admin/affix-pools/{1}", drogon::Patch,[service,db](const Request&req,Callback&cb,const std::string&idText){
    runHandler(cb,[&]{
      requireUser(service,req,true);const auto id=positiveId(idText);auto body=bodyOrEmpty(req);
      const auto existingRows=db->execSqlSync("SELECT row_to_json(p)::text AS json_text FROM affix_pools p WHERE id=$1",id);
      if(existingRows.empty())throw ApiError(drogon::k404NotFound,"找不到詞條池");const auto existing=rowJson(existingRows);
      const auto name=trimmed(str(body["name"],str(existing["name"])));if(name.empty())throw ApiError(drogon::k400BadRequest,"詞條池名稱不能空白");
      const auto type=trimmed(str(body["target_type"],str(existing["target_type"],"*")));
      const auto rank=normalizeRank(str(body["rank"],str(existing["rank"],"G")));
      const auto poolKind=normalizeAffixKind(str(body["pool_kind"],str(existing["pool_kind"],"normal")));
      const bool mixed=body.isMember("mixed_ranks")?body["mixed_ranks"].asBool():existing.get("mixed_ranks",false).asBool();
      const auto rankWeights=body.isMember("rank_weights")?objectOrEmpty(body["rank_weights"]):objectOrEmpty(existing["rank_weights"]);
      db->execSqlSync(
          "UPDATE affix_pools SET name=$1,target_type=$2,rank=$3,pool_kind=$4,mixed_ranks=$5,rank_weights=$6::jsonb,description=$7,active=$8,updated_at=CURRENT_TIMESTAMP WHERE id=$9",
          name,type,rank,poolKind,mixed,compactJson(rankWeights),str(body["description"],str(existing["description"])),
          body.isMember("active")?body["active"].asBool():existing.get("active",true).asBool(),id);
      Json::Value out;out["pool"]=poolWithEntries(db,id);reply(cb,std::move(out));
    });
  });
  register1("/api/admin/affix-pools/{1}", drogon::Delete,[service,db](const Request&req,Callback&cb,const std::string&idText){
    runHandler(cb,[&]{requireUser(service,req,true);db->execSqlSync("DELETE FROM affix_pools WHERE id=$1",positiveId(idText));Json::Value out;out["ok"]=true;reply(cb,std::move(out));});
  });
  register2("/api/admin/affix-pools/{1}/entries/{2}",drogon::Patch,[service,db](const Request&req,Callback&cb,const std::string&poolText,const std::string&affixText){
    runHandler(cb,[&]{
      requireUser(service,req,true);const auto poolId=positiveId(poolText),affixId=positiveId(affixText);auto body=bodyOrEmpty(req);
      const auto poolRows=db->execSqlSync("SELECT row_to_json(p)::text AS json_text FROM affix_pools p WHERE id=$1",poolId);
      const auto affixRows=db->execSqlSync("SELECT row_to_json(a)::text AS json_text FROM affix_definitions a WHERE id=$1",affixId);
      if(poolRows.empty()||affixRows.empty())throw ApiError(drogon::k404NotFound,"找不到詞條池或詞條");
      const auto pool=rowJson(poolRows),affix=rowJson(affixRows);
      if(normalizeAffixKind(str(pool["pool_kind"],"normal"))!=normalizeAffixKind(str(affix["affix_kind"],"normal")))
        throw ApiError(drogon::k400BadRequest,"一般詞條只能進一般池；特殊詞條只能進特殊池");
      if(!pool.get("mixed_ranks",false).asBool()&&str(pool["rank"],"G")!=str(affix["rank"],"G"))
        throw ApiError(drogon::k400BadRequest,"非混合品階詞條池只能加入與詞條池同品階的詞條");
      const auto weight=std::max(0.0,body.get("weight",1.0).asDouble());
      db->execSqlSync("INSERT INTO affix_pool_entries(pool_id,affix_id,weight,active) VALUES($1,$2,$3,$4) ON CONFLICT(pool_id,affix_id) DO UPDATE SET weight=EXCLUDED.weight,active=EXCLUDED.active,updated_at=CURRENT_TIMESTAMP",poolId,affixId,weight,body.get("active",true).asBool());
      Json::Value out;out["pool"]=poolWithEntries(db,poolId);reply(cb,std::move(out));
    });
  });
  register2("/api/admin/affix-pools/{1}/entries/{2}",drogon::Delete,[service,db](const Request&req,Callback&cb,const std::string&poolText,const std::string&affixText){runHandler(cb,[&]{requireUser(service,req,true);const auto poolId=positiveId(poolText);db->execSqlSync("DELETE FROM affix_pool_entries WHERE pool_id=$1 AND affix_id=$2",poolId,positiveId(affixText));Json::Value out;out["pool"]=poolWithEntries(db,poolId);reply(cb,std::move(out));});});

  // [功能備註｜59-cpp.20 額外特殊詞條規則]
  // DM 可指定「哪類裝備／哪個名稱／哪個品階」有多少機率，對哪類目標額外授予特殊詞條。
  register0("/api/admin/special-affix-rules",drogon::Get,[service,db](const Request&req,Callback&cb){
    runHandler(cb,[&]{requireUser(service,req,true);const auto rows=db->execSqlSync(
      "SELECT row_to_json(r)::text AS json_text FROM special_affix_rules r ORDER BY source_target_type,source_name,id");
      Json::Value out;out["rules"]=listJson(rows);reply(cb,std::move(out));});
  });
  register0("/api/admin/special-affix-rules",drogon::Post,[service,db](const Request&req,Callback&cb){
    runHandler(cb,[&]{
      requireUser(service,req,true);auto body=bodyOrEmpty(req);
      auto name=trimmed(str(body["name"]));if(name.empty())throw ApiError(drogon::k400BadRequest,"規則名稱不能空白");
      const auto poolId=integer(body["pool_id"]);if(poolId<=0)throw ApiError(drogon::k400BadRequest,"請選擇特殊詞條池");
      const auto poolRows=db->execSqlSync("SELECT pool_kind FROM affix_pools WHERE id=$1 AND active=TRUE",poolId);
      if(poolRows.empty()||normalizeAffixKind(poolRows[0]["pool_kind"].as<std::string>())!="special")
        throw ApiError(drogon::k400BadRequest,"額外特殊詞條規則只能使用特殊詞條池");
      auto sourceType=trimmed(str(body["source_target_type"],"equipment"));if(sourceType.empty())sourceType="equipment";
      auto recipientType=trimmed(str(body["recipient_target_type"],"puppet"));if(recipientType.empty())recipientType="puppet";
      auto sourceRank=trimmed(str(body["source_rank"],"*"));if(sourceRank!="*")sourceRank=normalizeRank(sourceRank);
      const auto chance=clampedPercent(body["chance_percent"]);
      const auto count=std::clamp<std::int64_t>(integer(body["grant_count"],1),1,20);
      const auto maxSpecial=std::max<std::int64_t>(0,integer(body["max_special_affixes"],1));
      const auto rows=db->execSqlSync(
        "INSERT INTO special_affix_rules(name,source_target_type,source_name,source_rank,recipient_target_type,pool_id,chance_percent,grant_count,max_special_affixes,once_per_pair,active) "
        "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11) RETURNING row_to_json(special_affix_rules)::text AS json_text",
        name,sourceType,trimmed(str(body["source_name"])),sourceRank,recipientType,poolId,chance,count,maxSpecial,
        body.get("once_per_pair",true).asBool(),body.get("active",true).asBool());
      Json::Value out;out["rule"]=rowJson(rows);reply(cb,std::move(out));
    });
  });
  register1("/api/admin/special-affix-rules/{1}",drogon::Delete,[service,db](const Request&req,Callback&cb,const std::string&idText){
    runHandler(cb,[&]{requireUser(service,req,true);db->execSqlSync("DELETE FROM special_affix_rules WHERE id=$1",positiveId(idText));Json::Value out;out["ok"]=true;reply(cb,std::move(out));});
  });

  // [功能備註｜洗煉規則] 任何 target_type 都能設定，不綁死人偶。
  register0("/api/admin/reforge-profiles",drogon::Get,[service,db](const Request&req,Callback&cb){runHandler(cb,[&]{requireUser(service,req,true);const auto rows=db->execSqlSync("SELECT row_to_json(r)::text AS json_text FROM reforge_profiles r ORDER BY target_type,rank,id");Json::Value out;out["profiles"]=listJson(rows);reply(cb,std::move(out));});});
  register0("/api/admin/reforge-profiles",drogon::Post,[service,db](const Request&req,Callback&cb){runHandler(cb,[&]{requireUser(service,req,true);auto body=bodyOrEmpty(req);const auto type=trimmed(str(body["target_type"],"*"));const auto rank=normalizeRank(str(body["rank"],"G"));const auto pool=integer(body["pool_id"]);db->execSqlSync("INSERT INTO reforge_profiles(target_type,rank,pool_id,initial_affixes,max_affixes,random_currency_code,random_cost,targeted_currency_code,targeted_cost,direct_grant,active) VALUES($1,$2,NULLIF($3,0),$4,$5,$6,$7,$8,$9,$10,$11) ON CONFLICT(target_type,rank) DO UPDATE SET pool_id=EXCLUDED.pool_id,initial_affixes=EXCLUDED.initial_affixes,max_affixes=EXCLUDED.max_affixes,random_currency_code=EXCLUDED.random_currency_code,random_cost=EXCLUDED.random_cost,targeted_currency_code=EXCLUDED.targeted_currency_code,targeted_cost=EXCLUDED.targeted_cost,direct_grant=EXCLUDED.direct_grant,active=EXCLUDED.active,updated_at=CURRENT_TIMESTAMP",type,rank,pool,std::max<std::int64_t>(0,integer(body["initial_affixes"])),std::max<std::int64_t>(0,integer(body["max_affixes"])),str(body["random_currency_code"],"trait_core"),std::max<std::int64_t>(0,integer(body["random_cost"],1)),str(body["targeted_currency_code"],"trait_core"),std::max<std::int64_t>(0,integer(body["targeted_cost"],5)),body.get("direct_grant",false).asBool(),body.get("active",true).asBool());Json::Value out;out["ok"]=true;reply(cb,std::move(out));});});

  // [功能備註｜通用詞條目標] 玩家持有物自動同步；人偶／特殊造物也可由 DM 建立自訂目標。
  register0("/api/me/affix-targets",drogon::Get,[service,db](const Request&req,Callback&cb){runHandler(cb,[&]{const auto user=requireUser(service,req);syncHeldTargets(service,user.id);const auto rows=db->execSqlSync("SELECT row_to_json(t)::text AS json_text FROM affix_targets t WHERE owner_user_id=$1 ORDER BY target_type,display_name,id",user.id);Json::Value targets{Json::arrayValue};for(std::size_t i=0;i<rows.size();++i)targets.append(expandTarget(db,rowJson(rows,i)));Json::Value out;out["targets"]=std::move(targets);out["currencies"]=currenciesFor(db,user.id);reply(cb,std::move(out));});});
  register0("/api/admin/affix-targets",drogon::Post,[service,db](const Request&req,Callback&cb){runHandler(cb,[&]{requireUser(service,req,true);auto body=bodyOrEmpty(req);const auto owner=integer(body["owner_user_id"]);if(owner<=0)throw ApiError(drogon::k400BadRequest,"需要玩家 ID");auto type=trimmed(str(body["target_type"],"custom"));auto name=trimmed(str(body["display_name"]));if(name.empty())throw ApiError(drogon::k400BadRequest,"目標名稱不能空白");auto key=trimmed(str(body["target_key"]));if(key.empty())key=type+":"+name+":"+std::to_string(std::time(nullptr));auto target=createTarget(db,owner,type,key,name,normalizeRank(str(body["rank"],"G")),body["data"].isObject()?body["data"]:Json::Value{Json::objectValue});Json::Value out;out["target"]=target;reply(cb,std::move(out));});});

  auto rerollHandler=[service,db](const Request&req,Callback&cb,const std::string&idText,bool targeted){
    runHandler(cb,[&]{
      const auto user=requireUser(service,req);const auto targetId=positiveId(idText);
      const auto rows=db->execSqlSync("SELECT row_to_json(t)::text AS json_text FROM affix_targets t WHERE id=$1",targetId);
      if(rows.empty())throw ApiError(drogon::k404NotFound,"找不到詞條目標");
      auto target=rowJson(rows);if(integer(target["owner_user_id"])!=user.id&&!user.isAdmin)throw ApiError(drogon::k403Forbidden,"不能洗煉其他玩家的物品");
      const auto owner=integer(target["owner_user_id"]);
      auto profile=profileFor(db,str(target["target_type"]),normalizeRank(str(target["rank"],"G")));
      if(profile.isNull())throw ApiError(drogon::k400BadRequest,"此類型／品階尚未設定洗煉規則");
      const auto pool=poolFor(db,profile,str(target["target_type"]),str(target["rank"]),target["data"]);
      if(!pool)throw ApiError(drogon::k400BadRequest,"尚未設定可用詞條池");
      auto body=bodyOrEmpty(req);auto current=arrayOrEmpty(target["current_affixes"]);
      const auto slot=integer(body["slot"],0);
      if(slot<0||slot>=static_cast<std::int64_t>(current.size()))throw ApiError(drogon::k400BadRequest,"要刷新的是既有一般詞條欄位");
      const auto code=str(profile[targeted?"targeted_currency_code":"random_currency_code"],"trait_core");
      const auto cost=std::max<std::int64_t>(0,integer(profile[targeted?"targeted_cost":"random_cost"],targeted?5:1));
      spendCurrency(db,owner,code,cost);
      try{
        std::int64_t next=0;
        if(targeted){
          next=integer(body["affix_id"]);if(next<=0)throw ApiError(drogon::k400BadRequest,"請指定詞條");
          const auto valid=db->execSqlSync(
            "SELECT 1 FROM affix_pool_entries e JOIN affix_definitions a ON a.id=e.affix_id "
            "JOIN affix_pools p ON p.id=e.pool_id WHERE e.pool_id=$1 AND e.affix_id=$2 "
            "AND e.active=TRUE AND a.active=TRUE AND a.affix_kind='normal' AND p.pool_kind='normal'",*pool,next);
          if(valid.empty())throw ApiError(drogon::k400BadRequest,"指定詞條不在此一般詞條池");
        }else{
          std::set<std::int64_t> excluded;
          for(Json::ArrayIndex i=0;i<current.size();++i)if(static_cast<std::int64_t>(i)!=slot)excluded.insert(integer(current[i]));
          next=pickAffixFromPool(db,*pool,excluded,"normal");
        }
        current[static_cast<Json::ArrayIndex>(slot)]=Json::Int64(next);
        db->execSqlSync("UPDATE affix_targets SET current_affixes=$1::jsonb,pool_id=$2,updated_at=CURRENT_TIMESTAMP WHERE id=$3",compactJson(current),*pool,targetId);
      }catch(...){refundCurrency(db,owner,code,cost);throw;}
      const auto updated=db->execSqlSync("SELECT row_to_json(t)::text AS json_text FROM affix_targets t WHERE id=$1",targetId);
      Json::Value out;out["target"]=expandTarget(db,rowJson(updated));out["currencies"]=currenciesFor(db,owner);reply(cb,std::move(out));
    },targeted?"指定詞條失敗":"刷新詞條失敗");
  };
  register1("/api/affix-targets/{1}/reroll",drogon::Post,[rerollHandler](const Request&req,Callback&cb,const std::string&id){rerollHandler(req,cb,id,false);});
  register1("/api/affix-targets/{1}/targeted",drogon::Post,[rerollHandler](const Request&req,Callback&cb,const std::string&id){rerollHandler(req,cb,id,true);});

  // [功能備註｜59-cpp.20 特殊詞條判定]
  // 判定成功只寫 special_affixes；current_affixes 完全不修改，因此不會吃掉一般詞條坑。
  register2("/api/affix-targets/{1}/special-roll/{2}",drogon::Post,
    [service,db](const Request&req,Callback&cb,const std::string&sourceText,const std::string&recipientText){
      runHandler(cb,[&]{
        const auto user=requireUser(service,req);const auto sourceId=positiveId(sourceText),recipientId=positiveId(recipientText);
        const auto sourceRows=db->execSqlSync("SELECT row_to_json(t)::text AS json_text FROM affix_targets t WHERE id=$1",sourceId);
        const auto recipientRows=db->execSqlSync("SELECT row_to_json(t)::text AS json_text FROM affix_targets t WHERE id=$1",recipientId);
        if(sourceRows.empty()||recipientRows.empty())throw ApiError(drogon::k404NotFound,"找不到來源裝備或特殊詞條接收目標");
        const auto source=rowJson(sourceRows),recipient=rowJson(recipientRows);
        const auto owner=integer(source["owner_user_id"]);
        if(owner!=integer(recipient["owner_user_id"]))throw ApiError(drogon::k400BadRequest,"來源與接收目標必須屬於同一玩家");
        if(owner!=user.id&&!user.isAdmin)throw ApiError(drogon::k403Forbidden,"不能替其他玩家進行特殊詞條判定");
        const auto body=bodyOrEmpty(req);const auto ruleId=integer(body["rule_id"]);
        if(ruleId<=0)throw ApiError(drogon::k400BadRequest,"缺少特殊詞條規則");
        const auto ruleRows=db->execSqlSync("SELECT row_to_json(r)::text AS json_text FROM special_affix_rules r WHERE id=$1 AND active=TRUE",ruleId);
        if(ruleRows.empty())throw ApiError(drogon::k404NotFound,"找不到可用特殊詞條規則");
        const auto rule=rowJson(ruleRows);
        const auto sourceType=str(source["target_type"]),sourceRank=str(source["rank"],"G"),sourceName=str(source["display_name"]);
        if(str(rule["source_target_type"],"*")!="*"&&str(rule["source_target_type"])!=sourceType)
          throw ApiError(drogon::k400BadRequest,"此規則不適用目前來源類型");
        if(str(rule["source_rank"],"*")!="*"&&str(rule["source_rank"])!=sourceRank)
          throw ApiError(drogon::k400BadRequest,"此規則不適用目前來源品階");
        if(!trimmed(str(rule["source_name"])).empty()&&trimmed(str(rule["source_name"]))!=sourceName)
          throw ApiError(drogon::k400BadRequest,"此規則只適用指定裝備／來源名稱");
        if(str(rule["recipient_target_type"],"puppet")!=str(recipient["target_type"]))
          throw ApiError(drogon::k400BadRequest,"接收目標類型不符合規則");
        if(rule.get("once_per_pair",true).asBool()){
          const auto prior=db->execSqlSync(
            "SELECT 1 FROM special_affix_roll_history WHERE rule_id=$1 AND source_target_id=$2 AND recipient_target_id=$3 LIMIT 1",
            ruleId,sourceId,recipientId);
          if(!prior.empty())throw ApiError(drogon::k409Conflict,"這件來源對此目標已完成過特殊詞條判定");
        }

        // [功能備註｜59-cpp.20 特殊詞條槽上限保護]
        // 槽位已滿時不消耗 once_per_pair 判定；池內可用唯一詞條不足 grant_count 時，保留已抽到的部分而不是整次失敗。
        auto current=arrayOrEmpty(recipient["special_affixes"]);
        std::set<std::int64_t> excluded;for(const auto& raw:current){const auto id=integer(raw);if(id>0)excluded.insert(id);}
        const auto ruleMax=std::max<std::int64_t>(0,integer(rule["max_special_affixes"],1));
        const auto targetMax=std::max<std::int64_t>(0,integer(recipient["max_special_affixes"],0));
        std::int64_t effectiveMax=ruleMax;
        if(targetMax>0)effectiveMax=effectiveMax>0?std::min(effectiveMax,targetMax):targetMax;
        const auto requested=std::clamp<std::int64_t>(integer(rule["grant_count"],1),1,20);
        const auto existingCount=static_cast<std::int64_t>(current.size());
        const auto availableSlots=effectiveMax>0?std::max<std::int64_t>(0,effectiveMax-existingCount):requested;
        const auto give=std::min(requested,availableSlots);
        if(give<=0)throw ApiError(drogon::k409Conflict,"特殊詞條已達上限");

        const auto success=rollPercent(clampedPercent(rule["chance_percent"]));
        Json::Value granted{Json::arrayValue};
        if(success){
          for(std::int64_t i=0;i<give;++i){
            try{
              const auto next=pickAffixFromPool(db,integer(rule["pool_id"]),excluded,"special");
              excluded.insert(next);current.append(Json::Int64(next));granted.append(Json::Int64(next));
            }catch(const ApiError&){
              if(granted.empty())throw;
              break;
            }
          }
          if(!granted.empty())db->execSqlSync(
            "UPDATE affix_targets SET special_affixes=$1::jsonb,max_special_affixes=CASE WHEN max_special_affixes>0 THEN max_special_affixes ELSE $2 END,updated_at=CURRENT_TIMESTAMP WHERE id=$3",
            compactJson(current),effectiveMax,recipientId);
        }
        db->execSqlSync(
          "INSERT INTO special_affix_roll_history(rule_id,source_target_id,recipient_target_id,success,granted_affixes) VALUES($1,$2,$3,$4,$5::jsonb)",
          ruleId,sourceId,recipientId,success,compactJson(granted));
        const auto updated=db->execSqlSync("SELECT row_to_json(t)::text AS json_text FROM affix_targets t WHERE id=$1",recipientId);
        Json::Value out;out["success"]=success;out["granted_affix_ids"]=granted;out["target"]=expandTarget(db,rowJson(updated));reply(cb,std::move(out));
      },"特殊詞條判定失敗");
    });

}

}  // namespace trpg
