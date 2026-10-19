#include "Native.h"
#include <bcrypt.h>
#include <shlobj.h>
#include <algorithm>
#include <climits>
#include <memory>

#pragma comment(lib, "bcrypt.lib")

#ifndef HLC_LAUNCHER_VERSION
#error "Версию запускателя задаёт scripts/build-launcher.ps1"
#endif
#ifndef HLC_REPOSITORY
#error "Репозиторий запускателя задаёт scripts/build-launcher.ps1"
#endif

namespace hlc
{
namespace
{
class Hasher
{
public:
    Hasher()
    {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            throw std::wstring(L"Не удалось открыть SHA256 Windows");
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0)
        {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            algorithm_ = nullptr;
            throw std::wstring(L"Не удалось подготовить SHA256 Windows");
        }
    }
    ~Hasher()
    {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    void Add(const std::uint8_t* data, std::size_t size)
    {
        if (size > ULONG_MAX || BCryptHashData(hash_, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) < 0)
            throw std::wstring(L"Не удалось вычислить SHA256");
    }
    std::string Finish()
    {
        std::uint8_t digest[32]{};
        if (BCryptFinishHash(hash_, digest, sizeof(digest), 0) < 0)
            throw std::wstring(L"Не удалось завершить SHA256");
        const char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (const auto byte : digest)
        {
            result += digits[byte >> 4];
            result += digits[byte & 15];
        }
        return result;
    }
private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

Handle OpenRead(const std::wstring& path)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.Valid()) throw WindowsError(L"Не удалось прочитать " + path);
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file.Get(), &information) ||
        (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        throw std::wstring(L"Ожидается обычный файл: ") + path;
    return file;
}
}

std::wstring WindowsError(const std::wstring& operation, DWORD error)
{
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring result = operation + L" (" + std::to_wstring(error) + L")";
    if (length && message)
    {
        std::wstring description(message, length);
        LocalFree(message);
        while (!description.empty() && (description.back() == L'\r' || description.back() == L'\n'))
            description.pop_back();
        result += L": " + description;
    }
    return result;
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty()) return {};
    if (value.size() > INT_MAX) throw std::wstring(L"Слишком длинный текст UTF-8");
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (!length) throw std::wstring(L"Текст содержит некорректную кодировку UTF-8");
    std::wstring result(length, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length))
        throw WindowsError(L"Не удалось прочитать UTF-8");
    return result;
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    if (value.size() > INT_MAX) throw std::wstring(L"Слишком длинный текст Windows");
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!length) throw std::wstring(L"Текст содержит некорректные символы Windows");
    std::string result(length, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr))
        throw WindowsError(L"Не удалось записать UTF-8");
    return result;
}

std::wstring FullPath(const std::wstring& path)
{
    if (path.empty() || path.find(L'\0') != std::wstring::npos)
        throw std::wstring(L"Пустой или некорректный путь");
    const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!needed || needed > 32767) throw WindowsError(L"Не удалось определить полный путь");
    std::vector<wchar_t> buffer(needed);
    const DWORD length = GetFullPathNameW(path.c_str(), needed, buffer.data(), nullptr);
    if (!length || length >= needed) throw WindowsError(L"Не удалось определить полный путь");
    std::wstring result(buffer.data(), length);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    while (result.size() > 3 && result.back() == L'\\') result.pop_back();
    if (result.size() < 3 || result[1] != L':' || result[2] != L'\\' || result.find(L':', 2) != std::wstring::npos)
        throw std::wstring(L"Выберите папку на локальном диске Windows");
    return result;
}

bool SamePath(const std::wstring& first, const std::wstring& second)
{
    return CompareStringOrdinal(first.c_str(), -1, second.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring ModulePath()
{
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) throw WindowsError(L"Не удалось определить путь программы");
    return FullPath(std::wstring(buffer.data(), length));
}

std::wstring GuidText()
{
    GUID value{};
    if (FAILED(CoCreateGuid(&value))) throw std::wstring(L"Не удалось создать уникальное имя");
    wchar_t buffer[40]{};
    if (!StringFromGUID2(value, buffer, ARRAYSIZE(buffer))) throw std::wstring(L"Не удалось записать уникальное имя");
    return std::wstring(buffer + 1, 36);
}

std::wstring AssertPlainPath(const std::wstring& path, const std::wstring& boundary)
{
    const auto full = FullPath(path);
    const auto root = FullPath(boundary);
    const auto prefix = root.back() == L'\\' ? root : root + L"\\";
    if (!SamePath(full, root) && (full.size() < prefix.size() ||
        CompareStringOrdinal(full.data(), static_cast<int>(prefix.size()), prefix.data(), static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL))
        throw std::wstring(L"Путь выходит за пределы разрешённой папки: ") + full;
    auto current = std::filesystem::path(full);
    for (;;)
    {
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
        {
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
                throw std::wstring(L"Ссылки и junction не поддерживаются: ") + current.wstring();
        }
        else
        {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                throw WindowsError(L"Недоступен путь " + current.wstring(), error);
        }
        const auto parent = current.parent_path();
        if (parent.empty() || parent == current) break;
        current = parent;
    }
    return full;
}

std::wstring StateDirectory()
{
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &value)))
        throw std::wstring(L"Не удалось определить папку настроек пользователя");
    const std::filesystem::path root(value);
    CoTaskMemFree(value);
    const auto path = AssertPlainPath((root / L"HalfLifeCoop").wstring(), root.wstring());
    std::filesystem::create_directories(path);
    AssertPlainPath(path, root.wstring());
    return path;
}

Bytes ReadBytes(const std::wstring& path, std::size_t maximumBytes)
{
    auto file = OpenRead(path);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.Get(), &size)) throw WindowsError(L"Не удалось определить размер " + path);
    if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > maximumBytes)
        throw std::wstring(L"Размер файла превышает допустимый: ") + path;
    Bytes result(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < result.size())
    {
        DWORD read = 0;
        const DWORD count = static_cast<DWORD>(std::min<std::size_t>(result.size() - offset, 65536));
        if (!ReadFile(file.Get(), result.data() + offset, count, &read, nullptr) || !read)
            throw WindowsError(L"Не удалось прочитать целиком " + path);
        offset += read;
    }
    return result;
}

void WriteBytes(const std::wstring& path, const Bytes& bytes)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file.Valid()) throw WindowsError(L"Не удалось записать " + path);
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file.Get(), &information) || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::wstring(L"Запись через ссылку запрещена: ") + path;
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        DWORD written = 0;
        const DWORD count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 65536));
        if (!WriteFile(file.Get(), bytes.data() + offset, count, &written, nullptr) || !written)
            throw WindowsError(L"Не удалось записать целиком " + path);
        offset += written;
    }
    if (!FlushFileBuffers(file.Get())) throw WindowsError(L"Не удалось сохранить " + path);
}

void WriteText(const std::wstring& path, const std::string& text)
{
    WriteBytes(path, Bytes(text.begin(), text.end()));
}

std::string Sha256(const Bytes& bytes)
{
    Hasher hasher;
    hasher.Add(bytes.data(), bytes.size());
    return hasher.Finish();
}

std::string Sha256File(const std::wstring& path)
{
    auto file = OpenRead(path);
    Hasher hasher;
    std::uint8_t buffer[65536];
    for (;;)
    {
        DWORD read = 0;
        if (!ReadFile(file.Get(), buffer, sizeof(buffer), &read, nullptr))
            throw WindowsError(L"Не удалось проверить SHA256 " + path);
        if (!read) break;
        hasher.Add(buffer, read);
    }
    return hasher.Finish();
}

Bytes ResourceBytes(HINSTANCE instance, int identifier)
{
    const HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(identifier), RT_RCDATA);
    if (!resource) throw WindowsError(L"Не найден встроенный файл программы");
    const DWORD size = SizeofResource(instance, resource);
    const HGLOBAL loaded = LoadResource(instance, resource);
    const auto data = static_cast<const std::uint8_t*>(LockResource(loaded));
    if (!data || !size || size > 134217728) throw std::wstring(L"Повреждён встроенный файл программы");
    return Bytes(data, data + size);
}

std::array<unsigned, 3> ParseVersion(const std::string& value)
{
    std::array<unsigned, 3> result{};
    std::size_t position = 0;
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        const auto first = position;
        while (position < value.size() && value[position] >= '0' && value[position] <= '9')
        {
            if (position - first > 4) throw std::wstring(L"Некорректный номер версии");
            result[index] = result[index] * 10 + static_cast<unsigned>(value[position++] - '0');
            if (result[index] > 65535) throw std::wstring(L"Некорректный номер версии");
        }
        if (first == position || (position - first > 1 && value[first] == '0'))
            throw std::wstring(L"Некорректный номер версии");
        if (index < 2 && (position == value.size() || value[position++] != '.'))
            throw std::wstring(L"Некорректный номер версии");
    }
    if (position != value.size()) throw std::wstring(L"Некорректный номер версии");
    return result;
}

int CompareVersions(const std::string& first, const std::string& second)
{
    const auto left = ParseVersion(first);
    const auto right = ParseVersion(second);
    return left == right ? 0 : left < right ? -1 : 1;
}

std::string LauncherVersion() { return HLC_LAUNCHER_VERSION; }
std::string Repository() { return HLC_REPOSITORY; }

void CheckPe(const std::wstring& path, bool dll)
{
    const auto bytes = ReadBytes(path, 134217728);
    if (bytes.size() < sizeof(IMAGE_DOS_HEADER) + sizeof(IMAGE_NT_HEADERS32))
        throw std::wstring(L"Повреждён исполняемый файл");
    IMAGE_DOS_HEADER dos{};
    memcpy(&dos, bytes.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos)) ||
        static_cast<std::size_t>(dos.e_lfanew) > bytes.size() - sizeof(IMAGE_NT_HEADERS32))
        throw std::wstring(L"Повреждён заголовок исполняемого файла");
    IMAGE_NT_HEADERS32 header{};
    memcpy(&header, bytes.data() + dos.e_lfanew, sizeof(header));
    if (header.Signature != IMAGE_NT_SIGNATURE || header.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        header.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        !(header.FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) ||
        ((header.FileHeader.Characteristics & IMAGE_FILE_DLL) != 0) != dll)
        throw std::wstring(L"Ожидается исполняемый файл Windows x86");
}

std::wstring QuoteArgument(const std::wstring& value)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : value)
    {
        if (character == L'\\') ++slashes;
        else
        {
            result.append(slashes * (character == L'\"' ? 2 : 1), L'\\');
            slashes = 0;
            if (character == L'\"') result += L'\\';
            result += character;
        }
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}

HANDLE StartProcess(const std::wstring& executable, const std::vector<std::wstring>& arguments,
    const std::wstring& workingDirectory)
{
    std::wstring command = QuoteArgument(executable);
    for (const auto& argument : arguments) command += L" " + QuoteArgument(argument);
    if (command.size() >= 32767) throw std::wstring(L"Слишком длинная команда запуска");
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0,
        nullptr, workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup, &process))
        throw WindowsError(L"Не удалось запустить " + executable);
    CloseHandle(process.hThread);
    return process.hProcess;
}
}
