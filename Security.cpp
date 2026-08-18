#include "trpg/Security.h"

#include <crypt.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace trpg {
namespace {

std::string base64UrlEncode(const unsigned char* input, std::size_t size) {
  if (size == 0) {
    return {};
  }
  std::string output(4 * ((size + 2) / 3), '\0');
  const int written = EVP_EncodeBlock(
      reinterpret_cast<unsigned char*>(output.data()), input,
      static_cast<int>(size));
  if (written < 0) {
    throw std::runtime_error("base64 encoding failed");
  }
  output.resize(static_cast<std::size_t>(written));
  for (auto& ch : output) {
    if (ch == '+') ch = '-';
    if (ch == '/') ch = '_';
  }
  while (!output.empty() && output.back() == '=') output.pop_back();
  return output;
}

std::optional<std::vector<unsigned char>> base64UrlDecode(std::string input) {
  for (auto& ch : input) {
    if (ch == '-') ch = '+';
    if (ch == '_') ch = '/';
  }
  if (input.size() % 4 == 1) return std::nullopt;
  while (input.size() % 4 != 0) input.push_back('=');

  std::vector<unsigned char> output(3 * input.size() / 4 + 1);
  const int decoded = EVP_DecodeBlock(output.data(),
                                      reinterpret_cast<const unsigned char*>(input.data()),
                                      static_cast<int>(input.size()));
  if (decoded < 0) return std::nullopt;
  std::size_t padding = 0;
  if (!input.empty() && input.back() == '=') ++padding;
  if (input.size() > 1 && input[input.size() - 2] == '=') ++padding;
  output.resize(static_cast<std::size_t>(decoded) - padding);
  return output;
}

std::string base64UrlEncode(std::string_view input) {
  return base64UrlEncode(reinterpret_cast<const unsigned char*>(input.data()),
                         input.size());
}

std::string jsonEscape(std::string_view input) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(input.size() + 8);
  for (const unsigned char ch : input) {
    switch (ch) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (ch < 0x20) {
          result += "\\u00";
          result.push_back(hex[(ch >> 4) & 0x0f]);
          result.push_back(hex[ch & 0x0f]);
        } else {
          result.push_back(static_cast<char>(ch));
        }
    }
  }
  return result;
}

std::optional<std::size_t> valuePosition(std::string_view json,
                                         std::string_view key) {
  const std::string marker = "\"" + std::string{key} + "\"";
  auto pos = json.find(marker);
  if (pos == std::string_view::npos) return std::nullopt;
  pos = json.find(':', pos + marker.size());
  if (pos == std::string_view::npos) return std::nullopt;
  ++pos;
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
    ++pos;
  }
  return pos;
}

std::optional<std::string> jsonString(std::string_view json,
                                      std::string_view key) {
  const auto startValue = valuePosition(json, key);
  if (!startValue || *startValue >= json.size() || json[*startValue] != '"') {
    return std::nullopt;
  }
  std::string result;
  for (std::size_t pos = *startValue + 1; pos < json.size(); ++pos) {
    char ch = json[pos];
    if (ch == '"') return result;
    if (ch != '\\') {
      result.push_back(ch);
      continue;
    }
    if (++pos >= json.size()) return std::nullopt;
    switch (json[pos]) {
      case '"': result.push_back('"'); break;
      case '\\': result.push_back('\\'); break;
      case '/': result.push_back('/'); break;
      case 'b': result.push_back('\b'); break;
      case 'f': result.push_back('\f'); break;
      case 'n': result.push_back('\n'); break;
      case 'r': result.push_back('\r'); break;
      case 't': result.push_back('\t'); break;
      default: return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<std::int64_t> jsonInteger(std::string_view json,
                                        std::string_view key) {
  const auto startValue = valuePosition(json, key);
  if (!startValue || *startValue >= json.size()) return std::nullopt;
  std::size_t pos = *startValue;
  const bool quoted = json[pos] == '"';
  if (quoted) ++pos;
  bool negative = false;
  if (pos < json.size() && json[pos] == '-') {
    negative = true;
    ++pos;
  }
  if (pos >= json.size() || !std::isdigit(static_cast<unsigned char>(json[pos]))) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
    const int digit = json[pos++] - '0';
    if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
      return std::nullopt;
    }
    value = value * 10 + digit;
  }
  if (quoted && (pos >= json.size() || json[pos] != '"')) return std::nullopt;
  return negative ? -value : value;
}

std::optional<bool> jsonBoolean(std::string_view json, std::string_view key) {
  const auto pos = valuePosition(json, key);
  if (!pos) return std::nullopt;
  if (json.substr(*pos, 4) == "true") return true;
  if (json.substr(*pos, 5) == "false") return false;
  return std::nullopt;
}

std::array<unsigned char, EVP_MAX_MD_SIZE> hmacSha256(std::string_view data,
                                                       std::string_view secret,
                                                       unsigned int& size) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  if (HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
           reinterpret_cast<const unsigned char*>(data.data()), data.size(),
           digest.data(), &size) == nullptr) {
    throw std::runtime_error("HMAC calculation failed");
  }
  return digest;
}

bool constantTimeEquals(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  return CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

}  // namespace

std::int64_t unixNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string hashPassword(const std::string& password, unsigned cost) {
  if (password.size() < 6) {
    throw std::invalid_argument("password must contain at least 6 characters");
  }
  if (password.size() > 72 || password.find('\0') != std::string::npos) {
    throw std::invalid_argument("password is not valid for bcrypt");
  }
  if (cost < 4 || cost > 15) {
    throw std::invalid_argument("bcrypt cost must be between 4 and 15");
  }
  std::array<char, 16> entropy{};
  if (RAND_bytes(reinterpret_cast<unsigned char*>(entropy.data()), entropy.size()) != 1) {
    throw std::runtime_error("secure random generation failed");
  }
  std::array<char, CRYPT_GENSALT_OUTPUT_SIZE> salt{};
  if (crypt_gensalt_rn("$2b$", cost, entropy.data(), entropy.size(), salt.data(),
                       salt.size()) == nullptr) {
    throw std::runtime_error("bcrypt salt generation failed");
  }
  crypt_data data{};
  const char* encoded = crypt_r(password.c_str(), salt.data(), &data);
  if (encoded == nullptr) throw std::runtime_error("password hashing failed");
  return encoded;
}

bool verifyPassword(const std::string& password, const std::string& encodedHash) {
  if (password.size() > 72 || password.find('\0') != std::string::npos ||
      encodedHash.size() < 20 || encodedHash.rfind("$2", 0) != 0) {
    return false;
  }
  crypt_data data{};
  const char* candidate = crypt_r(password.c_str(), encodedHash.c_str(), &data);
  return candidate != nullptr && constantTimeEquals(candidate, encodedHash);
}

std::string signJwt(const JwtClaims& sourceClaims, const std::string& secret) {
  if (secret.size() < 32) throw std::invalid_argument("JWT secret is too short");
  JwtClaims claims = sourceClaims;
  if (claims.issuedAt <= 0) claims.issuedAt = unixNow();
  if (claims.expiresAt <= claims.issuedAt) claims.expiresAt = claims.issuedAt + 604800;
  const std::string header = R"({"alg":"HS256","typ":"JWT"})";
  const std::string payload =
      "{\"id\":" + std::to_string(claims.id) +
      ",\"username\":\"" + jsonEscape(claims.username) +
      "\",\"is_admin\":" + (claims.isAdmin ? "true" : "false") +
      ",\"iat\":" + std::to_string(claims.issuedAt) +
      ",\"exp\":" + std::to_string(claims.expiresAt) + "}";
  const std::string unsignedToken = base64UrlEncode(header) + "." + base64UrlEncode(payload);
  unsigned int digestSize = 0;
  const auto digest = hmacSha256(unsignedToken, secret, digestSize);
  return unsignedToken + "." + base64UrlEncode(digest.data(), digestSize);
}

std::optional<JwtClaims> verifyJwt(const std::string& token,
                                   const std::string& secret,
                                   std::int64_t now) {
  try {
    if (secret.size() < 32) return std::nullopt;
    const auto first = token.find('.');
    const auto second = first == std::string::npos ? std::string::npos
                                                   : token.find('.', first + 1);
    if (first == std::string::npos || second == std::string::npos ||
        token.find('.', second + 1) != std::string::npos) {
      return std::nullopt;
    }
    const auto headerBytes = base64UrlDecode(token.substr(0, first));
    const auto payloadBytes = base64UrlDecode(token.substr(first + 1, second - first - 1));
    const auto signature = base64UrlDecode(token.substr(second + 1));
    if (!headerBytes || !payloadBytes || !signature || signature->size() != 32) {
      return std::nullopt;
    }
    const std::string header(headerBytes->begin(), headerBytes->end());
    if (jsonString(header, "alg") != std::optional<std::string>{"HS256"}) {
      return std::nullopt;
    }
    unsigned int digestSize = 0;
    const std::string unsignedToken = token.substr(0, second);
    const auto digest = hmacSha256(unsignedToken, secret, digestSize);
    if (digestSize != signature->size() ||
        CRYPTO_memcmp(digest.data(), signature->data(), signature->size()) != 0) {
      return std::nullopt;
    }

    const std::string payload(payloadBytes->begin(), payloadBytes->end());
    const auto id = jsonInteger(payload, "id");
    const auto username = jsonString(payload, "username");
    const auto isAdmin = jsonBoolean(payload, "is_admin");
    const auto issuedAt = jsonInteger(payload, "iat");
    const auto expiresAt = jsonInteger(payload, "exp");
    if (!id || *id <= 0 || !username || username->empty() || !isAdmin ||
        !issuedAt || !expiresAt) {
      return std::nullopt;
    }
    if (now <= 0) now = unixNow();
    if (*expiresAt <= now || *issuedAt > now + 300 || *expiresAt <= *issuedAt) {
      return std::nullopt;
    }
    return JwtClaims{*id, *username, *isAdmin, *issuedAt, *expiresAt};
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace trpg
