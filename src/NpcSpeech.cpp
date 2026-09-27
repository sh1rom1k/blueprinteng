#include "NpcSpeech.hpp"

#include "AudioSystem.hpp"
#include "GameFileSystem.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <random>
#include <string>

namespace {

std::string Lower(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

std::string StripComments(const std::string& text) {
    std::string clean;
    clean.reserve(text.size());
    bool quote = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '"') {
            quote = !quote;
        }
        if (!quote && character == '/' && index + 1 < text.size() && text[index + 1] == '/') {
            while (index < text.size() && text[index] != '\n') {
                ++index;
            }
            clean.push_back('\n');
            continue;
        }
        clean.push_back(character);
    }
    return clean;
}

struct Token {
    std::string text;
    bool brace = false;
};

std::vector<Token> Tokenize(const std::string& text) {
    std::vector<Token> tokens;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const unsigned char character = static_cast<unsigned char>(text[cursor]);
        if (std::isspace(character) != 0) {
            ++cursor;
            continue;
        }
        if (text[cursor] == '{' || text[cursor] == '}') {
            tokens.push_back(Token{std::string(1, text[cursor]), true});
            ++cursor;
            continue;
        }
        if (text[cursor] == '"') {
            ++cursor;
            const std::size_t start = cursor;
            while (cursor < text.size() && text[cursor] != '"') {
                ++cursor;
            }
            tokens.push_back(Token{text.substr(start, cursor - start), false});
            if (cursor < text.size()) {
                ++cursor;
            }
            continue;
        }
        const std::size_t start = cursor;
        while (cursor < text.size()) {
            const char next = text[cursor];
            if (std::isspace(static_cast<unsigned char>(next)) != 0 || next == '{' || next == '}' || next == '"') {
                break;
            }
            ++cursor;
        }
        tokens.push_back(Token{text.substr(start, cursor - start), false});
    }
    return tokens;
}

bool IsKeyword(const std::string& token) {
    const std::string lower = Lower(token);
    return lower == "criterion" || lower == "response" || lower == "rule" || lower == "enumeration"
        || lower == "#include" || lower == "include";
}

std::vector<std::string> ReadFileText(const GameFileSystem* files, const std::string& path) {
    std::vector<std::string> lines;
    if (files == nullptr) {
        return lines;
    }
    std::vector<std::uint8_t> bytes;
    if (!files->Read(path, bytes) || bytes.empty()) {
        return lines;
    }
    lines.push_back(std::string(bytes.begin(), bytes.end()));
    return lines;
}

std::string ReadText(const GameFileSystem* files, const std::string& path) {
    const std::vector<std::string> lines = ReadFileText(files, path);
    return lines.empty() ? std::string{} : lines.front();
}

struct KeyValue {
    std::string key;
    std::string value;
    std::vector<KeyValue> children;
};

void SkipSpace(const std::string& text, std::size_t& cursor) {
    while (cursor < text.size()) {
        if (std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
            continue;
        }
        if (text[cursor] == '/' && cursor + 1 < text.size() && text[cursor + 1] == '/') {
            while (cursor < text.size() && text[cursor] != '\n') {
                ++cursor;
            }
            continue;
        }
        break;
    }
}

std::string ReadToken(const std::string& text, std::size_t& cursor) {
    SkipSpace(text, cursor);
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
    SkipSpace(text, cursor);
    if (cursor < text.size() && text[cursor] == '{') {
        ++cursor;
    }
    while (cursor < text.size()) {
        SkipSpace(text, cursor);
        if (cursor >= text.size() || text[cursor] == '}') {
            if (cursor < text.size()) {
                ++cursor;
            }
            break;
        }
        KeyValue child;
        child.key = ReadToken(text, cursor);
        SkipSpace(text, cursor);
        if (cursor < text.size() && text[cursor] == '{') {
            const std::string key = child.key;
            child = ParseBlock(text, cursor);
            child.key = key;
            if (!key.empty()) {
                block.children.push_back(std::move(child));
            }
            continue;
        }
        child.value = ReadToken(text, cursor);
        if (!child.key.empty()) {
            block.children.push_back(std::move(child));
        }
    }
    return block;
}

void CollectWaves(const KeyValue& node, std::vector<std::string>& waves) {
    if (Lower(node.key) == "wave" && !node.value.empty()) {
        waves.push_back(node.value);
    }
    for (const KeyValue& child : node.children) {
        CollectWaves(child, waves);
    }
}

std::string CleanWave(std::string wave) {
    for (char& character : wave) {
        if (character == '\\') {
            character = '/';
        }
    }
    while (!wave.empty() && (wave.front() == ')' || wave.front() == '#' || wave.front() == '*' || wave.front() == ' ')) {
        wave.erase(wave.begin());
    }
    const std::size_t comma = wave.find(',');
    if (comma != std::string::npos) {
        wave.resize(comma);
    }
    return wave;
}

std::string GenderPath(std::string path, std::string_view gender) {
    const std::string token = "$gender01";
    const std::string lower = Lower(path);
    std::size_t found = lower.find(token);
    while (found != std::string::npos) {
        path.replace(found, token.size(), gender);
        found = Lower(path).find(token, found + gender.size());
    }
    return path;
}

std::string FirstWaveInScene(const std::string& text) {
    const std::string lower = Lower(text);
    std::size_t search = 0;
    while (search < lower.size()) {
        const std::size_t found = lower.find(".wav", search);
        if (found == std::string::npos) {
            break;
        }
        std::size_t start = found;
        while (start > 0 && text[start - 1] != '"' && text[start - 1] != ' ' && text[start - 1] != '\t' && text[start - 1] != '\n') {
            --start;
        }
        std::size_t end = found + 4;
        if (end < text.size() && text[end] == '"') {
            // already ended
        }
        return CleanWave(text.substr(start, end - start));
    }
    return {};
}

std::mt19937& Rng() {
    static std::mt19937 rng{std::random_device{}()};
    return rng;
}

template <typename T>
const T& Pick(const std::vector<T>& values) {
    std::uniform_int_distribution<std::size_t> distribution(0, values.size() - 1);
    return values[distribution(Rng())];
}

} // namespace

void NpcSpeech::Load(const GameFileSystem* files) {
    sounds_.clear();
    responses_.clear();
    rules_.clear();
    if (files == nullptr) {
        return;
    }

    std::vector<std::string> soundFiles = {
        "scripts/npc_sounds_citizen.txt",
        "scripts/npc_sounds_soldier.txt",
        "scripts/npc_sounds_metropolice.txt",
        "scripts/npc_sounds_alyx.txt",
        "scripts/npc_sounds_barney.txt",
    };
    const std::string manifest = ReadText(files, "scripts/game_sounds_manifest.txt");
    std::size_t cursor = 0;
    SkipSpace(manifest, cursor);
    ReadToken(manifest, cursor);
    const KeyValue manifestRoot = ParseBlock(manifest, cursor);
    for (const KeyValue& child : manifestRoot.children) {
        if (Lower(child.key) == "precache_file" && !child.value.empty()) {
            soundFiles.push_back(child.value);
        }
    }

    for (const std::string& path : soundFiles) {
        const std::string text = ReadText(files, path);
        if (text.empty()) {
            continue;
        }
        std::size_t soundCursor = 0;
        const KeyValue root = ParseBlock(text, soundCursor);
        for (const KeyValue& entry : root.children) {
            if (entry.key.find('.') == std::string::npos) {
                continue;
            }
            std::vector<std::string> waves;
            CollectWaves(entry, waves);
            std::vector<std::string> cleaned;
            for (std::string wave : waves) {
                wave = CleanWave(std::move(wave));
                if (wave.empty() || Lower(wave).find("null.wav") != std::string::npos) {
                    continue;
                }
                cleaned.push_back(std::move(wave));
            }
            if (!cleaned.empty()) {
                sounds_[Lower(entry.key)] = std::move(cleaned);
            }
        }
    }

    std::vector<std::string> pending = {"scripts/talker/response_rules.txt"};
    std::vector<std::string> loaded;
    while (!pending.empty()) {
        const std::string path = pending.back();
        pending.pop_back();
        if (std::find(loaded.begin(), loaded.end(), Lower(path)) != loaded.end()) {
            continue;
        }
        loaded.push_back(Lower(path));
        std::string text = ReadText(files, path);
        if (text.empty() && path.rfind("scripts/", 0) != 0) {
            text = ReadText(files, "scripts/talker/" + path);
        }
        if (text.empty()) {
            continue;
        }
        const std::vector<Token> tokens = Tokenize(StripComments(text));
        for (std::size_t index = 0; index < tokens.size(); ++index) {
            const std::string keyword = Lower(tokens[index].text);
            if (keyword == "#include" || keyword == "include") {
                if (index + 1 < tokens.size()) {
                    std::string include = tokens[++index].text;
                    for (char& character : include) {
                        if (character == '\\') {
                            character = '/';
                        }
                    }
                    if (include.find('/') == std::string::npos) {
                        include = "scripts/talker/" + include;
                    }
                    pending.push_back(include);
                }
                continue;
            }
            if (keyword == "response" && index + 1 < tokens.size()) {
                const std::string name = Lower(tokens[++index].text);
                std::vector<ResponseOption> options;
                if (index + 1 < tokens.size() && tokens[index + 1].text == "{") {
                    ++index;
                    int depth = 1;
                    while (index + 1 < tokens.size() && depth > 0) {
                        ++index;
                        if (tokens[index].text == "{") {
                            ++depth;
                            continue;
                        }
                        if (tokens[index].text == "}") {
                            --depth;
                            continue;
                        }
                        const std::string kind = Lower(tokens[index].text);
                        if ((kind == "scene" || kind == "speak" || kind == "sentence") && index + 1 < tokens.size()) {
                            options.push_back(ResponseOption{kind, tokens[++index].text});
                        }
                    }
                } else {
                    while (index + 1 < tokens.size() && !IsKeyword(tokens[index + 1].text)) {
                        ++index;
                        const std::string kind = Lower(tokens[index].text);
                        if ((kind == "scene" || kind == "speak" || kind == "sentence") && index + 1 < tokens.size()) {
                            options.push_back(ResponseOption{kind, tokens[++index].text});
                        }
                    }
                }
                if (!options.empty()) {
                    responses_[name] = std::move(options);
                }
                continue;
            }
            if (keyword == "criterion" && index + 3 < tokens.size()) {
                const std::string name = Lower(tokens[++index].text);
                const std::string key = Lower(tokens[++index].text);
                const std::string value = tokens[++index].text;
                if (key == "concept") {
                    Rule marker;
                    marker.speechConcept = value;
                    marker.response = name;
                    rules_.push_back(std::move(marker));
                } else if (key == "classname") {
                    for (auto it = rules_.rbegin(); it != rules_.rend(); ++it) {
                        if (it->response == name && it->classname.empty() && it->speechConcept.empty()) {
                            it->classname = Lower(value);
                            break;
                        }
                    }
                    Rule named;
                    named.classname = Lower(value);
                    named.response = name;
                    bool exists = false;
                    for (const Rule& rule : rules_) {
                        if (rule.response == name && rule.classname == named.classname && rule.speechConcept.empty()) {
                            exists = true;
                            break;
                        }
                    }
                    if (!exists) {
                        rules_.push_back(std::move(named));
                    }
                }
                while (index + 1 < tokens.size() && !IsKeyword(tokens[index + 1].text) && tokens[index + 1].text != "{") {
                    ++index;
                }
                continue;
            }
            if (keyword == "rule" && index + 1 < tokens.size()) {
                ++index;
                std::string response;
                std::string speechConcept;
                std::string classname;
                if (index + 1 < tokens.size() && tokens[index + 1].text == "{") {
                    ++index;
                    int depth = 1;
                    while (index + 1 < tokens.size() && depth > 0) {
                        ++index;
                        if (tokens[index].text == "{") {
                            ++depth;
                            continue;
                        }
                        if (tokens[index].text == "}") {
                            --depth;
                            continue;
                        }
                        const std::string field = Lower(tokens[index].text);
                        if (field == "response" && index + 1 < tokens.size()) {
                            response = Lower(tokens[++index].text);
                        } else if (field == "criteria" || field == "criterion") {
                            while (index + 1 < tokens.size() && !IsKeyword(tokens[index + 1].text)
                                && Lower(tokens[index + 1].text) != "response" && tokens[index + 1].text != "}") {
                                ++index;
                                const std::string criterionName = Lower(tokens[index].text);
                                for (const Rule& marker : rules_) {
                                    if (marker.response != criterionName) {
                                        continue;
                                    }
                                    if (!marker.speechConcept.empty()) {
                                        speechConcept = marker.speechConcept;
                                    }
                                    if (!marker.classname.empty()) {
                                        classname = marker.classname;
                                    }
                                }
                            }
                        }
                    }
                }
                if (!response.empty() && !speechConcept.empty()) {
                    Rule rule;
                    rule.speechConcept = speechConcept;
                    rule.classname = classname;
                    rule.response = response;
                    rules_.push_back(std::move(rule));
                }
            }
        }
    }
}

bool NpcSpeech::PlayEntry(const GameFileSystem* files, AudioSystem* audio, std::string_view soundName) const {
    if (files == nullptr || audio == nullptr || soundName.empty()) {
        return false;
    }
    const auto found = sounds_.find(Lower(std::string(soundName)));
    if (found == sounds_.end() || found->second.empty()) {
        return false;
    }
    std::string wave = CleanWave(Pick(found->second));
    if (wave.empty()) {
        return false;
    }
    for (char& character : wave) {
        if (character == '\\') {
            character = '/';
        }
    }
    std::string path = Lower(wave).rfind("sound/", 0) == 0 ? wave : "sound/" + wave;
    if (!files->Exists(path)) {
        return false;
    }
    audio->PlayOneShot(path, 1.0F);
    return true;
}

bool NpcSpeech::SpeakConcept(
    const GameFileSystem* files,
    AudioSystem* audio,
    std::string_view classname,
    std::string_view speechConcept,
    std::string_view gender
) const {
    if (files == nullptr || audio == nullptr) {
        return false;
    }
    const std::string wantedClass = Lower(std::string(classname));
    const std::string wantedConcept = std::string(speechConcept);
    std::vector<const Rule*> matches;
    for (const Rule& rule : rules_) {
        if (rule.speechConcept != wantedConcept || rule.response.empty()) {
            continue;
        }
        if (!rule.classname.empty() && rule.classname != wantedClass) {
            continue;
        }
        if (responses_.find(rule.response) == responses_.end()) {
            continue;
        }
        matches.push_back(&rule);
    }
    if (matches.empty()) {
        return false;
    }
    std::vector<const Rule*> specific;
    for (const Rule* rule : matches) {
        if (rule->classname == wantedClass) {
            specific.push_back(rule);
        }
    }
    const Rule& rule = specific.empty() ? *Pick(matches) : *Pick(specific);
    const auto response = responses_.find(rule.response);
    if (response == responses_.end() || response->second.empty()) {
        return false;
    }
    const ResponseOption& option = Pick(response->second);
    if (option.kind == "speak" || option.kind == "sentence") {
        return PlayEntry(files, audio, option.value);
    }
    if (option.kind != "scene") {
        return false;
    }
    const std::string scenePath = GenderPath(option.value, gender);
    const std::string scene = ReadText(files, scenePath);
    if (scene.empty()) {
        return false;
    }
    const std::string wave = FirstWaveInScene(scene);
    if (wave.empty()) {
        return false;
    }
    std::string path = Lower(wave).rfind("sound/", 0) == 0 ? wave : "sound/" + wave;
    if (!files->Exists(path)) {
        return false;
    }
    audio->PlayOneShot(path, 1.0F);
    return true;
}
