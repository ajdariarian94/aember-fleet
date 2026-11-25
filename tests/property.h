// Property-based tests without a framework (the SDK has none): a property
// is checked on many generated cases, each from its own seed.
//
//   ForAll("name", 200, [](Gen& g) { ... CHECK(...) ... });
//
// The seeds differ from run to run (more cases over time); a failing case
// prints the seed that reproduces it alone:
//
//   AEMBER_PROPERTY_SEED=<seed> AEMBER_PROPERTY_RUNS=1 <test binary>
//
// AEMBER_PROPERTY_RUNS=N also runs every property N times (longer local
// runs). No shrinking: generators keep sizes small, so a failing case is
// small already.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "check.h"

namespace property {

class Gen {
 public:
  explicit Gen(std::uint64_t seed) : rng_(seed) {}

  /// Uniform in [lo, hi].
  int Int(int lo, int hi) {
    return std::uniform_int_distribution<int>{lo, hi}(rng_);
  }
  bool Chance(double p) { return std::bernoulli_distribution{p}(rng_); }
  /// A size in [0, max], small ones more often (edge cases live there).
  int Size(int max) {
    return Chance(0.5) ? Int(0, std::min(max, 4)) : Int(0, max);
  }
  std::string Bytes(int max) {
    std::string s(static_cast<std::size_t>(Size(max)), '\0');
    for (auto& c : s) c = static_cast<char>(Int(0, 255));
    return s;
  }
  /// Characters from @p alphabet, length in [min, max].
  std::string Text(std::string_view alphabet, int min, int max) {
    std::string s(static_cast<std::size_t>(Int(min, max)), '\0');
    for (auto& c : s)
      c = alphabet[static_cast<std::size_t>(
          Int(0, static_cast<int>(alphabet.size()) - 1))];
    return s;
  }
  template <class T>
  const T& Pick(const std::vector<T>& v) {
    return v[static_cast<std::size_t>(Int(0, static_cast<int>(v.size()) - 1))];
  }

 private:
  std::mt19937_64 rng_;
};

inline std::uint64_t BaseSeed() {
  static const std::uint64_t seed = []() -> std::uint64_t {
    if (const char* s = std::getenv("AEMBER_PROPERTY_SEED"))
      return std::strtoull(s, nullptr, 10);
    return (std::uint64_t{std::random_device{}()} << 32) |
           std::random_device{}();
  }();
  return seed;
}

/// Checks @p property on @p runs cases (AEMBER_PROPERTY_RUNS overrides);
/// stops at the first failing case and prints its seed.
template <class F>
void ForAll(const char* name, int runs, F&& property) {
  if (const char* r = std::getenv("AEMBER_PROPERTY_RUNS")) runs = std::atoi(r);
  for (int i = 0; i < runs; ++i) {
    const std::uint64_t seed = BaseSeed() + static_cast<std::uint64_t>(i);
    const int before = check::failures();
    Gen g{seed};
    property(g);
    if (check::failures() != before) {
      std::cerr << "property '" << name << "' failed; reproduce with "
                << "AEMBER_PROPERTY_SEED=" << seed
                << " AEMBER_PROPERTY_RUNS=1\n";
      return;
    }
  }
}

}  // namespace property
