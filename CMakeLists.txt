cmake_minimum_required(VERSION 3.20)

project(BlueprintOpenGL
    VERSION 0.1.0
    DESCRIPTION "Minimal OpenGL 4.5 Core application using GLFW and GLAD"
    LANGUAGES C CXX
)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# Keep every static library on the same CRT. Jolt otherwise defaults to /MT while
# CMake's other dependencies use /MD, which fails the link with LNK2038.
# Jolt also turns warnings into errors and enables link-time optimization by default;
# both break a normal application link on MSVC and MinGW.
if(WIN32)
    set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
    set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
    set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG v2.0.8
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    jolt
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.2.0
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.22
    GIT_SHALLOW TRUE
)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JPH_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(JPH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(JPH_BUILD_SAMPLES_APP OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glfw glm)

FetchContent_GetProperties(glad)
if(NOT glad_POPULATED)
    FetchContent_Populate(glad)
endif()

add_subdirectory(${glad_SOURCE_DIR}/cmake glad_cmake)
glad_add_library(glad_gl_core_45 STATIC API gl:core=4.5)

FetchContent_GetProperties(jolt)
if(NOT jolt_POPULATED)
    FetchContent_Populate(jolt)
endif()
add_subdirectory(${jolt_SOURCE_DIR}/Build jolt_build)

FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
endif()

FetchContent_GetProperties(miniaudio)
if(NOT miniaudio_POPULATED)
    FetchContent_Populate(miniaudio)
endif()

add_executable(
    blueprinteng
    src/main.cpp
    src/Platform.cpp
    src/Mesh.cpp
    src/Shader.cpp
    src/bsp_loader.cpp
    src/vtf_loader.cpp
    src/PlayerController.cpp
    src/PhysicsWorld.cpp
    src/ShadowManager.cpp
    src/AudioSystem.cpp
    src/ui/MainMenu.cpp
    src/ui/MenuRenderer.cpp
    src/ui/OpenGLUIRenderBackend.cpp
    src/ui/VguiPauseMenu.cpp
    src/ui/ValveNetGraph.cpp
    src/ui/EntityLabelRenderer.cpp
    src/ui/WavUISoundBackend.cpp
)
target_include_directories(
    blueprinteng
    PRIVATE
        include
        ${jolt_SOURCE_DIR}
        ${stb_SOURCE_DIR}
        ${miniaudio_SOURCE_DIR}
)
target_link_libraries(blueprinteng PRIVATE glfw glad_gl_core_45 glm::glm Jolt)

file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" BLUEPRINT_SOURCE_DIR_PATH)
target_compile_definitions(
    blueprinteng
    PRIVATE
        GLFW_INCLUDE_NONE
        BLUEPRINT_SOURCE_DIR="${BLUEPRINT_SOURCE_DIR_PATH}"
)

if(MSVC)
    target_compile_options(blueprinteng PRIVATE /W4 /bigobj /utf-8 /Zc:__cplusplus /permissive-)
    target_compile_definitions(
        blueprinteng
        PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            _CRT_SECURE_NO_WARNINGS
    )
elseif(WIN32)
    target_compile_options(blueprinteng PRIVATE -Wall -Wextra -Wpedantic -Wa,-mbig-obj)
    target_compile_definitions(blueprinteng PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(blueprinteng PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(blueprinteng PRIVATE psapi)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT blueprinteng)
    set_target_properties(blueprinteng PROPERTIES
        WIN32_EXECUTABLE FALSE
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:blueprinteng>"
    )
    foreach(content_dir shaders maps sound textures assets)
        add_custom_command(
            TARGET blueprinteng POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/${content_dir}"
                "$<TARGET_FILE_DIR:blueprinteng>/${content_dir}"
            VERBATIM
        )
    endforeach()
endif()

if(APPLE)
    target_link_libraries(blueprinteng PRIVATE "-framework Cocoa" "-framework IOKit" "-framework CoreVideo")
elseif(UNIX AND NOT APPLE)
    target_link_libraries(blueprinteng PRIVATE dl pthread m)
endif()
