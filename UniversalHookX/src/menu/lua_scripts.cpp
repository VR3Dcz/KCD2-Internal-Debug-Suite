#include "lua_scripts.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

namespace Menu::LuaScripts {

    namespace {

        std::wstring Utf8ToWide(const std::string& input) {
            if (input.empty())
                return {};
            const int needed = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
            if (needed <= 0)
                return {};
            std::wstring out(static_cast<size_t>(needed), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), out.data(), needed);
            return out;
        }

        std::string WideToUtf8(const std::wstring& input) {
            if (input.empty())
                return {};
            const int needed = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
            if (needed <= 0)
                return {};
            std::string out(static_cast<size_t>(needed), '\0');
            WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), out.data(), needed, nullptr, nullptr);
            return out;
        }

        std::string ToLowerAscii(std::string text) {
            for (char& ch : text)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return text;
        }

        std::string TrimWhitespace(const std::string& text) {
            size_t start = 0;
            while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])))
                ++start;
            size_t end = text.size();
            while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])))
                --end;
            return text.substr(start, end - start);
        }

        bool EndsWithCaseInsensitive(const std::string& value, const std::string& suffix) {
            if (value.size() < suffix.size())
                return false;
            return ToLowerAscii(value.substr(value.size() - suffix.size())) == ToLowerAscii(suffix);
        }

        bool IsReservedWindowsDeviceName(const std::string& stem) {
            const std::string upper = [&]() {
                std::string s = stem;
                for (char& ch : s)
                    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                return s;
            }();

            static const char* kReserved[] = {
                "CON", "PRN", "AUX", "NUL",
                "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};

            for (const char* name : kReserved) {
                if (upper == name)
                    return true;
            }
            return false;
        }

        size_t FindBinSegmentCaseInsensitive(const std::wstring& path) {
            std::wstring lower = path;
            for (wchar_t& ch : lower) {
                if (ch >= L'A' && ch <= L'Z')
                    ch = static_cast<wchar_t>(ch - L'A' + L'a');
            }
            size_t pos = lower.find(L"\\bin\\");
            if (pos != std::wstring::npos)
                return pos;
            return lower.find(L"/bin/");
        }

        std::filesystem::path GetCurrentModuleDirectory() {
            HMODULE hModule = nullptr;
            if (!GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&ResolveStorageBaseDir),
                    &hModule)) {
                return std::filesystem::current_path();
            }

            std::wstring buf(MAX_PATH, L'\0');
            DWORD len = GetModuleFileNameW(hModule, buf.data(), static_cast<DWORD>(buf.size()));
            while (len == buf.size() && buf.size() < 32768) {
                buf.resize(buf.size() * 2);
                len = GetModuleFileNameW(hModule, buf.data(), static_cast<DWORD>(buf.size()));
            }
            if (len == 0)
                return std::filesystem::current_path();
            buf.resize(len);
            return std::filesystem::path(buf).parent_path();
        }

        std::string NormalizeLineEndings(const std::string& input) {
            std::string out;
            out.reserve(input.size());
            for (size_t i = 0; i < input.size(); ++i) {
                if (input[i] == '\r') {
                    if (i + 1 < input.size() && input[i + 1] == '\n')
                        continue;
                    out.push_back('\n');
                } else {
                    out.push_back(input[i]);
                }
            }
            return out;
        }

    } // namespace

    std::filesystem::path ResolveStorageBaseDir() {
        std::wstring exePath(MAX_PATH, L'\0');
        DWORD len = GetModuleFileNameW(nullptr, exePath.data(), static_cast<DWORD>(exePath.size()));
        while (len == exePath.size() && exePath.size() < 32768) {
            exePath.resize(exePath.size() * 2);
            len = GetModuleFileNameW(nullptr, exePath.data(), static_cast<DWORD>(exePath.size()));
        }

        if (len > 0) {
            exePath.resize(len);
            const size_t binPos = FindBinSegmentCaseInsensitive(exePath);
            if (binPos != std::wstring::npos && binPos > 0) {
                return std::filesystem::path(exePath.substr(0, binPos));
            }
        }

        return GetCurrentModuleDirectory();
    }

    bool NormalizeScriptFileName(const std::string& rawName, std::string& outFileName, std::string& outError) {
        std::string stem = TrimWhitespace(rawName);
        if (EndsWithCaseInsensitive(stem, ".lua")) {
            stem.resize(stem.size() - 4);
            stem = TrimWhitespace(stem);
        }

        if (stem.empty()) {
            outError = "Error: Script name cannot be empty.";
            return false;
        }

        if (stem.size() > 120) {
            outError = "Error: Script name is too long (max 120 characters).";
            return false;
        }

        if (stem == "." || stem == ".." || stem.find("..") != std::string::npos) {
            outError = "Error: Invalid script name.";
            return false;
        }

        static const char kInvalidChars[] = "<>:\"/\\|?*";
        for (unsigned char ch : stem) {
            if (ch < 0x20 || std::strchr(kInvalidChars, static_cast<char>(ch)) != nullptr) {
                outError = "Error: Script name contains invalid filename characters.";
                return false;
            }
        }

        if (stem.back() == '.') {
            outError = "Error: Script name cannot end with a dot.";
            return false;
        }

        if (IsReservedWindowsDeviceName(stem)) {
            outError = "Error: Script name uses a reserved Windows device name.";
            return false;
        }

        outFileName = stem + ".lua";
        return true;
    }

    bool ScriptStore::Initialize(std::string& outError) {
        m_directory = ResolveStorageBaseDir() / L"lua_scripts";

        std::error_code ec;
        std::filesystem::create_directories(m_directory, ec);
        if (ec) {
            outError = "Error: Failed to create lua_scripts directory (" + ec.message() + ").";
            return false;
        }

        m_initialized = true;
        return Refresh(outError);
    }

    bool ScriptStore::EnsureInitialized(std::string& outError) {
        if (m_initialized)
            return true;
        return Initialize(outError);
    }

    std::string ScriptStore::DirectoryUtf8() const {
        return WideToUtf8(m_directory.wstring());
    }

    const SavedScript* ScriptStore::ActiveScript() const {
        if (!m_activeIndex.has_value() || *m_activeIndex >= m_scripts.size())
            return nullptr;
        return &m_scripts[*m_activeIndex];
    }

    bool ScriptStore::Refresh(std::string& outError) {
        std::string activeName;
        if (const SavedScript* active = ActiveScript())
            activeName = active->fileName;
        return RefreshPreservingSelection(activeName, outError);
    }

    bool ScriptStore::RefreshPreservingSelection(const std::string& preferredFileName, std::string& outError) {
        if (!EnsureInitialized(outError))
            return false;

        std::error_code ec;
        std::filesystem::create_directories(m_directory, ec);
        if (ec) {
            outError = "Error: Failed to access lua_scripts directory (" + ec.message() + ").";
            return false;
        }

        std::vector<SavedScript> scanned;
        for (const auto& entry : std::filesystem::directory_iterator(m_directory, ec)) {
            if (ec) {
                outError = "Error: Failed to enumerate lua_scripts (" + ec.message() + ").";
                return false;
            }
            if (!entry.is_regular_file(ec))
                continue;

            const std::filesystem::path& p = entry.path();
            const std::string fileName = WideToUtf8(p.filename().wstring());
            if (!EndsWithCaseInsensitive(fileName, ".lua"))
                continue;

            const std::string stem = WideToUtf8(p.stem().wstring());
            scanned.push_back(SavedScript{stem, fileName, p});
        }

        std::sort(scanned.begin(), scanned.end(), [](const SavedScript& a, const SavedScript& b) {
            const std::string al = ToLowerAscii(a.displayName);
            const std::string bl = ToLowerAscii(b.displayName);
            if (al != bl)
                return al < bl;
            return a.fileName < b.fileName;
        });

        m_scripts = std::move(scanned);
        m_activeIndex.reset();

        if (!preferredFileName.empty()) {
            const std::string targetLower = ToLowerAscii(preferredFileName);
            for (size_t i = 0; i < m_scripts.size(); ++i) {
                if (ToLowerAscii(m_scripts[i].fileName) == targetLower) {
                    m_activeIndex = i;
                    break;
                }
            }
        }

        return true;
    }

    bool ScriptStore::Exists(const std::string& rawName, std::string& outNormalizedFileName, std::string& outError) const {
        if (!NormalizeScriptFileName(rawName, outNormalizedFileName, outError))
            return false;
        const std::filesystem::path target = m_directory / Utf8ToWide(outNormalizedFileName);
        std::error_code ec;
        return std::filesystem::exists(target, ec);
    }

    bool ScriptStore::ReadFileNormalized(const std::filesystem::path& path, std::string& outContent, std::string& outError) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            outError = "Error: Could not open script file.";
            return false;
        }

        const std::streamoff size = file.tellg();
        if (size < 0) {
            outError = "Error: Failed to determine script file size.";
            return false;
        }
        if (static_cast<size_t>(size) > kMaxScriptBytes) {
            outError = "Error: Script exceeds editor buffer limit (64 KB).";
            return false;
        }

        file.seekg(0, std::ios::beg);
        std::string raw(static_cast<size_t>(size), '\0');
        if (size > 0 && !file.read(raw.data(), size)) {
            outError = "Error: Failed to read script file.";
            return false;
        }

        outContent = NormalizeLineEndings(raw);
        if (outContent.size() > kMaxScriptBytes) {
            outError = "Error: Script exceeds editor buffer limit (64 KB).";
            return false;
        }
        return true;
    }

    bool ScriptStore::WriteFileAtomic(const std::filesystem::path& path, const std::string& content, std::string& outError) {
        std::filesystem::path tempPath = path;
        tempPath += L".tmp";

        {
            std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
            if (!file.is_open()) {
                outError = "Error: Could not write script file.";
                return false;
            }
            if (!content.empty()) {
                file.write(content.data(), static_cast<std::streamsize>(content.size()));
            }
            file.flush();
            if (!file.good()) {
                file.close();
                std::error_code cleanupEc;
                std::filesystem::remove(tempPath, cleanupEc);
                outError = "Error: Failed while writing script file.";
                return false;
            }
        }

        std::error_code ec;
        std::filesystem::rename(tempPath, path, ec);
        if (ec) {
            // Fallback on filesystems/Win32 rename semantics if target already exists
            if (!MoveFileExW(tempPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                std::error_code cleanupEc;
                std::filesystem::remove(tempPath, cleanupEc);
                outError = "Error: Failed to replace target script file.";
                return false;
            }
        }

        return true;
    }

    bool ScriptStore::LoadScript(size_t index, std::string& outContent, std::string& outError) {
        if (index >= m_scripts.size()) {
            outError = "Error: Invalid script selection.";
            return false;
        }
        if (!ReadFileNormalized(m_scripts[index].fullPath, outContent, outError))
            return false;

        m_activeIndex = index;
        return true;
    }

    bool ScriptStore::ReadScriptByIndex(size_t index, std::string& outContent, std::string& outError) const {
        if (index >= m_scripts.size()) {
            outError = "Error: Invalid script selection.";
            return false;
        }
        return ReadFileNormalized(m_scripts[index].fullPath, outContent, outError);
    }

    bool ScriptStore::SaveActive(const std::string& content, std::string& outError) {
        if (!EnsureInitialized(outError))
            return false;
        const SavedScript* active = ActiveScript();
        if (!active) {
            outError = "Error: No saved script is selected.";
            return false;
        }
        return WriteFileAtomic(active->fullPath, content, outError);
    }

    bool ScriptStore::SaveAs(const std::string& rawName, const std::string& content, bool allowOverwrite, std::string& outError) {
        if (!EnsureInitialized(outError))
            return false;

        std::string fileName;
        if (!NormalizeScriptFileName(rawName, fileName, outError))
            return false;

        const std::filesystem::path targetPath = m_directory / Utf8ToWide(fileName);
        std::error_code ec;
        if (!allowOverwrite && std::filesystem::exists(targetPath, ec)) {
            outError = "Error: A script with that name already exists.";
            return false;
        }

        if (!WriteFileAtomic(targetPath, content, outError))
            return false;

        return RefreshPreservingSelection(fileName, outError);
    }

    bool ScriptStore::RenameActive(const std::string& newRawName, std::string& outError) {
        if (!EnsureInitialized(outError))
            return false;

        const SavedScript* active = ActiveScript();
        if (!active) {
            outError = "Error: No saved script is selected.";
            return false;
        }

        std::string newFileName;
        if (!NormalizeScriptFileName(newRawName, newFileName, outError))
            return false;

        if (ToLowerAscii(newFileName) == ToLowerAscii(active->fileName)) {
            const std::filesystem::path samePath = m_directory / Utf8ToWide(newFileName);
            if (active->fileName == newFileName)
                return true;

            std::error_code ec;
            std::filesystem::rename(active->fullPath, samePath, ec);
            if (ec) {
                outError = "Error: Could not rename script (" + ec.message() + ").";
                return false;
            }
            return RefreshPreservingSelection(newFileName, outError);
        }

        const std::filesystem::path targetPath = m_directory / Utf8ToWide(newFileName);
        std::error_code ec;
        if (std::filesystem::exists(targetPath, ec)) {
            outError = "Error: A script named '" + newFileName + "' already exists.";
            return false;
        }

        std::filesystem::rename(active->fullPath, targetPath, ec);
        if (ec) {
            outError = "Error: Could not rename script (" + ec.message() + ").";
            return false;
        }

        return RefreshPreservingSelection(newFileName, outError);
    }

    bool ScriptStore::DeleteActive(std::string& outError) {
        if (!EnsureInitialized(outError))
            return false;

        const SavedScript* active = ActiveScript();
        if (!active) {
            outError = "Error: No saved script is selected.";
            return false;
        }

        std::error_code ec;
        if (!std::filesystem::remove(active->fullPath, ec) || ec) {
            outError = "Error: Could not delete script (" + ec.message() + ").";
            return false;
        }

        m_activeIndex.reset();
        return RefreshPreservingSelection("", outError);
    }

} // namespace Menu::LuaScripts
