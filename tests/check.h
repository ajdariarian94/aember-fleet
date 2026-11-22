// Minimal test helpers (the SDK has no test framework): CHECK records a
// failure and goes on, the test's main returns Failures().
#pragma once

#include <iostream>

namespace check {
inline int& failures() {
  static int n = 0;
  return n;
}
}  // namespace check

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ")\n"; \
      ++check::failures();                                                \
    }                                                                     \
  } while (0)

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    const auto check_a_ = (a);                                             \
    const auto check_b_ = (b);                                             \
    if (!(check_a_ == check_b_)) {                                         \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK_EQ(" #a ", " #b \
                << ")\n";                                                  \
      ++check::failures();                                                 \
    }                                                                      \
  } while (0)

inline int Failures() {
  if (check::failures() == 0) std::cerr << "ok\n";
  return check::failures() == 0 ? 0 : 1;
}
