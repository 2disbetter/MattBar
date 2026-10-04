#pragma once
// Shared Cairo widgets for MattBar Shell overlays (text field, etc.).
#include "bar.hpp"
#include "config.hpp"
#include <cairo/cairo.h>
#include <algorithm>
#include <cstdio>
#include <string>

// spawn.cpp (declared here rather than via util.hpp, whose helpers
// collide with file-local ones in some includers)
std::string run_capture_bounded(const std::string& cmd, int timeout_ms,
                                int* status, const std::string* input);

struct TextField {
    std::string text;
    size_t      cursor   = 0; // byte index
    bool        password = false;
    bool        focused  = true;
    double      pan      = 0; // horizontal scroll in px (long strings)
    double      last_text_w_  = 0;
    double      last_field_w_ = 0;

    void clear() {
        text.clear();
        cursor = 0;
        pan    = 0;
    }
    void scroll(int dir) {
        double maxp = std::max(0.0, last_text_w_ - std::max(0.0, last_field_w_ - 20));
        pan = std::clamp(pan + dir * 28.0, 0.0, maxp);
    }

    // Bounded: wl-paste blocks until the clipboard's source client hands
    // the data over, so a frozen source app used to hang the whole bar on
    // Ctrl+V until the watchdog killed it. Past the deadline the paste is
    // simply empty.
    static std::string clipboard_read() {
        return run_capture_bounded(
            "wl-paste --no-newline --type text 2>/dev/null || "
            "wl-paste --no-newline 2>/dev/null",
            400, nullptr, nullptr);
    }
    void clipboard_write() const {
        if (password || text.empty()) return;
        (void)run_capture_bounded(
            "wl-copy --type text/plain 2>/dev/null || wl-copy", 400, nullptr,
            &text);
    }
    void insert_text(const std::string& s) {
        std::string clean;
        clean.reserve(s.size());
        for (unsigned char c : s) {
            if (c < 32) continue; // drop CR/LF/tab/controls; URL/search fields
            clean += static_cast<char>(c);
        }
        if (clean.empty()) return;
        text.insert(cursor, clean);
        cursor += clean.size();
    }

    bool handle(const Bar::KeyEvent& e) {
        if (!e.pressed) return false;
        if (e.ctrl() && (e.keysym == 0x75 /* u */ || e.keysym == 0x55)) {
            clear();
            return true;
        }
        // Clipboard. Exclusive-kb overlays never see compositor paste, so
        // we pull/push via wl-paste / wl-copy. Password fields paste in
        // but never copy out.
        const bool insert_key =
            e.keysym == 0xff63 /* XK_Insert */ || e.keysym == 0xff9e;
        if ((e.ctrl() && (e.keysym == 0x76 || e.keysym == 0x56)) ||
            (e.shift() && insert_key)) {
            insert_text(clipboard_read());
            return true;
        }
        if ((e.ctrl() && (e.keysym == 0x63 || e.keysym == 0x43)) ||
            (e.ctrl() && insert_key)) {
            clipboard_write();
            return true;
        }
        if (e.ctrl() && (e.keysym == 0x78 || e.keysym == 0x58)) {
            clipboard_write();
            if (!password) clear();
            return true;
        }
        if (e.ctrl() && (e.keysym == 0x61 || e.keysym == 0x41)) {
            cursor = text.size();
            return true;
        }
        if (e.ctrl()) return false;
        if (e.backspace()) {
            if (cursor == 0) return true;
            size_t i = cursor;
            do {
                --i;
            } while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xc0) ==
                                   0x80);
            text.erase(i, cursor - i);
            cursor = i;
            return true;
        }
        if (e.left()) {
            if (cursor == 0) return true;
            do {
                --cursor;
            } while (cursor > 0 &&
                     (static_cast<unsigned char>(text[cursor]) & 0xc0) == 0x80);
            return true;
        }
        if (e.right()) {
            if (cursor >= text.size()) return true;
            unsigned char c = text[cursor];
            cursor += c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2
                                  : (c >> 4) == 0xe  ? 3
                                                     : 4;
            if (cursor > text.size()) cursor = text.size();
            return true;
        }
        if (e.utf8.empty() || e.utf8[0] < 32) return false;
        text.insert(cursor, e.utf8);
        cursor += e.utf8.size();
        return true;
    }

    void draw(cairo_t* cr, double x, double y, double w, double h,
              const char* placeholder = nullptr) {
        cairo_set_source_rgba(cr, cfg.c_ws_bg.r, cfg.c_ws_bg.g, cfg.c_ws_bg.b,
                              1);
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);
        std::string shown;
        if (password) shown.assign(text.size(), '*');
        else shown = text;
        const std::string& draw_s =
            shown.empty() && placeholder ? std::string(placeholder) : shown;
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        double ty = y + h / 2.0 + (fe.ascent - fe.descent) / 2.0;
        if (shown.empty() && placeholder) {
            cairo_set_source_rgba(cr, cfg.c_dim.r, cfg.c_dim.g, cfg.c_dim.b, 1);
        } else {
            cairo_set_source_rgba(cr, cfg.c_fg.r, cfg.c_fg.g, cfg.c_fg.b, 1);
        }
        cairo_text_extents_t full;
        cairo_text_extents(cr, shown.c_str(), &full);
        last_text_w_  = full.x_advance;
        last_field_w_ = w;
        cairo_text_extents_t cur_ext{};
        cairo_text_extents(cr, shown.substr(0, std::min(cursor, shown.size())).c_str(),
                           &cur_ext);
        double caret = 10 + cur_ext.x_advance;
        if (focused) {
            if (caret - pan > w - 12) pan = caret - (w - 12);
            if (caret - pan < 10) pan = caret - 10;
        }
        double maxp = std::max(0.0, last_text_w_ - (w - 20));
        if (pan > maxp) pan = maxp;
        if (pan < 0) pan = 0;
        cairo_save(cr);
        cairo_rectangle(cr, x, y, w, h);
        cairo_clip(cr);
        cairo_move_to(cr, x + 10 - pan, ty);
        cairo_show_text(cr, draw_s.c_str());
        if (focused && !password) {
            double cx = x + 10 - pan + cur_ext.x_advance;
            cairo_set_source_rgba(cr, cfg.c_accent.r, cfg.c_accent.g,
                                  cfg.c_accent.b, 1);
            cairo_rectangle(cr, cx, y + 6, 1.5, h - 12);
            cairo_fill(cr);
        }
        cairo_restore(cr);
        if (focused) {
            cairo_set_source_rgba(cr, cfg.c_accent.r, cfg.c_accent.g,
                                  cfg.c_accent.b, 1);
            cairo_set_line_width(cr, 1.2);
            cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1, h - 1);
            cairo_stroke(cr);
        }
    }
};
