/* SkParagraph owns shaping and layout; C copies retain fonts and expose UTF-16 offsets. */
#include "cfsk_internal.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontMetrics.h"
#include "modules/skparagraph/include/ParagraphBuilder.h"
#include "modules/skparagraph/include/TypefaceFontProvider.h"
#include "modules/skunicode/include/SkUnicode_icu.h"
#include "src/core/SkUTF.h"
#include <algorithm>

namespace para = skia::textlayout;
struct cfsk_font_provider { sk_sp<para::TypefaceFontProvider> value; };
struct cfsk_font_collection { sk_sp<para::FontCollection> value; };
struct cfsk_paragraph_builder { std::unique_ptr<para::ParagraphBuilder> value; };
struct ParagraphRun {
    cfsk_shaped_run metadata{};
    sk_sp<SkTypeface> face;
    std::vector<uint16_t> glyphs;
    std::vector<cfsk_point> positions;
    std::vector<uint32_t> clusters;
};
struct cfsk_paragraph {
    std::unique_ptr<para::Paragraph> value;
    std::vector<uint32_t> utf16;
    std::vector<SkUnicode::BidiRegion> bidi;
    std::vector<ParagraphRun> runs;
    bool laid_out = false;
};

namespace {
SkString string(cfsk_string text) { return SkString(text.data ? text.data : "", text.length); }
bool utf8(cfsk_string text) {
    return cfsk::valid(text) && text.length <= INT_MAX &&
        (!text.length || SkUTF::CountUTF8(text.data, text.length) >= 0);
}

void collect_runs(cfsk_paragraph* paragraph) {
    paragraph->runs.clear();
    size_t glyph_start = 0;
    size_t line_start = 0;
    paragraph->value->visit([&](int line, const para::Paragraph::VisitorInfo* info) {
        if (!info) {
            float top = std::numeric_limits<float>::max();
            float bottom = std::numeric_limits<float>::lowest();
            for (size_t i = line_start; i < paragraph->runs.size(); ++i) {
                top = std::min(top, paragraph->runs[i].metadata.line_top);
                bottom = std::max(bottom, paragraph->runs[i].metadata.line_bottom);
            }
            for (size_t i = line_start; i < paragraph->runs.size(); ++i) {
                paragraph->runs[i].metadata.line_top = top;
                paragraph->runs[i].metadata.line_bottom = bottom;
            }
            line_start = paragraph->runs.size();
            return;
        }
        if (info->count == 0) return;
        ParagraphRun run;
        auto& meta = run.metadata;
        meta.line_index = line;
        meta.glyph_start = glyph_start;
        meta.glyph_count = info->count;
        meta.font_size = info->font.getSize();
        meta.fake_bold = info->font.isEmbolden();
        meta.fake_italic = info->font.getSkewX() != 0;
        meta.flags = info->flags;
        run.face = info->font.refTypeface();
        SkFontMetrics metrics;
        info->font.getMetrics(&metrics);
        meta.line_top = info->origin.y() + metrics.fAscent;
        meta.line_bottom = info->origin.y() + metrics.fDescent;
        meta.line_baseline = info->origin.y();
        run.glyphs.assign(info->glyphs, info->glyphs + info->count);
        meta.text_start = SIZE_MAX;
        for (int i = 0; i <= info->count; ++i) {
            // The visitor includes a terminal cluster, including for RTL runs.
            size_t offset = std::min<size_t>(info->utf8Starts[i], paragraph->utf16.size() - 1);
            uint32_t cluster = paragraph->utf16[offset];
            run.clusters.push_back(cluster);
            meta.text_start = std::min<size_t>(meta.text_start, cluster);
            meta.text_end = std::max<size_t>(meta.text_end, cluster);
            if (i < info->count) {
                SkPoint point = info->positions[i] + info->origin;
                run.positions.push_back({point.x(), point.y()});
            }
        }
        run.positions.push_back({info->advanceX, run.positions.back().y});
        for (const auto& region : paragraph->bidi) {
            if (region.start <= info->utf8Starts[0] && info->utf8Starts[0] < region.end) {
                meta.bidi_level = region.level;
                break;
            }
        }
        glyph_start += info->count;
        paragraph->runs.push_back(std::move(run));
    });
}
}

cfsk_status cfsk_font_provider_make(cfsk_font_provider** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    return cfsk::resource(output, sk_make_sp<para::TypefaceFontProvider>());
}

cfsk_status cfsk_font_provider_register_bytes(cfsk_font_provider* provider, cfsk_bytes bytes, cfsk_string alias) {
    CFSK_BEGIN;
    CFSK_REQUIRE(provider && cfsk::valid(bytes) && utf8(alias));
    cfsk_typeface* face = nullptr;
    cfsk_status status = cfsk_typeface_from_data(bytes, 0, &face);
    if (status != CFSK_OK) return status;
    status = cfsk_font_provider_register_typeface(provider, face, alias);
    cfsk_typeface_destroy(face);
    return status;
}

cfsk_status cfsk_font_provider_register_typeface(cfsk_font_provider* provider, const cfsk_typeface* face, cfsk_string alias) {
    CFSK_BEGIN;
    CFSK_REQUIRE(provider && face && utf8(alias));
    if (alias.length) provider->value->registerTypeface(face->value, string(alias));
    else provider->value->registerTypeface(face->value);
    return CFSK_OK;
}

cfsk_status cfsk_font_collection_make(cfsk_font_collection** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    auto collection = sk_make_sp<para::FontCollection>();
    collection->disableFontFallback();
    return cfsk::resource(output, std::move(collection));
}

cfsk_status cfsk_font_collection_set_provider(cfsk_font_collection* collection, cfsk_font_provider* provider) {
    CFSK_BEGIN;
    CFSK_REQUIRE(collection && provider);
    collection->value->setDefaultFontManager(provider->value);
    // Disabling fallback removes the default manager from SkParagraph's lookup
    // order. Explicit byte-backed assets must remain searchable in that mode.
    collection->value->setAssetFontManager(provider->value);
    collection->value->clearCaches();
    return CFSK_OK;
}

cfsk_status cfsk_paragraph_builder_make(const cfsk_paragraph_style* style, cfsk_font_collection* fonts, cfsk_paragraph_builder** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(style && fonts && (!style->family_count || style->families));
    CFSK_REQUIRE(style->weight >= 0 && style->weight <= 1000 && style->width >= 1 && style->width <= 9);
    CFSK_REQUIRE(style->slant <= 2 && style->text_align <= 5 && style->text_direction <= 1);
    CFSK_REQUIRE(cfsk::finite(style->font_size) && style->font_size >= 0);
    CFSK_REQUIRE(cfsk::finite(style->letter_spacing) && cfsk::finite(style->word_spacing));
    CFSK_REQUIRE(cfsk::finite(style->height_multiplier) && style->height_multiplier >= 0);
    CFSK_REQUIRE(style->height_override <= 1 && style->half_leading <= 1 && cfsk::finite(style->color));
    CFSK_REQUIRE(utf8(style->locale) && utf8(style->ellipsis));
    CFSK_REQUIRE((!style->feature_count || style->features) && (!style->variation_count || style->variations));
    CFSK_REQUIRE(style->variation_count <= INT_MAX);
    para::TextStyle text;
    std::vector<SkString> families;
    for (size_t i = 0; i < style->family_count; ++i) {
        CFSK_REQUIRE(utf8(style->families[i]));
        families.push_back(string(style->families[i]));
    }
    text.setFontFamilies(std::move(families));
    text.setFontStyle(SkFontStyle(style->weight, style->width, static_cast<SkFontStyle::Slant>(style->slant)));
    text.setFontSize(style->font_size);
    text.setLetterSpacing(style->letter_spacing);
    text.setWordSpacing(style->word_spacing);
    text.setHeight(style->height_multiplier);
    text.setHeightOverride(style->height_override);
    text.setHalfLeading(style->half_leading);
    text.setLocale(string(style->locale));
    text.setColor(cfsk::color(style->color).toSkColor());
    for (size_t i = 0; i < style->feature_count; ++i) {
        CFSK_REQUIRE(utf8(style->features[i].tag) && style->features[i].tag.length == 4);
        text.addFontFeature(string(style->features[i].tag), style->features[i].value);
    }
    std::vector<SkFontArguments::VariationPosition::Coordinate> coordinates;
    for (size_t i = 0; i < style->variation_count; ++i) {
        CFSK_REQUIRE(cfsk::finite(style->variations[i].value));
        coordinates.push_back({style->variations[i].tag, style->variations[i].value});
    }
    if (!coordinates.empty()) {
        SkFontArguments arguments;
        arguments.setVariationDesignPosition({coordinates.data(), static_cast<int>(coordinates.size())});
        text.setFontArguments(arguments);
    }
    para::ParagraphStyle paragraph;
    paragraph.setTextStyle(text);
    paragraph.setTextAlign(static_cast<para::TextAlign>(style->text_align));
    paragraph.setTextDirection(static_cast<para::TextDirection>(style->text_direction));
    paragraph.setMaxLines(style->max_lines);
    paragraph.setEllipsis(string(style->ellipsis));
    auto unicode = SkUnicodes::ICU::Make();
    if (!unicode) return cfsk::fail(CFSK_FAILED, "ICU backend unavailable");
    auto builder = para::ParagraphBuilder::make(paragraph, fonts->value, std::move(unicode));
    if (!builder) return cfsk::fail(CFSK_FAILED, "paragraph builder factory failed");
    return cfsk::owned(output, std::move(builder));
}

cfsk_status cfsk_paragraph_builder_add_text(cfsk_paragraph_builder* builder, cfsk_string text) {
    CFSK_BEGIN;
    CFSK_REQUIRE(builder && utf8(text));
    if (text.length) builder->value->addText(text.data, text.length);
    return CFSK_OK;
}

cfsk_status cfsk_paragraph_builder_build(cfsk_paragraph_builder* builder, cfsk_paragraph** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(builder);
    auto text = builder->value->getText();
    const char* data = text.size() ? text.data() : "";
    CFSK_REQUIRE(text.size() <= INT_MAX);
    auto result = std::unique_ptr<cfsk_paragraph>(new (std::nothrow) cfsk_paragraph);
    if (!result) return cfsk::fail(CFSK_OUT_OF_MEMORY, "paragraph wrapper allocation failed");
    SkUnicode::extractUtfConversionMapping(SkSpan<const char>(data, text.size()),
        [](size_t) {}, [&](size_t index) { result->utf16.push_back(static_cast<uint32_t>(index)); });
    auto direction = builder->value->getParagraphStyle().getTextDirection() == para::TextDirection::kLtr
        ? SkUnicode::TextDirection::kLTR : SkUnicode::TextDirection::kRTL;
    auto unicode = SkUnicodes::ICU::Make();
    if (!unicode || !unicode->getBidiRegions(data, static_cast<int>(text.size()), direction, &result->bidi)) {
        return cfsk::fail(CFSK_FAILED, "paragraph bidi analysis failed");
    }
    result->value = builder->value->Build();
    if (!result->value) return cfsk::fail(CFSK_FAILED, "paragraph build failed");
    *output = result.release();
    return CFSK_OK;
}

cfsk_status cfsk_paragraph_layout(cfsk_paragraph* paragraph, float width) {
    CFSK_BEGIN;
    CFSK_REQUIRE(paragraph && cfsk::finite(width) && width >= 0);
    paragraph->value->layout(width);
    collect_runs(paragraph);
    paragraph->laid_out = true;
    return CFSK_OK;
}

cfsk_status cfsk_paragraph_longest_line(const cfsk_paragraph* paragraph, float* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(paragraph && paragraph->laid_out && output);
    *output = paragraph->value->getLongestLine();
    return CFSK_OK;
}

cfsk_status cfsk_paragraph_max_intrinsic_width(const cfsk_paragraph* paragraph, float* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(paragraph && paragraph->laid_out && output);
    *output = paragraph->value->getMaxIntrinsicWidth();
    return CFSK_OK;
}

cfsk_status cfsk_paragraph_line_metrics(const cfsk_paragraph* paragraph, cfsk_line_metrics* output, size_t capacity, size_t* required) {
    CFSK_BEGIN;
    CFSK_REQUIRE(paragraph && paragraph->laid_out && required && (output || !capacity));
    std::vector<para::LineMetrics> lines;
    paragraph->value->getLineMetrics(lines);
    *required = lines.size();
    if (!output && !capacity) return CFSK_OK;
    if (capacity < lines.size()) return cfsk::fail(CFSK_BUFFER_TOO_SMALL, "line metrics buffer too small");
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& m = lines[i];
        output[i] = {m.fStartIndex, m.fEndIndex, m.fEndExcludingWhitespaces, m.fEndIncludingNewline,
            static_cast<uint8_t>(m.fHardBreak), m.fAscent, m.fDescent, m.fUnscaledAscent,
            m.fHeight, m.fWidth, m.fLeft, m.fBaseline, m.fLineNumber};
    }
    return CFSK_OK;
}

cfsk_status cfsk_paragraph_shaped_runs(const cfsk_paragraph* paragraph, cfsk_shaped_run* output, size_t capacity, size_t* required) {
    CFSK_BEGIN;
    CFSK_REQUIRE(paragraph && paragraph->laid_out && required && (output || !capacity));
    *required = paragraph->runs.size();
    if (!output && !capacity) return CFSK_OK;
    if (capacity < *required) return cfsk::fail(CFSK_BUFFER_TOO_SMALL, "shaped run buffer too small");
    for (size_t i = 0; i < *required; ++i) {
        output[i] = paragraph->runs[i].metadata;
        cfsk_status status = cfsk::owned(&output[i].typeface, paragraph->runs[i].face);
        if (status != CFSK_OK) {
            for (size_t j = 0; j < i; ++j) {
                cfsk_typeface_destroy(output[j].typeface);
                output[j].typeface = nullptr;
            }
            return status;
        }
    }
    return CFSK_OK;
}

cfsk_status cfsk_paragraph_run_glyphs(const cfsk_paragraph* paragraph, size_t run_index, uint16_t* glyphs, cfsk_point* positions, uint32_t* clusters, size_t capacity, size_t* required) {
    CFSK_BEGIN;
    CFSK_REQUIRE(paragraph && paragraph->laid_out && run_index < paragraph->runs.size() && required);
    const auto& run = paragraph->runs[run_index];
    *required = run.glyphs.size();
    if (!glyphs && !positions && !clusters && !capacity) return CFSK_OK;
    CFSK_REQUIRE(glyphs && positions && clusters);
    if (capacity < *required) return cfsk::fail(CFSK_BUFFER_TOO_SMALL, "glyph buffer too small");
    std::copy(run.glyphs.begin(), run.glyphs.end(), glyphs);
    std::copy(run.positions.begin(), run.positions.end(), positions);
    std::copy(run.clusters.begin(), run.clusters.end(), clusters);
    return CFSK_OK;
}

void cfsk_font_provider_destroy(cfsk_font_provider* value) { delete value; }
void cfsk_font_collection_destroy(cfsk_font_collection* value) { delete value; }
void cfsk_paragraph_builder_destroy(cfsk_paragraph_builder* value) { delete value; }
void cfsk_paragraph_destroy(cfsk_paragraph* value) { delete value; }
