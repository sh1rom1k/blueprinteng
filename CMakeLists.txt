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
    src/Mesh.cpp
    src/Shader.cpp
    src/bsp_loader.cpp
    src/vtf_loader.cpp
    src/VpkArchive.cpp
    src/GameFileSystem.cpp
    src/StudioModel.cpp
    src/AiNavigation.cpp
    src/NpcSpeech.cpp
    src/GameSimulation.cpp
    src/PlayerController.cpp
    src/PhysicsWorld.cpp
    src/ShadowManager.cpp
    src/AudioSystem.cpp
    src/ui/MainMenu.cpp
    src/ui/MenuRenderer.cpp
    src/ui/ChapterCatalog.cpp
    src/ui/OpenGLUIRenderBackend.cpp
    src/ui/VguiPauseMenu.cpp
    src/ui/ValveNetGraph.cpp
    src/ui/EntityLabelRenderer.cpp
    src/ui/WavUISoundBackend.cpp
    src/ui/LoadingScreen.cpp
    src/ui/MenuBackground.cpp
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

target_compile_definitions(
    blueprinteng
    PRIVATE
        GLFW_INCLUDE_NONE
        BLUEPRINT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

if(MSVC)
    target_compile_options(blueprinteng PRIVATE /W4)
else()
    target_compile_options(blueprinteng PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(APPLE)
    target_link_libraries(blueprinteng PRIVATE "-framework Cocoa" "-framework IOKit" "-framework CoreVideo")
elseif(UNIX AND NOT APPLE)
    target_link_libraries(blueprinteng PRIVATE dl pthread m)
endif()
