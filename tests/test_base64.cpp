// Host unit tests for modules/mod_*/base64.hpp (drachtio::base64_encode/decode).
// base64.hpp is pure, header-only, and FreeSWITCH-independent.
#include "test_util.h"
#include "base64.hpp"

using drachtio::base64_encode;
using drachtio::base64_decode;

// RFC 4648 §10 test vectors.
TEST("rfc4648 encode vectors") {
  CHECK_EQ(base64_encode(std::string("")),       std::string(""));
  CHECK_EQ(base64_encode(std::string("f")),      std::string("Zg=="));
  CHECK_EQ(base64_encode(std::string("fo")),     std::string("Zm8="));
  CHECK_EQ(base64_encode(std::string("foo")),    std::string("Zm9v"));
  CHECK_EQ(base64_encode(std::string("foob")),   std::string("Zm9vYg=="));
  CHECK_EQ(base64_encode(std::string("fooba")),  std::string("Zm9vYmE="));
  CHECK_EQ(base64_encode(std::string("foobar")), std::string("Zm9vYmFy"));
}

TEST("rfc4648 decode vectors") {
  CHECK_EQ(base64_decode(std::string("")),         std::string(""));
  CHECK_EQ(base64_decode(std::string("Zg==")),     std::string("f"));
  CHECK_EQ(base64_decode(std::string("Zm8=")),     std::string("fo"));
  CHECK_EQ(base64_decode(std::string("Zm9v")),     std::string("foo"));
  CHECK_EQ(base64_decode(std::string("Zm9vYg==")), std::string("foob"));
  CHECK_EQ(base64_decode(std::string("Zm9vYmE=")), std::string("fooba"));
  CHECK_EQ(base64_decode(std::string("Zm9vYmFy")), std::string("foobar"));
}

TEST("round-trip over all byte values") {
  std::string raw;
  for (int i = 0; i < 256; i++) raw.push_back(static_cast<char>(i));
  CHECK_EQ(base64_decode(base64_encode(raw)), raw);
}

TEST("encode from buffer + length matches string overload") {
  const unsigned char buf[] = {0x00, 0xff, 0x10, 0x80, 0x7f};
  std::string a = base64_encode(buf, sizeof(buf));
  std::string b = base64_encode(std::string(reinterpret_cast<const char*>(buf), sizeof(buf)));
  CHECK_EQ(a, b);
  CHECK_EQ(base64_decode(a), std::string(reinterpret_cast<const char*>(buf), sizeof(buf)));
}

int main() { return tu::run(); }
