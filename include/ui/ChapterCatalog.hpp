#pragma once

#include <string>
#include <vector>

class GameFileSystem;

namespace ui {

struct ChapterInfo {
    int number = 0;
    std::string title;
    std::string mapName;
    unsigned int previewTexture = 0;
    float previewU0 = 0.0F;
    float previewV0 = 0.0F;
    float previewU1 = 1.0F;
    float previewV1 = 1.0F;
};

std::vector<ChapterInfo> LoadChapters(const GameFileSystem& files);
void ReleaseChapterPreviews(std::vector<ChapterInfo>& chapters);

} // namespace ui
