#pragma once
// Minimal self-contained test harness used by every layer of the suite.
// Each translation unit registers cases; the binary can list or run them.
// No external dependency is used anywhere in the test tree.

#include <cstdint>
#include <cstdio>
#include <type_traits>
#include <vector>
#include <cstring>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

namespace rudp_test {

struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

struct CheckFailure : std::exception {
  std::string message;
  explicit CheckFailure(std::string msg) : message(std::move(msg)) {}
  const char* what() const noexcept override { return message.c_str(); }
};

template <typename T, typename Enable = void>
struct Repr {
  static std::string get(const T& value) {
    std::ostringstream os;
    os << value;
    return os.str();
  }
};
template <typename T>
struct Repr<T, std::enable_if_t<std::is_enum_v<T>>> {
  static std::string get(const T& value) {
    return std::to_string(static_cast<long long>(value));
  }
};
template <typename T>
std::string repr(const T& value) {
  return Repr<T>::get(value);
}
template <typename T>
std::string repr(const std::vector<T>& value) {
  std::string out = "[";
  for (const auto& v : value) {
    out += repr(v);
    out += ",";
  }
  out += "]";
  return out;
}
inline std::string repr(std::byte b) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned>(b));
  return buf;
}

inline void fail(const std::string& what) { throw CheckFailure(what); }

#define RUDP_CHECK(cond)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      ::rudp_test::fail(std::string("check failed: ") + #cond + " at " +     \
                        __FILE__ + ":" + std::to_string(__LINE__));          \
    }                                                                        \
  } while (false)

#define RUDP_CHECK_EQ(a, b)                                                  \
  do {                                                                       \
    auto va_ = (a);                                                          \
    auto vb_ = (b);                                                          \
    if (!(va_ == vb_)) {                                                     \
      ::rudp_test::fail(std::string("check_eq failed: ") + #a + " == " + #b +\
                        " lhs=" + ::rudp_test::repr(va_) +                    \
                        " rhs=" + ::rudp_test::repr(vb_) + " at " + __FILE__ +\
                        ":" + std::to_string(__LINE__));                      \
    }                                                                        \
  } while (false)

#define RUDP_CHECK_NE(a, b)                                                  \
  do {                                                                       \
    if ((a) == (b)) {                                                        \
      ::rudp_test::fail(std::string("check_ne failed: ") + #a + " != " + #b +\
                        " at " + __FILE__ + ":" + std::to_string(__LINE__));  \
    }                                                                        \
  } while (false)

#define RUDP_CHECK_GT(a, b)                                                  \
  do {                                                                       \
    if (!((a) > (b))) {                                                      \
      ::rudp_test::fail(std::string("check_gt failed: ") + #a + " > " + #b + \
                        " lhs=" + ::rudp_test::repr(a) +                      \
                        " rhs=" + ::rudp_test::repr(b) + " at " + __FILE__ +  \
                        ":" + std::to_string(__LINE__));                      \
    }                                                                        \
  } while (false)

#define RUDP_CHECK_GE(a, b)                                                  \
  do {                                                                       \
    if (!((a) >= (b))) {                                                     \
      ::rudp_test::fail(std::string("check_ge failed: ") + #a + " >= " + #b +\
                        " at " + __FILE__ + ":" + std::to_string(__LINE__));  \
    }                                                                        \
  } while (false)

#define RUDP_CHECK_LE(a, b)                                                  \
  do {                                                                       \
    if (!((a) <= (b))) {                                                     \
      ::rudp_test::fail(std::string("check_le failed: ") + #a + " <= " + #b +\
                        " at " + __FILE__ + ":" + std::to_string(__LINE__));  \
    }                                                                        \
  } while (false)

#define RUDP_TEST(name)                                                  \
  static void name();                                                    \
  static ::rudp_test::Registrar registrar_##name(#name, name);           \
  static void name()

inline int run_case(const std::string& name) {
  for (const auto& c : registry()) {
    if (name == c.name) {
      try {
        c.fn();
        std::fprintf(stdout, "[PASS] %s\n", c.name);
        std::fflush(stdout);
        return 0;
      } catch (const std::exception& e) {
        std::fprintf(stdout, "[FAIL] %s: %s\n", c.name, e.what());
        std::fflush(stdout);
        return 1;
      }
    }
  }
  std::fprintf(stdout, "[NOCASE] %s\n", name.c_str());
  std::fflush(stdout);
  return 2;
}

inline int run_all() {
  int failures = 0;
  for (const auto& c : registry()) {
    try {
      c.fn();
      std::fprintf(stdout, "[PASS] %s\n", c.name);
    } catch (const std::exception& e) {
      ++failures;
      std::fprintf(stdout, "[FAIL] %s: %s\n", c.name, e.what());
    }
    std::fflush(stdout);
  }
  std::fprintf(stdout, "summary: %zu cases, %d failures\n", registry().size(),
               failures);
  std::fflush(stdout);
  return failures == 0 ? 0 : 1;
}

// Entry point shared by every test binary.
inline int main_dispatch(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (!args.empty() && args[0] == "--list") {
    for (const auto& c : registry()) std::fprintf(stdout, "%s\n", c.name);
    return 0;
  }
  if (!args.empty() && args[0] == "--case") {
    if (args.size() < 2) {
      std::fprintf(stderr, "usage: %s --case NAME\n", argv[0]);
      return 2;
    }
    return run_case(args[1]);
  }
  return run_all();
}

}  // namespace rudp_test
