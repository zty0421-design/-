#include "trpg/Security.h"

#include <cassert>
#include <iostream>
#include <string>

int main() {
  using trpg::JwtClaims;
  constexpr auto secret = "0123456789abcdef0123456789abcdef";

  const auto hash = trpg::hashPassword("correct-horse", 4);
  assert(hash.rfind("$2", 0) == 0);
  assert(trpg::verifyPassword("correct-horse", hash));
  assert(!trpg::verifyPassword("wrong-password", hash));

  const JwtClaims source{42, "測試_user-1", true, 1'700'000'000, 1'700'000'600};
  const auto token = trpg::signJwt(source, secret);
  const auto claims = trpg::verifyJwt(token, secret, 1'700'000'010);
  assert(claims.has_value());
  assert(claims->id == 42);
  assert(claims->username == "測試_user-1");
  assert(claims->isAdmin);
  assert(!trpg::verifyJwt(token + "x", secret, 1'700'000'010));
  assert(!trpg::verifyJwt(token, secret, 1'700'000'700));

  auto changed = token;
  changed[changed.size() / 2] = changed[changed.size() / 2] == 'a' ? 'b' : 'a';
  assert(!trpg::verifyJwt(changed, secret, 1'700'000'010));

  std::cout << "security tests passed\n";
  return 0;
}
