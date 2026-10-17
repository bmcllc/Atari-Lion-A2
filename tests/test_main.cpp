#include "test.h"

namespace test {
std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}
int failures = 0;
}  // namespace test

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0;
  for (const auto& c : test::registry()) {
    if (filter && !std::string(c.name).contains(filter)) continue;
    const int before = test::failures;
    c.fn();
    ++run;
    std::printf("%s %s\n", test::failures == before ? "ok  " : "FAIL", c.name);
  }
  std::printf("\n%d tests, %d failed checks\n", run, test::failures);
  return test::failures ? 1 : 0;
}
