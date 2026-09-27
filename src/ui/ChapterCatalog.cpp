#include "ui/ChapterCatalog.hpp"

#include "GameFileSystem.hpp"
#include "VtfTexture.hpp"

#include <glad/gl.h>

#include <cctype>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

namespace ui {
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

void AppendUtf8(std::string& out, unsigned int codepoint) {
    if (codepoint < 0x80U) {
        out.push_back(static_cast<char>(codepoint));
        return;
    }
    if (codepoint < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        return;
    }
    out.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
}

std::string DecodeResourceText(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        std::string text;
        text.reserve(bytes.size() / 2);
        for (std::size_t index = 2; index + 1 < bytes.size(); index += 2) {
            const unsigned int codepoint = static_cast<unsigned int>(bytes[index])
                | (static_cast<unsigned int>(bytes[index + 1]) << 8U);
            if (codepoint == 0) {
                continue;
            }
            AppendUtf8(text, codepoint);
        }
        return text;
    }
    std::size_t start = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        start = 3;
    }
    return std::string(reinterpret_cast<const char*>(bytes.data() + start), bytes.size() - start);
}

std::string Trim(std::string value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string UnquoteTitle(std::string value) {
    value = Trim(std::move(value));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    std::string cleaned;
    cleaned.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '\\' && index + 1 < value.size()) {
            cleaned.push_back(value[index + 1]);
            ++index;
            continue;
        }
        cleaned.push_back(value[index]);
    }
    return Trim(cleaned);
}

std::string MapFromChapterConfig(const std::string& text) {
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const std::size_t comment = line.find("//");
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        std::istringstream tokens(line);
        std::string command;
        std::string argument;
        tokens >> command >> argument;
        if (Lower(command) == "map" && !argument.empty()) {
            if (argument.size() >= 2 && argument.front() == '"') {
                argument = argument.substr(1);
                if (!argument.empty() && argument.back() == '"') {
                    argument.pop_back();
                }
            }
            const std::size_t slash = argument.find_last_of("/\\");
            if (slash != std::string::npos) {
                argument = argument.substr(slash + 1);
            }
            if (EndsWith(Lower(argument), ".bsp")) {
                argument.resize(argument.size() - 4);
            }
            return argument;
        }
    }
    return {};
}

std::string FindChapterTitle(const std::string& text, int chapterNumber) {
    const std::string needle = "_chapter" + std::to_string(chapterNumber) + "_title";
    const std::string lowered = Lower(text);
    std::size_t position = 0;
    while ((position = lowered.find(needle, position)) != std::string::npos) {
        std::size_t cursor = position + needle.size();
        if (cursor < text.size() && text[cursor] == '"') {
            ++cursor;
        }
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }
        if (cursor < text.size() && text[cursor] == '"') {
            ++cursor;
            const std::size_t start = cursor;
            while (cursor < text.size() && text[cursor] != '"') {
                if (text[cursor] == '\\' && cursor + 1 < text.size()) {
                    cursor += 2;
                    continue;
                }
                ++cursor;
            }
            return UnquoteTitle(text.substr(start, cursor - start));
        }
        position += needle.size();
    }
    return {};
}

std::string QuotedAfter(const std::string& text, const std::string& key) {
    const std::string lowered = Lower(text);
    const std::string needle = Lower(key);
    const std::size_t found = lowered.find(needle);
    if (found == std::string::npos) {
        return {};
    }
    std::size_t cursor = found + needle.size();
    while (cursor < text.size() && text[cursor] != '"') {
        ++cursor;
    }
    if (cursor >= text.size()) {
        return {};
    }
    ++cursor;
    const std::size_t start = cursor;
    while (cursor < text.size() && text[cursor] != '"') {
        ++cursor;
    }
    return text.substr(start, cursor - start);
}

std::string NormalizeTexturePath(std::string path) {
    for (char& character : path) {
        if (character == '\\') {
            character = '/';
        } else {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
    }
    while (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    if (path.rfind("materials/", 0) != 0) {
        path = "materials/" + path;
    }
    if (!EndsWith(path, ".vtf")) {
        path += ".vtf";
    }
    return path;
}

void ApplyChapterCrop(ChapterInfo& chapter, const std::vector<std::uint8_t>& textureBytes) {
    if (textureBytes.size() < 20) {
        return;
    }
    const unsigned int width = static_cast<unsigned int>(textureBytes[16])
        | (static_cast<unsigned int>(textureBytes[17]) << 8U);
    const unsigned int height = static_cast<unsigned int>(textureBytes[18])
        | (static_cast<unsigned int>(textureBytes[19]) << 8U);
    // Half-Life 2 paints the chapter picture in the top-left 152x86 of a 256x128 texture.
    if (width == 256 && height == 128) {
        chapter.previewU0 = 0.0F;
        chapter.previewV0 = 0.0F;
        chapter.previewU1 = 152.0F / 256.0F;
        chapter.previewV1 = 86.0F / 128.0F;
    }
}

unsigned int LoadChapterPreview(const GameFileSystem& files, ChapterInfo& chapter) {
    const std::string stem = "materials/vgui/chapters/chapter" + std::to_string(chapter.number);
    std::string texturePath = stem + ".vtf";
    std::vector<std::uint8_t> materialBytes;
    if (files.Read(stem + ".vmt", materialBytes)) {
        const std::string material(materialBytes.begin(), materialBytes.end());
        const std::string base = QuotedAfter(material, "$basetexture");
        if (!base.empty()) {
            texturePath = NormalizeTexturePath(base);
        }
    }
    std::vector<std::uint8_t> textureBytes;
    if (!files.Read(texturePath, textureBytes) && texturePath != stem + ".vtf") {
        texturePath = stem + ".vtf";
        if (!files.Read(texturePath, textureBytes)) {
            textureBytes.clear();
        }
    }
    if (textureBytes.empty()) {
        return 0;
    }
    ApplyChapterCrop(chapter, textureBytes);
    try {
        return vtf::LoadVtfTextureData(textureBytes, texturePath);
    } catch (const std::exception& error) {
        std::cerr << "Chapter " << chapter.number << " preview: " << error.what() << '\n';
        return 0;
    }
}

} // namespace

std::vector<ChapterInfo> LoadChapters(const GameFileSystem& files) {
    std::vector<std::string> localization;
    for (const std::string& path : files.List("resource/", ".txt")) {
        if (Lower(path).find("english") == std::string::npos) {
            continue;
        }
        std::vector<std::uint8_t> bytes;
        if (!files.Read(path, bytes)) {
            continue;
        }
        localization.push_back(DecodeResourceText(bytes));
    }

    std::vector<ChapterInfo> chapters;
    for (int number = 1; number <= 32; ++number) {
        const std::string configPath = "cfg/chapter" + std::to_string(number) + ".cfg";
        std::vector<std::uint8_t> configBytes;
        if (!files.Read(configPath, configBytes)) {
            break;
        }
        ChapterInfo chapter;
        chapter.number = number;
        chapter.mapName = MapFromChapterConfig(std::string(configBytes.begin(), configBytes.end()));
        for (const std::string& text : localization) {
            chapter.title = FindChapterTitle(text, number);
            if (!chapter.title.empty()) {
                break;
            }
        }
        if (chapter.title.empty()) {
            chapter.title = "Chapter " + std::to_string(number);
        }
        chapter.previewTexture = LoadChapterPreview(files, chapter);
        chapters.push_back(std::move(chapter));
    }
    if (!chapters.empty()) {
        std::cout << "Loaded " << chapters.size() << " chapters\n";
    }
    return chapters;
}

void ReleaseChapterPreviews(std::vector<ChapterInfo>& chapters) {
    for (ChapterInfo& chapter : chapters) {
        if (chapter.previewTexture != 0) {
            const GLuint texture = chapter.previewTexture;
            glDeleteTextures(1, &texture);
            chapter.previewTexture = 0;
        }
    }
}

} // namespace ui
