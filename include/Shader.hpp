#pragma once

#include <string>
#include <unordered_map>

#include <glad/gl.h>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

class Shader {
public:
    Shader(const std::string& vertexPath, const std::string& fragmentPath, const std::string& geometryPath = "");
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    GLuint Program() const { return program_; }
    void Use() const;
    void SetMat4(const std::string& name, const glm::mat4& matrix) const;
    void SetMat4Array(const std::string& name, const glm::mat4* matrices, GLsizei count) const;
    void SetInt(const std::string& name, int value) const;
    void SetBool(const std::string& name, bool value) const;
    void SetFloat(const std::string& name, float value) const;
    void SetVec3(const std::string& name, const glm::vec3& value) const;

    // Fast light uniform uploads avoiding per-frame string formatting and map lookups
    void CacheLightUniformLocations() const;
    void SetPointLightUniform(
        int index,
        const glm::vec3& position,
        const glm::vec3& color,
        float intensity,
        float constant,
        float linear,
        float quadratic
    ) const;
    void SetSpotlightUniform(
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
    ) const;

private:
    GLint GetUniformLocation(const std::string& name) const;
    static std::string ReadFile(const std::string& path);
    static GLuint Compile(GLenum type, const std::string& source);

    struct PointLightLocs {
        GLint pos = -1;
        GLint color = -1;
        GLint intensity = -1;
        GLint constant = -1;
        GLint linear = -1;
        GLint quadratic = -1;
    };
    struct SpotLightLocs {
        GLint pos = -1;
        GLint dir = -1;
        GLint color = -1;
        GLint intensity = -1;
        GLint innerCutOff = -1;
        GLint outerCutOff = -1;
        GLint constant = -1;
        GLint linear = -1;
        GLint quadratic = -1;
        GLint enabled = -1;
    };

    GLuint program_ = 0;
    mutable std::unordered_map<std::string, GLint> uniformLocations_;
    mutable bool lightUniformsCached_ = false;
    mutable PointLightLocs pointLightLocs_[32];
    mutable SpotLightLocs spotLightLocs_{};
};
