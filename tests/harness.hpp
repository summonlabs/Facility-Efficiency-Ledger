#pragma once

// Minimal deterministic test harness.
//
// Deliberately dependency-free: the runtime claims to be buildable with a
// portable C++20 compiler and no network access, and the tests must be provable
// under exactly those conditions. There are no timeouts anywhere: a hang is a
// defect to diagnose, not something to paper over.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <ostream>
#include <sstream>
#include <type_traits>
#include <string>
#include <vector>

namespace feltest {

struct TestCase {
    std::string suite;
    std::string name;
    void (*body)();
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const char* suite, const char* name, void (*body)());
};

struct Failure {
    std::string message;
};

[[noreturn]] void fail(const std::string& message);

void check(bool condition, const std::string& expression, const char* file, int line);

// Renders a value for a failure message when a stream operator exists, and falls
// back to a placeholder otherwise. The rendered text is only ever used for the
// message: check_equal always throws when the values differ, so an unprintable
// type can never turn a real mismatch into a silent pass.
template <class T, class = void>
struct has_stream_operator : std::false_type {};

template <class T>
struct has_stream_operator<
    T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <class T>
std::string describe_value(const T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_arithmetic_v<T>) {
        return std::to_string(value);
    } else if constexpr (has_stream_operator<T>::value) {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    } else {
        return "<value>";
    }
}

template <class T, class U>
void check_equal(const T& left, const U& right, const std::string& lexpr, const std::string& rexpr,
                 const char* file, int line) {
    if (!(left == right)) {
        throw Failure{"check failed: " + lexpr + " == " + rexpr + " (" + describe_value(left) +
                      " vs " + describe_value(right) + ") at " + file + ":" +
                      std::to_string(line)};
    }
}

int run_all(int argc, char** argv);

// ---------------------------------------------------------------- utilities

// Creates a directory under the system temporary directory that is removed when
// the returned guard is destroyed.
class TempDir {
public:
    explicit TempDir(const std::string& label);
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }
    std::filesystem::path file(const std::string& name) const { return path_ / name; }
    void keep() noexcept { keep_ = true; }

private:
    std::filesystem::path path_{};
    bool keep_{false};
};

// Runs a child process and returns its exit code. There is no timeout: the child
// is expected to terminate on its own.
int run_process(const std::filesystem::path& executable, const std::vector<std::string>& arguments);

// A child that runs concurrently with the test process.
struct ChildProcess {
    void* handle{nullptr};
    long identifier{0};
};

ChildProcess spawn_process(const std::filesystem::path& executable,
                           const std::vector<std::string>& arguments);
int wait_process(ChildProcess& child);

// Waits, with a bounded number of polls, until a marker file appears. Exceeding
// the bound is a defect and fails the test rather than hanging.
void wait_for_file(const std::filesystem::path& path, const std::string& what);

std::string read_text(const std::filesystem::path& path);
void write_text(const std::filesystem::path& path, const std::string& text);

std::filesystem::path helper_executable();

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path);
void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes);
void flip_bit(const std::filesystem::path& path, std::uint64_t offset, unsigned bit);

std::string repeat(char value, std::size_t count);

}  // namespace feltest

#define FEL_TEST(suite, name)                                                      \
    void suite##_##name();                                                         \
    static ::feltest::Registrar fel_registrar_##suite##_##name(#suite, #name,       \
                                                               &suite##_##name);   \
    void suite##_##name()

#define CHECK(expression) ::feltest::check((expression), #expression, __FILE__, __LINE__)

#define CHECK_EQ(left, right)     ::feltest::check_equal((left), (right), #left, #right, __FILE__, __LINE__)

#define REQUIRE(expression)                                                          \
    do {                                                                             \
        if (!(expression)) {                                                         \
            ::feltest::fail(std::string{"required: "} + #expression + " at " +        \
                            __FILE__ + ":" + std::to_string(__LINE__));               \
        }                                                                            \
    } while (false)
