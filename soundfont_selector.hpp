#pragma once

#include "configuration.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace zeta {

using SoundFontIndex = std::size_t;

class SoundFontSelector final {
public:
    explicit SoundFontSelector(
        std::span<const SoundFontDefinition> soundfonts
    ) noexcept;

    const SoundFontDefinition& current() const;
    const SoundFontDefinition& next();
    const SoundFontDefinition* selectByKey(int key);

private:
    static constexpr SoundFontIndex midi_key_count = 128;

    std::span<const SoundFontDefinition> soundfonts_;
    SoundFontIndex current_{};
    std::array<std::optional<SoundFontIndex>, midi_key_count> keyed_soundfonts_{};
};

} // namespace zeta
