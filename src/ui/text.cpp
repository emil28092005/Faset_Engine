#include <algorithm>
#include <cctype>
#include <faset/ui/ui.hpp>
#include <stdexcept>

namespace faset::ui {
namespace {
bool continuation(unsigned char c) {
    return (c & 0xc0) == 0x80;
}
std::size_t previous(std::string_view text, std::size_t at) {
    if (!at)
        return 0;
    --at;
    while (at && continuation(static_cast<unsigned char>(text[at])))
        --at;
    return at;
}
std::size_t next(std::string_view text, std::size_t at) {
    if (at >= text.size())
        return text.size();
    ++at;
    while (at < text.size() && continuation(static_cast<unsigned char>(text[at])))
        ++at;
    return at;
}
bool word(unsigned char c) {
    return c >= 128 || std::isalnum(c) || c == '_';
}
} // namespace
bool TextBuffer::valid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i++]);
        if (lead < 128)
            continue;
        unsigned cp = 0;
        int n = 0;
        unsigned minimum = 0;
        if ((lead & 0xe0) == 0xc0) {
            cp = lead & 31;
            n = 1;
            minimum = 128;
        } else if ((lead & 0xf0) == 0xe0) {
            cp = lead & 15;
            n = 2;
            minimum = 2048;
        } else if ((lead & 0xf8) == 0xf0) {
            cp = lead & 7;
            n = 3;
            minimum = 65536;
        } else
            return false;
        if (i + static_cast<std::size_t>(n) > text.size())
            return false;
        while (n--) {
            auto c = static_cast<unsigned char>(text[i++]);
            if (!continuation(c))
                return false;
            cp = (cp << 6) | (c & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
    }
    return true;
}
TextBuffer::TextBuffer(std::string text) {
    reset(std::move(text));
}
void TextBuffer::reset(std::string text) {
    if (!valid_utf8(text))
        throw std::invalid_argument("Invalid UTF-8 text");
    text_ = std::move(text);
    cursor_ = anchor_ = text_.size();
    undo_.clear();
    redo_.clear();
}
void TextBuffer::set_cursor(std::size_t byte, bool select) {
    cursor_ = std::min(byte, text_.size());
    while (cursor_ && cursor_ < text_.size() &&
           continuation(static_cast<unsigned char>(text_[cursor_])))
        --cursor_;
    if (!select)
        anchor_ = cursor_;
}
void TextBuffer::select_all() {
    anchor_ = 0;
    cursor_ = text_.size();
}
std::string TextBuffer::selected_text() const {
    return text_.substr(std::min(cursor_, anchor_),
                        std::max(cursor_, anchor_) - std::min(cursor_, anchor_));
}
void TextBuffer::left(bool select, bool by_word) {
    if (!select && has_selection()) {
        set_cursor(std::min(cursor_, anchor_));
        return;
    }
    auto position = previous(text_, cursor_);
    if (by_word) {
        while (position && !word(static_cast<unsigned char>(text_[position])))
            position = previous(text_, position);
        while (position && word(static_cast<unsigned char>(text_[previous(text_, position)])))
            position = previous(text_, position);
    }
    set_cursor(position, select);
}
void TextBuffer::right(bool select, bool by_word) {
    if (!select && has_selection()) {
        set_cursor(std::max(cursor_, anchor_));
        return;
    }
    auto position = next(text_, cursor_);
    if (by_word) {
        while (position < text_.size() && word(static_cast<unsigned char>(text_[position])))
            position = next(text_, position);
        while (position < text_.size() && !word(static_cast<unsigned char>(text_[position])))
            position = next(text_, position);
    }
    set_cursor(position, select);
}
void TextBuffer::home(bool select) {
    set_cursor(0, select);
}
void TextBuffer::end(bool select) {
    set_cursor(text_.size(), select);
}
void TextBuffer::remember() {
    undo_.push_back({text_, cursor_, anchor_});
    if (undo_.size() > 256)
        undo_.erase(undo_.begin());
    redo_.clear();
}
void TextBuffer::erase_selection() {
    const auto begin = std::min(cursor_, anchor_), end = std::max(cursor_, anchor_);
    text_.erase(begin, end - begin);
    cursor_ = anchor_ = begin;
}
bool TextBuffer::insert(std::string_view utf8) {
    if (!valid_utf8(utf8) || text_.size() + utf8.size() > 1024 * 1024)
        return false;
    if (utf8.empty() && !has_selection())
        return false;
    remember();
    erase_selection();
    text_.insert(cursor_, utf8);
    cursor_ += utf8.size();
    anchor_ = cursor_;
    return true;
}
bool TextBuffer::backspace() {
    if (!has_selection() && !cursor_)
        return false;
    remember();
    if (has_selection())
        erase_selection();
    else {
        auto begin = previous(text_, cursor_);
        text_.erase(begin, cursor_ - begin);
        cursor_ = anchor_ = begin;
    }
    return true;
}
bool TextBuffer::delete_forward() {
    if (!has_selection() && cursor_ == text_.size())
        return false;
    remember();
    if (has_selection())
        erase_selection();
    else
        text_.erase(cursor_, next(text_, cursor_) - cursor_);
    anchor_ = cursor_;
    return true;
}
bool TextBuffer::undo() {
    if (undo_.empty())
        return false;
    redo_.push_back({text_, cursor_, anchor_});
    auto old = std::move(undo_.back());
    undo_.pop_back();
    text_ = std::move(old.text);
    cursor_ = old.cursor;
    anchor_ = old.anchor;
    return true;
}
bool TextBuffer::redo() {
    if (redo_.empty())
        return false;
    undo_.push_back({text_, cursor_, anchor_});
    auto old = std::move(redo_.back());
    redo_.pop_back();
    text_ = std::move(old.text);
    cursor_ = old.cursor;
    anchor_ = old.anchor;
    return true;
}
} // namespace faset::ui
