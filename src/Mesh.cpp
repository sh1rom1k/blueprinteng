#include "Mesh.hpp"

Mesh::Mesh(const std::vector<float>& vertices, const std::vector<unsigned int>& indices)
    : indexCount_(static_cast<GLsizei>(indices.size())) {
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);

    glBindVertexArray(vao_);

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
        vertices.data(),
        GL_STATIC_DRAW
    );

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(
        GL_ELEMENT_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(indices.size() * sizeof(unsigned int)),
        indices.data(),
        GL_STATIC_DRAW
    );

    constexpr GLsizei kVertexStride = static_cast<GLsizei>(Mesh::kFloatsPerVertex * sizeof(float));

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kVertexStride, nullptr);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(
        1,
        3,
        GL_FLOAT,
        GL_FALSE,
        kVertexStride,
        reinterpret_cast<void*>(3 * sizeof(float))
    );
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(
        2,
        2,
        GL_FLOAT,
        GL_FALSE,
        kVertexStride,
        reinterpret_cast<void*>(6 * sizeof(float))
    );
    glEnableVertexAttribArray(2);

    glVertexAttribPointer(
        3,
        2,
        GL_FLOAT,
        GL_FALSE,
        kVertexStride,
        reinterpret_cast<void*>(8 * sizeof(float))
    );
    glEnableVertexAttribArray(3);

    glBindVertexArray(0);
}

Mesh::~Mesh() {
    glDeleteBuffers(1, &ebo_);
    glDeleteBuffers(1, &vbo_);
    glDeleteVertexArrays(1, &vao_);
}

void Mesh::Draw() const {
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
}
