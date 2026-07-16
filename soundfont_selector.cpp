#include "soundfont_selector.hpp"

namespace zeta {

SoundFontSelector::SoundFontSelector(
    std::span<const SoundFontDefinition> soundfonts
) noexcept : soundfonts_(soundfonts) {
    for (SoundFontIndex index = 0; index < soundfonts_.size(); ++index) {
        const auto key = soundfonts_[index].key;
        if (key) {
            keyed_soundfonts_[static_cast<SoundFontIndex>(key.value())] = index;
        }
    }
}

const SoundFontDefinition& SoundFontSelector::current() const {
    return soundfonts_[current_];
}

const SoundFontDefinition& SoundFontSelector::next() {
    current_ = (current_ + 1) % soundfonts_.size();
    return current();
}

const SoundFontDefinition* SoundFontSelector::selectByKey(int key) {
    const auto selected = keyed_soundfonts_[static_cast<SoundFontIndex>(key)];
    if (!selected) {
        return nullptr;
    }

    current_ = selected.value();
    return &current();
}

} // namespace zeta
