#include "Native.h"

#include <fdi.h>
#include <fcntl.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <map>
#include <memory>
#include <regex>
#include <set>

#pragma comment(lib, "cabinet.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace hlc
{
namespace
{
namespace fs = std::filesystem;
constexpr std::uint64_t kMaximumFile = 128ull * 1024 * 1024;
constexpr std::uint64_t kMaximumPayload = 512ull * 1024 * 1024;
const wchar_t kJournalName[] = L".hlcoop-native-transaction.json";
const wchar_t kNativeManifest[] = L"native-install.json";
const wchar_t kLegacyManifest[] = L"update-manifest.json";
const char kJournalProject[] = "half-life-coop/native-install";
const std::set<std::string> kPayloadPaths = {
    "autoexec.cfg", "campaign.cfg", "campaign.json", "cl_dlls/client.dll",
    "continueauto.cfg", "continuecoop.cfg", "coop.cfg", "CSBotConfig.vdf",
    "dlls/hl.dll", "game.cfg", "liblist.gam", "LICENSE", "listenserver.cfg",
    "mapcycle.txt", "motd.txt", "newcampaign.cfg", "README.txt",
    "resource/CreateMultiplayerGameServerPage.res", "resource/GameMenu.res",
    "server.cfg", "settings.scr"
};

std::wstring Join(const std::wstring& parent, const std::wstring& child)
{
    return (fs::path(parent) / child).wstring();
}

std::wstring RelativePath(const std::wstring& root, const std::string& relative)
{
    auto path = Utf8ToWide(relative);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    return AssertPlainPath(Join(root, path), root);
}

bool Exists(const std::wstring& path)
{
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
    throw WindowsError(L"Не удалось проверить путь " + path, error);
}

bool IsFile(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

void MakeDirectory(const std::wstring& path, const std::wstring& boundary)
{
    AssertPlainPath(path, boundary);
    fs::create_directories(path);
    AssertPlainPath(path, boundary);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        throw std::wstring(L"Не удалось создать папку " + path);
}

std::uint64_t FileSize(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA information{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &information))
        throw WindowsError(L"Не удалось прочитать размер " + path);
    if (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::wstring(L"Ожидается обычный файл: ") + path;
    return (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
}

bool IsHash(const std::string& value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool IsTransactionId(const std::string& value)
{
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string NewTransactionId()
{
    auto value = WideToUtf8(GuidText());
    value.erase(std::remove(value.begin(), value.end(), '-'), value.end());
    for (auto& c : value) if (c >= 'A' && c <= 'F') c += 'a' - 'A';
    if (!IsTransactionId(value)) throw std::wstring(L"Не удалось создать номер установки");
    return value;
}

std::string ReadText(const std::wstring& path, std::size_t limit = 1024 * 1024)
{
    const auto bytes = ReadBytes(path, limit);
    std::string result(bytes.begin(), bytes.end());
    if (result.compare(0, 3, "\xEF\xBB\xBF") == 0) result.erase(0, 3);
    return result;
}

Json ReadJson(const std::wstring& path)
{
    try { return Json::parse(ReadText(path)); }
    catch (const std::exception&) { throw std::wstring(L"Повреждён JSON-файл: ") + path; }
}

bool Preserved(const std::string& path)
{
    return (path.size() >= 4 && path.substr(path.size() - 4) == ".cfg") ||
        path == "settings.scr" || path == "CSBotConfig.vdf";
}

bool JournalFileAllowed(const std::string& path)
{
    return kPayloadPaths.count(path) || path == "native-install.json" || path == "update-manifest.json";
}

void CheckResourceManifest(const Json& manifest)
{
    if (!manifest.is_object() || manifest.value("schemaVersion", 0) != 2 ||
        !manifest.contains("version") || !manifest["version"].is_string() ||
        !manifest.contains("files") || !manifest["files"].is_array() ||
        manifest["files"].size() != kPayloadPaths.size())
        throw std::wstring(L"Неподдерживаемый манифест встроенного мода");
    ParseVersion(manifest["version"].get<std::string>());
    std::set<std::string> paths, names;
    std::uint64_t total = 0;
    for (const auto& entry : manifest["files"])
    {
        if (!entry.is_object() || !entry.contains("path") || !entry["path"].is_string() ||
            !entry.contains("cabinetName") || !entry["cabinetName"].is_string() ||
            !entry.contains("sha256") || !entry["sha256"].is_string() ||
            !entry.contains("bytes") || !entry["bytes"].is_number_unsigned())
            throw std::wstring(L"Повреждён список файлов встроенного мода");
        const auto path = entry["path"].get<std::string>();
        const auto name = entry["cabinetName"].get<std::string>();
        const auto hash = entry["sha256"].get<std::string>();
        const auto size = entry["bytes"].get<std::uint64_t>();
        if (!kPayloadPaths.count(path) || !paths.insert(path).second || !names.insert(name).second ||
            name.size() != 5 || name[0] != 'f' ||
            !std::all_of(name.begin() + 1, name.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
            !IsHash(hash) || size > kMaximumFile || total > kMaximumPayload - size)
            throw std::wstring(L"Недопустимый путь, размер или SHA256 во встроенном моде");
        total += size;
    }
}

std::string ModVersion(const std::wstring& mod)
{
    const auto text = ReadText(Join(mod, L"liblist.gam"), 16384);
    const std::regex game("(^|[\\r\\n])game[ \\t]+\"Half-Life Co-op\"[ \\t]*(?=[\\r\\n]|$)");
    const std::regex dll("(^|[\\r\\n])gamedll[ \\t]+\"dlls[\\\\/]hl\\.dll\"[ \\t]*(?=[\\r\\n]|$)");
    const std::regex fallback("(^|[\\r\\n])fallback_dir[ \\t]+\"valve\"[ \\t]*(?=[\\r\\n]|$)");
    const std::regex version("(^|[\\r\\n])version[ \\t]+\"([0-9]+\\.[0-9]+\\.[0-9]+)\"[ \\t]*(?=[\\r\\n]|$)");
    if (!std::regex_search(text, game) || !std::regex_search(text, dll) || !std::regex_search(text, fallback))
        throw std::wstring(L"Папка hlcoop не распознана как Half-Life Co-op");
    std::sregex_iterator it(text.begin(), text.end(), version), end;
    if (it == end) throw std::wstring(L"В liblist.gam не найдена версия мода");
    const auto result = (*it)[2].str();
    if (++it != end) throw std::wstring(L"В liblist.gam указано несколько версий мода");
    ParseVersion(result);
    return result;
}

std::wstring CheckGameDirectory(const std::wstring& path)
{
    const auto full = FullPath(path);
    AssertPlainPath(full, full);
    const auto engine = AssertPlainPath(Join(full, L"hl.exe"), full);
    const auto metadata = AssertPlainPath(Join(full, L"valve\\liblist.gam"), full);
    if (!IsFile(engine) || !IsFile(metadata))
        throw std::wstring(L"Выберите оригинальную Half-Life: нужны hl.exe и valve\\liblist.gam");
    CheckPe(engine, false);
    return full;
}

void AtomicText(const std::wstring& target, const std::string& text, const std::wstring& boundary)
{
    AssertPlainPath(target, boundary);
    const auto temporary = AssertPlainPath(target + L"." + GuidText() + L".tmp", boundary);
    try
    {
        if (Exists(temporary)) throw std::wstring(L"Временный файл установки уже существует");
        WriteText(temporary, text);
        if (Exists(target))
        {
            if (!ReplaceFileW(target.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr))
                throw WindowsError(L"Не удалось заменить " + target);
        }
        else if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
            throw WindowsError(L"Не удалось сохранить " + target);
    }
    catch (...)
    {
        DeleteFileW(temporary.c_str());
        throw;
    }
}

void AtomicCopy(const std::wstring& source, const std::wstring& target,
    const std::wstring& boundary, const std::string& transaction)
{
    AssertPlainPath(source, fs::path(source).parent_path().wstring());
    AssertPlainPath(target, boundary);
    const auto temporary = AssertPlainPath(target + L".hlcoop-" + Utf8ToWide(transaction) + L".tmp", boundary);
    try
    {
        if (!CopyFileW(source.c_str(), temporary.c_str(), TRUE))
            throw WindowsError(L"Не удалось подготовить файл " + target);
        Handle handle(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!handle.Valid() || !FlushFileBuffers(handle.Get()))
            throw WindowsError(L"Не удалось сохранить файл " + target);
        handle.Reset();
        if (Exists(target))
        {
            if (!ReplaceFileW(target.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr))
                throw WindowsError(L"Не удалось обновить " + target);
        }
        else if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
            throw WindowsError(L"Не удалось установить " + target);
    }
    catch (...)
    {
        DeleteFileW(temporary.c_str());
        throw;
    }
}

void WriteJournal(const std::wstring& target, const Json& journal,
    const std::wstring& work, const std::wstring& game)
{
    const auto path = AssertPlainPath(target, game);
    const auto temporary = AssertPlainPath(Join(work, L"journal.tmp"), work);
    if (Exists(path) || Exists(temporary)) throw std::wstring(L"Журнал установки уже существует");
    // До атомарного переноса журнал не оставляет посторонних файлов в новой папке hlcoop
    WriteText(temporary, journal.dump(2) + "\n");
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH))
        throw WindowsError(L"Не удалось сохранить журнал установки");
}

void RemoveOwnWork(const std::wstring& game, const std::string& transaction)
{
    if (!IsTransactionId(transaction)) throw std::wstring(L"Некорректный номер временной папки");
    const auto work = AssertPlainPath(Join(game, L".hlcoop-native-" + Utf8ToWide(transaction)), game);
    if (!Exists(work)) return;
    std::vector<fs::path> files, directories;
    for (fs::recursive_directory_iterator it(work), end; it != end; ++it)
    {
        const auto path = AssertPlainPath(it->path().wstring(), work);
        const auto relative = fs::relative(path, work).generic_string();
        if (relative == "journal.tmp" && !it->is_directory())
        {
            files.push_back(path);
            continue;
        }
        const auto slash = relative.find('/');
        const auto group = relative.substr(0, slash);
        if (group != "stage" && group != "backup")
            throw std::wstring(L"Неожиданный файл во временной папке установки");
        if (it->is_directory()) directories.push_back(path);
        else
        {
            if (slash == std::string::npos || !JournalFileAllowed(relative.substr(slash + 1)))
                throw std::wstring(L"Неожиданный файл во временной папке установки");
            files.push_back(path);
        }
    }
    for (const auto& path : files)
        if (!DeleteFileW(path.c_str())) throw WindowsError(L"Не удалось удалить временный файл " + path.wstring());
    std::sort(directories.begin(), directories.end(), [](const fs::path& a, const fs::path& b) {
        return a.native().size() > b.native().size();
    });
    for (const auto& path : directories)
        if (!RemoveDirectoryW(path.c_str())) throw WindowsError(L"Не удалось удалить временную папку " + path.wstring());
    if (!RemoveDirectoryW(work.c_str())) throw WindowsError(L"Не удалось удалить временную папку " + work);
}

struct CabinetStream
{
    bool input = false;
    Handle file;
    std::uint64_t position = 0;
    std::uint64_t extent = 0;
    std::uint64_t limit = 0;
    std::string name;
    std::string hash;
    std::wstring path;
};

struct CabinetContext
{
    const Bytes* cabinet = nullptr;
    const Json* manifest = nullptr;
    std::wstring stage;
    std::wstring error;
    std::set<std::string> seen, completed;
    std::map<INT_PTR, std::unique_ptr<CabinetStream>> streams;
};

thread_local CabinetContext* gCabinet = nullptr;

FNALLOC(CabinetAllocate)
{
    if (cb > 64u * 1024 * 1024) return nullptr;
    return std::malloc(cb);
}

FNFREE(CabinetFree) { std::free(pv); }

FNOPEN(CabinetOpen)
{
    (void)pmode;
    try
    {
        if (!gCabinet || !pszFile || std::strcmp(pszFile, "payload.cab") != 0 ||
            (oflag & (_O_WRONLY | _O_RDWR | _O_CREAT | _O_TRUNC | _O_APPEND))) return -1;
        auto stream = std::make_unique<CabinetStream>();
        stream->input = true;
        stream->limit = gCabinet->cabinet->size();
        const auto id = reinterpret_cast<INT_PTR>(stream.get());
        gCabinet->streams.emplace(id, std::move(stream));
        return id;
    }
    catch (...) { return -1; }
}

FNREAD(CabinetRead)
{
    if (!gCabinet) return static_cast<UINT>(-1);
    const auto found = gCabinet->streams.find(hf);
    if (found == gCabinet->streams.end() || !found->second->input) return static_cast<UINT>(-1);
    auto& stream = *found->second;
    const auto count = static_cast<UINT>(std::min<std::uint64_t>(cb, stream.limit - stream.position));
    if (count) std::memcpy(pv, gCabinet->cabinet->data() + static_cast<std::size_t>(stream.position), count);
    stream.position += count;
    return count;
}

FNWRITE(CabinetWrite)
{
    if (!gCabinet) return static_cast<UINT>(-1);
    const auto found = gCabinet->streams.find(hf);
    if (found == gCabinet->streams.end() || found->second->input) return static_cast<UINT>(-1);
    auto& stream = *found->second;
    if (cb > stream.limit - stream.position) return static_cast<UINT>(-1);
    DWORD written = 0;
    if (!WriteFile(stream.file.Get(), pv, cb, &written, nullptr) || written != cb) return static_cast<UINT>(-1);
    stream.position += written;
    stream.extent = std::max(stream.extent, stream.position);
    return written;
}

FNCLOSE(CabinetClose)
{
    if (!gCabinet) return -1;
    return gCabinet->streams.erase(hf) == 1 ? 0 : -1;
}

FNSEEK(CabinetSeek)
{
    if (!gCabinet) return -1;
    const auto found = gCabinet->streams.find(hf);
    if (found == gCabinet->streams.end()) return -1;
    auto& stream = *found->second;
    const std::int64_t base = seektype == SEEK_SET ? 0 :
        seektype == SEEK_CUR ? static_cast<std::int64_t>(stream.position) :
        seektype == SEEK_END ? static_cast<std::int64_t>(stream.limit) : -1;
    const auto next = base + dist;
    if (base < 0 || next < 0 || static_cast<std::uint64_t>(next) > stream.limit) return -1;
    if (!stream.input)
    {
        LARGE_INTEGER location{};
        location.QuadPart = next;
        if (!SetFilePointerEx(stream.file.Get(), location, nullptr, FILE_BEGIN)) return -1;
    }
    stream.position = static_cast<std::uint64_t>(next);
    return static_cast<long>(next);
}

FNFDINOTIFY(CabinetNotify)
{
    try
    {
        if (!gCabinet || pfdin->pv != gCabinet) return -1;
        if (fdint == fdintNEXT_CABINET || fdint == fdintPARTIAL_FILE)
            throw std::wstring(L"Встроенный пакет не должен состоять из нескольких CAB");
        if (fdint == fdintCOPY_FILE)
        {
            const std::string name = pfdin->psz1 ? pfdin->psz1 : "";
            const Json* entry = nullptr;
            for (const auto& candidate : (*gCabinet->manifest)["files"])
                if (candidate["cabinetName"].get<std::string>() == name) { entry = &candidate; break; }
            if (!entry || !gCabinet->seen.insert(name).second || pfdin->cb < 0 ||
                static_cast<std::uint64_t>(pfdin->cb) != (*entry)["bytes"].get<std::uint64_t>())
                throw std::wstring(L"Список файлов CAB не соответствует манифесту");
            auto stream = std::make_unique<CabinetStream>();
            stream->name = name;
            stream->hash = (*entry)["sha256"].get<std::string>();
            stream->limit = (*entry)["bytes"].get<std::uint64_t>();
            stream->path = RelativePath(gCabinet->stage, (*entry)["path"].get<std::string>());
            MakeDirectory(fs::path(stream->path).parent_path().wstring(), gCabinet->stage);
            stream->file.Reset(CreateFileW(stream->path.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!stream->file.Valid()) throw WindowsError(L"Не удалось распаковать " + stream->path);
            const auto id = reinterpret_cast<INT_PTR>(stream.get());
            gCabinet->streams.emplace(id, std::move(stream));
            return id;
        }
        if (fdint == fdintCLOSE_FILE_INFO)
        {
            const auto found = gCabinet->streams.find(pfdin->hf);
            if (found == gCabinet->streams.end() || found->second->input)
                throw std::wstring(L"Неизвестный выходной файл CAB");
            auto& stream = *found->second;
            if (stream.extent != stream.limit || !FlushFileBuffers(stream.file.Get()))
                throw std::wstring(L"Не удалось записать файл CAB целиком");
            stream.file.Reset();
            if (FileSize(stream.path) != stream.limit || Sha256File(stream.path) != stream.hash)
                throw std::wstring(L"Не совпал SHA256 распакованного файла: ") + stream.path;
            gCabinet->completed.insert(stream.name);
            gCabinet->streams.erase(found);
            return TRUE;
        }
        return 0;
    }
    catch (const std::wstring& error) { if (gCabinet) gCabinet->error = error; }
    catch (...) { if (gCabinet) gCabinet->error = L"Не удалось прочитать встроенный CAB"; }
    return -1;
}

void ExtractCabinet(const Bytes& cabinet, const Json& manifest, const std::wstring& stage)
{
    if (cabinet.empty() || cabinet.size() > kMaximumFile)
        throw std::wstring(L"Размер встроенного CAB вне допустимых пределов");
    MakeDirectory(stage, fs::path(stage).parent_path().wstring());
    CabinetContext context;
    context.cabinet = &cabinet;
    context.manifest = &manifest;
    context.stage = stage;
    if (gCabinet) throw std::wstring(L"Распаковка CAB уже запущена в этом потоке");
    gCabinet = &context;
    ERF error{};
    HFDI fdi = FDICreate(CabinetAllocate, CabinetFree, CabinetOpen, CabinetRead,
        CabinetWrite, CabinetClose, CabinetSeek, cpu80386, &error);
    try
    {
        if (!fdi) throw std::wstring(L"Не удалось открыть декомпрессор Windows CAB");
        char filename[] = "payload.cab";
        const auto input = CabinetOpen(filename, _O_RDONLY, 0);
        FDICABINETINFO information{};
        const bool valid = input != -1 && FDIIsCabinet(fdi, input, &information);
        if (input != -1) CabinetClose(input);
        if (!valid || information.hasprev || information.hasnext || information.iCabinet != 0 ||
            information.cFiles != manifest["files"].size() ||
            information.cbCabinet != static_cast<long>(cabinet.size()))
            throw std::wstring(L"Повреждён встроенный CAB или его таблица файлов");
        char folder[] = "";
        if (!FDICopy(fdi, filename, folder, 0, CabinetNotify, nullptr, &context) ||
            context.completed.size() != manifest["files"].size())
            throw context.error.empty() ? std::wstring(L"Не удалось распаковать CAB, код ") +
                std::to_wstring(error.erfOper) : context.error;
        FDIDestroy(fdi);
        fdi = nullptr;
        gCabinet = nullptr;
    }
    catch (...)
    {
        if (fdi) FDIDestroy(fdi);
        gCabinet = nullptr;
        throw;
    }
}

struct RegistryKey
{
    HKEY value = nullptr;
    ~RegistryKey() { if (value) RegCloseKey(value); }
};

std::wstring RegistryString(HKEY root, const wchar_t* key, const wchar_t* name, REGSAM view)
{
    RegistryKey opened;
    if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE | view, &opened.value) != ERROR_SUCCESS) return {};
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(opened.value, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t) || bytes > 32768) return {};
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, 0);
    if (RegQueryValueExW(opened.value, name, nullptr, &type,
        reinterpret_cast<BYTE*>(buffer.data()), &bytes) != ERROR_SUCCESS) return {};
    std::wstring value(buffer.data());
    if (type == REG_EXPAND_SZ)
    {
        const auto count = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (!count || count > 32768) return {};
        std::vector<wchar_t> expanded(count);
        if (!ExpandEnvironmentStringsW(value.c_str(), expanded.data(), count)) return {};
        value.assign(expanded.data());
    }
    return value;
}

struct PathLess
{
    bool operator()(const std::wstring& a, const std::wstring& b) const
    {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    }
};
using Paths = std::set<std::wstring, PathLess>;

void AddDirectory(Paths& paths, const std::wstring& path)
{
    try
    {
        if (path.empty() || !fs::path(path).is_absolute()) return;
        const auto full = FullPath(path);
        const auto attributes = GetFileAttributesW(full.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return;
        AssertPlainPath(full, full);
        paths.insert(full);
    }
    catch (...) { }
}

Paths SteamRoots()
{
    Paths result;
    for (const REGSAM view : { KEY_WOW64_32KEY, KEY_WOW64_64KEY })
    {
        AddDirectory(result, RegistryString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", view));
        const auto executable = RegistryString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamExe", view);
        if (!executable.empty()) AddDirectory(result, fs::path(executable).parent_path().wstring());
        AddDirectory(result, RegistryString(HKEY_LOCAL_MACHINE, L"Software\\Valve\\Steam", L"InstallPath", view));
    }
    for (const auto variable : { L"ProgramFiles(x86)", L"ProgramFiles" })
    {
        wchar_t buffer[32768]{};
        const DWORD count = GetEnvironmentVariableW(variable, buffer, ARRAYSIZE(buffer));
        if (count && count < ARRAYSIZE(buffer)) AddDirectory(result, Join(buffer, L"Steam"));
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> VdfPairs(const std::wstring& path)
{
    std::vector<std::pair<std::string, std::string>> result;
    try
    {
        const auto text = ReadText(path, 4 * 1024 * 1024);
        std::string pending;
        bool hasPending = false;
        for (std::size_t offset = 0; offset < text.size();)
        {
            if (text[offset] == '/' && offset + 1 < text.size() && text[offset + 1] == '/')
            {
                offset = text.find('\n', offset);
                if (offset == std::string::npos) break;
            }
            else if (text[offset] == '{' || text[offset] == '}') { hasPending = false; ++offset; }
            else if (text[offset] == '"')
            {
                std::string value;
                bool closed = false;
                for (++offset; offset < text.size(); ++offset)
                {
                    const auto c = text[offset];
                    if (c == '"') { ++offset; closed = true; break; }
                    if (c == '\\' && offset + 1 < text.size() &&
                        (text[offset + 1] == '\\' || text[offset + 1] == '"')) value += text[++offset];
                    else value += c;
                }
                if (!closed) return {};
                if (hasPending) { result.emplace_back(pending, value); hasPending = false; }
                else { pending = value; hasPending = true; }
            }
            else ++offset;
        }
    }
    catch (...) { }
    return result;
}

Paths SteamInstallations()
{
    const auto roots = SteamRoots();
    Paths libraries = roots, games;
    for (const auto& root : roots)
        for (const auto& pair : VdfPairs(Join(root, L"steamapps\\libraryfolders.vdf")))
            if (pair.first == "path" || (!pair.first.empty() &&
                std::all_of(pair.first.begin(), pair.first.end(), [](char c) { return c >= '0' && c <= '9'; })))
                try { AddDirectory(libraries, Utf8ToWide(pair.second)); } catch (...) { }
    for (const auto& library : libraries)
    {
        std::vector<std::string> directories;
        for (const auto& pair : VdfPairs(Join(library, L"steamapps\\appmanifest_70.acf")))
            if (pair.first == "installdir") directories.push_back(pair.second);
        if (directories.size() != 1 || directories[0].empty() || directories[0].size() > 128 ||
            directories[0] == "." || directories[0] == ".." ||
            directories[0].find_first_of("\\/:*?\"<>|\r\n") != std::string::npos) continue;
        try
        {
            const auto game = Join(Join(library, L"steamapps\\common"), Utf8ToWide(directories[0]));
            games.insert(CheckGameDirectory(game));
        }
        catch (...) { }
    }
    return games;
}

std::wstring PickGameDirectory(HWND owner)
{
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(initialized);
    IFileOpenDialog* dialog = nullptr;
    IShellItem* item = nullptr;
    PWSTR selected = nullptr;
    try
    {
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
            throw std::wstring(L"Не удалось открыть выбор папки Windows");
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) throw std::wstring(L"Не удалось открыть выбор папки Half-Life");
        DWORD flags = 0;
        if (FAILED(dialog->GetOptions(&flags)) ||
            FAILED(dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST)) ||
            FAILED(dialog->SetTitle(L"Выберите папку Half-Life с hl.exe и папкой valve")) ||
            FAILED(dialog->Show(owner)) || FAILED(dialog->GetResult(&item)) ||
            FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected)))
            throw std::wstring(L"Папка Half-Life не выбрана");
        const auto result = CheckGameDirectory(selected);
        CoTaskMemFree(selected);
        item->Release();
        dialog->Release();
        if (uninitialize) CoUninitialize();
        return result;
    }
    catch (...)
    {
        if (selected) CoTaskMemFree(selected);
        if (item) item->Release();
        if (dialog) dialog->Release();
        if (uninitialize) CoUninitialize();
        throw;
    }
}

class InstallationLock
{
public:
    explicit InstallationLock(const std::wstring& mod)
    {
        auto canonical = FullPath(mod);
        if (!canonical.empty()) CharUpperBuffW(&canonical[0], static_cast<DWORD>(canonical.size()));
        const auto utf8 = WideToUtf8(canonical);
        auto hash = Sha256(Bytes(utf8.begin(), utf8.end()));
        for (auto& c : hash) if (c >= 'a' && c <= 'f') c += 'A' - 'a';
        const auto name = L"Global\\HalfLifeCoopUpdate-" + Utf8ToWide(hash);
        mutex_.Reset(CreateMutexW(nullptr, FALSE, name.c_str()));
        if (!mutex_.Valid()) throw WindowsError(L"Не удалось заблокировать папку мода");
        const auto wait = WaitForSingleObject(mutex_.Get(), 0);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
            throw std::wstring(L"Другой установщик уже работает с этой папкой Half-Life");
        owned_ = true;
    }
    ~InstallationLock() { if (owned_) ReleaseMutex(mutex_.Get()); }
private:
    Handle mutex_;
    bool owned_ = false;
};

void CheckLegacyManifest(const std::wstring& mod, const std::string& version)
{
    const auto path = RelativePath(mod, "update-manifest.json");
    if (!Exists(path)) return;
    const auto manifest = ReadJson(path);
    if (!manifest.is_object() || manifest.value("schemaVersion", 0) != 1 ||
        manifest.value("version", std::string()) != version ||
        !manifest.contains("files") || !manifest["files"].is_array() || manifest["files"].size() > 64)
        throw std::wstring(L"Манифест прежней установки не соответствует версии мода");
    for (const auto relative : { "liblist.gam", "dlls/hl.dll", "cl_dlls/client.dll" })
    {
        std::size_t found = 0;
        for (const auto& entry : manifest["files"])
            if (entry.is_object() && entry.value("path", std::string()) == relative)
            {
                const auto hash = entry.value("sha256", std::string());
                if (!IsHash(hash) || Sha256File(RelativePath(mod, relative)) != hash)
                    throw std::wstring(L"Файл прежней установки не прошёл проверку: ") + Utf8ToWide(relative);
                ++found;
            }
        if (found != 1) throw std::wstring(L"Манифест прежней установки не содержит обязательный файл");
    }
}

void CheckNativeFiles(const std::wstring& mod, const Json& manifest)
{
    CheckResourceManifest(manifest);
    for (const auto& entry : manifest["files"])
    {
        const auto relative = entry["path"].get<std::string>();
        const auto path = RelativePath(mod, relative);
        if (!IsFile(path))
            throw std::wstring(L"Отсутствует файл установленного мода: ") + Utf8ToWide(relative);
        if (Preserved(relative)) continue;
        if (!IsFile(path) || FileSize(path) != entry["bytes"].get<std::uint64_t>() ||
            Sha256File(path) != entry["sha256"].get<std::string>())
            throw std::wstring(L"Файл установленного мода повреждён: ") + Utf8ToWide(relative);
    }
}

void CheckJournal(const Json& journal, const std::wstring& game)
{
    if (!journal.is_object() || journal.value("schemaVersion", 0) != 1 ||
        journal.value("project", std::string()) != kJournalProject ||
        !IsTransactionId(journal.value("transactionId", std::string())) ||
        !SamePath(Utf8ToWide(journal.value("gamePath", std::string())), game) ||
        !journal.contains("files") || !journal["files"].is_array() || journal["files"].empty() ||
        journal["files"].size() > kPayloadPaths.size() + 2 ||
        !journal.contains("createdDirectories") || !journal["createdDirectories"].is_array() ||
        journal["createdDirectories"].size() > 4)
        throw std::wstring(L"Журнал установки повреждён: автоматическое восстановление остановлено");
    std::set<std::string> seen;
    for (const auto& file : journal["files"])
    {
        if (!file.is_object() || !file.contains("path") || !file["path"].is_string() ||
            !JournalFileAllowed(file["path"].get<std::string>()) ||
            !seen.insert(file["path"].get<std::string>()).second ||
            !file.contains("existed") || !file["existed"].is_boolean() ||
            !IsHash(file.value("newSha256", std::string())) ||
            (file["existed"].get<bool>() && !IsHash(file.value("oldSha256", std::string()))))
            throw std::wstring(L"Журнал установки содержит недопустимый файл");
    }
    for (const auto& path : journal["createdDirectories"])
        if (!path.is_string() || (path != "dlls" && path != "cl_dlls" && path != "resource"))
            throw std::wstring(L"Журнал установки содержит недопустимую папку");
}

void Rollback(const std::wstring& game, const Json& journal, const Log& log)
{
    CheckJournal(journal, game);
    AssertGameStopped(game);
    const auto mod = Join(game, L"hlcoop");
    const auto transaction = journal["transactionId"].get<std::string>();
    const auto work = AssertPlainPath(Join(game, L".hlcoop-native-" + Utf8ToWide(transaction)), game);
    const auto backup = AssertPlainPath(Join(work, L"backup"), work);
    // До восстановления проверяем все копии, чтобы не начать частичный откат по повреждённому журналу
    for (const auto& file : journal["files"])
    {
        const auto relative = file["path"].get<std::string>();
        if (file["existed"].get<bool>())
        {
            const auto saved = RelativePath(backup, relative);
            if (!IsFile(saved) || Sha256File(saved) != file["oldSha256"].get<std::string>())
                throw std::wstring(L"Резервная копия повреждена. Запуск остановлен, папка: ") + work;
        }
        const auto target = RelativePath(mod, relative);
        if (Exists(target))
        {
            const auto hash = Sha256File(target);
            if (hash != file["newSha256"].get<std::string>() &&
                (!file["existed"].get<bool>() || hash != file["oldSha256"].get<std::string>()))
                throw std::wstring(L"Файл изменён после прерванной установки. Резервная копия: ") + work;
        }
    }
    for (auto it = journal["files"].rbegin(); it != journal["files"].rend(); ++it)
    {
        const auto relative = (*it)["path"].get<std::string>();
        const auto target = RelativePath(mod, relative);
        const auto temporary = AssertPlainPath(target + L".hlcoop-" + Utf8ToWide(transaction) + L".tmp", mod);
        if (Exists(temporary) && !DeleteFileW(temporary.c_str()))
            throw WindowsError(L"Не удалось удалить незавершённый файл " + temporary);
        if ((*it)["existed"].get<bool>())
        {
            MakeDirectory(fs::path(target).parent_path().wstring(), mod);
            AtomicCopy(RelativePath(backup, relative), target, mod, transaction);
            if (Sha256File(target) != (*it)["oldSha256"].get<std::string>())
                throw std::wstring(L"Не удалось проверить восстановленный файл " + target);
        }
        else if (Exists(target) && !DeleteFileW(target.c_str()))
            throw WindowsError(L"Не удалось убрать файл прерванной установки " + target);
    }
    for (const auto& directory : journal["createdDirectories"])
    {
        const auto path = RelativePath(mod, directory.get<std::string>());
        if (Exists(path) && fs::is_directory(path) && fs::is_empty(path))
            if (!RemoveDirectoryW(path.c_str())) throw WindowsError(L"Не удалось убрать пустую папку " + path);
    }
    const auto journalPath = AssertPlainPath(Join(mod, kJournalName), mod);
    if (!DeleteFileW(journalPath.c_str())) throw WindowsError(L"Не удалось завершить восстановление мода");
    log(L"Предыдущая версия восстановлена после прерванной установки\r\n");
    try { RemoveOwnWork(game, transaction); }
    catch (...) { log(L"Резервная папка оставлена: " + work + L"\r\n"); }
}

void RefreshLegacyManifest(const std::wstring& mod, const std::wstring& stage,
    const Json& manifest)
{
    const auto existing = RelativePath(mod, "update-manifest.json");
    if (!Exists(existing)) return;
    std::set<std::string> legacy = kPayloadPaths;
    legacy.insert("start-hlcoop.cmd");
    legacy.insert("update-hlcoop.ps1");
    legacy.insert("update.json");
    Json result = { {"schemaVersion", 1}, {"version", manifest["version"]}, {"files", Json::array()} };
    for (const auto& relative : legacy)
    {
        const auto oldPath = RelativePath(mod, relative);
        const auto newPath = RelativePath(stage, relative);
        const auto path = Preserved(relative) && IsFile(oldPath) ? oldPath :
            IsFile(newPath) ? newPath : oldPath;
        if (IsFile(path)) result["files"].push_back({ {"path", relative},
            {"bytes", FileSize(path)}, {"sha256", Sha256File(path)} });
    }
    WriteText(RelativePath(stage, "update-manifest.json"), result.dump(2) + "\n");
}
}

std::wstring ResolveGameDirectory(const std::wstring& explicitPath, HWND owner)
{
    if (!explicitPath.empty()) return CheckGameDirectory(explicitPath);
    try
    {
        const auto statePath = AssertPlainPath(Join(StateDirectory(), L"settings.json"), StateDirectory());
        if (Exists(statePath))
        {
            const auto state = ReadJson(statePath);
            if (state.is_object() && state.value("schemaVersion", 0) == 1 &&
                state.contains("halfLifePath") && state["halfLifePath"].is_string())
                return CheckGameDirectory(Utf8ToWide(state["halfLifePath"].get<std::string>()));
        }
    }
    catch (...) { }
    const auto games = SteamInstallations();
    if (games.size() == 1) return *games.begin();
    return PickGameDirectory(owner);
}

void SaveGameDirectory(const std::wstring& gameDirectory)
{
    const auto state = StateDirectory();
    const Json data = { {"schemaVersion", 1}, {"halfLifePath", WideToUtf8(CheckGameDirectory(gameDirectory))} };
    AtomicText(Join(state, L"settings.json"), data.dump(2) + "\n", state);
}

void AssertGameStopped(const std::wstring& gameDirectory)
{
    const auto game = FullPath(gameDirectory);
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.Valid()) throw WindowsError(L"Не удалось проверить запущенные игры");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.Get(), &entry))
        throw WindowsError(L"Не удалось прочитать список запущенных игр");
    do
    {
        if (!SamePath(entry.szExeFile, L"hl.exe") && !SamePath(entry.szExeFile, L"hlds.exe")) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID));
        if (!process.Valid()) throw std::wstring(L"Не удалось проверить Half-Life. Закройте игру перед установкой");
        if (WaitForSingleObject(process.Get(), 0) == WAIT_OBJECT_0) continue;
        wchar_t path[32768]{};
        DWORD size = ARRAYSIZE(path);
        if (!QueryFullProcessImageNameW(process.Get(), 0, path, &size))
            throw std::wstring(L"Не удалось определить папку запущенной Half-Life. Обновление отложено");
        if (SamePath(FullPath(fs::path(path).parent_path().wstring()), game))
            throw std::wstring(L"Half-Life или её сервер уже запущены. Закройте игру перед обновлением");
    } while (Process32NextW(snapshot.Get(), &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) throw WindowsError(L"Не удалось закончить проверку запущенных игр");
}

std::string ValidateInstalled(const std::wstring& gameDirectory)
{
    const auto game = CheckGameDirectory(gameDirectory);
    const auto mod = AssertPlainPath(Join(game, L"hlcoop"), game);
    if (Exists(AssertPlainPath(Join(mod, kJournalName), mod)) ||
        Exists(AssertPlainPath(Join(mod, L".hlcoop-update-transaction.json"), mod)))
        throw std::wstring(L"Установка мода не завершена. Откройте установщик повторно для восстановления");
    const auto version = ModVersion(mod);
    CheckPe(RelativePath(mod, "dlls/hl.dll"), true);
    CheckPe(RelativePath(mod, "cl_dlls/client.dll"), true);
    const auto metadata = AssertPlainPath(Join(mod, kNativeManifest), mod);
    if (Exists(metadata))
    {
        const auto manifest = ReadJson(metadata);
        if (manifest.value("version", std::string()) == version) CheckNativeFiles(mod, manifest);
        else
        {
            if (!Exists(AssertPlainPath(Join(mod, kLegacyManifest), mod)))
                throw std::wstring(L"Версия установленного мода не соответствует нативному манифесту");
            CheckLegacyManifest(mod, version);
        }
    }
    else CheckLegacyManifest(mod, version);
    return version;
}

void InstallEmbedded(HINSTANCE instance, const std::wstring& gameDirectory, const Log& log)
{
    const auto game = CheckGameDirectory(gameDirectory);
    const auto mod = AssertPlainPath(Join(game, L"hlcoop"), game);
    InstallationLock lock(mod);
    AssertGameStopped(game);
    const auto journalPath = AssertPlainPath(Join(mod, kJournalName), mod);
    if (Exists(journalPath)) Rollback(game, ReadJson(journalPath), log);
    if (Exists(AssertPlainPath(Join(mod, L".hlcoop-update-transaction.json"), mod)))
        throw std::wstring(L"Незавершённое обновление прежнего установщика: сохраните указанную в журнале резервную копию");
    const auto manifestBytes = ResourceBytes(instance, 102);
    if (manifestBytes.empty() || manifestBytes.size() > 1024 * 1024)
        throw std::wstring(L"Размер манифеста встроенного мода вне допустимых пределов");
    const auto manifest = Json::parse(manifestBytes.begin(), manifestBytes.end());
    CheckResourceManifest(manifest);
    const auto version = manifest["version"].get<std::string>();
    if (version != LauncherVersion())
        throw std::wstring(L"Версия встроенного мода не совпадает с версией запускателя");
    std::string current = "0.0.0";
    if (Exists(mod))
    {
        if (!fs::is_directory(mod)) throw std::wstring(L"Вместо папки hlcoop существует файл");
        if (!fs::is_empty(mod))
        {
            try { current = ModVersion(mod); }
            catch (...)
            {
                // Повреждённый liblist восстанавливаем только у распознанной нативной установки
                const auto installedManifest = ReadJson(AssertPlainPath(Join(mod, kNativeManifest), mod));
                CheckResourceManifest(installedManifest);
                current = installedManifest["version"].get<std::string>();
                log(L"Восстановление описания установленного мода...\r\n");
            }
        }
    }
    if (CompareVersions(current, version) > 0)
    {
        ValidateInstalled(game);
        log(L"Установлена версия " + Utf8ToWide(current) + L"\r\n");
        return;
    }
    if (CompareVersions(current, version) == 0)
    {
        bool healthy = true;
        try
        {
            ValidateInstalled(game);
            for (const auto& relative : kPayloadPaths)
                if (!IsFile(RelativePath(mod, relative))) { healthy = false; break; }
        }
        catch (...) { healthy = false; }
        if (healthy)
        {
            log(L"Установлена версия " + Utf8ToWide(current) + L"\r\n");
            return;
        }
        log(L"Восстановление файлов установленного мода...\r\n");
    }
    const auto cabinet = ResourceBytes(instance, 101);
    const auto transaction = NewTransactionId();
    const auto work = AssertPlainPath(Join(game, L".hlcoop-native-" + Utf8ToWide(transaction)), game);
    const auto stage = Join(work, L"stage");
    const auto backup = Join(work, L"backup");
    bool journalWritten = false;
    try
    {
        if (Exists(work)) throw std::wstring(L"Временная папка установки уже существует");
        MakeDirectory(work, game);
        log(L"Распаковка Half-Life Co-op " + Utf8ToWide(version) + L"...\r\n");
        ExtractCabinet(cabinet, manifest, stage);
        if (ModVersion(stage) != version) throw std::wstring(L"Версия встроенных файлов не совпала с манифестом");
        CheckPe(RelativePath(stage, "dlls/hl.dll"), true);
        CheckPe(RelativePath(stage, "cl_dlls/client.dll"), true);
        RefreshLegacyManifest(mod, stage, manifest);
        WriteText(AssertPlainPath(Join(stage, kNativeManifest), stage), manifest.dump(2) + "\n");
        MakeDirectory(mod, game);
        Json journal = { {"schemaVersion", 1}, {"project", kJournalProject},
            {"transactionId", transaction}, {"gamePath", WideToUtf8(game)},
            {"oldVersion", current}, {"newVersion", version},
            {"files", Json::array()}, {"createdDirectories", Json::array()} };
        std::vector<std::string> paths;
        for (const auto& file : manifest["files"]) paths.push_back(file["path"].get<std::string>());
        if (Exists(AssertPlainPath(Join(stage, kLegacyManifest), stage))) paths.push_back("update-manifest.json");
        // Нативный манифест записываем последним, когда DLL и данные уже на месте
        paths.push_back("native-install.json");
        for (const auto& relative : paths)
        {
            const auto target = RelativePath(mod, relative);
            const bool existed = Exists(target);
            if (existed && Preserved(relative)) continue;
            if (existed && !IsFile(target)) throw std::wstring(L"Вместо файла существует папка: ") + target;
            Json file = { {"path", relative}, {"existed", existed},
                {"newSha256", Sha256File(RelativePath(stage, relative))} };
            if (existed)
            {
                const auto saved = RelativePath(backup, relative);
                MakeDirectory(fs::path(saved).parent_path().wstring(), work);
                if (!CopyFileW(target.c_str(), saved.c_str(), TRUE))
                    throw WindowsError(L"Не удалось сделать резервную копию " + target);
                Handle savedHandle(CreateFileW(saved.c_str(), GENERIC_WRITE, 0, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                if (!savedHandle.Valid() || !FlushFileBuffers(savedHandle.Get()))
                    throw WindowsError(L"Не удалось сохранить резервную копию " + target);
                savedHandle.Reset();
                file["oldSha256"] = Sha256File(saved);
                if (Sha256File(target) != file["oldSha256"].get<std::string>())
                    throw std::wstring(L"Файл изменился во время резервного копирования: ") + target;
            }
            journal["files"].push_back(file);
        }
        for (const auto directory : { "dlls", "cl_dlls", "resource" })
            if (!Exists(RelativePath(mod, directory))) journal["createdDirectories"].push_back(directory);
        CheckJournal(journal, game);
        AssertGameStopped(game);
        for (const auto& file : journal["files"])
            if (file["existed"].get<bool>() &&
                Sha256File(RelativePath(mod, file["path"].get<std::string>())) != file["oldSha256"].get<std::string>())
                throw std::wstring(L"Файл изменился перед установкой. Установка отменена");
        WriteJournal(journalPath, journal, work, game);
        journalWritten = true;
        for (const auto& file : journal["files"])
        {
            const auto relative = file["path"].get<std::string>();
            const auto target = RelativePath(mod, relative);
            MakeDirectory(fs::path(target).parent_path().wstring(), mod);
            AtomicCopy(RelativePath(stage, relative), target, mod, transaction);
            if (Sha256File(target) != file["newSha256"].get<std::string>())
                throw std::wstring(L"Файл не прошёл проверку после установки: ") + target;
        }
        if (ModVersion(mod) != version) throw std::wstring(L"Версия установленного мода не совпала");
        CheckNativeFiles(mod, manifest);
        CheckPe(RelativePath(mod, "dlls/hl.dll"), true);
        CheckPe(RelativePath(mod, "cl_dlls/client.dll"), true);
        CheckLegacyManifest(mod, version);
        if (!DeleteFileW(journalPath.c_str())) throw WindowsError(L"Не удалось завершить журнал установки");
        journalWritten = false;
        log(L"Установлена версия " + Utf8ToWide(version) + L". Настройки и сохранения сохранены\r\n");
        try { RemoveOwnWork(game, transaction); }
        catch (...) { log(L"Временная папка оставлена: " + work + L"\r\n"); }
    }
    catch (...)
    {
        if (journalWritten || Exists(journalPath))
        {
            try { Rollback(game, ReadJson(journalPath), log); }
            catch (...) { throw std::wstring(L"Восстановление установки не завершено. Запуск заблокирован. Резервная копия: ") + work; }
        }
        else
        {
            try { RemoveOwnWork(game, transaction); } catch (...) { }
        }
        throw;
    }
}

void LaunchGame(const std::wstring& gameDirectory)
{
    const auto game = CheckGameDirectory(gameDirectory);
    ValidateInstalled(game);
    AssertGameStopped(game);
    bool steamRunning = false;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.Valid()) throw WindowsError(L"Не удалось проверить запуск Steam");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.Get(), &entry))
        do { if (SamePath(entry.szExeFile, L"steam.exe")) { steamRunning = true; break; } }
        while (Process32NextW(snapshot.Get(), &entry));
    if (!steamRunning)
        throw std::wstring(L"Запустите Steam и войдите в аккаунт, на котором куплена оригинальная Half-Life");
    LPWCH inherited = GetEnvironmentStringsW();
    if (!inherited) throw WindowsError(L"Не удалось подготовить окружение Half-Life");
    std::vector<std::wstring> variables;
    try
    {
        for (const wchar_t* current = inherited; *current; current += std::wcslen(current) + 1)
        {
            const std::wstring value(current);
            const auto separator = value.find(L'=', value.front() == L'=' ? 1 : 0);
            const auto name = value.substr(0, separator);
            if (!SamePath(name, L"SteamAppId") && !SamePath(name, L"SteamGameId")) variables.push_back(value);
        }
        FreeEnvironmentStringsW(inherited);
        inherited = nullptr;
        variables.push_back(L"SteamAppId=70");
        variables.push_back(L"SteamGameId=70");
        std::sort(variables.begin(), variables.end(), PathLess());
        std::vector<wchar_t> environment;
        for (const auto& value : variables)
        {
            environment.insert(environment.end(), value.begin(), value.end());
            environment.push_back(0);
        }
        environment.push_back(0);
        const auto executable = Join(game, L"hl.exe");
        auto command = QuoteArgument(executable) + L" -game hlcoop -console -insecure";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), &command[0], nullptr, nullptr, FALSE,
            CREATE_UNICODE_ENVIRONMENT, environment.data(), game.c_str(), &startup, &process))
            throw WindowsError(L"Не удалось запустить Half-Life");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    catch (...)
    {
        if (inherited) FreeEnvironmentStringsW(inherited);
        throw;
    }
}
}
