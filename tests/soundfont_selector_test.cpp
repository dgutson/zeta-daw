#include "../soundfont_selector.hpp"

#include <gtest/gtest.h>
#include <hegel/gtest.h>

#include <string>
#include <vector>

namespace {

namespace gs = hegel::generators;

using zeta::SoundFontDefinition;
using zeta::SoundFontIndex;
using zeta::SoundFontSelector;

std::vector<SoundFontDefinition> definitionsFor(const std::vector<int>& keys) {
    std::vector<SoundFontDefinition> definitions;
    definitions.reserve(keys.size());
    for (SoundFontIndex index = 0; index < keys.size(); ++index) {
        definitions.push_back({
            .id = std::to_string(index),
            .file = std::to_string(index) + ".sf2",
            .key = keys[index],
        });
    }
    return definitions;
}

TEST(SoundFontSelectorPropertyTest, KeySelectionMatchesCatalogOrder) {
    hegel::test([](hegel::TestCase& tc) {
        const auto keys = tc.draw("keys", gs::vectors(
            gs::integers<int>({.min_value = 0, .max_value = 127}),
            {.min_size = 1, .max_size = 16, .unique = true}
        ));
        const auto selected_index = tc.draw(
            "selected_index",
            gs::integers<SoundFontIndex>({
                .min_value = 0,
                .max_value = keys.size() - 1,
            })
        );
        const auto definitions = definitionsFor(keys);
        SoundFontSelector selector{definitions};

        const auto* selected = selector.selectByKey(keys[selected_index]);
        ASSERT_EQ(selected, &definitions[selected_index])
            << "physical-key selection disagrees with catalog order";
        ASSERT_EQ(&selector.current(), &definitions[selected_index])
            << "current SoundFont disagrees with the key selection";
    });
}

TEST(SoundFontSelectorTest, NextAndDirectSelectionShareCurrentSoundFont) {
    const std::vector definitions{
        SoundFontDefinition{.id = "piano", .file = "piano.sf2", .key = 67},
        SoundFontDefinition{.id = "bass", .file = "bass.sf2", .key = 69},
        SoundFontDefinition{
            .id = "organ",
            .file = "organ.sf2",
            .key = std::nullopt,
        },
    };
    SoundFontSelector selector{definitions};

    EXPECT_EQ(selector.current().id, "piano");
    EXPECT_EQ(selector.next().id, "bass");
    const auto* selected = selector.selectByKey(67);
    ASSERT_NE(selected, nullptr);
    EXPECT_EQ(selected->id, "piano");
    EXPECT_EQ(selector.next().id, "bass");
    EXPECT_EQ(selector.next().id, "organ");
    EXPECT_EQ(selector.next().id, "piano");
}

TEST(SoundFontSelectorTest, UnmappedPhysicalKeyLeavesSelectionUnchanged) {
    const std::vector definitions{
        SoundFontDefinition{.id = "piano", .file = "piano.sf2", .key = 67},
        SoundFontDefinition{.id = "bass", .file = "bass.sf2", .key = 69},
    };
    SoundFontSelector selector{definitions};

    selector.next();
    EXPECT_EQ(selector.selectByKey(68), nullptr);
    EXPECT_EQ(selector.current().id, "bass");
}

} // namespace
