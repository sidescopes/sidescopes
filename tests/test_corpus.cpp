#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/photo_region_detector.h"

// Runs the detector against the screenshot corpus (corpus/README.md).
// Every sidecar names rectangles the detector MUST find; a failure names
// the case, so a regression is a diff away from its screenshot.

namespace sidescopes {
namespace {

struct CorpusExpectation {
    IntRect rect;
    int tolerance = 8;
    // Goals are known limitations being worked toward: tracked and
    // reported, but not failing. Promote to expect when they hold.
    bool required = true;
};

struct CorpusCase {
    std::string name;
    std::filesystem::path image;
    float pixels_per_point = 1.0f;
    std::vector<IntRect> masks;
    std::vector<CorpusExpectation> expectations;
};

std::vector<CorpusCase> LoadCases(const std::filesystem::path& directory) {
    std::vector<CorpusCase> cases;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (entry.path().extension() != ".txt") continue;
        CorpusCase item;
        item.name = entry.path().stem().string();
        item.image = entry.path();
        item.image.replace_extension(".ppm");
        if (!std::filesystem::exists(item.image)) continue;

        std::ifstream sidecar(entry.path());
        std::string line;
        while (std::getline(sidecar, line)) {
            std::istringstream tokens(line);
            std::string keyword;
            if (!(tokens >> keyword) || keyword.front() == '#') continue;
            if (keyword == "pixels_per_point") {
                tokens >> item.pixels_per_point;
            } else if (keyword == "mask") {
                IntRect rect;
                if (tokens >> rect.x >> rect.y >> rect.width >> rect.height)
                    item.masks.push_back(rect);
            } else if (keyword == "expect" || keyword == "goal") {
                CorpusExpectation expectation;
                expectation.required = keyword == "expect";
                if (tokens >> expectation.rect.x >> expectation.rect.y >> expectation.rect.width >>
                    expectation.rect.height) {
                    tokens >> expectation.tolerance;
                    item.expectations.push_back(expectation);
                }
            }
        }
        if (!item.expectations.empty()) cases.push_back(std::move(item));
    }
    return cases;
}

// Minimal P6 reader for the corpus frames; BGRA out, like capture.
bool LoadPpm(const std::filesystem::path& path, std::vector<uint8_t>& bgra, int& width,
             int& height) {
    std::ifstream input(path, std::ios::binary);
    std::string magic;
    int maxval = 0;
    input >> magic >> width >> height >> maxval;
    if (magic != "P6" || maxval != 255 || width <= 0 || height <= 0) return false;
    input.get();  // the single whitespace after the header
    std::vector<char> rgb(static_cast<std::size_t>(width) * height * 3);
    input.read(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    if (!input) return false;
    bgra.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t pixel = 0, count = static_cast<std::size_t>(width) * height; pixel < count;
         ++pixel) {
        bgra[pixel * 4 + 0] = static_cast<uint8_t>(rgb[pixel * 3 + 2]);
        bgra[pixel * 4 + 1] = static_cast<uint8_t>(rgb[pixel * 3 + 1]);
        bgra[pixel * 4 + 2] = static_cast<uint8_t>(rgb[pixel * 3 + 0]);
        bgra[pixel * 4 + 3] = 255;
    }
    return true;
}

bool Matches(const RegionCandidate& candidate, const CorpusExpectation& expectation) {
    const IntRect& a = candidate.rect;
    const IntRect& b = expectation.rect;
    const int tolerance = expectation.tolerance;
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance &&
           std::abs(a.x + a.width - (b.x + b.width)) <= tolerance &&
           std::abs(a.y + a.height - (b.y + b.height)) <= tolerance;
}

}  // namespace

TEST_CASE("Detector finds every annotated rectangle in the corpus") {
    const std::filesystem::path directory(SIDESCOPES_CORPUS_DIR);
    const std::vector<CorpusCase> cases = LoadCases(directory);
    if (cases.empty()) {
        SUCCEED("corpus is empty - see corpus/README.md");
        return;
    }

    int met = 0;
    int total = 0;
    int goals_met = 0;
    int goals_total = 0;
    for (const CorpusCase& item : cases) {
        std::vector<uint8_t> bgra;
        int width = 0;
        int height = 0;
        INFO("case " << item.name);
        REQUIRE(LoadPpm(item.image, bgra, width, height));
        const FrameView frame{bgra.data(), width * 4, width, height, ColorSpaceHint::Srgb, 1};
        const auto candidates =
            DetectPhotoRegions(frame, item.masks, item.pixels_per_point, /*max_candidates=*/24);

        for (const CorpusExpectation& expectation : item.expectations) {
            (expectation.required ? total : goals_total) += 1;
            bool found = false;
            for (const RegionCandidate& candidate : candidates) {
                if (Matches(candidate, expectation)) {
                    found = true;
                    break;
                }
            }
            std::ostringstream got;
            for (const RegionCandidate& candidate : candidates)
                got << " [" << candidate.rect.x << "," << candidate.rect.y << " "
                    << candidate.rect.width << "x" << candidate.rect.height << "]";
            INFO("expected " << expectation.rect.x << "," << expectation.rect.y << " "
                             << expectation.rect.width << "x" << expectation.rect.height << " +-"
                             << expectation.tolerance << "; candidates:" << got.str());
            if (expectation.required) {
                CHECK(found);
                if (found) ++met;
            } else if (found) {
                ++goals_met;
            }

            // The picker hands the hover to the smallest suggestion under
            // the cursor, so any smaller candidate centered inside the
            // photograph steals picks from it. None may survive.
            for (const RegionCandidate& candidate : candidates) {
                const IntRect& c = candidate.rect;
                const IntRect& e = expectation.rect;
                const bool centered =
                    c.x + c.width / 2 > e.x && c.x + c.width / 2 < e.x + e.width &&
                    c.y + c.height / 2 > e.y && c.y + c.height / 2 < e.y + e.height;
                const bool smaller = static_cast<int64_t>(c.width) * c.height <
                                     static_cast<int64_t>(e.width) * e.height;
                if (!centered || !smaller || Matches(candidate, expectation)) continue;
                INFO("hover thief " << c.x << "," << c.y << " " << c.width << "x" << c.height
                                    << " inside expected " << e.x << "," << e.y << " " << e.width
                                    << "x" << e.height);
                CHECK(false);
            }
        }
    }
    std::printf("corpus: %d/%d expectations met, %d/%d goals met, %zu cases\n", met, total,
                goals_met, goals_total, cases.size());
}

}  // namespace sidescopes
