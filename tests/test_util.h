// Minimal, dependency-free test harness for host-side unit tests of
// FreeSWITCH-independent logic. No external framework (the build is offline and
// we do not want to vendor one). Usage:
//
//   #include "test_util.h"
//   TEST(name) { CHECK(cond); CHECK_EQ(a, b); }
//   int main() { return tu::run(); }
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace tu {

struct Case { std::string name; std::function<void()> fn; };

inline std::vector<Case>& cases() { static std::vector<Case> c; return c; }
inline int& failures() { static int f = 0; return f; }

struct Reg { Reg(const std::string& n, std::function<void()> fn) { cases().push_back({n, fn}); } };

inline void fail(const char* file, int line, const std::string& msg) {
  failures()++;
  std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, msg.c_str());
}

inline int run() {
  int passed = 0;
  for (auto& c : cases()) {
    int before = failures();
    c.fn();
    if (failures() == before) { std::printf("  ok   %s\n", c.name.c_str()); passed++; }
    else                      { std::printf("  bad  %s\n", c.name.c_str()); }
  }
  std::printf("%d/%zu test(s) passed\n", passed, cases().size());
  return failures() == 0 ? 0 : 1;
}

} // namespace tu

#define TU_CONCAT2(a, b) a##b
#define TU_CONCAT(a, b) TU_CONCAT2(a, b)
#define TEST(name)                                                            \
  static void TU_CONCAT(tu_case_, __LINE__)();                                \
  static tu::Reg TU_CONCAT(tu_reg_, __LINE__)(name, TU_CONCAT(tu_case_, __LINE__)); \
  static void TU_CONCAT(tu_case_, __LINE__)()

#define CHECK(cond) \
  do { if (!(cond)) tu::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); } while (0)

#define CHECK_EQ(a, b) \
  do { auto va = (a); auto vb = (b); if (!(va == vb)) \
       tu::fail(__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b ")"); } while (0)

#endif // TEST_UTIL_H
