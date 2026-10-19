#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "vendor/json.hpp"

namespace hlc
{
using Json = nlohmann::json;
using Bytes = std::vector<std::uint8_t>;
using Log = std::function<void(const std::wstring&)>;

struct Options
{
    std::wstring gameDirectory;
    bool installOnly = false;
    bool skipUpdate = false;
    bool afterUpdate = false;
    std::wstring applyUpdateState;
    std::wstring cleanupUpdateState;
};

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
        if (this != &other) Reset(other.Release());
        return *this;
    }
    HANDLE Get() const { return value_; }
    bool Valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE Release() { HANDLE value = value_; value_ = nullptr; return value; }
    void Reset(HANDLE value = nullptr)
    {
        if (Valid()) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_ = nullptr;
};

std::wstring WindowsError(const std::wstring& operation, DWORD error = GetLastError());
std::wstring Utf8ToWide(const std::string& value);
std::string WideToUtf8(const std::wstring& value);
std::wstring FullPath(const std::wstring& path);
bool SamePath(const std::wstring& first, const std::wstring& second);
std::wstring ModulePath();
std::wstring GuidText();
std::wstring StateDirectory();
std::wstring AssertPlainPath(const std::wstring& path, const std::wstring& boundary);
Bytes ReadBytes(const std::wstring& path, std::size_t maximumBytes);
void WriteBytes(const std::wstring& path, const Bytes& bytes);
void WriteText(const std::wstring& path, const std::string& text);
std::string Sha256(const Bytes& bytes);
std::string Sha256File(const std::wstring& path);
Bytes ResourceBytes(HINSTANCE instance, int identifier);
std::array<unsigned, 3> ParseVersion(const std::string& value);
int CompareVersions(const std::string& first, const std::string& second);
std::string LauncherVersion();
std::string Repository();
void CheckPe(const std::wstring& path, bool dll);
std::wstring QuoteArgument(const std::wstring& value);
HANDLE StartProcess(const std::wstring& executable, const std::vector<std::wstring>& arguments,
    const std::wstring& workingDirectory);

std::wstring ResolveGameDirectory(const std::wstring& explicitPath, HWND owner);
void SaveGameDirectory(const std::wstring& gameDirectory);
void AssertGameStopped(const std::wstring& gameDirectory);
std::string ValidateInstalled(const std::wstring& gameDirectory);
void InstallEmbedded(HINSTANCE instance, const std::wstring& gameDirectory, const Log& log);
void LaunchGame(const std::wstring& gameDirectory);

bool TryStartSelfUpdate(const Options& options, const std::wstring& gameDirectory, const Log& log);
int ApplyUpdate(const std::wstring& statePath, const Log& log);
bool CleanupUpdate(const std::wstring& statePath, const Log& log);
}
