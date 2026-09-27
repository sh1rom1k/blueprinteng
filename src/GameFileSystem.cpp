#include "GameFileSystem.hpp"

#include "VpkArchive.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <system_error>

namespace {

std::string Lower(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

bool EndsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size()
        && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

struct KeyValue {
    std::string key;
    std::string value;
    std::vector<KeyValue> children;
};

void SkipWhitespace(const std::string& text, std::size_t& cursor) {
    while (cursor < text.size()) {
        if (std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
            continue;
        }
        if (text[cursor] == '/' && cursor + 1 < text.size() && text[cursor + 1] == '/') {
            cursor += 2;
            while (cursor < text.size() && text[cursor] != '\n') {
                ++cursor;
            }
            continue;
        }
        break;
    }
}

std::string ReadQuotedOrToken(const std::string& text, std::size_t& cursor) {
    SkipWhitespace(text, cursor);
    if (cursor >= text.size()) {
        return {};
    }
    if (text[cursor] == '"') {
        ++cursor;
        const std::size_t start = cursor;
        while (cursor < text.size() && text[cursor] != '"') {
            ++cursor;
        }
        const std::string value = text.substr(start, cursor - start);
        if (cursor < text.size()) {
            ++cursor;
        }
        return value;
    }
    const std::size_t start = cursor;
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) == 0
        && text[cursor] != '{' && text[cursor] != '}' && text[cursor] != '"') {
        ++cursor;
    }
    return text.substr(start, cursor - start);
}

KeyValue ParseBlock(const std::string& text, std::size_t& cursor) {
    KeyValue block;
    SkipWhitespace(text, cursor);
    if (cursor < text.size() && text[cursor] == '{') {
        ++cursor;
    }
    while (cursor < text.size()) {
        SkipWhitespace(text, cursor);
        if (cursor >= text.size()) {
            break;
        }
        if (text[cursor] == '}') {
            ++cursor;
            break;
        }
        KeyValue child;
        child.key = ReadQuotedOrToken(text, cursor);
        SkipWhitespace(text, cursor);
        if (cursor < text.size() && text[cursor] == '{') {
            const std::string key = child.key;
            child = ParseBlock(text, cursor);
            child.key = key;
            if (!key.empty()) {
                block.children.push_back(std::move(child));
            }
            continue;
        }
        child.value = ReadQuotedOrToken(text, cursor);
        if (!child.key.empty()) {
            block.children.push_back(std::move(child));
        }
    }
    return block;
}

const KeyValue* FindChild(const KeyValue& node, const std::string& key) {
    const std::string lowered = Lower(key);
    for (const KeyValue& child : node.children) {
        if (Lower(child.key) == lowered) {
            return &child;
        }
    }
    return nullptr;
}

std::filesystem::path WithTrailingSlash(const std::filesystem::path& path) {
    std::string text = path.generic_string();
    if (!text.empty() && text.back() != '/') {
        text.push_back('/');
    }
    return text;
}

} // namespace

bool GameFileSystem::Mount(const std::filesystem::path& gameDirectory) {
    std::error_code error;
    gameDirectory_ = std::filesystem::weakly_canonical(gameDirectory, error);
    if (error) {
        gameDirectory_ = gameDirectory;
    }
    cacheDirectory_ = gameDirectory_ / ".blueprint_cache";
    gameLabel_.clear();
    mounts_.clear();
    if (!std::filesystem::is_directory(gameDirectory_)) {
        std::cerr << "Game directory not found: " << gameDirectory_ << '\n';
        return false;
    }

    MountCustom(gameDirectory_ / "custom");

    const std::filesystem::path gameInfoPath = gameDirectory_ / "gameinfo.txt";
    if (!std::filesystem::is_regular_file(gameInfoPath)) {
        std::cout << "No gameinfo.txt in " << gameDirectory_ << ", mounting the folder directly\n";
        MountDirectory(gameDirectory_);
        return !mounts_.empty();
    }

    std::ifstream file(gameInfoPath, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::size_t cursor = 0;
    const std::string rootKey = ReadQuotedOrToken(text, cursor);
    KeyValue root = ParseBlock(text, cursor);
    root.key = rootKey;
    const KeyValue* gameInfo = FindChild(root, "GameInfo");
    if (gameInfo == nullptr && Lower(root.key) == "gameinfo") {
        gameInfo = &root;
    }
    if (gameInfo != nullptr) {
        if (const KeyValue* game = FindChild(*gameInfo, "game")) {
            gameLabel_ = game->value;
        }
    }
    const KeyValue* fileSystem = gameInfo != nullptr ? FindChild(*gameInfo, "FileSystem") : nullptr;
    const KeyValue* searchPaths = fileSystem != nullptr ? FindChild(*fileSystem, "SearchPaths") : nullptr;
    if (searchPaths == nullptr) {
        std::cerr << "gameinfo.txt has no FileSystem/SearchPaths, mounting the folder directly\n";
        MountDirectory(gameDirectory_);
        return !mounts_.empty();
    }

    for (const KeyValue& entry : searchPaths->children) {
        if (entry.value.empty()) {
            continue;
        }
        const std::filesystem::path resolved(Expand(entry.value));
        MountSearchPath(resolved.lexically_normal());
    }
    if (mounts_.empty()) {
        MountDirectory(gameDirectory_);
    }
    std::cout << "Mounted " << mounts_.size() << " search paths from " << gameDirectory_ << '\n';
    return true;
}

const std::filesystem::path& GameFileSystem::GameDirectory() const {
    return gameDirectory_;
}

const std::string& GameFileSystem::GameLabel() const {
    return gameLabel_;
}

std::string GameFileSystem::Normalize(std::string_view relativePath) {
    std::string value(relativePath);
    for (char& character : value) {
        if (character == '\\') {
            character = '/';
        } else {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
    }
    while (!value.empty() && value.front() == '/') {
        value.erase(value.begin());
    }
    return value;
}

std::string GameFileSystem::Expand(std::string value) const {
    auto replaceToken = [&](const char* token, const std::filesystem::path& path) {
        const std::string replacement = WithTrailingSlash(path).generic_string();
        const std::string needle = token;
        std::size_t position = 0;
        while ((position = value.find(needle, position)) != std::string::npos) {
            value.replace(position, needle.size(), replacement);
            position += replacement.size();
        }
    };
    replaceToken("|gameinfo_path|", gameDirectory_);
    replaceToken("|all_source_engine_paths|", gameDirectory_.parent_path());
    return value;
}

void GameFileSystem::MountVpk(const std::filesystem::path& directoryArchive) {
    if (!std::filesystem::is_regular_file(directoryArchive)) {
        return;
    }
    for (const MountPoint& mount : mounts_) {
        if (mount.archive != nullptr && mount.loose.empty()) {
            continue;
        }
    }
    try {
        auto archive = std::make_unique<VpkArchive>(directoryArchive);
        MountPoint mount;
        mount.archive = std::move(archive);
        mounts_.push_back(std::move(mount));
        std::cout << "  vpk " << directoryArchive.filename().string() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Skipping VPK " << directoryArchive << ": " << error.what() << '\n';
    }
}

void GameFileSystem::MountDirectory(const std::filesystem::path& directory) {
    if (!std::filesystem::is_directory(directory)) {
        return;
    }
    MountPoint loose;
    loose.loose = directory;
    mounts_.push_back(std::move(loose));
    std::vector<std::filesystem::path> archives;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const std::string name = Lower(entry.path().filename().string());
        if (entry.is_regular_file() && EndsWith(name, "_dir.vpk")) {
            archives.push_back(entry.path());
        }
    }
    std::sort(archives.begin(), archives.end());
    for (const auto& archive : archives) {
        MountVpk(archive);
    }
}

void GameFileSystem::MountSearchPath(const std::filesystem::path& path) {
    std::string name = Lower(path.filename().string());
    if (EndsWith(name, ".vpk")) {
        std::filesystem::path archive = path;
        if (!std::filesystem::is_regular_file(archive)) {
            std::string filename = path.filename().string();
            const std::string extension = ".vpk";
            if (filename.size() > extension.size()) {
                filename.insert(filename.size() - extension.size(), "_dir");
                archive = path.parent_path() / filename;
            }
        }
        MountVpk(archive);
        return;
    }
    MountDirectory(path);
}

void GameFileSystem::MountCustom(const std::filesystem::path& customDirectory) {
    if (!std::filesystem::is_directory(customDirectory)) {
        return;
    }
    std::vector<std::filesystem::path> entries;
    for (const auto& entry : std::filesystem::directory_iterator(customDirectory)) {
        entries.push_back(entry.path());
    }
    std::sort(entries.begin(), entries.end());
    for (const auto& entry : entries) {
        const std::string name = Lower(entry.filename().string());
        if (std::filesystem::is_directory(entry)) {
            MountDirectory(entry);
        } else if (EndsWith(name, ".vpk")) {
            MountSearchPath(entry);
        }
    }
}

bool GameFileSystem::Exists(std::string_view relativePath) const {
    const std::string relative = Normalize(relativePath);
    for (const MountPoint& mount : mounts_) {
        if (!mount.loose.empty()) {
            const std::filesystem::path candidate = mount.loose / relative;
            if (std::filesystem::is_regular_file(candidate)) {
                return true;
            }
        }
        if (mount.archive != nullptr && mount.archive->Contains(relative)) {
            return true;
        }
    }
    return false;
}

bool GameFileSystem::Read(std::string_view relativePath, std::vector<std::uint8_t>& out) const {
    const std::string relative = Normalize(relativePath);
    for (const MountPoint& mount : mounts_) {
        if (!mount.loose.empty()) {
            const std::filesystem::path candidate = mount.loose / relative;
            std::ifstream file(candidate, std::ios::binary);
            if (!file) {
                continue;
            }
            file.seekg(0, std::ios::end);
            const std::streamoff size = file.tellg();
            if (size < 0) {
                continue;
            }
            file.seekg(0, std::ios::beg);
            out.resize(static_cast<std::size_t>(size));
            if (!out.empty() && !file.read(reinterpret_cast<char*>(out.data()), size)) {
                out.clear();
                continue;
            }
            return true;
        }
        if (mount.archive != nullptr && mount.archive->Read(relative, out)) {
            return true;
        }
    }
    out.clear();
    return false;
}

std::vector<std::string> GameFileSystem::List(std::string_view directory, std::string_view extension) const {
    const std::string prefix = Normalize(directory);
    const std::string suffix = Lower(std::string(extension));
    std::vector<std::string> results;
    auto consider = [&](std::string path) {
        path = Normalize(path);
        if (!prefix.empty() && path.rfind(prefix, 0) != 0) {
            return;
        }
        if (!suffix.empty() && !EndsWith(path, suffix)) {
            return;
        }
        if (std::find(results.begin(), results.end(), path) == results.end()) {
            results.push_back(std::move(path));
        }
    };

    for (const MountPoint& mount : mounts_) {
        if (!mount.loose.empty()) {
            const std::filesystem::path folder = mount.loose / prefix;
            if (!std::filesystem::is_directory(folder)) {
                continue;
            }
            for (const auto& entry : std::filesystem::directory_iterator(folder)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                consider(prefix + entry.path().filename().string());
            }
        }
        if (mount.archive != nullptr) {
            for (const std::string& entry : mount.archive->Entries()) {
                consider(entry);
            }
        }
    }
    std::sort(results.begin(), results.end());
    return results;
}

std::filesystem::path GameFileSystem::Materialize(std::string_view relativePath) const {
    const std::string relative = Normalize(relativePath);
    for (const MountPoint& mount : mounts_) {
        if (mount.loose.empty()) {
            continue;
        }
        const std::filesystem::path candidate = mount.loose / relative;
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }
    std::vector<std::uint8_t> bytes;
    if (!Read(relative, bytes)) {
        return {};
    }
    const std::filesystem::path cached = cacheDirectory_ / relative;
    std::error_code error;
    std::filesystem::create_directories(cached.parent_path(), error);
    std::ofstream output(cached, std::ios::binary | std::ios::trunc);
    if (!output) {
        return {};
    }
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return cached;
}
