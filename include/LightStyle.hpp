#pragma once

#include <array>
#include <cmath>
#include <string>
#include <string_view>

inline void FillDefaultLightStyles(std::array<std::string, 64>& styles) {
    static constexpr const char* kDefaults[] = {
        "m",
        "mmnmmommommnonmmonqnmmo",
        "abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba",
        "mmmmmaaaaammmmmaaaaaabcdefgabcdefg",
        "mamamamamama",
        "jklmnopqrstuvwxyzyxwvutsrqponmlkj",
        "nmonqnmomnmomomno",
        "mmmaaaabcdefgmmmmaaaammmaamm",
        "mmmaaammmaaammmabcdefaaaammmmabcdefmmmaaaa",
        "aaaaaaaazzzzzzzz",
        "mmamammmmammamamaaamammma",
        "abcdefghijklmnopqrrqponmlkjihgfedcba",
    };
    for (std::string& style : styles) {
        style = "m";
    }
    for (std::size_t index = 0; index < sizeof(kDefaults) / sizeof(kDefaults[0]); ++index) {
        styles[index] = kDefaults[index];
    }
}

inline float LightStyleAt(std::string_view pattern, float timeSeconds) {
    if (pattern.empty()) {
        return 1.0F;
    }
    const int length = static_cast<int>(pattern.size());
    int frame = static_cast<int>(std::floor(timeSeconds * 10.0F));
    frame %= length;
    if (frame < 0) {
        frame += length;
    }
    const char sample = pattern[static_cast<std::size_t>(frame)];
    if (sample < 'a' || sample > 'z') {
        return 1.0F;
    }
    return static_cast<float>(sample - 'a') / 12.0F;
}
