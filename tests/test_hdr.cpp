#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <string>

#include "core/analysis_worker.h"
#include "core/frame_mailbox.h"
#include "core/hdr.h"
#include "core/region_hash.h"
#include "core/scrgb.h"
#include "modules/module_registry.h"
#include "test_frame.h"

using namespace sidescopes;

namespace {
std::array<uint8_t, 8> halfPixel(uint16_t r, uint16_t g, uint16_t b)
{
    return {static_cast<uint8_t>(r),
            static_cast<uint8_t>(r >> 8),
            static_cast<uint8_t>(g),
            static_cast<uint8_t>(g >> 8),
            static_cast<uint8_t>(b),
            static_cast<uint8_t>(b >> 8),
            0,
            0x3C};
}

std::string readingOf(const ScopeInstance& scope)
{
    const auto* extension = static_cast<const SsReadingExtension*>(scope.getExtension(ReadingExtension));
    REQUIRE(extension != nullptr);
    return extension->text(scope.raw());
}
}  // namespace

TEST_CASE("PQ decoding retains the ST 2084 absolute reference points", "[hdr]")
{
    CHECK(pqNits(0.0) == 0.0);
    CHECK(pqNits(0.508078421517399) == Catch::Approx(100.0).epsilon(1e-10));
    CHECK(pqNits(0.751827096247041) == Catch::Approx(1000.0).epsilon(1e-10));
    CHECK(pqNits(1.0) == Catch::Approx(10000.0));
    CHECK(std::isnan(pqNits(-0.01)));
    CHECK(std::isnan(pqNits(1.01)));
    CHECK(std::isnan(pqNits(std::numeric_limits<double>::infinity())));
    CHECK(std::isnan(pqNits(std::numeric_limits<double>::quiet_NaN())));
}

TEST_CASE("PQ capture separates unclipped luminance from SDR display codes", "[hdr]")
{
    PqCaptureDecoder decoder;
    auto source = halfPixel(0x3C00, 0x3C00, 0x3C00);  // PQ 1 = 10000 nominal nits
    std::array<uint8_t, 4> codes{};
    float luminance = 0.0f;
    decoder.convertRow(source.data(), codes.data(), &luminance, 1);
    CHECK(luminance == Catch::Approx(100.0).epsilon(1e-5));
    CHECK(Argb2101010Pixels::read(codes.data()).r == 1023);
    CHECK(Argb2101010Pixels::read(codes.data()).g == 1023);
    CHECK(Argb2101010Pixels::read(codes.data()).b == 1023);

    source = halfPixel(0x3C00, 0, 0);
    decoder.convertRow(source.data(), codes.data(), &luminance, 1);
    // Independently derived Display P3 primary luminance (D65 RGB->XYZ Y).
    CHECK(luminance == Catch::Approx(22.8974564069749).epsilon(1e-5));
    CHECK(Argb2101010Pixels::read(codes.data()).r == 1023);
    CHECK(Argb2101010Pixels::read(codes.data()).g == 0);
    CHECK(Argb2101010Pixels::read(codes.data()).b == 0);

    source = halfPixel(0x7E00, 0, 0);
    decoder.convertRow(source.data(), codes.data(), &luminance, 1);
    CHECK(std::isnan(luminance));
}

TEST_CASE("scRGB HDR luminance follows SDR white without clipping", "[hdr]")
{
    ScrgbToDisplayCodes decoder(160.0);
    const auto white = halfPixel(0x4000, 0x4000, 0x4000);      // linear 2 at 80 nits/unit
    const auto highlight = halfPixel(0x4400, 0x4400, 0x4400);  // linear 4
    std::array<uint8_t, 4> codes{};
    float luminance = 0.0f;
    CHECK(decoder.convertRow(white.data(), codes.data(), 1, &luminance) == 0);
    CHECK(luminance == 1.0f);
    CHECK(decoder.convertRow(highlight.data(), codes.data(), 1, &luminance) == 1);
    CHECK(luminance == 2.0f);
    CHECK(Argb2101010Pixels::read(codes.data()).r == 1023);
    decoder.setSdrWhiteNits(320.0);
    decoder.convertRow(highlight.data(), codes.data(), 1, &luminance);
    CHECK(luminance == 1.0f);
    CHECK(hdrLuminance709(-1.0f, 2.0f, 0.0f) == Catch::Approx(1.217699f));
    CHECK(std::isnan(hdrLuminance709(0.0f, std::numeric_limits<float>::infinity(), 0.0f)));
}

TEST_CASE("HDR waveform reads all pixels and distinguishes absent data", "[hdr]")
{
    auto scope = builtinModules().createInstance("org.sidescopes.waveform.hdr");
    REQUIRE(scope.valid());
    std::array<uint8_t, 16> pixels{};
    std::array<float, 4> luminance{0.0f, 1.0f, 2.0f, 128.0f};
    SsFrameView frame{pixels.data(),    16,   4, 1, SS_COLOR_SPACE_SRGB, 1, SS_PIXEL_FORMAT_ARGB2101010,
                      luminance.data(), 100.0};
    REQUIRE(scope.accumulate(frame, {0, 0, 4, 1}));
    CHECK(readingOf(scope).find("Peak +7.00 stops | 50.00% above white | above plot range") == 0);
    const auto image = scope.image();
    CHECK(image.rgba[(static_cast<std::size_t>(192) * image.width + 128) * 4] > 0);  // white at center
    CHECK(image.rgba[(static_cast<std::size_t>(160) * image.width + 256) * 4] > 0);  // +1 stop
    CHECK(scope.markers(SsColor{255, 255, 255}).empty());
    REQUIRE(scope.accumulate(frame, {1, 0, 1, 1}));
    CHECK(readingOf(scope) == "Peak +0.00 stops | 0.00% above white");
    luminance[1] = 0.9999f;
    REQUIRE(scope.accumulate(frame, {1, 0, 1, 1}));
    CHECK(readingOf(scope) == "Peak +0.00 stops | 0.00% above white");
    luminance[1] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(scope.accumulate(frame, {1, 0, 1, 1}));
    CHECK(readingOf(scope) == "HDR luminance: no valid pixels");
    frame.hdr_luminance = nullptr;
    frame.hdr_white_nits = 0;
    REQUIRE(scope.accumulate(frame, {0, 0, 4, 1}));
    CHECK(readingOf(scope) == "HDR luminance unavailable for this capture");
    for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(image.width) * image.height; ++pixel) {
        REQUIRE(scope.image().rgba[pixel * 4] == 0);
    }
}

TEST_CASE("HDR change detection sees highlights on every row and respects masking", "[hdr]")
{
    test::TestFrame pixels(4, 4);
    std::array<float, 16> luminance{};
    FrameView frame = pixels.view();
    frame.hdrLuminance = luminance.data();
    frame.hdrWhiteNits = 100.0;
    const IntRect region{0, 0, 4, 4};
    const uint64_t before = hashRegion(frame, region);
    luminance[5] = 2.0f;
    CHECK(hashRegion(frame, region) != before);
    const IntRect mask{1, 1, 1, 1};
    const uint64_t masked = hashRegion(frame, region, mask);
    luminance[5] = 4.0f;
    CHECK(hashRegion(frame, region, mask) == masked);
    frame.hdrWhiteNits = 200.0;
    CHECK(hashRegion(frame, region, mask) != masked);
}

TEST_CASE("HDR frame storage follows recycled frames and is released for SDR", "[hdr]")
{
    FrameBuffer producer;
    producer.width = 2;
    producer.height = 1;
    producer.strideBytes = 8;
    producer.sizeTo(8);
    producer.sizeHdrTo(2, 100.0);
    producer.hdrLuminance[1] = 4.0f;
    const FrameView hdrView = producer.view();
    REQUIRE(hdrView.hdrLuminance != nullptr);
    CHECK(hdrView.hdrLuminance[1] == 4.0f);
    CHECK(producer.view().hdrWhiteNits == 100.0);
    producer.sizeHdrTo(0, 0);
    CHECK(producer.hdrLuminance.capacity() == 0);
    CHECK(producer.view().hdrLuminance == nullptr);
    CHECK(producer.view().hdrWhiteNits == 0.0);
}

TEST_CASE("HDR worker publishes highlight changes and replaces stale readings", "[hdr]")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    const std::string id = "org.sidescopes.waveform.hdr";
    settings.enabledScopes = {id};
    settings.region = RegionOfInterest{};
    worker.updateSettings(settings);
    worker.startInline();
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    for (int sequence = 1; sequence <= 2; ++sequence) {
        auto frame = test::makeSolidFrameBuffer(4, 4, Color{255, 255, 255}, static_cast<uint64_t>(sequence));
        frame.sizeHdrTo(16, 100.0);
        std::fill(frame.hdrLuminance.begin(), frame.hdrLuminance.end(), 1.0f);
        frame.hdrLuminance[5] = sequence == 1 ? 2.0f : 4.0f;
        (void)mailbox.publish(std::move(frame));
        worker.pump();
        REQUIRE(worker.fetchOutput(seen, output));
        CHECK(output.frameSequence == static_cast<uint64_t>(sequence));
        CHECK(output.readings.at(id).find(sequence == 1 ? "+1.00 stops" : "+2.00 stops") != std::string::npos);
        CHECK(output.readings.at(id).find("6.25% above white") != std::string::npos);
    }
    settings.region.reset();
    worker.updateSettings(settings);
    worker.pump();
    CHECK_FALSE(worker.fetchOutput(seen, output));  // the host hides readings without a region
    settings.region = RegionOfInterest{};
    worker.updateSettings(settings);
    (void)mailbox.publish(test::makeSolidFrameBuffer(4, 4, Color{255, 255, 255}, 3));
    worker.pump();
    REQUIRE(worker.fetchOutput(seen, output));
    CHECK(output.readings.at(id) == "HDR luminance unavailable for this capture");
}
