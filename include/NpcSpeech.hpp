#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class AudioSystem;
class GameFileSystem;

class NpcSpeech {
public:
    void Load(const GameFileSystem* files);

    bool SpeakConcept(
        const GameFileSystem* files,
        AudioSystem* audio,
        std::string_view classname,
        std::string_view speechConcept,
        std::string_view gender
    ) const;

    bool PlayEntry(
        const GameFileSystem* files,
        AudioSystem* audio,
        std::string_view soundName
    ) const;

private:
    struct Criterion {
        std::string name;
        std::string key;
        std::string value;
    };

    struct ResponseOption {
        std::string kind;
        std::string value;
    };

    struct Rule {
        std::string speechConcept;
        std::string classname;
        std::string response;
    };

    std::unordered_map<std::string, std::vector<std::string>> sounds_;
    std::unordered_map<std::string, std::vector<ResponseOption>> responses_;
    std::vector<Rule> rules_;
};
