#pragma once

#include <string_view>

namespace ui {

class IUISoundBackend {
public:
    virtual ~IUISoundBackend() = default;

    virtual void PlayHover() = 0;
    virtual void PlaySelect() = 0;
    virtual void PlayCancel() = 0;

    virtual void PlayRaw(std::string_view path) = 0;
};

class NullUISoundBackend final : public IUISoundBackend {
public:
    void PlayHover() override {}
    void PlaySelect() override {}
    void PlayCancel() override {}
    void PlayRaw(std::string_view /*path*/) override {}
};

} // namespace ui
