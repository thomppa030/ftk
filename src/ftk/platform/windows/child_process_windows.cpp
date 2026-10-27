#include "ftk/platform/child_process.hpp"

#include "ftk/platform/windows/launch.hpp"
#include "ftk/platform/windows/pipes.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <format>
#include <string>
#include <vector>

namespace ftk::platform {

struct ChildProcess::Impl {
    HANDLE process{nullptr};
    HANDLE input{nullptr};
    HANDLE output{nullptr};
    HANDLE errors{nullptr};
};

namespace {

// What Windows ends a child with when it is ended from outside, read back
// as -1.
constexpr UINT KILLED = static_cast<UINT>(-1);

void close_handle(HANDLE& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    handle = nullptr;
}

// A pipe whose end `kept` stays with this program, never handed to a
// child.
bool make_pipe(HANDLE& read_end, HANDLE& write_end, HANDLE& kept) {
    SECURITY_ATTRIBUTES inherit{.nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr,
                                .bInheritHandle = TRUE};
    if (!CreatePipe(&read_end, &write_end, &inherit, 0)) return false;
    return SetHandleInformation(kept, HANDLE_FLAG_INHERIT, 0) != 0;
}

} // namespace

ChildProcess::ChildProcess(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Result<std::unique_ptr<ChildProcess>> ChildProcess::start(std::span<const std::string> args,
                                                           const ChildOptions& options) {
    if (args.empty()) return make_error("a child needs a program to run");
    Result<detail::Launch> to_run = detail::launch(args);
    if (!to_run) return make_error(std::format("can't run {}: {}", args[0], to_run.error()));

    HANDLE input_read = nullptr;
    HANDLE input_write = nullptr;
    HANDLE output_read = nullptr;
    HANDLE output_write = nullptr;
    HANDLE errors_read = nullptr;
    HANDLE errors_write = nullptr;
    const auto close_all = [&] {
        for (HANDLE* handle : {&input_read, &input_write, &output_read, &output_write, &errors_read, &errors_write}) {
            close_handle(*handle);
        }
    };
    bool made = make_pipe(input_read, input_write, input_write) && make_pipe(output_read, output_write, output_read);
    SECURITY_ATTRIBUTES inherit{.nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr,
                                .bInheritHandle = TRUE};
    if (made && options.errors == ChildErrors::Kept) {
        made = make_pipe(errors_read, errors_write, errors_read);
    } else if (made && options.errors == ChildErrors::Shown) {
        // This program's own, copied so the child may have it; nowhere when
        // there is none, as a program with no console has none.
        const HANDLE own = GetStdHandle(STD_ERROR_HANDLE);
        if (own == nullptr || own == INVALID_HANDLE_VALUE ||
            !DuplicateHandle(GetCurrentProcess(), own, GetCurrentProcess(), &errors_write, 0, TRUE,
                             DUPLICATE_SAME_ACCESS)) {
            errors_write = nullptr;
        }
    }
    if (made && errors_write == nullptr) {
        errors_write = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        made = errors_write != INVALID_HANDLE_VALUE;
    }
    if (!made) {
        const DWORD error = GetLastError();
        close_all();
        return make_error(std::format("can't make pipes for {}: {}", args[0], detail::reason(error)));
    }

    // Only the child's three handles go to it, whatever else this program
    // has open to inherit at the time.
    HANDLE handed[] = {input_read, output_write, errors_write};
    SIZE_T list_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size);
    std::vector<std::byte> list_storage(list_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &list_size) ||
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handed, sizeof(handed), nullptr,
                                   nullptr)) {
        const DWORD error = GetLastError();
        close_all();
        return make_error(std::format("can't hand {} its pipes: {}", args[0], detail::reason(error)));
    }

    STARTUPINFOEXW info{};
    info.StartupInfo.cb = sizeof(info);
    info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    info.StartupInfo.hStdInput = input_read;
    info.StartupInfo.hStdOutput = output_write;
    info.StartupInfo.hStdError = errors_write;
    info.lpAttributeList = attributes;

    const std::wstring folder = options.folder.wstring();

    PROCESS_INFORMATION started{};
    const BOOL ran = CreateProcessW(to_run->program.empty() ? nullptr : to_run->program.c_str(), to_run->line.data(),
                                    nullptr, nullptr, TRUE,
                                    CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                    folder.empty() ? nullptr : folder.c_str(), &info.StartupInfo, &started);
    const DWORD failure = ran ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    // The child's ends are its own now.
    close_handle(input_read);
    close_handle(output_write);
    close_handle(errors_write);
    if (!ran) {
        close_all();
        return make_error(std::format("can't run {}: {}", args[0], detail::reason(failure)));
    }
    CloseHandle(started.hThread);

    auto impl = std::make_unique<Impl>();
    impl->process = started.hProcess;
    impl->input = input_write;
    impl->output = output_read;
    impl->errors = errors_read;
    return std::unique_ptr<ChildProcess>(new ChildProcess(std::move(impl)));
}

ChildProcess::~ChildProcess() {
    kill();
    (void)wait();
    close_handle(impl_->input);
    close_handle(impl_->output);
    close_handle(impl_->errors);
    close_handle(impl_->process);
}

Result<> ChildProcess::write(std::string_view bytes) {
    return detail::write_all(impl_->input, bytes, "the child's input");
}

void ChildProcess::close_input() {
    close_handle(impl_->input);
}

Result<std::size_t> ChildProcess::read(std::span<char> into) {
    return detail::read_some(impl_->output, into, "the child's output");
}

Result<std::size_t> ChildProcess::read_errors(std::span<char> into) {
    return detail::read_some(impl_->errors, into, "the child's errors");
}

std::optional<int> ChildProcess::exit_code() {
    if (WaitForSingleObject(impl_->process, 0) != WAIT_OBJECT_0) return std::nullopt;
    DWORD code = 0;
    if (!GetExitCodeProcess(impl_->process, &code)) return -1;
    return static_cast<int>(code);
}

int ChildProcess::wait() {
    WaitForSingleObject(impl_->process, INFINITE);
    DWORD code = 0;
    if (!GetExitCodeProcess(impl_->process, &code)) return -1;
    return static_cast<int>(code);
}

void ChildProcess::kill() {
    // The process's handle keeps it known until the handle closes, so it
    // can never be another's.
    if (WaitForSingleObject(impl_->process, 0) != WAIT_OBJECT_0) TerminateProcess(impl_->process, KILLED);
}

} // namespace ftk::platform
