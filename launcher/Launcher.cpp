#include "Native.h"
#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>
#include <memory>
#include <thread>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace
{
using namespace hlc;
const wchar_t kWindowClass[] = L"HalfLifeCoopLauncherWindow";
const wchar_t kTitle[] = L"Half-Life Co-op";
const UINT kLogMessage = WM_APP + 1;
const UINT kFinishedMessage = WM_APP + 2;
const int kCloseButtonId = 1001;
const UINT kLicensesMenuId = 0x100;

struct Result
{
    DWORD exitCode = 1;
    std::wstring error;
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
    std::wstring logPath;
    bool running = true;
    DWORD exitCode = 1;
};

Application gApplication;

Options ParseOptions()
{
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) throw WindowsError(L"Не удалось прочитать параметры запуска");
    std::unique_ptr<wchar_t*, decltype(&LocalFree)> ownedArguments(arguments, LocalFree);
    Options options;
    for (int index = 1; index < count; ++index)
    {
        const std::wstring argument = arguments[index];
        if (argument == L"--game-dir" || argument == L"--apply-update" || argument == L"--cleanup-update")
        {
            if (++index >= count || !arguments[index][0])
                throw std::wstring(L"После параметра укажите путь: ") + argument;
            const auto path = FullPath(arguments[index]);
            if (argument == L"--game-dir") options.gameDirectory = path;
            else if (argument == L"--apply-update") options.applyUpdateState = path;
            else options.cleanupUpdateState = path;
        }
        else if (argument == L"--install-only") options.installOnly = true;
        else if (argument == L"--skip-update") options.skipUpdate = true;
        else if (argument == L"--after-update") options.afterUpdate = true;
        else throw std::wstring(L"Неизвестный параметр запуска: ") + argument;
    }
    if (!options.applyUpdateState.empty() && !options.cleanupUpdateState.empty())
        throw std::wstring(L"Нельзя одновременно применять и завершать обновление");
    return options;
}

void PostLog(const std::wstring& message)
{
    const auto line = message.empty() || message.back() != L'\n' ? message + L"\r\n" : message;
    if (!gApplication.logPath.empty())
    {
        Handle file(CreateFileW(gApplication.logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (file.Valid())
        {
            const auto bytes = WideToUtf8(line);
            DWORD written = 0;
            WriteFile(file.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        }
    }
    auto text = std::make_unique<std::wstring>(line);
    if (PostMessageW(gApplication.window, kLogMessage, 0, reinterpret_cast<LPARAM>(text.get()))) text.release();
}

void Work()
{
    auto result = std::make_unique<Result>();
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try
    {
        const auto state = StateDirectory();
        // Отдельный журнал помощника не мешает чтению журнала обычного запуска
        gApplication.logPath = AssertPlainPath((std::filesystem::path(state) /
            (gApplication.options.applyUpdateState.empty() ? L"launcher.log" : L"update-helper.log")).wstring(), state);
        WriteText(gApplication.logPath, "");
        PostLog(L"Half-Life Co-op " + Utf8ToWide(LauncherVersion()));
        if (!gApplication.options.applyUpdateState.empty())
        {
            result->exitCode = static_cast<DWORD>(ApplyUpdate(gApplication.options.applyUpdateState, PostLog));
        }
        else
        {
            if (!gApplication.options.cleanupUpdateState.empty() &&
                !CleanupUpdate(gApplication.options.cleanupUpdateState, PostLog))
                throw std::wstring(L"Не удалось завершить предыдущее обновление запускателя");
            const auto game = ResolveGameDirectory(gApplication.options.gameDirectory, gApplication.window);
            SaveGameDirectory(game);
            PostLog(L"Half-Life: " + game);
            AssertGameStopped(game);
            if (!TryStartSelfUpdate(gApplication.options, game, PostLog))
            {
                InstallEmbedded(gApplication.instance, game, PostLog);
                ValidateInstalled(game);
                if (!gApplication.options.installOnly)
                {
                    PostLog(L"Запуск Half-Life Co-op...");
                    LaunchGame(game);
                }
                else PostLog(L"Установка завершена");
            }
            result->exitCode = 0;
        }
    }
    catch (const std::wstring& error)
    {
        result->error = error;
    }
    catch (const std::exception& error)
    {
        result->error = L"Не удалось продолжить: " + Utf8ToWide(error.what());
    }
    catch (...)
    {
        result->error = L"Не удалось продолжить установку";
    }
    if (!result->error.empty()) PostLog(result->error);
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (PostMessageW(gApplication.window, kFinishedMessage, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
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
        AppendMenuW(GetSystemMenu(window, FALSE), MF_SEPARATOR, 0, nullptr);
        AppendMenuW(GetSystemMenu(window, FALSE), MF_STRING, kLicensesMenuId, L"Лицензии компонентов");
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
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == kLicensesMenuId)
        {
            try
            {
                const auto bytes = ResourceBytes(gApplication.instance, 104);
                const auto license = Utf8ToWide(std::string(bytes.begin(), bytes.end()));
                MessageBoxW(window, license.c_str(), L"JSON for Modern C++ — лицензия MIT", MB_OK | MB_ICONINFORMATION);
            }
            catch (...)
            {
                MessageBoxW(window, L"Не удалось прочитать сведения о компонентах", kTitle, MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
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

using namespace hlc;
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    try
    {
        gApplication.instance = instance;
        gApplication.options = ParseOptions();
        gApplication.launcherPath = ModulePath();

        Handle mutex(CreateMutexW(nullptr, FALSE, gApplication.options.applyUpdateState.empty() ?
            L"Local\\HalfLifeCoopLauncher" : nullptr));
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
