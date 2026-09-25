#pragma once

#include <cstddef>
#include <vector>

#include <glad/gl.h>

class Mesh {
public:
    static constexpr std::size_t kFloatsPerVertex = 10;

    Mesh(const std::vector<float>& vertices, const std::vector<unsigned int>& indices);
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    void Draw() const;

private:
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ebo_ = 0;
    GLsizei indexCount_ = 0;
};
