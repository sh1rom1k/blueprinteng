#include "Shader.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <glm/gtc/type_ptr.hpp>

Shader::Shader(
    const std::filesystem::path& vertexPath,
    const std::filesystem::path& fragmentPath,
    const std::filesystem::path& geometryPath
) {
    const GLuint vertexShader = Compile(GL_VERTEX_SHADER, ReadFile(vertexPath));
    const GLuint fragmentShader = Compile(GL_FRAGMENT_SHADER, ReadFile(fragmentPath));
    GLuint geometryShader = 0;
    if (!geometryPath.empty()) {
        geometryShader = Compile(GL_GEOMETRY_SHADER, ReadFile(geometryPath));
    }

    program_ = glCreateProgram();
    glAttachShader(program_, vertexShader);
    if (geometryShader != 0) {
        glAttachShader(program_, geometryShader);
    }
    glAttachShader(program_, fragmentShader);
    glLinkProgram(program_);

    GLint linkStatus = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linkStatus);
    if (linkStatus == GL_FALSE) {
        GLint logLength = 0;
        glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<std::size_t>(logLength), '\0');
        glGetProgramInfoLog(program_, logLength, nullptr, log.data());
        glDeleteProgram(program_);
        program_ = 0;
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        if (geometryShader != 0) {
            glDeleteShader(geometryShader);
        }
        throw std::runtime_error("Shader program linking failed:\n" + log);
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    if (geometryShader != 0) {
        glDeleteShader(geometryShader);
    }
}

Shader::~Shader() {
    if (program_ != 0) {
        glDeleteProgram(program_);
    }
}

void Shader::Use() const {
    glUseProgram(program_);
}

GLint Shader::GetUniformLocation(const std::string& name) const {
    const auto it = uniformLocations_.find(name);
    if (it != uniformLocations_.end()) {
        return it->second;
    }
    const GLint location = glGetUniformLocation(program_, name.c_str());
    uniformLocations_[name] = location;
    return location;
}

void Shader::SetMat4(const std::string& name, const glm::mat4& matrix) const {
    const GLint location = GetUniformLocation(name);
    if (location != -1) {
        glUniformMatrix4fv(location, 1, GL_FALSE, glm::value_ptr(matrix));
    }
}

void Shader::SetMat4Array(const std::string& name, const glm::mat4* matrices, GLsizei count) const {
    const GLint location = GetUniformLocation(name);
    if (location != -1 && count > 0 && matrices != nullptr) {
        glUniformMatrix4fv(location, count, GL_FALSE, glm::value_ptr(matrices[0]));
    }
}

void Shader::CacheLightUniformLocations() const {
    for (int i = 0; i < 32; ++i) {
        const std::string base = "PointLights[" + std::to_string(i) + "].";
        pointLightLocs_[i].pos = glGetUniformLocation(program_, (base + "position").c_str());
        pointLightLocs_[i].color = glGetUniformLocation(program_, (base + "color").c_str());
        pointLightLocs_[i].intensity = glGetUniformLocation(program_, (base + "intensity").c_str());
        pointLightLocs_[i].constant = glGetUniformLocation(program_, (base + "constant").c_str());
        pointLightLocs_[i].linear = glGetUniformLocation(program_, (base + "linear").c_str());
        pointLightLocs_[i].quadratic = glGetUniformLocation(program_, (base + "quadratic").c_str());
    }
    spotLightLocs_.pos = glGetUniformLocation(program_, "Flashlight.position");
    spotLightLocs_.dir = glGetUniformLocation(program_, "Flashlight.direction");
    spotLightLocs_.color = glGetUniformLocation(program_, "Flashlight.color");
    spotLightLocs_.intensity = glGetUniformLocation(program_, "Flashlight.intensity");
    spotLightLocs_.innerCutOff = glGetUniformLocation(program_, "Flashlight.innerCutOff");
    spotLightLocs_.outerCutOff = glGetUniformLocation(program_, "Flashlight.outerCutOff");
    spotLightLocs_.constant = glGetUniformLocation(program_, "Flashlight.constant");
    spotLightLocs_.linear = glGetUniformLocation(program_, "Flashlight.linear");
    spotLightLocs_.quadratic = glGetUniformLocation(program_, "Flashlight.quadratic");
    spotLightLocs_.enabled = glGetUniformLocation(program_, "Flashlight.enabled");
    lightUniformsCached_ = true;
}

void Shader::SetPointLightUniform(
    int index,
    const glm::vec3& position,
    const glm::vec3& color,
    float intensity,
    float constant,
    float linear,
    float quadratic
) const {
    if (!lightUniformsCached_) {
        CacheLightUniformLocations();
    }
    if (index < 0 || index >= 32) return;
    const auto& loc = pointLightLocs_[index];
    if (loc.pos != -1) glUniform3fv(loc.pos, 1, glm::value_ptr(position));
    if (loc.color != -1) glUniform3fv(loc.color, 1, glm::value_ptr(color));
    if (loc.intensity != -1) glUniform1f(loc.intensity, intensity);
    if (loc.constant != -1) glUniform1f(loc.constant, constant);
    if (loc.linear != -1) glUniform1f(loc.linear, linear);
    if (loc.quadratic != -1) glUniform1f(loc.quadratic, quadratic);
}

void Shader::SetSpotlightUniform(
    const glm::vec3& position,
    const glm::vec3& direction,
    const glm::vec3& color,
    float intensity,
    float innerCutOff,
    float outerCutOff,
    float constant,
    float linear,
    float quadratic,
    bool enabled
) const {
    if (!lightUniformsCached_) {
        CacheLightUniformLocations();
    }
    if (spotLightLocs_.enabled != -1) glUniform1i(spotLightLocs_.enabled, enabled ? 1 : 0);
    if (spotLightLocs_.pos != -1) glUniform3fv(spotLightLocs_.pos, 1, glm::value_ptr(position));
    if (spotLightLocs_.dir != -1) glUniform3fv(spotLightLocs_.dir, 1, glm::value_ptr(direction));
    if (spotLightLocs_.color != -1) glUniform3fv(spotLightLocs_.color, 1, glm::value_ptr(color));
    if (spotLightLocs_.intensity != -1) glUniform1f(spotLightLocs_.intensity, intensity);
    if (spotLightLocs_.innerCutOff != -1) glUniform1f(spotLightLocs_.innerCutOff, innerCutOff);
    if (spotLightLocs_.outerCutOff != -1) glUniform1f(spotLightLocs_.outerCutOff, outerCutOff);
    if (spotLightLocs_.constant != -1) glUniform1f(spotLightLocs_.constant, constant);
    if (spotLightLocs_.linear != -1) glUniform1f(spotLightLocs_.linear, linear);
    if (spotLightLocs_.quadratic != -1) glUniform1f(spotLightLocs_.quadratic, quadratic);
}

void Shader::SetInt(const std::string& name, int value) const {
    const GLint location = GetUniformLocation(name);
    if (location != -1) {
        glUniform1i(location, value);
    }
}

void Shader::SetBool(const std::string& name, bool value) const {
    SetInt(name, value ? 1 : 0);
}

void Shader::SetFloat(const std::string& name, float value) const {
    const GLint location = GetUniformLocation(name);
    if (location != -1) {
        glUniform1f(location, value);
    }
}

void Shader::SetVec3(const std::string& name, const glm::vec3& value) const {
    const GLint location = GetUniformLocation(name);
    if (location != -1) {
        glUniform3fv(location, 1, glm::value_ptr(value));
    }
}

std::string Shader::ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Unable to open shader file: " + path.string());
    }

    std::ostringstream source;
    source << file.rdbuf();
    return source.str();
}

GLuint Shader::Compile(GLenum type, const std::string& source) {
    const GLuint shader = glCreateShader(type);
    const char* sourceData = source.c_str();
    glShaderSource(shader, 1, &sourceData, nullptr);
    glCompileShader(shader);

    GLint compileStatus = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compileStatus);
    if (compileStatus == GL_FALSE) {
        GLint logLength = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<std::size_t>(logLength), '\0');
        glGetShaderInfoLog(shader, logLength, nullptr, log.data());
        glDeleteShader(shader);
        throw std::runtime_error("Shader compilation failed:\n" + log);
    }

    return shader;
}
