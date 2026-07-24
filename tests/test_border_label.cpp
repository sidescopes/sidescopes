#include <catch2/catch_test_macros.hpp>
#include <string>

#include "app/border_label.h"

namespace sidescopes {

TEST_CASE("A border label wears the title, or the fallback when there is none")
{
    CHECK(borderLabelFrom("DSC_0042.NEF", "Display") == "DSC_0042.NEF");
    // A window with no title (or none this application may read) still gets a
    // label, so the border never wears an empty band.
    CHECK(borderLabelFrom("", "Display") == "Display");
}

TEST_CASE("A pathological title is cut short with an ellipsis")
{
    // Ninety-six bytes pass through whole; the next one is cut.
    const std::string atLimit(96, 'a');
    CHECK(borderLabelFrom(atLimit, "Display") == atLimit);

    const std::string overLimit(200, 'a');
    const std::string cut = borderLabelFrom(overLimit, "Display");
    CHECK(cut == std::string(96, 'a') + "...");
}

TEST_CASE("A cut title never breaks a character in half")
{
    // Ninety-five ASCII bytes, then a three-byte character straddling the cut:
    // the whole character goes rather than a byte of it, which would leave the
    // label unrenderable.
    const std::string label = std::string(95, 'a') + "\xE2\x9C\x93" + std::string(50, 'b');
    const std::string cut = borderLabelFrom(label, "Display");

    CHECK(cut == std::string(95, 'a') + "...");

    // A character that ends exactly on the limit is kept whole.
    const std::string aligned = std::string(93, 'a') + "\xE2\x9C\x93" + std::string(50, 'b');
    CHECK(borderLabelFrom(aligned, "Display") == std::string(93, 'a') + "\xE2\x9C\x93" + "...");
}

}  // namespace sidescopes
