#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Menu::LuaScripts {

    /**
     * @brief Metadata for a persisted custom Lua script file on disk.
     */
    struct SavedScript {
        std::string displayName;
        std::string fileName;
        std::filesystem::path fullPath;
    };

    /**
     * @brief Resolves the base storage directory (game root above \Bin\, or DLL directory as fallback).
     */
    std::filesystem::path ResolveStorageBaseDir();

    /**
     * @brief Validates and normalizes a user-provided script name into a safe `<stem>.lua` filename.
     * @param rawName Input script name (with or without `.lua` extension).
     * @param outFileName Normalized filename including `.lua`.
     * @param outError Human-readable reason if validation fails.
     * @return true if valid, false otherwise.
     */
    bool NormalizeScriptFileName(const std::string& rawName, std::string& outFileName, std::string& outError);

    /**
     * @brief Manages persistence and enumeration of custom `.lua` scripts under `<base>\lua_scripts\`.
     */
    class ScriptStore {
    public:
        static constexpr size_t kMaxScriptBytes = 65535;

        bool Initialize(std::string& outError);
        bool Refresh(std::string& outError);

        const std::filesystem::path& Directory() const { return m_directory; }
        std::string DirectoryUtf8() const;

        const std::vector<SavedScript>& Scripts() const { return m_scripts; }
        std::optional<size_t> ActiveIndex() const { return m_activeIndex; }
        const SavedScript* ActiveScript() const;
        void ClearSelection() { m_activeIndex.reset(); }

        bool Exists(const std::string& rawName, std::string& outNormalizedFileName, std::string& outError) const;

        bool LoadScript(size_t index, std::string& outContent, std::string& outError);
        bool ReadScriptByIndex(size_t index, std::string& outContent, std::string& outError) const;

        bool SaveActive(const std::string& content, std::string& outError);
        bool SaveAs(const std::string& rawName, const std::string& content, bool allowOverwrite, std::string& outError);
        bool RenameActive(const std::string& newRawName, std::string& outError);
        bool DeleteActive(std::string& outError);

    private:
        std::filesystem::path m_directory;
        std::vector<SavedScript> m_scripts;
        std::optional<size_t> m_activeIndex;
        bool m_initialized = false;

        bool EnsureInitialized(std::string& outError);
        bool RefreshPreservingSelection(const std::string& preferredFileName, std::string& outError);
        static bool ReadFileNormalized(const std::filesystem::path& path, std::string& outContent, std::string& outError);
        static bool WriteFileAtomic(const std::filesystem::path& path, const std::string& content, std::string& outError);
    };

} // namespace Menu::LuaScripts
