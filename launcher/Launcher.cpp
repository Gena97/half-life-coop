#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef HLC_LAUNCHER_VERSION
#error "Версию запускателя задаёт scripts/build-launcher.ps1"
#endif

#define HLC_WIDEN_IMPL(value) L##value
#define HLC_WIDEN(value) HLC_WIDEN_IMPL(value)

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace
{
const wchar_t kWindowClass[] = L"HalfLifeCoopLauncherWindow";
const wchar_t kTitle[] = L"Half-Life Co-op";
const wchar_t kVersion[] = HLC_WIDEN(HLC_LAUNCHER_VERSION);
const UINT kLogMessage = WM_APP + 1;
const UINT kFinishedMessage = WM_APP + 2;
const int kCloseButtonId = 1001;
const int kPackageResourceId = 101;
const int kBootstrapResourceId = 102;

class Handle
{
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.Release()) {}
    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other)
        {
            Reset(other.Release());
        }
        return *this;
    }
    HANDLE Get() const { return value_; }
    bool Valid() const { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
    HANDLE Release()
    {
        const HANDLE value = value_;
        value_ = nullptr;
        return value;
    }
    void Reset(HANDLE value = nullptr)
    {
        if (Valid())
        {
            CloseHandle(value_);
        }
        value_ = value;
    }

private:
    HANDLE value_ = nullptr;
};

struct Options
{
    std::wstring gameDirectory;
    bool installOnly = false;
    bool skipUpdate = false;
    bool afterUpdate = false;
};

struct Result
{
    DWORD exitCode = 1;
    std::wstring error;
    std::wstring relaunchPath;
};

struct Application
{
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND status = nullptr;
    HWND progress = nullptr;
    HWND log = nullptr;
    HWND closeButton = nullptr;
    HFONT font = nullptr;
    std::thread worker;
    Options options;
    std::wstring launcherPath;
    bool running = true;
    DWORD exitCode = 1;
};

Application gApplication;

std::wstring WindowsError(const std::wstring& operation, DWORD error = GetLastError())
{
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring result = operation + L" (" + std::to_wstring(error) + L")";
    if (length != 0 && message != nullptr)
    {
        std::wstring description(message, length);
        LocalFree(message);
        while (!description.empty() && (description.back() == L'\r' || description.back() == L'\n'))
        {
            description.pop_back();
        }
        result += L": " + description;
    }
    return result;
}

std::wstring FullPath(const std::wstring& path)
{
    const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed == 0 || needed > 32767)
    {
        throw WindowsError(L"Не удалось определить полный путь");
    }
    std::vector<wchar_t> buffer(needed);
    const DWORD length = GetFullPathNameW(path.c_str(), needed, buffer.data(), nullptr);
    if (length == 0 || length >= needed)
    {
        throw WindowsError(L"Не удалось определить полный путь");
    }
    std::wstring result(buffer.data(), length);
    while (result.size() > 3 && (result.back() == L'\\' || result.back() == L'/'))
    {
        result.pop_back();
    }
    return result;
}

std::wstring ModulePath()
{
    std::vector<wchar_t> buffer(512);
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
        {
            throw WindowsError(L"Не удалось определить путь программы");
        }
        if (length < buffer.size())
        {
            return FullPath(std::wstring(buffer.data(), length));
        }
        if (buffer.size() >= 32768)
        {
            throw std::wstring(L"Слишком длинный путь программы");
        }
        buffer.resize(std::min<size_t>(buffer.size() * 2, 32768));
    }
}

bool SamePath(const std::wstring& first, const std::wstring& second)
{
    return CompareStringOrdinal(first.c_str(), -1, second.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring GuidText()
{
    GUID value{};
    if (FAILED(CoCreateGuid(&value)))
    {
        throw std::wstring(L"Не удалось создать временный каталог");
    }
    wchar_t buffer[40]{};
    if (StringFromGUID2(value, buffer, ARRAYSIZE(buffer)) == 0)
    {
        throw std::wstring(L"Не удалось создать имя временного каталога");
    }
    return std::wstring(buffer + 1, 36);
}

void CheckPlainDirectory(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        throw WindowsError(L"Недоступен каталог " + path);
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        throw std::wstring(L"Ожидается обычный каталог без перенаправлений: ") + path;
    }
}

std::wstring QuoteArgument(const std::wstring& value)
{
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (const wchar_t character : value)
    {
        if (character == L'\\')
        {
            ++backslashes;
        }
        else
        {
            result.append(backslashes * (character == L'\"' ? 2 : 1), L'\\');
            backslashes = 0;
            if (character == L'\"')
            {
                result += L'\\';
            }
            result += character;
        }
    }
    result.append(backslashes * 2, L'\\');
    result += L'\"';
    return result;
}

Options ParseOptions()
{
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr)
    {
        throw WindowsError(L"Не удалось прочитать параметры запуска");
    }
    std::unique_ptr<wchar_t*, decltype(&LocalFree)> ownedArguments(arguments, LocalFree);
    Options options;
    for (int index = 1; index < count; ++index)
    {
        const std::wstring argument = arguments[index];
        if (argument == L"--game-dir")
        {
            if (++index >= count || arguments[index][0] == L'\0')
            {
                throw std::wstring(L"После --game-dir укажите папку Half-Life");
            }
            options.gameDirectory = FullPath(arguments[index]);
        }
        else if (argument == L"--install-only")
        {
            options.installOnly = true;
        }
        else if (argument == L"--skip-update")
        {
            options.skipUpdate = true;
        }
        else if (argument == L"--after-update")
        {
            options.afterUpdate = true;
        }
        else
        {
            throw std::wstring(L"Неизвестный параметр запуска: ") + argument;
        }
    }
    return options;
}

void PostLog(const std::wstring& message)
{
    auto text = std::make_unique<std::wstring>(message);
    if (PostMessageW(gApplication.window, kLogMessage, 0, reinterpret_cast<LPARAM>(text.get())))
    {
        text.release();
    }
}

std::wstring DecodeUtf8(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length == 0)
    {
        return L"Не удалось прочитать строку журнала";
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), &result[0], length);
    return result;
}

class ExtractedResources
{
public:
    ExtractedResources()
    {
        wchar_t buffer[32768]{};
        const DWORD length = GetTempPathW(ARRAYSIZE(buffer), buffer);
        if (length == 0 || length >= ARRAYSIZE(buffer))
        {
            throw WindowsError(L"Не найден временный каталог Windows");
        }
        const std::wstring base = FullPath(std::wstring(buffer, length));
        CheckPlainDirectory(base);
        root = base + L"\\hlcoop-launcher-" + GuidText();
        if (!CreateDirectoryW(root.c_str(), nullptr))
        {
            throw WindowsError(L"Не удалось создать временный каталог установщика");
        }
        directory_.Reset(CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!directory_.Valid())
        {
            throw WindowsError(L"Не удалось защитить временный каталог");
        }
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(directory_.Get(), &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            throw std::wstring(L"Временный каталог перенаправлен. Запуск остановлен");
        }
        package = root + L"\\payload.zip";
        bootstrap = root + L"\\bootstrap.ps1";
    }

    ~ExtractedResources()
    {
        directory_.Reset();
        if (!root.empty())
        {
            // Удаляем только три известных файла установщика, без обхода чужих каталогов
            const DWORD attributes = GetFileAttributesW(root.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
            {
                DeleteFileW(package.c_str());
                DeleteFileW(bootstrap.c_str());
                DeleteFileW((root + L"\\updater-library.ps1").c_str());
                RemoveDirectoryW(root.c_str());
            }
        }
    }

    void Extract()
    {
        WriteResource(kPackageResourceId, package);
        WriteResource(kBootstrapResourceId, bootstrap);
    }

    std::wstring root;
    std::wstring package;
    std::wstring bootstrap;

private:
    void WriteResource(int identifier, const std::wstring& path)
    {
        const HRSRC resource = FindResourceW(gApplication.instance,
            MAKEINTRESOURCEW(identifier), RT_RCDATA);
        if (resource == nullptr)
        {
            throw WindowsError(L"В программе отсутствуют установочные файлы");
        }
        const DWORD size = SizeofResource(gApplication.instance, resource);
        const HGLOBAL loaded = LoadResource(gApplication.instance, resource);
        const void* bytes = loaded == nullptr ? nullptr : LockResource(loaded);
        if (size == 0 || bytes == nullptr)
        {
            throw WindowsError(L"Не удалось прочитать установочные файлы");
        }
        Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!file.Valid())
        {
            throw WindowsError(L"Не удалось сохранить установочные файлы");
        }
        DWORD written = 0;
        if (!WriteFile(file.Get(), bytes, size, &written, nullptr) || written != size ||
            !FlushFileBuffers(file.Get()))
        {
            throw WindowsError(L"Не удалось полностью записать установочные файлы");
        }
    }

    Handle directory_;
};

std::wstring PowerShellPath()
{
    wchar_t buffer[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(buffer, ARRAYSIZE(buffer));
    if (length == 0 || length >= ARRAYSIZE(buffer))
    {
        throw WindowsError(L"Не найден системный каталог Windows");
    }
    const std::wstring path = std::wstring(buffer, length) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        throw std::wstring(L"Не найден Windows PowerShell 5.1: ") + path;
    }
    return path;
}

void ConsumeLine(std::string line, Result& result)
{
    if (!line.empty() && line.back() == '\r')
    {
        line.pop_back();
    }
    const std::wstring text = DecodeUtf8(line);
    const std::wstring prefix = L"HLCOOP_RELAUNCH ";
    if (text.compare(0, prefix.size(), prefix) == 0)
    {
        const std::wstring candidate = text.substr(prefix.size());
        if (!candidate.empty() && SamePath(FullPath(candidate), gApplication.launcherPath))
        {
            result.relaunchPath = gApplication.launcherPath;
            PostLog(L"Обновление программы подготовлено. Перезапуск...\r\n");
        }
        else
        {
            throw std::wstring(L"Получен недопустимый путь перезапуска программы");
        }
    }
    else
    {
        PostLog(text + L"\r\n");
    }
}

void AppendOutput(std::string& pending, const char* bytes, DWORD length, Result& result)
{
    pending.append(bytes, length);
    size_t start = 0;
    for (;;)
    {
        const size_t newline = pending.find('\n', start);
        if (newline == std::string::npos)
        {
            break;
        }
        ConsumeLine(pending.substr(start, newline - start), result);
        start = newline + 1;
    }
    pending.erase(0, start);
    if (pending.size() > 65536)
    {
        // Длинная строка не должна бесконечно занимать память
        size_t boundary = pending.size();
        while (boundary > 0 && (static_cast<unsigned char>(pending[boundary - 1]) & 0xC0) == 0x80)
        {
            --boundary;
        }
        if (boundary > 0)
        {
            --boundary;
        }
        PostLog(DecodeUtf8(pending.substr(0, boundary)));
        pending.erase(0, boundary);
    }
}

Result RunBootstrap(const ExtractedResources& resources)
{
    Result result;
    const std::wstring powershell = PowerShellPath();
    std::wstring command = QuoteArgument(powershell) +
        L" -NoLogo -NoProfile -ExecutionPolicy Bypass -File " + QuoteArgument(resources.bootstrap) +
        L" -PackagePath " + QuoteArgument(resources.package) +
        L" -LauncherPath " + QuoteArgument(gApplication.launcherPath) +
        L" -LauncherVersion " + QuoteArgument(kVersion) +
        L" -ParentProcessId " + std::to_wstring(GetCurrentProcessId());
    const Options& options = gApplication.options;
    if (!options.gameDirectory.empty())
    {
        command += L" -HalfLifePath " + QuoteArgument(options.gameDirectory);
    }
    if (options.installOnly)
    {
        command += L" -InstallOnly";
    }
    if (options.skipUpdate)
    {
        command += L" -SkipUpdate";
    }
    if (options.afterUpdate)
    {
        command += L" -AfterUpdate";
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    if (!CreatePipe(&readHandle, &writeHandle, &security, 0))
    {
        throw WindowsError(L"Не удалось создать журнал запуска");
    }
    Handle outputRead(readHandle);
    Handle outputWrite(writeHandle);
    if (!SetHandleInformation(outputRead.Get(), HANDLE_FLAG_INHERIT, 0))
    {
        throw WindowsError(L"Не удалось подготовить журнал запуска");
    }
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!input.Valid())
    {
        throw WindowsError(L"Не удалось подготовить ввод установщика");
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = input.Get();
    startup.hStdOutput = outputWrite.Get();
    startup.hStdError = outputWrite.Get();
    PROCESS_INFORMATION information{};
    if (!CreateProcessW(powershell.c_str(), &command[0], nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, resources.root.c_str(),
        &startup, &information))
    {
        throw WindowsError(L"Не удалось запустить установку");
    }
    Handle process(information.hProcess);
    Handle thread(information.hThread);
    outputWrite.Reset();
    input.Reset();

    std::string pending;
    bool reading = true;
    bool finished = false;
    std::wstring outputError;
    while (!finished)
    {
        if (reading)
        {
            for (;;)
            {
                DWORD available = 0;
                if (!PeekNamedPipe(outputRead.Get(), nullptr, 0, nullptr, &available, nullptr))
                {
                    if (GetLastError() != ERROR_BROKEN_PIPE)
                    {
                        outputError = WindowsError(L"Не удалось прочитать журнал установки");
                    }
                    reading = false;
                    break;
                }
                if (available == 0)
                {
                    break;
                }
                char buffer[8192];
                DWORD received = 0;
                if (!ReadFile(outputRead.Get(), buffer, std::min<DWORD>(available, sizeof(buffer)), &received, nullptr))
                {
                    if (GetLastError() != ERROR_BROKEN_PIPE)
                    {
                        outputError = WindowsError(L"Не удалось прочитать журнал установки");
                    }
                    reading = false;
                    break;
                }
                try
                {
                    AppendOutput(pending, buffer, received, result);
                }
                catch (const std::wstring& error)
                {
                    // Дожидаемся установщика даже при повреждённом служебном выводе
                    outputError = error;
                    pending.clear();
                }
            }
        }

        const DWORD wait = WaitForSingleObject(process.Get(), 40);
        if (wait == WAIT_OBJECT_0)
        {
            // Игра может унаследовать дескриптор потока, поэтому не ждём закрытия всех копий
            finished = true;
            if (reading)
            {
                DWORD available = 0;
                while (PeekNamedPipe(outputRead.Get(), nullptr, 0, nullptr, &available, nullptr) && available > 0)
                {
                    char buffer[8192];
                    DWORD received = 0;
                    if (!ReadFile(outputRead.Get(), buffer, std::min<DWORD>(available, sizeof(buffer)), &received, nullptr) || received == 0)
                    {
                        break;
                    }
                    try
                    {
                        AppendOutput(pending, buffer, received, result);
                    }
                    catch (const std::wstring& error)
                    {
                        outputError = error;
                        pending.clear();
                    }
                }
            }
        }
        else if (wait == WAIT_FAILED)
        {
            throw WindowsError(L"Не удалось дождаться завершения установки");
        }
    }
    if (!pending.empty())
    {
        try
        {
            ConsumeLine(pending, result);
        }
        catch (const std::wstring& error)
        {
            outputError = error;
        }
    }
    if (!GetExitCodeProcess(process.Get(), &result.exitCode))
    {
        throw WindowsError(L"Не удалось получить результат установки");
    }
    if (!outputError.empty())
    {
        result.error = outputError;
        if (result.exitCode == 0)
        {
            result.exitCode = 1;
        }
    }
    else if (result.exitCode != 0)
    {
        result.error = L"Установка или запуск завершились с ошибкой. Подробности в журнале выше";
    }
    return result;
}

void Work()
{
    auto result = std::make_unique<Result>();
    try
    {
        PostLog(std::wstring(L"Half-Life Co-op ") + kVersion + L"\r\nПодготовка установки...\r\n");
        ExtractedResources resources;
        resources.Extract();
        *result = RunBootstrap(resources);
    }
    catch (const std::wstring& error)
    {
        result->error = error;
        result->exitCode = 1;
    }
    catch (const std::exception&)
    {
        result->error = L"Не удалось продолжить установку из-за внутренней ошибки программы";
        result->exitCode = 1;
    }
    catch (...)
    {
        result->error = L"Не удалось продолжить установку";
        result->exitCode = 1;
    }
    if (PostMessageW(gApplication.window, kFinishedMessage, 0, reinterpret_cast<LPARAM>(result.get())))
    {
        result.release();
    }
}

void AppendLog(const std::wstring& value)
{
    const int length = GetWindowTextLengthW(gApplication.log);
    if (length > 1500000)
    {
        SendMessageW(gApplication.log, EM_SETSEL, 0, 500000);
        SendMessageW(gApplication.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
    }
    SendMessageW(gApplication.log, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageW(gApplication.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(value.c_str()));
    SendMessageW(gApplication.log, EM_SCROLLCARET, 0, 0);
}

void Layout(HWND window)
{
    RECT client{};
    GetClientRect(window, &client);
    const int width = std::max(150L, client.right - 32);
    const int height = std::max(80L, client.bottom - 134);
    MoveWindow(gApplication.status, 16, 14, width, 28, TRUE);
    MoveWindow(gApplication.progress, 16, 47, width, 18, TRUE);
    MoveWindow(gApplication.log, 16, 78, width, height, TRUE);
    MoveWindow(gApplication.closeButton, client.right - 126, client.bottom - 43, 110, 29, TRUE);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
    {
        gApplication.window = window;
        const HINSTANCE instance = gApplication.instance;
        gApplication.status = CreateWindowExW(0, L"STATIC", L"Подготовка установки и проверка обновлений...",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, instance, nullptr);
        gApplication.progress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
            WS_CHILD | WS_VISIBLE | PBS_MARQUEE, 0, 0, 0, 0, window, nullptr, instance, nullptr);
        gApplication.log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 0, 0, window, nullptr, instance, nullptr);
        gApplication.closeButton = CreateWindowExW(0, L"BUTTON", L"Закрыть",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(kCloseButtonId), instance, nullptr);
        if (gApplication.status == nullptr || gApplication.progress == nullptr ||
            gApplication.log == nullptr || gApplication.closeButton == nullptr)
        {
            return -1;
        }
        HDC context = GetDC(window);
        const int dpi = context != nullptr ? GetDeviceCaps(context, LOGPIXELSY) : 96;
        if (context != nullptr)
        {
            ReleaseDC(window, context);
        }
        gApplication.font = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE,
            FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        for (const HWND control : {gApplication.status, gApplication.log, gApplication.closeButton})
        {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(gApplication.font), TRUE);
        }
        SendMessageW(gApplication.log, EM_SETLIMITTEXT, 2000000, 0);
        SendMessageW(gApplication.progress, PBM_SETMARQUEE, TRUE, 35);
        EnableMenuItem(GetSystemMenu(window, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
        Layout(window);
        return 0;
    }
    case WM_SIZE:
        Layout(window);
        return 0;
    case WM_GETMINMAXINFO:
    {
        auto information = reinterpret_cast<MINMAXINFO*>(lParam);
        information->ptMinTrackSize.x = 520;
        information->ptMinTrackSize.y = 340;
        return 0;
    }
    case kLogMessage:
    {
        std::unique_ptr<std::wstring> value(reinterpret_cast<std::wstring*>(lParam));
        AppendLog(*value);
        return 0;
    }
    case kFinishedMessage:
    {
        std::unique_ptr<Result> result(reinterpret_cast<Result*>(lParam));
        if (gApplication.worker.joinable())
        {
            gApplication.worker.join();
        }
        gApplication.running = false;
        gApplication.exitCode = result->exitCode;
        SendMessageW(gApplication.progress, PBM_SETMARQUEE, FALSE, 0);
        if (result->exitCode == 0)
        {
            DestroyWindow(window);
            return 0;
        }
        SetWindowTextW(gApplication.status, L"Не удалось завершить установку или запуск");
        if (!result->error.empty())
        {
            AppendLog(L"\r\n" + result->error + L"\r\n");
        }
        EnableMenuItem(GetSystemMenu(window, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);
        EnableWindow(gApplication.closeButton, TRUE);
        SetFocus(gApplication.closeButton);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == kCloseButtonId && !gApplication.running)
        {
            DestroyWindow(window);
        }
        return 0;
    case WM_CLOSE:
        if (!gApplication.running)
        {
            DestroyWindow(window);
        }
        return 0;
    case WM_ENDSESSION:
        // Завершение Windows обрабатывает журнал транзакции в установщике
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    try
    {
        gApplication.instance = instance;
        gApplication.options = ParseOptions();
        gApplication.launcherPath = ModulePath();

        Handle mutex(CreateMutexW(nullptr, FALSE, L"Local\\HalfLifeCoopLauncher"));
        if (!mutex.Valid())
        {
            throw WindowsError(L"Не удалось защитить установку от одновременного запуска");
        }
        const DWORD ownership = WaitForSingleObject(mutex.Get(), 0);
        if (ownership != WAIT_OBJECT_0 && ownership != WAIT_ABANDONED)
        {
            if (ownership == WAIT_TIMEOUT)
            {
                const HWND existing = FindWindowW(kWindowClass, kTitle);
                if (existing != nullptr)
                {
                    ShowWindow(existing, SW_RESTORE);
                    SetForegroundWindow(existing);
                }
                else
                {
                    MessageBoxW(nullptr, L"Установщик Half-Life Co-op уже запущен", kTitle, MB_OK | MB_ICONINFORMATION);
                }
                return ERROR_ALREADY_EXISTS;
            }
            throw WindowsError(L"Не удалось заблокировать параллельную установку");
        }

        INITCOMMONCONTROLSEX controls{};
        controls.dwSize = sizeof(controls);
        controls.dwICC = ICC_PROGRESS_CLASS;
        if (!InitCommonControlsEx(&controls))
        {
            throw WindowsError(L"Не удалось подготовить окно программы");
        }
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.hInstance = instance;
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        windowClass.hIconSm = windowClass.hIcon;
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&windowClass))
        {
            throw WindowsError(L"Не удалось зарегистрировать окно программы");
        }
        HWND window = CreateWindowExW(0, kWindowClass, kTitle,
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 760, 490,
            nullptr, nullptr, instance, nullptr);
        if (window == nullptr)
        {
            throw WindowsError(L"Не удалось открыть окно программы");
        }
        ShowWindow(window, showCommand);
        UpdateWindow(window);
        try
        {
            gApplication.worker = std::thread(Work);
        }
        catch (...)
        {
            gApplication.running = false;
            DestroyWindow(window);
            throw std::wstring(L"Не удалось запустить установку в фоновом потоке");
        }
        MSG message{};
        int status = 0;
        while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0)
        {
            if (!IsDialogMessageW(window, &message))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (gApplication.worker.joinable())
        {
            gApplication.worker.join();
        }
        if (gApplication.font != nullptr)
        {
            DeleteObject(gApplication.font);
            gApplication.font = nullptr;
        }
        ReleaseMutex(mutex.Get());
        return status == -1 ? 1 : static_cast<int>(gApplication.exitCode);
    }
    catch (const std::wstring& error)
    {
        MessageBoxW(nullptr, error.c_str(), kTitle, MB_OK | MB_ICONERROR);
    }
    catch (...)
    {
        MessageBoxW(nullptr, L"Не удалось запустить Half-Life Co-op", kTitle, MB_OK | MB_ICONERROR);
    }
    return 1;
}
