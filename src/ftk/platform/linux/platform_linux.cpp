#include "ftk/platform/platform.hpp"

#include "ftk/platform/linux/pipes.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace ftk::platform {

std::filesystem::path executable_path() {
    std::array<char, 4096> buf{};
    auto len = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (len <= 0) {
        throw std::runtime_error("Failed to read /proc/self/exe");
    }
    buf[static_cast<std::size_t>(len)] = '\0';
    return std::filesystem::path(buf.data());
}

std::uint32_t process_id() {
    return static_cast<std::uint32_t>(::getpid());
}

std::tm local_time(std::chrono::system_clock::time_point when) {
    const std::time_t at = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
    ::localtime_r(&at, &local);
    return local;
}

namespace {

// $`variable` when it is set and not empty, else `fallback` under the home
// folder, else the temporary folder.
std::filesystem::path xdg_dir(const char* variable, const char* fallback) {
    if (const char* set = std::getenv(variable); set != nullptr && set[0] != '\0') return set;
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / fallback;
    }
    return std::filesystem::temp_directory_path();
}

} // namespace

std::filesystem::path config_dir() {
    return xdg_dir("XDG_CONFIG_HOME", ".config");
}

std::filesystem::path state_dir() {
    return xdg_dir("XDG_STATE_HOME", ".local/state");
}

std::filesystem::path cache_dir() {
    return xdg_dir("XDG_CACHE_HOME", ".cache");
}

std::filesystem::path saved_games_dir() {
    return xdg_dir("XDG_DATA_HOME", ".local/share");
}

std::FILE* open_file(const std::filesystem::path& path, const char* mode) {
    return std::fopen(path.c_str(), mode);
}

namespace {

// What the system said went wrong with the call that just failed.
std::string last_error() {
    return std::system_category().message(errno);
}

// Puts what `descriptor` holds on the disk.
Result<> sync_descriptor(int descriptor, const std::filesystem::path& path) {
    if (::fsync(descriptor) != 0) return make_error("can't put " + path.string() + " on the disk: " + last_error());
    return {};
}

// Puts the folder's entries on the disk, so a rename in it holds after a
// power cut.
Result<> sync_folder(const std::filesystem::path& folder) {
    const int descriptor = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) return make_error("can't open the folder " + folder.string() + ": " + last_error());
    Result<> synced = sync_descriptor(descriptor, folder);
    ::close(descriptor);
    return synced;
}

// `file` under the name `backup` too, in place of whatever `backup` named:
// a second name for the same file where the file system has them, else a
// copy put on the disk. Never touches `file`.
Result<> keep_as(const std::filesystem::path& file, const std::filesystem::path& backup) {
    std::filesystem::path staged = backup;
    staged += ".new";
    std::error_code ignored;
    std::filesystem::remove(staged, ignored);
    if (::link(file.c_str(), staged.c_str()) != 0) {
        // FAT on a USB stick and some network shares give a file one name only.
        std::error_code copied;
        std::filesystem::copy_file(file, staged, copied);
        if (copied) return make_error("can't keep " + file.string() + " as " + backup.string() + ": " + copied.message());
        const int descriptor = ::open(staged.c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) return make_error("can't open " + staged.string() + ": " + last_error());
        Result<> synced = sync_descriptor(descriptor, staged);
        ::close(descriptor);
        if (!synced) return synced;
    }
    if (::rename(staged.c_str(), backup.c_str()) != 0) {
        const std::string why = last_error();
        std::filesystem::remove(staged, ignored);
        return make_error("can't keep " + file.string() + " as " + backup.string() + ": " + why);
    }
    return {};
}

} // namespace

Result<> sync_file(std::FILE* file) {
    if (std::fflush(file) != 0) return make_error("can't write the file out: " + last_error());
    if (::fsync(::fileno(file)) != 0) return make_error("can't put the file on the disk: " + last_error());
    return {};
}

Result<> replace_file(const std::filesystem::path& from, const std::filesystem::path& to,
                      const std::filesystem::path& backup) {
    std::error_code ec;
    // Checked first, so a replace that can't happen leaves the backup alone.
    if (!std::filesystem::exists(from, ec)) return make_error("can't put " + from.string() + " anywhere: it isn't there");
    if (!backup.empty() && std::filesystem::exists(to, ec)) {
        if (Result<> kept = keep_as(to, backup); !kept) return kept;
    }
    if (::rename(from.c_str(), to.c_str()) != 0) {
        return make_error("can't put " + from.string() + " in place of " + to.string() + ": " + last_error());
    }
    const std::filesystem::path folder = to.has_parent_path() ? to.parent_path() : std::filesystem::path(".");
    return sync_folder(folder);
}

namespace {

// The user's folder `key` names in user-dirs.dirs ("XDG_DOCUMENTS_DIR"),
// where a line sets it as "$HOME/Documents" or an absolute path; else
// `fallback` under the home folder.
std::filesystem::path user_dir(std::string_view key, const char* fallback) {
    const char* set = std::getenv("HOME");
    const std::filesystem::path home = set != nullptr && set[0] != '\0' ? set : std::filesystem::temp_directory_path();
    std::ifstream dirs(config_dir() / "user-dirs.dirs");
    for (std::string line; std::getline(dirs, line);) {
        if (!line.starts_with(key) || line.size() <= key.size() || line[key.size()] != '=') continue;
        std::string value = line.substr(key.size() + 1);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        constexpr std::string_view HOME_VAR = "$HOME";
        if (value.starts_with(HOME_VAR)) {
            const std::string rest = value.substr(HOME_VAR.size());
            return rest.empty() || rest == "/" ? home : home / std::filesystem::path(rest).relative_path();
        }
        if (!value.empty() && value.front() == '/') return value;
    }
    return home / fallback;
}

} // namespace

std::filesystem::path documents_dir() {
    return user_dir("XDG_DOCUMENTS_DIR", "Documents");
}

std::filesystem::path pictures_dir() {
    return user_dir("XDG_PICTURES_DIR", "Pictures");
}

std::optional<std::filesystem::path> find_program(std::string_view program) {
    const auto runnable = [](const std::filesystem::path& path) {
        std::error_code error;
        return std::filesystem::is_regular_file(path, error) && ::access(path.c_str(), X_OK) == 0;
    };
    if (program.empty()) return std::nullopt;
    if (program.find('/') != std::string_view::npos) {
        const std::filesystem::path path(program);
        return runnable(path) ? std::optional(path) : std::nullopt;
    }
    // Without a PATH, the folders execvp() looks in then.
    const char* set = std::getenv("PATH");
    const std::string_view folders = set != nullptr ? set : "/bin:/usr/bin";
    for (std::size_t start = 0; start <= folders.size();) {
        const std::size_t end = std::min(folders.find(':', start), folders.size());
        const std::string_view folder = folders.substr(start, end - start);
        // An empty entry is the current folder.
        const std::filesystem::path path = std::filesystem::path(folder.empty() ? "." : folder) / program;
        if (runnable(path)) return path;
        start = end + 1;
    }
    return std::nullopt;
}

int run_command(const std::string& cmd, std::string& output) {
    output.clear();
    auto* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return -1;

    std::array<char, 256> line{};
    while (fgets(line.data(), static_cast<int>(line.size()), pipe)) {
        output += line.data();
    }

    int status = pclose(pipe);
    // pclose returns the exit status in the format of wait(2)
    return WEXITSTATUS(status);
}

Result<> spawn_detached(const std::vector<std::string>& args) {
    if (args.empty()) return make_error("spawn_detached: empty args");

    pid_t pid = fork();
    if (pid < 0) return make_error("spawn_detached: fork failed");

    if (pid == 0) {
        // First child: fork again so the grandchild is orphaned and
        // reparented to init — no zombie possible.
        pid_t pid2 = fork();
        if (pid2 < 0) _exit(1);
        if (pid2 > 0) _exit(0); // first child exits immediately

        // Grandchild: start a new session and exec
        setsid();

        std::vector<const char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) {
            argv.push_back(a.c_str());
        }
        argv.push_back(nullptr);

        execvp(argv[0], const_cast<char* const*>(argv.data()));
        _exit(1);
    }

    // Parent: reap the first child (exits immediately after double-fork)
    waitpid(pid, nullptr, 0);
    return {};
}

Result<> exec_replace(const std::vector<std::string>& args) {
    if (args.empty()) return make_error("exec_replace: empty args");

    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);

    execvp(argv[0], const_cast<char* const*>(argv.data()));
    // execvp only returns on failure
    return make_error("exec_replace: execvp failed");
}

Result<std::size_t> read_standard_input(std::span<char> into) {
    return detail::read_some(STDIN_FILENO, into, "standard input");
}

Result<> write_standard_output(std::string_view bytes) {
    return detail::write_all(STDOUT_FILENO, bytes, "standard output");
}

Result<> open_path(const std::filesystem::path& path) {
    return spawn_detached({"xdg-open", path.string()});
}

// ── Subprocess with stdout pipe ─────────────────────────────────────────

Result<SubprocessHandle> spawn_with_pipe(const std::vector<std::string>& args) {
    if (args.empty()) return make_error("spawn_with_pipe: empty args");

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        return make_error("spawn_with_pipe: pipe() failed");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return make_error("spawn_with_pipe: fork() failed");
    }

    if (pid == 0) {
        // Child: redirect stdout to pipe write end
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        std::vector<const char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) {
            argv.push_back(a.c_str());
        }
        argv.push_back(nullptr);

        execvp(argv[0], const_cast<char* const*>(argv.data()));
        _exit(1);
    }

    // Parent: close write end, set read end to non-blocking
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);

    SubprocessHandle handle;
    handle.process = static_cast<std::uintptr_t>(pid);
    handle.stdout_read = static_cast<std::uintptr_t>(pipefd[0]);
    return handle;
}

std::vector<std::string> read_lines(SubprocessHandle& handle, std::string& line_buffer) {
    std::vector<std::string> lines;
    if (handle.stdout_read == SubprocessHandle::INVALID) return lines;

    const int fd = static_cast<int>(handle.stdout_read);
    char buf[4096];
    for (;;) {
        auto n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        line_buffer.append(buf, static_cast<size_t>(n));
    }

    // Extract complete lines
    size_t pos = 0;
    while (true) {
        auto nl = line_buffer.find('\n', pos);
        if (nl == std::string::npos) break;
        lines.push_back(line_buffer.substr(pos, nl - pos));
        pos = nl + 1;
    }
    if (pos > 0) {
        line_buffer.erase(0, pos);
    }

    return lines;
}

bool poll_exit(SubprocessHandle& handle) {
    if (handle.finished || handle.process == SubprocessHandle::INVALID) return handle.finished;

    const auto pid = static_cast<pid_t>(handle.process);
    int status = 0;
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid) {
        handle.finished = true;
        handle.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return handle.finished;
}

void close_subprocess(SubprocessHandle& handle) {
    if (handle.process != SubprocessHandle::INVALID && !handle.finished) {
        const auto pid = static_cast<pid_t>(handle.process);
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        handle.finished = true;
        handle.exit_code = -1;
    }
    if (handle.stdout_read != SubprocessHandle::INVALID) {
        close(static_cast<int>(handle.stdout_read));
        handle.stdout_read = SubprocessHandle::INVALID;
    }
    handle.process = SubprocessHandle::INVALID;
}

} // namespace ftk::platform
