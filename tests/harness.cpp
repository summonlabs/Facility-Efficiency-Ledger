#include "harness.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace feltest {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

Registrar::Registrar(const char* suite, const char* name, void (*body)()) {
    registry().push_back(TestCase{suite, name, body});
}

void fail(const std::string& message) { throw Failure{message}; }

void check(bool condition, const std::string& expression, const char* file, int line) {
    if (!condition) {
        throw Failure{"check failed: " + expression + " at " + file + ":" +
                      std::to_string(line)};
    }
}

void check_equal_text(const std::string& left, const std::string& right, const std::string& lexpr,
                      const std::string& rexpr, const char* file, int line) {
    if (left != right) {
        throw Failure{"check failed: " + lexpr + " == " + rexpr + " (" + left + " vs " + right +
                      ") at " + file + ":" + std::to_string(line)};
    }
}

TempDir::TempDir(const std::string& label) {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t id = counter.fetch_add(1);
#ifdef _WIN32
    const unsigned long process_id = ::GetCurrentProcessId();
#else
    const unsigned long process_id = static_cast<unsigned long>(::getpid());
#endif
    std::error_code error;
    for (int attempt = 0; attempt < 64; ++attempt) {
        const std::string name = "fel-test-" + label + "-" + std::to_string(process_id) + "-" +
                                 std::to_string(id) + "-" + std::to_string(attempt);
        path_ = std::filesystem::temp_directory_path(error) / name;
        if (error) {
            path_ = std::filesystem::path{name};
        }
        if (std::filesystem::create_directories(path_, error) && !error) {
            return;
        }
        error.clear();
    }
    throw Failure{"cannot create a temporary directory for the test"};
}

TempDir::~TempDir() {
    if (keep_) {
        return;
    }
    std::error_code error;
    std::filesystem::remove_all(path_, error);
}

std::filesystem::path helper_executable() {
    // Resolved at build time so the test never depends on the working directory.
    return std::filesystem::path{FEL_HELPER_PATH};
}

int run_process(const std::filesystem::path& executable, const std::vector<std::string>& arguments) {
#ifdef _WIN32
    std::string command = "\"" + executable.string() + "\"";
    for (const std::string& argument : arguments) {
        command.append(" \"");
        command.append(argument);
        command.append("\"");
    }
    std::vector<wchar_t> wide(command.size() + 1);
    ::MultiByteToWideChar(CP_UTF8, 0, command.c_str(), static_cast<int>(command.size()),
                          wide.data(), static_cast<int>(wide.size()));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    if (::CreateProcessW(nullptr, wide.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                         &startup, &information) == 0) {
        return -1;
    }
    ::WaitForSingleObject(information.hProcess, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(information.hProcess, &code);
    ::CloseHandle(information.hThread);
    ::CloseHandle(information.hProcess);
    return static_cast<int>(code);
#else
    std::vector<std::string> storage;
    storage.push_back(executable.string());
    for (const std::string& argument : arguments) {
        storage.push_back(argument);
    }
    std::vector<char*> argv;
    for (std::string& value : storage) {
        argv.push_back(value.data());
    }
    argv.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::execv(executable.c_str(), argv.data());
        ::_exit(127);
    }
    if (pid < 0) {
        return -1;
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

namespace {

#ifdef _WIN32
std::vector<wchar_t> to_wide(const std::string& text) {
    std::vector<wchar_t> wide(text.size() + 1);
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(),
                          static_cast<int>(wide.size()));
    return wide;
}

std::string build_command_line(const std::filesystem::path& executable,
                               const std::vector<std::string>& arguments) {
    std::string command = "\"" + executable.string() + "\"";
    for (const std::string& argument : arguments) {
        command.append(" \"");
        command.append(argument);
        command.append("\"");
    }
    return command;
}
#endif

}  // namespace

ChildProcess spawn_process(const std::filesystem::path& executable,
                           const std::vector<std::string>& arguments) {
    ChildProcess child;
#ifdef _WIN32
    std::vector<wchar_t> wide = to_wide(build_command_line(executable, arguments));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    if (::CreateProcessW(nullptr, wide.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                         &startup, &information) == 0) {
        throw Failure{"cannot spawn " + executable.string()};
    }
    ::CloseHandle(information.hThread);
    child.handle = information.hProcess;
    child.identifier = static_cast<long>(information.dwProcessId);
#else
    std::vector<std::string> storage;
    storage.push_back(executable.string());
    for (const std::string& argument : arguments) {
        storage.push_back(argument);
    }
    std::vector<char*> argv;
    for (std::string& value : storage) {
        argv.push_back(value.data());
    }
    argv.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::execv(executable.c_str(), argv.data());
        ::_exit(127);
    }
    if (pid < 0) {
        throw Failure{"cannot spawn " + executable.string()};
    }
    child.handle = reinterpret_cast<void*>(static_cast<intptr_t>(pid));
    child.identifier = static_cast<long>(pid);
#endif
    return child;
}

int wait_process(ChildProcess& child) {
    if (child.handle == nullptr) {
        throw Failure{"waiting on a process that was never spawned"};
    }
#ifdef _WIN32
    ::WaitForSingleObject(static_cast<HANDLE>(child.handle), INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(static_cast<HANDLE>(child.handle), &code);
    ::CloseHandle(static_cast<HANDLE>(child.handle));
    child.handle = nullptr;
    return static_cast<int>(code);
#else
    int status = 0;
    if (::waitpid(static_cast<pid_t>(child.identifier), &status, 0) < 0) {
        child.handle = nullptr;
        return -1;
    }
    child.handle = nullptr;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

void wait_for_file(const std::filesystem::path& path, const std::string& what) {
    constexpr int kMaxIterations = 60000;
    for (int iteration = 0; iteration < kMaxIterations; ++iteration) {
        std::error_code error;
        if (std::filesystem::exists(path, error) && !error) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw Failure{"timed out waiting for " + what + " at " + path.string()};
}

std::string read_text(const std::filesystem::path& path) {
    const std::vector<std::uint8_t> bytes = read_bytes(path);
    return std::string{bytes.begin(), bytes.end()};
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    write_bytes(path, std::vector<std::uint8_t>{text.begin(), text.end()});
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw Failure{"cannot read " + path.string()};
    }
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>(stream),
                                     std::istreambuf_iterator<char>()};
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw Failure{"cannot write " + path.string()};
    }
    if (!bytes.empty()) {
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    stream.flush();
    if (!stream) {
        throw Failure{"cannot flush " + path.string()};
    }
}

void flip_bit(const std::filesystem::path& path, std::uint64_t offset, unsigned bit) {
    std::vector<std::uint8_t> bytes = read_bytes(path);
    if (offset >= bytes.size()) {
        throw Failure{"bit flip offset is past the end of " + path.string()};
    }
    bytes[static_cast<std::size_t>(offset)] ^= static_cast<std::uint8_t>(1U << bit);
    write_bytes(path, bytes);
}

std::string repeat(char value, std::size_t count) { return std::string(count, value); }

int run_all(int argc, char** argv) {
    std::string filter;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--list") {
            list = true;
        } else if (argument == "--filter" && i + 1 < argc) {
            filter = argv[++i];
        }
    }

    std::vector<TestCase> cases = registry();
    std::stable_sort(cases.begin(), cases.end(), [](const TestCase& a, const TestCase& b) {
        if (a.suite != b.suite) {
            return a.suite < b.suite;
        }
        return a.name < b.name;
    });

    if (list) {
        for (const TestCase& test : cases) {
            std::cout << test.suite << "." << test.name << "\n";
        }
        return 0;
    }

    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    for (const TestCase& test : cases) {
        const std::string full = test.suite + "." + test.name;
        if (!filter.empty() && full.find(filter) == std::string::npos) {
            ++skipped;
            continue;
        }
        std::cout << "[ RUN      ] " << full << std::endl;
        try {
            test.body();
            ++passed;
            std::cout << "[       OK ] " << full << std::endl;
        } catch (const Failure& failure) {
            ++failed;
            std::cout << "[  FAILED  ] " << full << ": " << failure.message << std::endl;
        } catch (const std::exception& error) {
            ++failed;
            std::cout << "[  FAILED  ] " << full << ": unexpected exception: " << error.what()
                      << std::endl;
        } catch (...) {
            ++failed;
            std::cout << "[  FAILED  ] " << full << ": unexpected non-standard exception"
                      << std::endl;
        }
    }
    std::cout << "\n"
              << passed << " passed, " << failed << " failed, " << skipped << " filtered out of "
              << cases.size() << " tests" << std::endl;
    return failed == 0 ? 0 : 1;
}

}  // namespace feltest

int main(int argc, char** argv) { return feltest::run_all(argc, argv); }
