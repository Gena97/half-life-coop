#include "Native.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <winver.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <set>
#include <stdexcept>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "version.lib")

namespace hlc
{
namespace
{
namespace fs = std::filesystem;
constexpr std::size_t kMaximumExecutable = 134217728;
constexpr std::size_t kMaximumManifest = 65536;
constexpr std::size_t kSignatureLength = 384;
const wchar_t kStagePrefix[] = L"hlcoop-update-";
const wchar_t* const kStageFiles[] = {
    L"state.json", L"state.new", L"helper.exe", L"HLCoopLauncher.exe",
    L"previous.exe", L"launcher-update.json", L"launcher-update.sig"
};

class InternetHandle
{
public:
    explicit InternetHandle(HINTERNET value = nullptr) : value_(value) {}
    ~InternetHandle() { if (value_) WinHttpCloseHandle(value_); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET Get() const { return value_; }
private:
    HINTERNET value_;
};

class UpdateMutex
{
public:
    void Lock()
    {
        handle_.Reset(CreateMutexW(nullptr, FALSE, L"Local\\HalfLifeCoopLauncher"));
        if (!handle_.Valid()) throw WindowsError(L"Не удалось защитить замену запускателя");
        DWORD result = WaitForSingleObject(handle_.Get(), 30000);
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
            throw std::wstring(L"Другой запускатель ещё работает. Замена отложена");
        owned_ = true;
    }
    void Unlock()
    {
        if (owned_) { ReleaseMutex(handle_.Get()); owned_ = false; }
        handle_.Reset();
    }
    ~UpdateMutex() { Unlock(); }
private:
    Handle handle_;
    bool owned_ = false;
};

struct VerifiedManifest
{
    std::string version;
    std::string tag;
    std::string sha256;
    std::uint64_t bytes = 0;
};

struct UpdateState
{
    std::wstring root;
    std::wstring parent;
    std::wstring target;
    Json data;
    VerifiedManifest manifest;
};

bool Exists(const std::wstring& path)
{
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) return true;
    DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
    throw WindowsError(L"Не удалось проверить файл " + path, error);
}

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool IsHash(const std::string& value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

bool IsStageName(const std::wstring& value)
{
    const std::wstring prefix = kStagePrefix;
    if (value.compare(0, prefix.size(), prefix) != 0) return false;
    const std::wstring suffix = value.substr(prefix.size());
    if (suffix.size() != 32 && suffix.size() != 36) return false;
    for (std::size_t index = 0; index < suffix.size(); ++index)
    {
        if (suffix.size() == 36 && (index == 8 || index == 13 || index == 18 || index == 23))
        {
            if (suffix[index] != L'-') return false;
        }
        else if (!((suffix[index] >= L'0' && suffix[index] <= L'9') ||
            (suffix[index] >= L'a' && suffix[index] <= L'f') ||
            (suffix[index] >= L'A' && suffix[index] <= L'F'))) return false;
    }
    return true;
}

bool IsTargetName(const std::wstring& value)
{
    if (value.empty() || value.size() > 255 || value == L"." || value == L".." ||
        value.find_first_of(L"<>:\"/\\|?*\r\n") != std::wstring::npos ||
        value.back() == L'.' || value.back() == L' ') return false;
    if (value.size() < 5) return false;
    return CompareStringOrdinal(value.c_str() + value.size() - 4, 4, L".exe", 4, TRUE) == CSTR_EQUAL;
}

std::string JsonString(const Json& value, const char* name, std::size_t maximum)
{
    const Json& field = value.at(name);
    if (!field.is_string()) throw std::wstring(L"Повреждены сведения об обновлении");
    std::string text = field.get<std::string>();
    if (text.empty() || text.size() > maximum || text.find('\0') != std::string::npos)
        throw std::wstring(L"Недопустимая строка в сведениях об обновлении");
    return text;
}

std::uint64_t JsonNumber(const Json& value, const char* name)
{
    const Json& field = value.at(name);
    if (!field.is_number_unsigned() && !(field.is_number_integer() && field.get<std::int64_t>() >= 0))
        throw std::wstring(L"Недопустимое число в сведениях об обновлении");
    return field.get<std::uint64_t>();
}

Json ParseJson(const Bytes& bytes)
{
    try { return Json::parse(bytes.begin(), bytes.end()); }
    catch (const std::exception&) { throw std::wstring(L"Не удалось прочитать сведения об обновлении"); }
}

std::wstring QueryHeader(HINTERNET request, DWORD field)
{
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, field, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0 || bytes > 32768)
        throw WindowsError(L"Не удалось прочитать заголовок GitHub");
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1);
    if (!WinHttpQueryHeaders(request, field, WINHTTP_HEADER_NAME_BY_INDEX,
        buffer.data(), &bytes, WINHTTP_NO_HEADER_INDEX))
        throw WindowsError(L"Не удалось прочитать заголовок GitHub");
    return std::wstring(buffer.data());
}

Bytes Download(const std::wstring& initialUrl, std::size_t maximum)
{
    InternetHandle session(WinHttpOpen(L"HalfLifeCoopLauncher/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.Get()) throw WindowsError(L"Не удалось открыть соединение с GitHub");
    if (!WinHttpSetTimeouts(session.Get(), 5000, 5000, 10000, 15000))
        throw WindowsError(L"Не удалось ограничить время соединения");
    const ULONGLONG deadline = GetTickCount64() + 60000;
    std::wstring url = initialUrl;
    for (unsigned redirects = 0; redirects <= 5; ++redirects)
    {
        if (GetTickCount64() > deadline) throw std::wstring(L"GitHub не ответил вовремя");
        if (url.empty() || url.size() > 16384 || url.find(L'#') != std::wstring::npos)
            throw std::wstring(L"Недопустимый адрес обновления");
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) ||
            parts.nScheme != INTERNET_SCHEME_HTTPS || parts.nPort != INTERNET_DEFAULT_HTTPS_PORT ||
            parts.dwUserNameLength != 0 || parts.dwPasswordLength != 0)
            throw std::wstring(L"Обновления разрешены только по HTTPS без учётных данных в адресе");
        std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::transform(host.begin(), host.end(), host.begin(), [](wchar_t ch) -> wchar_t {
            return ch >= L'A' && ch <= L'Z' ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
        });
        const std::set<std::wstring> allowed = {
            L"api.github.com", L"github.com", L"objects.githubusercontent.com",
            L"release-assets.githubusercontent.com", L"github-releases.githubusercontent.com"
        };
        if (allowed.count(host) == 0) throw std::wstring(L"Адрес загрузки не принадлежит разрешённым серверам GitHub");
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        if (path.empty()) path = L"/";
        InternetHandle connection(WinHttpConnect(session.Get(), host.c_str(), parts.nPort, 0));
        if (!connection.Get()) throw WindowsError(L"Не удалось подключиться к GitHub");
        InternetHandle request(WinHttpOpenRequest(connection.Get(), L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (!request.Get()) throw WindowsError(L"Не удалось подготовить загрузку");
        DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
        if (!WinHttpSetOption(request.Get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)))
            throw WindowsError(L"Не удалось проверить перенаправления GitHub");
        const wchar_t* headers = host == L"api.github.com" ?
            L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n" :
            L"Accept: application/octet-stream\r\n";
        if (!WinHttpSendRequest(request.Get(), headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA,
            0, 0, 0) || !WinHttpReceiveResponse(request.Get(), nullptr))
            throw WindowsError(L"Не удалось получить обновление с GitHub");
        DWORD status = 0, statusBytes = sizeof(status);
        if (!WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes, WINHTTP_NO_HEADER_INDEX))
            throw WindowsError(L"Не удалось прочитать ответ GitHub");
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
        {
            std::wstring redirect = QueryHeader(request.Get(), WINHTTP_QUERY_LOCATION);
            if (!redirect.empty() && redirect[0] == L'/' && (redirect.size() == 1 || redirect[1] != L'/'))
                redirect = L"https://" + host + redirect;
            url = redirect;
            continue;
        }
        if (status != 200) throw std::wstring(L"GitHub вернул код ") + std::to_wstring(status);
        DWORD advertised = 0, advertisedBytes = sizeof(advertised);
        if (WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &advertised, &advertisedBytes, WINHTTP_NO_HEADER_INDEX) && advertised > maximum)
            throw std::wstring(L"Файл обновления превышает допустимый размер");
        Bytes result;
        if (advertised) result.reserve(advertised);
        for (;;)
        {
            if (GetTickCount64() > deadline) throw std::wstring(L"Превышено время загрузки обновления");
            std::uint8_t buffer[65536];
            DWORD received = 0;
            if (!WinHttpReadData(request.Get(), buffer, sizeof(buffer), &received))
                throw WindowsError(L"Не удалось прочитать загружаемый файл");
            if (received == 0) break;
            if (received > maximum || result.size() > maximum - received)
                throw std::wstring(L"Файл обновления превышает допустимый размер");
            result.insert(result.end(), buffer, buffer + received);
        }
        if (advertised && result.size() != advertised) throw std::wstring(L"GitHub передал неполный файл");
        return result;
    }
    throw std::wstring(L"Слишком много перенаправлений при загрузке");
}

Bytes DecodeBase64(const std::string& text)
{
    DWORD size = 0;
    if (text.empty() || text.size() > 8192 || !CryptStringToBinaryA(text.c_str(),
        static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT,
        nullptr, &size, nullptr, nullptr) || size > 4096)
        throw std::wstring(L"Повреждён встроенный ключ обновлений");
    Bytes bytes(size);
    if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()),
        CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, bytes.data(), &size, nullptr, nullptr))
        throw std::wstring(L"Повреждён встроенный ключ обновлений");
    bytes.resize(size);
    return bytes;
}

Bytes DigestBytes(const Bytes& data)
{
    const std::string hash = Lower(Sha256(data));
    if (!IsHash(hash)) throw std::wstring(L"Не удалось вычислить контрольную сумму");
    Bytes bytes(32);
    auto digit = [](char value) -> unsigned { return value <= '9' ? value - '0' : value - 'a' + 10; };
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::uint8_t>((digit(hash[index * 2]) << 4) | digit(hash[index * 2 + 1]));
    return bytes;
}

VerifiedManifest VerifyManifest(const Bytes& data, const Bytes& signature)
{
    if (data.empty() || data.size() > kMaximumManifest || signature.size() != kSignatureLength)
        throw std::wstring(L"Нет корректной цифровой подписи обновления");
    Json configuration = ParseJson(ResourceBytes(GetModuleHandleW(nullptr), 103));
    if (JsonString(configuration, "algorithm", 32) != "RSA-SHA256")
        throw std::wstring(L"Неизвестный алгоритм подписи обновлений");
    const std::string keyId = JsonString(configuration, "keyId", 128);
    Bytes modulus = DecodeBase64(JsonString(configuration, "modulus", 8192));
    Bytes exponent = DecodeBase64(JsonString(configuration, "exponent", 128));
    if (modulus.size() != 384 || exponent.empty() || exponent.size() > 8)
        throw std::wstring(L"Встроенный ключ должен быть RSA 3072 бит");
    BCRYPT_RSAKEY_BLOB header{};
    header.Magic = BCRYPT_RSAPUBLIC_MAGIC;
    header.BitLength = 3072;
    header.cbPublicExp = static_cast<ULONG>(exponent.size());
    header.cbModulus = static_cast<ULONG>(modulus.size());
    Bytes blob(sizeof(header));
    memcpy(blob.data(), &header, sizeof(header));
    blob.insert(blob.end(), exponent.begin(), exponent.end());
    blob.insert(blob.end(), modulus.begin(), modulus.end());
    Bytes digest = DigestBytes(data);
    BCRYPT_ALG_HANDLE provider = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_RSA_ALGORITHM, nullptr, 0) < 0)
        throw std::wstring(L"Не удалось открыть проверку цифровой подписи");
    NTSTATUS status = BCryptImportKeyPair(provider, nullptr, BCRYPT_RSAPUBLIC_BLOB,
        &key, blob.data(), static_cast<ULONG>(blob.size()), 0);
    if (status < 0)
    {
        BCryptCloseAlgorithmProvider(provider, 0);
        throw std::wstring(L"Не удалось прочитать открытый ключ обновлений");
    }
    BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
    status = BCryptVerifySignature(key, &padding, digest.data(), static_cast<ULONG>(digest.size()),
        const_cast<PUCHAR>(signature.data()), static_cast<ULONG>(signature.size()), BCRYPT_PAD_PKCS1);
    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(provider, 0);
    if (status < 0) throw std::wstring(L"Цифровая подпись обновления не подтверждена. Файл не будет запущен");

    // Поля манифеста используем только после проверки подписи над исходными байтами
    Json manifest = ParseJson(data);
    if (!manifest.is_object() || JsonNumber(manifest, "schemaVersion") != 1 ||
        JsonString(manifest, "repository", 256) != Repository() ||
        JsonString(manifest, "file", 128) != "HLCoopLauncher.exe" ||
        JsonString(manifest, "keyId", 128) != keyId)
        throw std::wstring(L"Подписанный манифест относится к другому запускателю");
    VerifiedManifest result;
    result.version = JsonString(manifest, "version", 64);
    ParseVersion(result.version);
    result.tag = JsonString(manifest, "tag", 128);
    result.sha256 = Lower(JsonString(manifest, "sha256", 64));
    result.bytes = JsonNumber(manifest, "bytes");
    if (result.tag != "v" + result.version || !IsHash(result.sha256) ||
        result.bytes < 4096 || result.bytes > kMaximumExecutable)
        throw std::wstring(L"Недопустимые параметры подписанного обновления");
    return result;
}

void CheckExecutable(const std::wstring& path, const VerifiedManifest& manifest)
{
    AssertPlainPath(path, fs::path(path).parent_path().wstring());
    if (fs::file_size(path) != manifest.bytes || Lower(Sha256File(path)) != manifest.sha256)
        throw std::wstring(L"Размер или контрольная сумма нового запускателя не совпали с подписью");
    CheckPe(path, false);
    DWORD ignored = 0;
    DWORD bytes = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!bytes || bytes > 1048576) throw std::wstring(L"Новый запускатель не содержит корректной версии");
    Bytes versionData(bytes);
    if (!GetFileVersionInfoW(path.c_str(), 0, bytes, versionData.data()))
        throw WindowsError(L"Не удалось прочитать версию нового запускателя");
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedBytes = 0;
    if (!VerQueryValueW(versionData.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixedBytes) ||
        fixedBytes < sizeof(VS_FIXEDFILEINFO) || fixed->dwSignature != 0xFEEF04BD || fixed->dwFileType != VFT_APP)
        throw std::wstring(L"Повреждены сведения о новом запускателе");
    const auto expected = ParseVersion(manifest.version);
    if (HIWORD(fixed->dwFileVersionMS) != expected[0] || LOWORD(fixed->dwFileVersionMS) != expected[1] ||
        HIWORD(fixed->dwFileVersionLS) != expected[2] || LOWORD(fixed->dwFileVersionLS) != 0)
        throw std::wstring(L"Версия EXE не совпала с подписанным манифестом");
    wchar_t* product = nullptr;
    UINT productLength = 0;
    if (!VerQueryValueW(versionData.data(), L"\\StringFileInfo\\040904b0\\ProductName",
        reinterpret_cast<void**>(&product), &productLength) || productLength == 0 ||
        product[productLength - 1] != L'\0' || std::wstring(product, productLength - 1) != L"Half-Life Co-op")
        throw std::wstring(L"Подписанный EXE не является запускателем Half-Life Co-op");
}

std::uint64_t CreationTime(HANDLE process)
{
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user))
        throw WindowsError(L"Не удалось проверить время запуска процесса");
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}

void WaitForProcess(std::uint64_t processId, std::uint64_t creation, DWORD milliseconds)
{
    if (!processId || processId > MAXDWORD || processId == GetCurrentProcessId())
        throw std::wstring(L"Повреждён идентификатор процесса обновления");
    Handle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processId)));
    if (!process.Valid())
    {
        if (GetLastError() == ERROR_INVALID_PARAMETER) return;
        throw WindowsError(L"Не удалось проверить завершение запускателя");
    }
    if (CreationTime(process.Get()) != creation) return;
    if (WaitForSingleObject(process.Get(), milliseconds) != WAIT_OBJECT_0)
        throw std::wstring(L"Предыдущий запускатель ещё работает. Обновление оставлено до его завершения");
}

std::wstring StageFile(const UpdateState& state, const wchar_t* name)
{
    return AssertPlainPath((fs::path(state.root) / name).wstring(), state.root);
}

void CheckStageContents(const std::wstring& root)
{
    for (const fs::directory_entry& item : fs::directory_iterator(root))
    {
        const std::wstring name = item.path().filename().wstring();
        if (std::find(std::begin(kStageFiles), std::end(kStageFiles), name) == std::end(kStageFiles))
            throw std::wstring(L"Во временной папке обновления обнаружен посторонний файл");
        AssertPlainPath(item.path().wstring(), root);
        DWORD attributes = GetFileAttributesW(item.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            throw std::wstring(L"Недопустимый файл во временной папке обновления");
    }
}

void SaveState(UpdateState& state)
{
    const std::wstring temporary = StageFile(state, L"state.new");
    if (Exists(temporary) && !DeleteFileW(temporary.c_str()))
        throw WindowsError(L"Не удалось подготовить журнал обновления");
    const std::string text = state.data.dump(2);
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file.Valid()) throw WindowsError(L"Не удалось создать журнал обновления");
    DWORD written = 0;
    if (!WriteFile(file.Get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
        written != text.size() || !FlushFileBuffers(file.Get()))
        throw WindowsError(L"Не удалось полностью записать журнал обновления");
    file.Reset();
    const std::wstring destination = StageFile(state, L"state.json");
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw WindowsError(L"Не удалось сохранить журнал обновления");
}

UpdateState LoadState(const std::wstring& statePath, bool applying)
{
    UpdateState state;
    const std::wstring full = FullPath(statePath);
    fs::path file(full);
    state.root = file.parent_path().wstring();
    state.parent = file.parent_path().parent_path().wstring();
    if (file.filename() != L"state.json" || !IsStageName(file.parent_path().filename().wstring()) ||
        state.parent.empty()) throw std::wstring(L"Недопустимый путь журнала обновления");
    AssertPlainPath(state.root, state.parent);
    AssertPlainPath(full, state.root);
    CheckStageContents(state.root);
    state.data = ParseJson(ReadBytes(full, kMaximumManifest));
    if (!state.data.is_object() || JsonNumber(state.data, "schemaVersion") != 1)
        throw std::wstring(L"Неподдерживаемый журнал обновления");
    const std::wstring name = Utf8ToWide(JsonString(state.data, "targetName", 1024));
    if (!IsTargetName(name)) throw std::wstring(L"Недопустимое имя обновляемого EXE");
    state.target = AssertPlainPath((fs::path(state.parent) / name).wstring(), state.parent);
    if (!SamePath(ModulePath(), applying ? StageFile(state, L"helper.exe") : state.target))
        throw std::wstring(L"Журнал обновления не относится к этому EXE");
    const std::string oldHash = JsonString(state.data, "oldHash", 64);
    if (!IsHash(oldHash)) throw std::wstring(L"Повреждена контрольная сумма предыдущего запускателя");
    state.manifest = VerifyManifest(ReadBytes(StageFile(state, L"launcher-update.json"), kMaximumManifest),
        ReadBytes(StageFile(state, L"launcher-update.sig"), kSignatureLength));
    if (JsonString(state.data, "newVersion", 64) != state.manifest.version ||
        JsonString(state.data, "newHash", 64) != state.manifest.sha256)
        throw std::wstring(L"Журнал не совпадает с подписанным манифестом");
    const std::string phase = JsonString(state.data, "phase", 32);
    if (phase != "prepared" && phase != "replacing" && phase != "replaced" &&
        phase != "committed" && phase != "rolledBack")
        throw std::wstring(L"Неизвестная стадия транзакции обновления");
    JsonString(state.data, "gameDirectory", 32768);
    if (!state.data.at("installOnly").is_boolean() || !state.data.at("skipUpdate").is_boolean())
        throw std::wstring(L"Повреждены параметры повторного запуска");
    return state;
}

void RemoveStage(const UpdateState& state)
{
    AssertPlainPath(state.root, state.parent);
    CheckStageContents(state.root);
    // Удаляем только известные файлы этой транзакции без рекурсивного обхода
    for (const wchar_t* name : kStageFiles)
    {
        const std::wstring path = StageFile(state, name);
        if (Exists(path) && !DeleteFileW(path.c_str())) throw WindowsError(L"Не удалось удалить временный файл обновления");
    }
    if (!RemoveDirectoryW(state.root.c_str())) throw WindowsError(L"Не удалось удалить временную папку обновления");
}

bool HelperIsRunning(const UpdateState& state)
{
    const std::wstring expected = StageFile(state, L"helper.exe");
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.Valid()) throw WindowsError(L"Не удалось проверить работающий помощник обновления");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.Get(), &entry)) throw WindowsError(L"Не удалось получить список процессов");
    do
    {
        if (!SamePath(entry.szExeFile, L"helper.exe")) continue;
        Handle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        if (!process.Valid())
        {
            if (GetLastError() == ERROR_INVALID_PARAMETER) continue;
            throw WindowsError(L"Не удалось проверить процесс помощника обновления");
        }
        wchar_t path[32768]{};
        DWORD length = ARRAYSIZE(path);
        if (!QueryFullProcessImageNameW(process.Get(), 0, path, &length))
        {
            if (WaitForSingleObject(process.Get(), 0) == WAIT_OBJECT_0) continue;
            throw WindowsError(L"Не удалось проверить путь помощника обновления");
        }
        if (SamePath(FullPath(std::wstring(path, length)), expected) &&
            WaitForSingleObject(process.Get(), 0) == WAIT_TIMEOUT) return true;
    } while (Process32NextW(snapshot.Get(), &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) throw WindowsError(L"Не удалось завершить проверку процессов");
    return false;
}

void RecoverStoppedUpdate(UpdateState& state, const Log& log)
{
    const std::string oldHash = JsonString(state.data, "oldHash", 64);
    const std::string actualHash = Lower(Sha256File(state.target));
    const std::wstring backup = StageFile(state, L"previous.exe");
    const std::wstring staged = StageFile(state, L"HLCoopLauncher.exe");
    const std::wstring helper = StageFile(state, L"helper.exe");
    if (Exists(helper) && Lower(Sha256File(helper)) != oldHash)
        throw std::wstring(L"Помощник прерванного обновления изменён. Автоматическое восстановление остановлено");
    if (Exists(backup) && Lower(Sha256File(backup)) != oldHash)
        throw std::wstring(L"Резервная копия прерванного обновления повреждена");
    if (Exists(staged)) CheckExecutable(staged, state.manifest);
    if (actualHash == state.manifest.sha256)
    {
        CheckExecutable(state.target, state.manifest);
        if (!Exists(backup)) throw std::wstring(L"Нет резервной копии для завершения прерванного обновления");
        log(L"Новая версия уже установлена и проверена. Завершение прерванного обновления...");
    }
    else if (actualHash == oldHash)
    {
        log(L"Предыдущая версия не повреждена. Очистка прерванного обновления...");
    }
    else throw std::wstring(L"Файл запускателя не совпадает ни с прежней, ни с подписанной новой версией");
    RemoveStage(state);
}

bool CheckPending(const Log& log)
{
    const fs::path executable(ModulePath());
    const std::wstring parent = executable.parent_path().wstring();
    AssertPlainPath(parent, parent);
    for (const fs::directory_entry& entry : fs::directory_iterator(parent))
    {
        if (!IsStageName(entry.path().filename().wstring())) continue;
        AssertPlainPath(entry.path().wstring(), parent);
        if (!entry.is_directory()) continue;
        const std::wstring statePath = (entry.path() / L"state.json").wstring();
        if (!Exists(statePath)) continue;
        const Json state = ParseJson(ReadBytes(AssertPlainPath(statePath, entry.path().wstring()), kMaximumManifest));
        if (state.is_object() && state.contains("targetName") && state["targetName"].is_string() &&
            !SamePath(executable.filename().wstring(), Utf8ToWide(state["targetName"].get<std::string>()))) continue;
        try
        {
            UpdateState pending = LoadState(statePath, false);
            if (HelperIsRunning(pending))
            {
                log(L"Помощник предыдущего обновления ещё открыт. Дождитесь его завершения или закройте окно с ошибкой");
                return true;
            }
            RecoverStoppedUpdate(pending, log);
        }
        catch (const std::wstring& error)
        {
            throw std::wstring(L"Нельзя безопасно завершить прерванное обновление: ") + error + L". Журнал: " + statePath;
        }
    }
    return false;
}

Json FindAsset(const Json& release, const std::string& name, std::size_t maximum)
{
    if (!release.at("assets").is_array() || release["assets"].size() > 1000)
        throw std::wstring(L"Недопустимый список файлов релиза");
    const Json* found = nullptr;
    for (const Json& asset : release["assets"])
    {
        if (asset.is_object() && asset.value("name", std::string()) == name)
        {
            if (found) throw std::wstring(L"В релизе несколько файлов с одинаковым именем");
            found = &asset;
        }
    }
    if (!found || JsonNumber(*found, "size") == 0 || JsonNumber(*found, "size") > maximum)
        throw std::wstring(L"В релизе нет необходимого подписанного файла обновления");
    const std::string expected = "https://github.com/" + Repository() + "/releases/download/" +
        JsonString(release, "tag_name", 128) + "/" + name;
    if (JsonString(*found, "browser_download_url", 16384) != expected)
        throw std::wstring(L"Адрес файла не относится к ожидаемому репозиторию и релизу");
    return *found;
}

Bytes DownloadAsset(const Json& asset, std::size_t maximum)
{
    Bytes bytes = Download(Utf8ToWide(JsonString(asset, "browser_download_url", 16384)), maximum);
    if (bytes.size() != JsonNumber(asset, "size")) throw std::wstring(L"Размер скачанного файла не совпал с релизом");
    return bytes;
}

void RollBack(UpdateState& state)
{
    const std::wstring backup = StageFile(state, L"previous.exe");
    const std::string oldHash = JsonString(state.data, "oldHash", 64);
    if (!Exists(backup) || Lower(Sha256File(backup)) != oldHash)
        throw std::wstring(L"Не найдена корректная резервная копия запускателя");
    if (!Exists(state.target) || Lower(Sha256File(state.target)) != state.manifest.sha256)
        throw std::wstring(L"Запускатель изменён другим процессом. Автоматический откат остановлен");
    if (!ReplaceFileW(state.target.c_str(), backup.c_str(), nullptr, 0, nullptr, nullptr))
        throw WindowsError(L"Не удалось восстановить предыдущий запускатель");
    if (Lower(Sha256File(state.target)) != oldHash)
        throw std::wstring(L"Контрольная сумма восстановленного запускателя не совпала");
    state.data["phase"] = "rolledBack";
    SaveState(state);
}
}

bool TryStartSelfUpdate(const Options& options, const std::wstring& gameDirectory, const Log& log)
{
    if (CheckPending(log)) return true;
    if (options.skipUpdate || options.afterUpdate) return false;
    UpdateState state;
    bool stageCreated = false;
    bool helperStarted = false;
    try
    {
        log(L"Проверка новой версии запускателя на GitHub...");
        const std::string repository = Repository();
        if (repository.empty() || repository.size() > 256 || std::count(repository.begin(), repository.end(), '/') != 1 ||
            repository.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-/.") != std::string::npos)
            throw std::wstring(L"Не настроен адрес репозитория GitHub");
        const Json release = ParseJson(Download(Utf8ToWide("https://api.github.com/repos/" + repository + "/releases/latest"), 4194304));
        if (!release.is_object() || release.at("draft") != false || release.at("prerelease") != false)
            throw std::wstring(L"GitHub не вернул стабильный релиз");
        const std::string tag = JsonString(release, "tag_name", 128);
        if (tag.empty() || tag[0] != 'v') throw std::wstring(L"Недопустимый тег релиза");
        const std::string version = tag.substr(1);
        ParseVersion(version);
        if (CompareVersions(version, LauncherVersion()) <= 0)
        {
            log(L"Установлена актуальная версия запускателя");
            return false;
        }
        const Json manifestAsset = FindAsset(release, "launcher-update.json", kMaximumManifest);
        const Json signatureAsset = FindAsset(release, "launcher-update.sig", kSignatureLength);
        const Bytes manifestBytes = DownloadAsset(manifestAsset, kMaximumManifest);
        const Bytes signature = DownloadAsset(signatureAsset, kSignatureLength);
        state.manifest = VerifyManifest(manifestBytes, signature);
        if (state.manifest.tag != tag || state.manifest.version != version)
            throw std::wstring(L"Подпись относится к другому релизу");
        const Json executableAsset = FindAsset(release, "HLCoopLauncher.exe", kMaximumExecutable);
        if (JsonNumber(executableAsset, "size") != state.manifest.bytes)
            throw std::wstring(L"Подписанный размер EXE не совпал с релизом");
        log(L"Загрузка подписанного запускателя " + Utf8ToWide(version) + L"...");
        const Bytes executableBytes = DownloadAsset(executableAsset, kMaximumExecutable);
        if (executableBytes.size() != state.manifest.bytes || Lower(Sha256(executableBytes)) != state.manifest.sha256)
            throw std::wstring(L"Подписанная контрольная сумма EXE не совпала");

        const fs::path current(ModulePath());
        state.parent = current.parent_path().wstring();
        state.target = AssertPlainPath(current.wstring(), state.parent);
        state.root = AssertPlainPath((current.parent_path() / (std::wstring(kStagePrefix) + GuidText())).wstring(), state.parent);
        if (!IsStageName(fs::path(state.root).filename().wstring()) ||
            !CreateDirectoryW(state.root.c_str(), nullptr)) throw WindowsError(L"Не удалось подготовить папку обновления рядом с EXE");
        stageCreated = true;
        WriteBytes(StageFile(state, L"launcher-update.json"), manifestBytes);
        WriteBytes(StageFile(state, L"launcher-update.sig"), signature);
        WriteBytes(StageFile(state, L"HLCoopLauncher.exe"), executableBytes);
        CheckExecutable(StageFile(state, L"HLCoopLauncher.exe"), state.manifest);
        const std::string oldHash = Lower(Sha256File(state.target));
        const std::wstring helperPath = StageFile(state, L"helper.exe");
        if (!CopyFileW(state.target.c_str(), helperPath.c_str(), TRUE) || Lower(Sha256File(helperPath)) != oldHash)
            throw std::wstring(L"Не удалось подготовить проверенную копию помощника обновления");
        state.data = {
            {"schemaVersion", 1}, {"phase", "prepared"}, {"targetName", WideToUtf8(current.filename().wstring())},
            {"oldHash", oldHash}, {"newHash", state.manifest.sha256}, {"newVersion", state.manifest.version},
            {"parentPid", GetCurrentProcessId()}, {"parentCreation", CreationTime(GetCurrentProcess())},
            {"helperPid", 0}, {"helperCreation", 0}, {"gameDirectory", WideToUtf8(FullPath(gameDirectory))},
            {"installOnly", options.installOnly}, {"skipUpdate", options.skipUpdate}
        };
        SaveState(state);
        Handle helper(StartProcess(helperPath, {L"--apply-update", StageFile(state, L"state.json")}, state.parent));
        if (!helper.Valid()) throw std::wstring(L"Не удалось запустить помощник обновления");
        helperStarted = true;
        state.data["helperPid"] = GetProcessId(helper.Get());
        state.data["helperCreation"] = CreationTime(helper.Get());
        SaveState(state);
        log(L"Новая версия проверена. Запускатель перезапустится после замены");
        return true;
    }
    catch (const std::wstring& error)
    {
        if (helperStarted) return true;
        log(L"Обновление отложено: " + error);
    }
    catch (const std::exception&)
    {
        if (helperStarted) return true;
        log(L"Обновление отложено: не удалось проверить или загрузить новый релиз");
    }
    if (stageCreated && !state.root.empty() && Exists(state.root))
    {
        try { RemoveStage(state); }
        catch (const std::wstring& error) { log(L"Временная папка оставлена: " + state.root + L". " + error); }
        catch (...) { log(L"Временная папка оставлена: " + state.root); }
    }
    return false;
}

int ApplyUpdate(const std::wstring& statePath, const Log& log)
{
    UpdateState state;
    UpdateMutex mutex;
    bool replaced = false;
    bool restarted = false;
    try
    {
        state = LoadState(statePath, true);
        if (JsonString(state.data, "phase", 32) != "prepared" ||
            CompareVersions(state.manifest.version, LauncherVersion()) <= 0)
            throw std::wstring(L"Эта транзакция обновления уже выполнялась или версия не новее текущей");
        const std::string oldHash = JsonString(state.data, "oldHash", 64);
        if (Lower(Sha256File(ModulePath())) != oldHash)
            throw std::wstring(L"Помощник не совпадает с предыдущим запускателем");
        log(L"Ожидание завершения предыдущего запускателя...");
        WaitForProcess(JsonNumber(state.data, "parentPid"), JsonNumber(state.data, "parentCreation"), 120000);
        mutex.Lock();
        state.data["helperPid"] = GetCurrentProcessId();
        state.data["helperCreation"] = CreationTime(GetCurrentProcess());
        SaveState(state);
        CheckExecutable(StageFile(state, L"HLCoopLauncher.exe"), state.manifest);
        if (!Exists(state.target) || Lower(Sha256File(state.target)) != oldHash)
            throw std::wstring(L"Предыдущий запускатель изменился. Замена остановлена");
        const std::wstring backup = StageFile(state, L"previous.exe");
        if (Exists(backup)) throw std::wstring(L"Резервная копия уже существует. Замена остановлена");
        state.data["phase"] = "replacing";
        SaveState(state);
        if (!ReplaceFileW(state.target.c_str(), StageFile(state, L"HLCoopLauncher.exe").c_str(),
            backup.c_str(), 0, nullptr, nullptr)) throw WindowsError(L"Не удалось заменить запускатель");
        replaced = true;
        CheckExecutable(state.target, state.manifest);
        if (Lower(Sha256File(backup)) != oldHash) throw std::wstring(L"Резервная копия запускателя повреждена");
        state.data["phase"] = "replaced";
        SaveState(state);
        std::vector<std::wstring> arguments = {L"--after-update", L"--cleanup-update", StageFile(state, L"state.json"),
            L"--game-dir", Utf8ToWide(JsonString(state.data, "gameDirectory", 32768))};
        if (state.data.at("installOnly").get<bool>()) arguments.push_back(L"--install-only");
        if (state.data.at("skipUpdate").get<bool>()) arguments.push_back(L"--skip-update");
        // Новый процесс сам ждёт завершения помощника перед удалением его файлов
        mutex.Unlock();
        Handle process(StartProcess(state.target, arguments, state.parent));
        if (!process.Valid()) throw std::wstring(L"Не удалось запустить обновлённый запускатель");
        restarted = true;
        state.data["phase"] = "committed";
        SaveState(state);
        log(L"Запускатель обновлён до " + Utf8ToWide(state.manifest.version));
        return 0;
    }
    catch (const std::wstring& error)
    {
        log(L"Обновление не завершено: " + error);
    }
    catch (const std::exception&)
    {
        log(L"Не удалось завершить обновление запускателя");
    }
    if (restarted)
    {
        // Новая версия уже работает и завершит очистку по состоянию replaced
        log(L"Новый запускатель запущен. Завершение очистки передано ему");
        return 0;
    }
    if (replaced)
    {
        try
        {
            RollBack(state);
            log(L"Предыдущий запускатель восстановлен. Журнал: " + StageFile(state, L"state.json"));
        }
        catch (const std::wstring& error) { log(L"Откат не завершён: " + error + L". Резервная копия: " + state.root); }
        catch (...) { log(L"Не удалось восстановить запускатель. Резервная копия: " + state.root); }
    }
    else if (!state.root.empty() && !state.data.empty())
    {
        try
        {
            const std::string oldHash = JsonString(state.data, "oldHash", 64);
            if (Exists(state.target) && Lower(Sha256File(state.target)) == oldHash &&
                JsonNumber(state.data, "helperPid") == GetCurrentProcessId())
            {
                state.data["phase"] = "rolledBack";
                SaveState(state);
                log(L"Предыдущий запускатель сохранён. При следующем запуске временные файлы будут удалены");
            }
        }
        catch (...) { log(L"Журнал незавершённого обновления сохранён: " + state.root); }
    }
    return 1;
}

bool CleanupUpdate(const std::wstring& statePath, const Log& log)
{
    try
    {
        UpdateState state = LoadState(statePath, false);
        const std::uint64_t helper = JsonNumber(state.data, "helperPid");
        if (!helper) throw std::wstring(L"Помощник обновления не подтвердил выполнение транзакции");
        WaitForProcess(helper, JsonNumber(state.data, "helperCreation"), 60000);
        // После завершения помощника перечитываем его окончательное состояние
        state = LoadState(statePath, false);
        const std::string phase = JsonString(state.data, "phase", 32);
        const std::string current = Lower(Sha256File(ModulePath()));
        if (phase == "rolledBack")
        {
            if (current != JsonString(state.data, "oldHash", 64))
                throw std::wstring(L"Восстановленный EXE не совпадает с журналом обновления");
        }
        else if (phase == "committed" || phase == "replaced")
        {
            CheckExecutable(ModulePath(), state.manifest);
            const std::wstring backup = StageFile(state, L"previous.exe");
            if (Exists(backup) && Lower(Sha256File(backup)) != JsonString(state.data, "oldHash", 64))
                throw std::wstring(L"Резервная копия изменилась. Автоматическая очистка остановлена");
        }
        else throw std::wstring(L"Операция обновления не завершена. Временные файлы сохранены");
        if (Lower(Sha256File(StageFile(state, L"helper.exe"))) != JsonString(state.data, "oldHash", 64))
            throw std::wstring(L"Помощник обновления изменён. Автоматическая очистка остановлена");
        RemoveStage(state);
        log(L"Временные файлы обновления удалены");
        return true;
    }
    catch (const std::wstring& error) { log(L"Очистка обновления отложена: " + error); }
    catch (const std::exception&) { log(L"Очистка обновления отложена: журнал не прошёл проверку"); }
    return false;
}
}
