#pragma once

#include <cstddef>
#include <vector>

#include <glad/gl.h>

class Mesh {
public:
    static constexpr std::size_t kFloatsPerVertex = 11;

    Mesh(const std::vector<float>& vertices, const std::vector<unsigned int>& indices);
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    void Draw() const;
    void MultiDraw(const GLsizei* counts, const void* const* offsets, GLsizei drawCount) const;
    void UpdateVertices(const std::vector<float>& vertices);

private:
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ebo_ = 0;
    GLsizei indexCount_ = 0;
    std::size_t vertexFloats_ = 0;
};
