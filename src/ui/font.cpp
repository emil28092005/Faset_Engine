#include <faset/core/io.hpp>
#include <faset/ui/ui.hpp>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <cmath>
#include <fstream>
#include <hb-ft.h>
#include <hb.h>
#include <limits>
#include <map>
#include <stdexcept>

namespace faset::ui {
namespace {
void quad(render::Snapshot& snapshot, render::Quad q, const Rect& clip) {
    const Rect area{q.x, q.y, q.width, q.height};
    const auto visible = area.intersection(clip);
    if (visible.width <= 0 || visible.height <= 0 || q.width <= 0 || q.height <= 0)
        return;
    const float u0 = q.uv_rect[0], v0 = q.uv_rect[1], du = q.uv_rect[2] - u0,
                dv = q.uv_rect[3] - v0;
    q.uv_rect = {u0 + (visible.x - q.x) / q.width * du, v0 + (visible.y - q.y) / q.height * dv,
                 u0 + (visible.x + visible.width - q.x) / q.width * du,
                 v0 + (visible.y + visible.height - q.y) / q.height * dv};
    q.x = visible.x;
    q.y = visible.y;
    q.width = visible.width;
    q.height = visible.height;
    snapshot.ui_quads.push_back(std::move(q));
}
} // namespace
struct FontAtlas::Impl {
    FT_Library library{};
    FT_Face face{};
    hb_font_t* font{};
    std::vector<unsigned char> font_bytes;
    std::shared_ptr<render::Texture> atlas = std::make_shared<render::Texture>();
    struct Glyph {
        unsigned x, y, width, height;
        int left, top;
    };
    std::map<std::pair<unsigned, unsigned>, Glyph> glyphs;
    unsigned x = 2, y = 2, row_height = 0, size = 0;
    explicit Impl(const std::filesystem::path& path) {
        // FreeType's Windows path backend uses CreateFileA. Read a native path
        // ourselves and retain its bytes until hb_font/FT_Face are destroyed.
        std::ifstream in(native_io_path(path), std::ios::binary | std::ios::ate);
        if (!in)
            throw std::runtime_error("Cannot open UI font: " + path_to_utf8(path));
        auto length = in.tellg();
        if (length <= 0)
            throw std::runtime_error("Empty UI font");
        if (length > std::numeric_limits<FT_Long>::max())
            throw std::runtime_error("UI font exceeds FreeType's supported buffer size");
        font_bytes.resize(static_cast<std::size_t>(length));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(font_bytes.data()), length);
        if (!in)
            throw std::runtime_error("Cannot read UI font");
        if (FT_Init_FreeType(&library))
            throw std::runtime_error("FreeType initialization failed");
        if (FT_New_Memory_Face(library, font_bytes.data(), static_cast<FT_Long>(font_bytes.size()),
                               0, &face)) {
            FT_Done_FreeType(library);
            library = nullptr;
            throw std::runtime_error("Cannot load UI font face");
        }
        font = hb_ft_font_create_referenced(face);
        hb_ft_font_set_load_flags(font, FT_LOAD_DEFAULT);
        atlas->width = atlas->height = 2048;
        atlas->rgba.resize(2048 * 2048 * 4, 0);
        atlas->revision = 1;
        atlas->srgb = false;
    }
    ~Impl() {
        if (font)
            hb_font_destroy(font);
        if (face)
            FT_Done_Face(face);
        if (library)
            FT_Done_FreeType(library);
    }
    void set_size(float pixels) {
        const auto wanted = static_cast<unsigned>(std::clamp(std::round(pixels), 6.f, 128.f));
        if (wanted != size) {
            size = wanted;
            if (FT_Set_Pixel_Sizes(face, 0, size))
                throw std::runtime_error("Invalid font size");
            hb_ft_font_changed(font);
        }
    }
    Glyph glyph(unsigned index) {
        const auto key = std::make_pair(size, index);
        if (auto found = glyphs.find(key); found != glyphs.end())
            return found->second;
        if (FT_Load_Glyph(face, index, FT_LOAD_DEFAULT) ||
            FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL))
            throw std::runtime_error("Cannot rasterize UI glyph");
        const auto& bitmap = face->glyph->bitmap;
        if (x + bitmap.width + 2 > atlas->width) {
            x = 2;
            y += row_height + 2;
            row_height = 0;
        }
        if (y + bitmap.rows + 2 > atlas->height)
            throw std::runtime_error("UI font atlas exhausted");
        Glyph result{
            x, y, bitmap.width, bitmap.rows, face->glyph->bitmap_left, face->glyph->bitmap_top};
        for (unsigned py = 0; py < bitmap.rows; ++py) {
            const auto* row = bitmap.buffer + (bitmap.pitch >= 0 ? py : bitmap.rows - 1 - py) *
                                                  std::abs(bitmap.pitch);
            for (unsigned px = 0; px < bitmap.width; ++px) {
                const auto offset = ((y + py) * atlas->width + x + px) * 4;
                atlas->rgba[offset] = atlas->rgba[offset + 1] = atlas->rgba[offset + 2] = 255;
                atlas->rgba[offset + 3] = row[px];
            }
        }
        x += bitmap.width + 2;
        row_height = std::max(row_height, bitmap.rows);
        ++atlas->revision;
        glyphs.emplace(key, result);
        return result;
    }
    hb_buffer_t* shape(std::string_view text, float pixels) {
        set_size(pixels);
        auto* buffer = hb_buffer_create();
        hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), 0,
                           static_cast<int>(text.size()));
        hb_buffer_guess_segment_properties(buffer);
        hb_shape(font, buffer, nullptr, 0);
        return buffer;
    }
};
FontAtlas::FontAtlas(const std::filesystem::path& path) : impl_(std::make_unique<Impl>(path)) {}
FontAtlas::~FontAtlas() = default;
float FontAtlas::measure(std::string_view text, float pixels) {
    auto* buffer = impl_->shape(text, pixels);
    std::unique_ptr<hb_buffer_t, decltype(&hb_buffer_destroy)> guard(buffer, &hb_buffer_destroy);
    unsigned count = 0;
    const auto* positions = hb_buffer_get_glyph_positions(buffer, &count);
    float advance = 0;
    for (unsigned i = 0; i < count; ++i)
        advance += positions[i].x_advance / 64.f;
    return advance;
}
void FontAtlas::draw(render::Snapshot& snapshot, std::string_view text, float x, float y,
                     float pixels, Color color, const Rect& clip) {
    auto* buffer = impl_->shape(text, pixels);
    std::unique_ptr<hb_buffer_t, decltype(&hb_buffer_destroy)> guard(buffer, &hb_buffer_destroy);
    unsigned count = 0;
    const auto* infos = hb_buffer_get_glyph_infos(buffer, &count);
    const auto* positions = hb_buffer_get_glyph_positions(buffer, &count);
    const float baseline = y + impl_->face->size->metrics.ascender / 64.f;
    for (unsigned i = 0; i < count; ++i) {
        const auto glyph = impl_->glyph(infos[i].codepoint);
        if (glyph.width && glyph.height) {
            render::Quad q;
            q.x = x + positions[i].x_offset / 64.f + glyph.left;
            q.y = baseline - positions[i].y_offset / 64.f - glyph.top;
            q.width = static_cast<float>(glyph.width);
            q.height = static_cast<float>(glyph.height);
            q.color = color;
            q.texture = impl_->atlas;
            q.uv_rect = {float(glyph.x) / impl_->atlas->width,
                         float(glyph.y) / impl_->atlas->height,
                         float(glyph.x + glyph.width) / impl_->atlas->width,
                         float(glyph.y + glyph.height) / impl_->atlas->height};
            quad(snapshot, std::move(q), clip);
        }
        x += positions[i].x_advance / 64.f;
    }
}
std::shared_ptr<const render::Texture> FontAtlas::texture() const {
    return impl_->atlas;
}
} // namespace faset::ui
