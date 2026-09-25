# Инструкции для GitHub Copilot (Source OpenGL Engine)

- **Язык проекта:** C++20 (или Rust).
- **Графический API:** OpenGL 3.3+ / 4.5 Core Profile (используй glad/GLFW или glutin/gl).
- **Сборка проекта (C++):** `cmake -B build && cmake --build build`
- **Сборка и тест (Rust):** `cargo build && cargo test`
- **Правила кодирования:**
  - Не используй тяжелые сторонние движки (Godot, Unreal, Unity).
  - Пиши чистый OpenGL код с шейдерами (GLSL).
  - Всегда проверяй ошибки компиляции после внесения изменений.