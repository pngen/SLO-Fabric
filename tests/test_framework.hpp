#pragma once

// Minimal dependency-free test framework. Deterministic, no timeouts.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace slofabric_test {

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

class Registry {
 public:
  static Registry& instance() {
    static Registry r;
    return r;
  }
  void add(std::string name, std::function<void()> fn) {
    cases_.push_back(TestCase{std::move(name), std::move(fn)});
  }
  const std::vector<TestCase>& cases() const { return cases_; }

 private:
  std::vector<TestCase> cases_;
};

inline void register_test(std::string name, std::function<void()> fn) {
  Registry::instance().add(std::move(name), std::move(fn));
}

struct Failure {
  std::string file;
  int line = 0;
  std::string message;
};

inline std::vector<Failure>& failures() {
  static std::vector<Failure> f;
  return f;
}

inline int& assertion_count() {
  static int c = 0;
  return c;
}

inline void check(bool cond, const char* file, int line, std::string msg) {
  ++assertion_count();
  if (!cond) failures().push_back(Failure{file, line, std::move(msg)});
}

// Returns process exit code (0 = all pass).
inline int run_all(const char* filter = nullptr) {
  int passed = 0;
  int failed = 0;
  for (const auto& c : Registry::instance().cases()) {
    if (filter && c.name.find(filter) == std::string::npos) continue;
    std::size_t before = failures().size();
    try {
      c.fn();
    } catch (const std::exception& e) {
      failures().push_back(Failure{"<exception>", 0, std::string("unhandled exception: ") + e.what()});
    } catch (...) {
      failures().push_back(Failure{"<exception>", 0, "unhandled non-std exception"});
    }
    std::size_t after = failures().size();
    if (after == before) {
      ++passed;
      std::printf("[ PASS ] %s\n", c.name.c_str());
    } else {
      ++failed;
      std::printf("[ FAIL ] %s\n", c.name.c_str());
    }
  }
  std::printf("\n%d assertions, %d test(s) passed, %d test(s) failed\n",
              assertion_count(), passed, failed);
  for (const auto& f : failures()) {
    std::printf("   %s:%d: %s\n", f.file.c_str(), f.line, f.message.c_str());
  }
  return failed == 0 ? 0 : 1;
}

}  // namespace slofabric_test

#define SLOFABRIC_CONCAT_IMPL(a, b) a##b
#define SLOFABRIC_CONCAT(a, b) SLOFABRIC_CONCAT_IMPL(a, b)

#define TEST(name)                                                       \
  static void SLOFABRIC_CONCAT(slofabric_test_func_, __LINE__)();        \
  static struct SLOFABRIC_CONCAT(slofabric_test_reg_, __LINE__) {        \
    SLOFABRIC_CONCAT(slofabric_test_reg_, __LINE__)() {                  \
      ::slofabric_test::register_test(name,                              \
          SLOFABRIC_CONCAT(slofabric_test_func_, __LINE__));             \
    }                                                                    \
  } SLOFABRIC_CONCAT(slofabric_test_reg_inst_, __LINE__);                \
  static void SLOFABRIC_CONCAT(slofabric_test_func_, __LINE__)()

#define CHECK(cond) ::slofabric_test::check((cond), __FILE__, __LINE__, "CHECK failed: " #cond)
#define CHECK_TRUE(cond) ::slofabric_test::check((cond), __FILE__, __LINE__, "CHECK_TRUE failed: " #cond)
#define CHECK_FALSE(cond) ::slofabric_test::check(!(cond), __FILE__, __LINE__, "CHECK_FALSE failed: " #cond)
#define CHECK_EQ(a, b) ::slofabric_test::check_eq_impl((a), (b), __FILE__, __LINE__, #a " == " #b)
#define CHECK_NE(a, b) ::slofabric_test::check((!((a) == (b))), __FILE__, __LINE__, #a " != " #b)
#define REQUIRE(cond) do { if (!(cond)) { ::slofabric_test::check(false, __FILE__, __LINE__, "REQUIRE failed: " #cond); return; } } while (0)

namespace slofabric_test {

template <typename A, typename B>
void check_eq_impl(const A& a, const B& b, const char* file, int line, const char* expr) {
  ++assertion_count();
  if (!(a == b)) {
    failures().push_back(Failure{file, line, std::string(expr) + " (see above)"});
  }
}

}  // namespace slofabric_test
