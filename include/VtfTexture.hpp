#pragma once

#include <string>

#include <glad/gl.h>

namespace vtf {

// Loads the largest mip level of a VTF into a new OpenGL texture object.
GLuint LoadVtfTexture(const std::string& path);
GLuint LoadVtfTextureFromVpk(const std::string& vpkPath, const std::string& internalPath);
GLuint LoadRasterTexture(const std::string& path);

}
