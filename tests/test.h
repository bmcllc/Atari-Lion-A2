#pragma once
// Tiny self-registering test harness.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "m88kasm.h"
#include "ppcasm.h"
#include "system.h"

namespace test {

struct Case {
  const char* name;
  std::function<void()> fn;
};
std::vector<Case>& registry();
extern int failures;

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

// Runs Tuffy code assembled at `a` until it reaches the label "halt".
inline void run_tuffy(lion::Lion& sys, ppcasm::Asm& a, u64 max_cycles = 1'000'000) {
  auto code = a.finish();
  std::copy(code.begin(), code.end(), sys.bus.host_ptr(a.origin()));
  sys.tuffy.reset(a.origin(), lion::Lion::kBootMsr);
  sys.tuffy.gpr[1] = lion::Lion::kBootStack;
  const u32 halt = a.addr("halt");
  for (u64 n = 0; n < max_cycles && sys.tuffy.pc != halt; n += 1) sys.tuffy.run(1);
}

}  // namespace test

#define TEST_CAT2(a, b) a##b
#define TEST_CAT(a, b) TEST_CAT2(a, b)
#define TEST(name)                                                          \
  static void name();                                                       \
  static ::test::Registrar TEST_CAT(reg_, name)(#name, name);               \
  static void name()

#define CHECK(cond)                                                                  \
  do {                                                                               \
    if (!(cond)) {                                                                   \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
      ++::test::failures;                                                            \
    }                                                                                \
  } while (0)

#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    auto va_ = (a);                                                                                 \
    auto vb_ = (b);                                                                                 \
    if (!(va_ == vb_)) {                                                                            \
      std::printf("  FAIL %s:%d: %s == %s (0x%llX vs 0x%llX)\n", __FILE__, __LINE__, #a, #b,        \
                  (unsigned long long)va_, (unsigned long long)vb_);                                \
      ++::test::failures;                                                                           \
    }                                                                                               \
  } while (0)
