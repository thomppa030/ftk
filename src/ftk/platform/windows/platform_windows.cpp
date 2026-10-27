#include "ftk/platform/platform.hpp"

#include "ftk/base/utf8.hpp"
#include "ftk/platform/detail/program_search.hpp"
#include "ftk/platform/windows/launch.hpp"
#include "ftk/platform/windows/pipes.hpp"

#ifndef _WIN32
#error "This file should only be compiled on Windows"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <ShlObj.h>
#include <shellapi.h>

#include <io.h>

#include <array>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <string>
#include <system_error>

namespace ftk::platform {

std::filesystem::path executable_path() {
    std::array<wchar_t, MAX_PATH> buf{};
    DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0 || len >= buf.size()) {
        throw std::runtime_error("Failed to get executable path via GetModuleFileNameW");
    }
    return std::filesystem::path(buf.data());
}

std::uint32_t process_id() {
    return static_cast<std::uint32_t>(GetCurrentProcessId());
}

std::tm local_time(std::chrono::system_clock::time_point when) {
    const std::time_t at = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
    localtime_s(&local, &at);
    return local;
}

namespace {

// The known folder `id`, or the temporary folder when Windows has none.
std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR found = nullptr;
    std::filesystem::path path = std::filesystem::temp_directory_path();
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &found))) path = found;
    CoTaskMemFree(found);
    return path;
}

} // namespace

std::filesystem::path config_dir() {
    return known_folder(FOLDERID_RoamingAppData);
}

std::filesystem::path state_dir() {
    return known_folder(FOLDERID_LocalAppData);
}

std::filesystem::path cache_dir() {
    return known_folder(FOLDERID_LocalAppData);
}

std::filesystem::path documents_dir() {
    return known_folder(FOLDERID_Documents);
}

std::filesystem::path pictures_dir() {
    return known_folder(FOLDERID_Pictures);
}

std::filesystem::path saved_games_dir() {
    return known_folder(FOLDERID_SavedGames);
}

std::FILE* open_file(const std::filesystem::path& path, const char* mode) {
    std::wstring wide_mode;
    for (const char* c = mode; *c != '\0'; ++c) wide_mode += static_cast<wchar_t>(*c);
    return _wfopen(path.c_str(), wide_mode.c_str());
}

Result<> sync_file(std::FILE* file) {
    if (std::fflush(file) != 0) return make_error("can't write the file out");
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file)));
    if (handle == INVALID_HANDLE_VALUE) return make_error("can't put the file on the disk: it has no handle");
    if (!FlushFileBuffers(handle)) return make_error("can't put the file on the disk: " + detail::reason(GetLastError()));
    return {};
}

Result<> replace_file(const std::filesystem::path& from, const std::filesystem::path& to,
                      const std::filesystem::path& backup) {
    std::error_code ec;
    if (std::filesystem::exists(to, ec)) {
        // ReplaceFileW keeps to's old file as the backup and puts from in its
        // place, so to names a whole file throughout.
        const wchar_t* kept = backup.empty() ? nullptr : backup.c_str();
        if (!ReplaceFileW(to.c_str(), from.c_str(), kept, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
            return make_error("can't put " + utf8::from_path(from) + " in place of " + utf8::from_path(to) + ": " +
                              detail::reason(GetLastError()));
        }
        return {};
    }
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return make_error("can't put " + utf8::from_path(from) + " in place of " + utf8::from_path(to) + ": " +
                          detail::reason(GetLastError()));
    }
    return {};
}

namespace {

// The environment variable `name`; empty when it isn't set.
std::wstring environment(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return {};
    std::wstring value(size, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), size);
    value.resize(length < size ? length : 0);
    return value;
}

// Whether `path` is a file rather than a folder.
bool is_file(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Where CreateProcess looks for a program named without a folder, in its
// order: this program's own folder, the current one, the system's, the old
// 16-bit system's, Windows' own, then each the PATH lists.
std::vector<std::filesystem::path> program_folders() {
    std::vector<std::filesystem::path> folders{executable_path().parent_path()};
    std::error_code error;
    if (std::filesystem::path current = std::filesystem::current_path(error); !error) folders.push_back(current);
    std::array<wchar_t, MAX_PATH> buffer{};
    const UINT system_length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    if (system_length > 0 && system_length < buffer.size()) {
        folders.emplace_back(std::wstring(buffer.data(), system_length));
    }
    const UINT windows_length = GetWindowsDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    if (windows_length > 0 && windows_length < buffer.size()) {
        const std::filesystem::path windows_folder(std::wstring(buffer.data(), windows_length));
        folders.push_back(windows_folder / L"System");
        folders.push_back(windows_folder);
    }
    for (const std::wstring& folder : detail::semicolon_list<wchar_t>(environment(L"PATH"))) folders.emplace_back(folder);
    return folders;
}

} // namespace

std::optional<std::filesystem::path> find_program(std::string_view program) {
    const std::filesystem::path given = utf8::to_path(program);
    const std::wstring name = given.filename().wstring();
    if (name.empty()) return std::nullopt;
    const std::vector<std::wstring> names =
        detail::program_names<wchar_t>(name, detail::program_extensions<wchar_t>(environment(L"PATHEXT")));
    // A name with a folder is looked for there alone.
    const std::vector<std::filesystem::path> folders =
        given.has_parent_path() ? std::vector<std::filesystem::path>{given.parent_path()} : program_folders();
    for (const std::filesystem::path& folder : folders) {
        for (const std::wstring& candidate : names) {
            if (std::filesystem::path path = folder / candidate; is_file(path)) return path;
        }
    }
    return std::nullopt;
}

int run_command(const std::string& cmd, std::string& output) {
    output.clear();
    // _popen runs the command through `cmd /c`, which drops the first and
    // the last quote of a command starting with one: `"C:\a b\x.exe" "in"`
    // would run `C:\a b\x.exe" "in`. In quotes of its own, the command loses
    // those instead and runs as written.
    FILE* pipe = _popen(("\"" + cmd + "\"").c_str(), "r");
    if (!pipe) return -1;

    std::array<char, 256> line{};
    while (fgets(line.data(), static_cast<int>(line.size()), pipe)) {
        output += line.data();
    }

    int status = _pclose(pipe);
    return status;
}

Result<> spawn_detached(const std::vector<std::string>& args) {
    if (args.empty()) return make_error("spawn_detached: empty args");
    Result<detail::Launch> to_run = detail::launch(args);
    if (!to_run) return make_error("spawn_detached: " + to_run.error());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // CREATE_NEW_PROCESS_GROUP detaches the child from our console
    BOOL ok = CreateProcessW(
        to_run->program.empty() ? nullptr : to_run->program.c_str(),
        to_run->line.data(), // must be mutable for CreateProcessW
        nullptr, nullptr,
        FALSE,
        CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS,
        nullptr, nullptr,
        &si, &pi);

    if (!ok) return make_error("spawn_detached: can't run " + args[0] + ": " + detail::reason(GetLastError()));

    // Close handles — we don't wait on the child
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return {};
}

Result<> exec_replace(const std::vector<std::string>& args) {
    if (args.empty()) return make_error("exec_replace: empty args");
    Result<detail::Launch> to_run = detail::launch(args);
    if (!to_run) return make_error("exec_replace: " + to_run.error());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessW(
        to_run->program.empty() ? nullptr : to_run->program.c_str(),
        to_run->line.data(),
        nullptr, nullptr,
        FALSE,
        0,
        nullptr, nullptr,
        &si, &pi);

    if (!ok) return make_error("exec_replace: can't run " + args[0] + ": " + detail::reason(GetLastError()));

    // Wait for the child, then exit this process with its exit code
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    ExitProcess(exit_code);
}

Result<std::size_t> read_standard_input(std::span<char> into) {
    return detail::read_some(GetStdHandle(STD_INPUT_HANDLE), into, "standard input");
}

Result<> write_standard_output(std::string_view bytes) {
    return detail::write_all(GetStdHandle(STD_OUTPUT_HANDLE), bytes, "standard output");
}

Result<> open_path(const std::filesystem::path& path) {
    auto result = ShellExecuteA(nullptr, "open", path.string().c_str(),
                                nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<intptr_t>(result) <= 32) {
        return make_error("open_path: ShellExecuteA failed");
    }
    return {};
}

// ── Subprocess with stdout pipe ─────────────────────────────────────────

Result<SubprocessHandle> spawn_with_pipe(const std::vector<std::string>& args) {
    if (args.empty()) return make_error("spawn_with_pipe: empty args");
    Result<detail::Launch> to_run = detail::launch(args);
    if (!to_run) return make_error("spawn_with_pipe: " + to_run.error());

    // The child inherits the write end; the read end must not be inherited or
    // the pipe never reports EOF once the child exits.
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        return make_error("spawn_with_pipe: CreatePipe failed");
    }
    if (!SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(read_end);
        CloseHandle(write_end);
        return make_error("spawn_with_pipe: SetHandleInformation failed");
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(to_run->program.empty() ? nullptr : to_run->program.c_str(), to_run->line.data(),
                             nullptr, nullptr, /*bInheritHandles=*/TRUE, 0, nullptr, nullptr, &si, &pi);
    const DWORD failure = ok ? 0 : GetLastError();

    // The parent has no use for the write end; releasing it is what lets the
    // read end see EOF after the child is gone.
    CloseHandle(write_end);

    if (!ok) {
        CloseHandle(read_end);
        return make_error("spawn_with_pipe: can't run " + args[0] + ": " + detail::reason(failure));
    }
    CloseHandle(pi.hThread);

    SubprocessHandle handle;
    handle.process = reinterpret_cast<std::uintptr_t>(pi.hProcess);
    handle.stdout_read = reinterpret_cast<std::uintptr_t>(read_end);
    return handle;
}

std::vector<std::string> read_lines(SubprocessHandle& handle, std::string& line_buffer) {
    std::vector<std::string> lines;
    if (handle.stdout_read == SubprocessHandle::INVALID) return lines;

    auto pipe = reinterpret_cast<HANDLE>(handle.stdout_read);

    // Anonymous pipes have no non-blocking mode, so peek for buffered bytes and
    // only read what is already there — ReadFile would otherwise block until the
    // child writes or exits.
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available == 0) break;

        std::array<char, 4096> buf{};
        DWORD want = available < buf.size() ? available : static_cast<DWORD>(buf.size());
        DWORD got = 0;
        if (!ReadFile(pipe, buf.data(), want, &got, nullptr) || got == 0) break;
        line_buffer.append(buf.data(), got);
    }

    size_t pos = 0;
    for (;;) {
        auto nl = line_buffer.find('\n', pos);
        if (nl == std::string::npos) break;
        auto end = nl;
        // fjimport writes CRLF through the console runtime.
        if (end > pos && line_buffer[end - 1] == '\r') --end;
        lines.push_back(line_buffer.substr(pos, end - pos));
        pos = nl + 1;
    }
    if (pos > 0) {
        line_buffer.erase(0, pos);
    }

    return lines;
}

bool poll_exit(SubprocessHandle& handle) {
    if (handle.finished || handle.process == SubprocessHandle::INVALID) return handle.finished;

    auto process = reinterpret_cast<HANDLE>(handle.process);
    if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) return false;

    DWORD code = 0;
    handle.exit_code = GetExitCodeProcess(process, &code) ? static_cast<int>(code) : -1;
    handle.finished = true;
    return true;
}

void close_subprocess(SubprocessHandle& handle) {
    if (handle.process != SubprocessHandle::INVALID) {
        auto process = reinterpret_cast<HANDLE>(handle.process);
        if (!handle.finished) {
            TerminateProcess(process, 1);
            WaitForSingleObject(process, INFINITE);
            handle.finished = true;
            handle.exit_code = -1;
        }
        CloseHandle(process);
        handle.process = SubprocessHandle::INVALID;
    }
    if (handle.stdout_read != SubprocessHandle::INVALID) {
        CloseHandle(reinterpret_cast<HANDLE>(handle.stdout_read));
        handle.stdout_read = SubprocessHandle::INVALID;
    }
}

} // namespace ftk::platform
