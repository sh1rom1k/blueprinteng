#pragma once

#include <string>

#include <glad/gl.h>

#include <cstdint>
#include <vector>

namespace vtf {

// Loads the largest mip level of a VTF into a new OpenGL texture object.
GLuint LoadVtfTexture(const std::string& path);
GLuint LoadVtfTextureData(const std::vector<std::uint8_t>& data, const std::string& source);
GLuint LoadVtfCubemapData(const std::vector<std::uint8_t>& data, const std::string& source);
GLuint LoadVtfTextureFromVpk(const std::string& vpkPath, const std::string& internalPath);
GLuint LoadRasterTexture(const std::string& path);

// Source-style missing texture: repeating purple and black squares.
GLuint CreateMissingTexture();

}
