#include "elfeed.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <cstdarg>

#include <imgui.h>
#include <SDL3/SDL.h>

void elfeed_log(Elfeed *app, LogKind kind, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    LogEntry entry;
    entry.kind = kind;
    entry.time = (double)time(nullptr);
    entry.message = buf;

    std::lock_guard lock(app->log_mutex);
    app->log.push_back(std::move(entry));
}

void elfeed_init(Elfeed *app)
{
    config_load(app);
    db_open(app);
    db_load_feeds(app);
    snprintf(app->filter_buf, sizeof(app->filter_buf), "%s",
             app->default_filter.c_str());

    // Restore UI state
    std::string s;
    s = db_load_ui_state(app, "show_log");
    if (!s.empty()) app->show_log = (s == "1");
    s = db_load_ui_state(app, "show_downloads");
    if (!s.empty()) app->show_downloads = (s == "1");
}

static void format_date(double epoch, char *buf, size_t len)
{
    time_t t = (time_t)epoch;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%d", &tm);
}

static void open_in_browser(const char *url)
{
    SDL_OpenURL(url);
}

static bool has_tag(const Entry &e, const char *tag)
{
    return std::find(e.tags.begin(), e.tags.end(), tag) != e.tags.end();
}

static void clamp_cursor(Elfeed *app)
{
    int n = (int)app->entries.size();
    if (n == 0) { app->cursor = 0; return; }
    if (app->cursor < 0) app->cursor = 0;
    if (app->cursor >= n) app->cursor = n - 1;
}

static int selection_start(Elfeed *app)
{
    if (app->sel_anchor < 0) return app->cursor;
    return std::min(app->cursor, app->sel_anchor);
}

static int selection_end(Elfeed *app)
{
    if (app->sel_anchor < 0) return app->cursor;
    return std::max(app->cursor, app->sel_anchor);
}

void elfeed_frame(Elfeed *app)
{
    // Process fetch results from worker thread
    fetch_process_results(app);

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Elfeed")) {
            if (ImGui::MenuItem("Fetch All", "f"))
                fetch_all(app);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Log", nullptr, &app->show_log);
            ImGui::MenuItem("Downloads", nullptr, &app->show_downloads);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                 ImGuiDockNodeFlags_PassthruCentralNode);

    ImGui::Begin("Elfeed");

    // --- Header ---
    int active = app->fetches_active.load();
    if (active > 0) {
        ImGui::Text("Elfeed  fetching %d/%d", active,
                    app->fetches_total.load());
        app->needs_redraw = true;
    } else {
        // Count unread
        int unread = 0;
        for (auto &e : app->entries)
            if (has_tag(e, "unread")) unread++;
        ImGui::Text("Elfeed  %d/%d:%d",
                    unread, (int)app->entries.size(), (int)app->feeds.size());
    }

    // Filter display / edit
    ImGui::SameLine();
    bool filter_just_closed = false;
    if (app->filter_editing) {
        // Manual text input — bypasses InputText activation issues
        ImGuiIO &io = ImGui::GetIO();
        int len = (int)strlen(app->filter_buf);

        for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
            unsigned c = (unsigned)io.InputQueueCharacters[i];
            if (c >= 32 && c < 127 && len < (int)sizeof(app->filter_buf) - 1) {
                app->filter_buf[len++] = (char)c;
                app->filter_buf[len] = 0;
                app->filter_dirty = true;
            }
        }
        io.InputQueueCharacters.resize(0);

        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true) && len > 0) {
            if (io.KeyCtrl) {
                // Delete back to last space
                while (len > 0 && app->filter_buf[len - 1] == ' ')
                    len--;
                while (len > 0 && app->filter_buf[len - 1] != ' ')
                    len--;
                app->filter_buf[len] = 0;
            } else {
                app->filter_buf[--len] = 0;
            }
            app->filter_dirty = true;
        }
        bool phys_ctrl = io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl;
        if (ImGui::IsKeyPressed(ImGuiKey_W) && phys_ctrl && len > 0) {
            while (len > 0 && app->filter_buf[len - 1] == ' ')
                len--;
            while (len > 0 && app->filter_buf[len - 1] != ' ')
                len--;
            app->filter_buf[len] = 0;
            app->filter_dirty = true;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            app->filter_editing = false;
            filter_just_closed = true;
            SDL_StopTextInput(app->window);
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            memcpy(app->filter_buf, app->filter_buf_backup,
                   sizeof(app->filter_buf));
            app->filter_dirty = true;
            app->filter_editing = false;
            filter_just_closed = true;
            SDL_StopTextInput(app->window);
        }

        // Render filter with input background and block cursor
        ImVec2 text_size = ImGui::CalcTextSize(app->filter_buf);
        float h = ImGui::GetFrameHeight();
        float avail = ImGui::GetContentRegionAvail().x;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImGuiStyle &style = ImGui::GetStyle();

        // Input background
        dl->AddRectFilled(pos, ImVec2(pos.x + avail, pos.y + h),
                          ImGui::GetColorU32(ImGuiCol_FrameBg),
                          style.FrameRounding);

        // Text
        ImVec2 text_pos(pos.x + style.FramePadding.x,
                        pos.y + style.FramePadding.y);
        dl->AddText(text_pos, ImGui::GetColorU32(ImGuiCol_Text),
                    app->filter_buf);

        // Solid block cursor
        float cursor_x = text_pos.x + text_size.x;
        float char_w = ImGui::CalcTextSize("M").x;
        dl->AddRectFilled(
            ImVec2(cursor_x, text_pos.y),
            ImVec2(cursor_x + char_w,
                   text_pos.y + ImGui::GetTextLineHeight()),
            ImGui::GetColorU32(ImGuiCol_Text));

        // Reserve space
        ImGui::Dummy(ImVec2(avail, h));
    } else {
        ImGui::TextUnformatted(app->filter_buf);
    }

    ImGui::Separator();

    // --- Key handling ---
    bool elfeed_focused = ImGui::IsWindowFocused(
        ImGuiFocusedFlags_RootAndChildWindows);
    if (elfeed_focused && !app->filter_editing && !filter_just_closed) {
        ImGuiIO &io = ImGui::GetIO();
        int n = (int)app->entries.size();
        bool shift = io.KeyShift;

        if (ImGui::IsKeyPressed(ImGuiKey_J)) {
            app->cursor++;
            clamp_cursor(app);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_K)) {
            app->cursor--;
            clamp_cursor(app);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_G) && shift) {
            app->cursor = n > 0 ? n - 1 : 0;
        } else if (ImGui::IsKeyPressed(ImGuiKey_G)) {
            app->cursor = 0;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_B) && n > 0) {
            int s = selection_start(app);
            int e = selection_end(app);
            for (int i = s; i <= e && i < n; i++) {
                if (!app->entries[(size_t)i].link.empty())
                    open_in_browser(app->entries[(size_t)i].link.c_str());
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Y) && n > 0) {
            std::string urls;
            int s = selection_start(app);
            int e = selection_end(app);
            for (int i = s; i <= e && i < n; i++) {
                if (!urls.empty()) urls += '\n';
                urls += app->entries[(size_t)i].link;
            }
            SDL_SetClipboardText(urls.c_str());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_U)) {
            int s = selection_start(app);
            int e_idx = selection_end(app);
            for (int i = s; i <= e_idx && i < n; i++) {
                auto &e = app->entries[(size_t)i];
                if (!has_tag(e, "unread")) {
                    e.tags.push_back("unread");
                    std::sort(e.tags.begin(), e.tags.end());
                    db_tag(app, e.namespace_, e.id, "unread");
                }
            }
            if (app->sel_anchor < 0) {
                app->cursor++;
                clamp_cursor(app);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_R) && !shift) {
            int s = selection_start(app);
            int e_idx = selection_end(app);
            for (int i = s; i <= e_idx && i < n; i++) {
                auto &e = app->entries[(size_t)i];
                if (has_tag(e, "unread")) {
                    e.tags.erase(std::find(e.tags.begin(),
                                            e.tags.end(), "unread"));
                    db_untag(app, e.namespace_, e.id, "unread");
                }
            }
            if (app->sel_anchor < 0) {
                app->cursor++;
                clamp_cursor(app);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_R) && shift) {
            app->filter_dirty = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_V)) {
            if (app->sel_anchor < 0)
                app->sel_anchor = app->cursor;
            else
                app->sel_anchor = -1;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Slash) ||
            (ImGui::IsKeyPressed(ImGuiKey_S) && !shift)) {
            memcpy(app->filter_buf_backup, app->filter_buf,
                   sizeof(app->filter_buf));
            app->filter_editing = true;
            SDL_StartTextInput(app->window);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F)) {
            fetch_all(app);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_D) && !shift) {
            if (n > 0 && app->cursor < n) {
                auto &e = app->entries[(size_t)app->cursor];
                for (auto &enc : e.enclosures) {
                    bool video = enc.type.find("video") != std::string::npos ||
                        enc.url.find("youtube") != std::string::npos ||
                        enc.url.find("youtu.be") != std::string::npos;
                    download_enqueue(app, enc.url, e.title, video);
                }
                if (e.enclosures.empty()) {
                    download_enqueue(app, e.link, e.title, true);
                }
                if (has_tag(e, "unread")) {
                    e.tags.erase(std::find(e.tags.begin(),
                                            e.tags.end(), "unread"));
                    db_untag(app, e.namespace_, e.id, "unread");
                }
                if (app->sel_anchor < 0) {
                    app->cursor++;
                    clamp_cursor(app);
                }
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_D) && shift) {
            app->show_downloads = !app->show_downloads;
        }

        // Enter: show entry detail view
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) && n > 0 &&
            app->cursor < n) {
            app->show_entry = true;
            app->show_entry_idx = app->cursor;
            auto &e = app->entries[(size_t)app->cursor];
            if (has_tag(e, "unread")) {
                e.tags.erase(std::find(e.tags.begin(), e.tags.end(), "unread"));
                db_untag(app, e.namespace_, e.id, "unread");
            }
        }

        // Escape: clear selection
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            app->sel_anchor = -1;
    }

    // Re-query entries if filter changed (after InputText so focus is stable)
    if (app->filter_dirty) {
        app->filter_dirty = false;
        std::string sel_ns, sel_id;
        if (app->cursor >= 0 && app->cursor < (int)app->entries.size()) {
            sel_ns = app->entries[(size_t)app->cursor].namespace_;
            sel_id = app->entries[(size_t)app->cursor].id;
        }
        app->current_filter = filter_parse(app->filter_buf);
        db_query_entries(app, app->current_filter, app->entries);
        if (!sel_ns.empty()) {
            for (int i = 0; i < (int)app->entries.size(); i++) {
                if (app->entries[(size_t)i].namespace_ == sel_ns &&
                    app->entries[(size_t)i].id == sel_id) {
                    app->cursor = i;
                    break;
                }
            }
        }
        clamp_cursor(app);
    }

    // --- Entry table ---
    if (app->feeds.empty()) {
        ImGui::TextDisabled("No feeds configured.");
        ImGui::TextDisabled("Edit %s", app->config_path.c_str());
    } else if (app->entries.empty()) {
        ImGui::TextDisabled("No entries. Press F or Fetch to update feeds.");
    } else {
        if (ImGui::BeginTable("entries", 4,
                              ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed,
                                    80.0f);
            ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Feed", ImGuiTableColumnFlags_WidthFixed,
                                    150.0f);
            ImGui::TableSetupColumn("Tags", ImGuiTableColumnFlags_WidthFixed,
                                    100.0f);
            ImGui::TableHeadersRow();

            int n = (int)app->entries.size();
            int sel_s = selection_start(app);
            int sel_e = selection_end(app);

            ImGuiListClipper clipper;
            clipper.Begin(n);
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                    auto &e = app->entries[(size_t)i];
                    ImGui::TableNextRow();

                    bool is_unread = has_tag(e, "unread");
                    bool is_cursor = (i == app->cursor);
                    bool is_selected = (app->sel_anchor >= 0 &&
                                        i >= sel_s && i <= sel_e);

                    // Highlight cursor/selection
                    if (is_cursor || is_selected) {
                        ImU32 color = is_cursor
                            ? IM_COL32(60, 80, 120, 255)
                            : IM_COL32(50, 60, 90, 200);
                        ImGui::TableSetBgColor(
                            ImGuiTableBgTarget_RowBg1, color);
                    }

                    // Date
                    ImGui::TableSetColumnIndex(0);
                    char date_buf[32];
                    format_date(e.date, date_buf, sizeof(date_buf));
                    if (is_unread)
                        ImGui::TextUnformatted(date_buf);
                    else
                        ImGui::TextDisabled("%s", date_buf);

                    // Title
                    ImGui::TableSetColumnIndex(1);
                    std::string display_title = html_strip(e.title);
                    if (is_unread)
                        ImGui::TextUnformatted(display_title.c_str());
                    else
                        ImGui::TextDisabled("%s", display_title.c_str());

                    // Click to select
                    if (ImGui::IsItemClicked()) {
                        app->cursor = i;
                        if (ImGui::GetIO().KeyShift) {
                            if (app->sel_anchor < 0)
                                app->sel_anchor = i;
                        } else {
                            app->sel_anchor = -1;
                        }
                    }

                    // Feed title
                    ImGui::TableSetColumnIndex(2);
                    for (auto &f : app->feeds) {
                        if (f.url == e.feed_url) {
                            ImGui::TextColored(
                                ImVec4(0.8f, 0.8f, 0.2f, 1.0f),
                                "%s", f.title.c_str());
                            break;
                        }
                    }

                    // Tags
                    ImGui::TableSetColumnIndex(3);
                    std::string tags_str;
                    for (auto &t : e.tags) {
                        if (!tags_str.empty()) tags_str += ",";
                        tags_str += t;
                    }
                    ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f),
                                       "%s", tags_str.c_str());
                }
            }

            // Scroll cursor into view
            if (app->cursor >= 0 && app->cursor < n) {
                float item_height = ImGui::GetTextLineHeightWithSpacing();
                float scroll_y = ImGui::GetScrollY();
                float visible_height = ImGui::GetWindowHeight() -
                    ImGui::GetCursorStartPos().y;
                float cursor_y = (float)app->cursor * item_height;
                if (cursor_y < scroll_y)
                    ImGui::SetScrollY(cursor_y);
                else if (cursor_y + item_height > scroll_y + visible_height)
                    ImGui::SetScrollY(cursor_y + item_height - visible_height);
            }

            ImGui::EndTable();
        }
    }

    ImGui::End();

    // --- Entry detail view ---
    if (app->show_entry && app->show_entry_idx >= 0 &&
        app->show_entry_idx < (int)app->entries.size()) {
        auto &e = app->entries[(size_t)app->show_entry_idx];

        ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("Entry", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Header
        std::string display_title = html_strip(e.title);
        ImGui::TextWrapped("%s", display_title.c_str());

        // Feed + Date
        char date_buf[32];
        format_date(e.date, date_buf, sizeof(date_buf));
        std::string feed_title;
        for (auto &f : app->feeds) {
            if (f.url == e.feed_url) { feed_title = f.title; break; }
        }
        ImGui::TextDisabled("%s  %s", feed_title.c_str(), date_buf);

        // Authors
        if (!e.authors.empty()) {
            std::string authors_str;
            for (auto &a : e.authors) {
                if (!authors_str.empty()) authors_str += ", ";
                authors_str += a.name.empty() ? a.email : a.name;
            }
            ImGui::TextDisabled("By %s", authors_str.c_str());
        }

        // Link
        if (!e.link.empty()) {
            ImGui::TextColored(ImVec4(0.4f, 0.6f, 1.0f, 1.0f),
                               "%s", e.link.c_str());
            if (ImGui::IsItemClicked())
                open_in_browser(e.link.c_str());
        }

        // Tags
        {
            std::string tags_str;
            for (auto &t : e.tags) {
                if (!tags_str.empty()) tags_str += ", ";
                tags_str += t;
            }
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f),
                               "(%s)", tags_str.c_str());
        }

        // Enclosures
        for (size_t i = 0; i < e.enclosures.size(); i++) {
            auto &enc = e.enclosures[i];
            ImGui::TextDisabled("Enclosure %d:", (int)(i + 1));
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.6f, 1.0f, 1.0f),
                               "%s", enc.url.c_str());
        }

        ImGui::Separator();

        // Content
        ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        if (!e.content.empty()) {
            std::string text = html_strip(e.content);
            ImGui::TextWrapped("%s", text.c_str());
        } else {
            ImGui::TextDisabled("(no content)");
        }
        ImGui::EndChild();

        // Key handling in entry view
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            if (ImGui::IsKeyPressed(ImGuiKey_Q) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                app->show_entry = false;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_N)) {
                if (app->show_entry_idx < (int)app->entries.size() - 1) {
                    app->show_entry_idx++;
                    app->cursor = app->show_entry_idx;
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_P)) {
                if (app->show_entry_idx > 0) {
                    app->show_entry_idx--;
                    app->cursor = app->show_entry_idx;
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_B) && !e.link.empty()) {
                open_in_browser(e.link.c_str());
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Y)) {
                SDL_SetClipboardText(e.link.c_str());
            }
            if (ImGui::IsKeyPressed(ImGuiKey_D)) {
                for (auto &enc : e.enclosures) {
                    bool video = enc.type.find("video") != std::string::npos;
                    download_enqueue(app, enc.url, e.title, video);
                }
                if (e.enclosures.empty())
                    download_enqueue(app, e.link, e.title, true);
                if (has_tag(e, "unread")) {
                    e.tags.erase(std::find(e.tags.begin(),
                                            e.tags.end(), "unread"));
                    db_untag(app, e.namespace_, e.id, "unread");
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_U)) {
                if (has_tag(e, "unread")) {
                    e.tags.erase(std::find(e.tags.begin(),
                                            e.tags.end(), "unread"));
                    db_untag(app, e.namespace_, e.id, "unread");
                } else {
                    e.tags.push_back("unread");
                    std::sort(e.tags.begin(), e.tags.end());
                    db_tag(app, e.namespace_, e.id, "unread");
                }
            }
        }

        ImGui::End();
    }

    // --- Log window ---
    if (app->show_log) {
        ImGui::SetNextWindowSize(ImVec2(600, 300), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Log", &app->show_log)) {
            ImGui::Checkbox("Info", &app->log_show_info);
            ImGui::SameLine();
            ImGui::Checkbox("Requests", &app->log_show_requests);
            ImGui::SameLine();
            ImGui::Checkbox("Success", &app->log_show_success);
            ImGui::SameLine();
            ImGui::Checkbox("Errors", &app->log_show_errors);
            ImGui::SameLine();
            ImGui::Checkbox("Auto-scroll", &app->log_auto_scroll);
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) {
                std::lock_guard lock(app->log_mutex);
                app->log.clear();
            }

            ImGui::Separator();

            std::lock_guard lock(app->log_mutex);
            if (ImGui::BeginTable("log_table", 4,
                    ImGuiTableFlags_ScrollY |
                    ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_Resizable |
                    ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Time",
                    ImGuiTableColumnFlags_WidthFixed, 140.0f);
                ImGui::TableSetupColumn("Type",
                    ImGuiTableColumnFlags_WidthFixed, 40.0f);
                ImGui::TableSetupColumn("URL",
                    ImGuiTableColumnFlags_WidthStretch, 0.4f);
                ImGui::TableSetupColumn("Result",
                    ImGuiTableColumnFlags_WidthStretch, 0.6f);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                for (auto &entry : app->log) {
                    bool show = false;
                    ImVec4 color;
                    const char *type;
                    switch (entry.kind) {
                    case LOG_INFO:
                        show = app->log_show_info;
                        color = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
                        type = "info";
                        break;
                    case LOG_REQUEST:
                        show = app->log_show_requests;
                        color = ImVec4(0.5f, 0.7f, 1.0f, 1.0f);
                        type = "req";
                        break;
                    case LOG_SUCCESS:
                        show = app->log_show_success;
                        color = ImVec4(0.2f, 0.9f, 0.2f, 1.0f);
                        type = "ok";
                        break;
                    case LOG_ERROR:
                        show = app->log_show_errors;
                        color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                        type = "err";
                        break;
                    }
                    if (!show) continue;

                    ImGui::TableNextRow();

                    // Timestamp
                    ImGui::TableSetColumnIndex(0);
                    char time_buf[32];
                    time_t tt = (time_t)entry.time;
                    struct tm tm;
                    localtime_r(&tt, &tm);
                    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm);
                    ImGui::TextUnformatted(time_buf);

                    // Type
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(color, "%s", type);

                    // Split message at first ": " into URL and result
                    ImGui::TableSetColumnIndex(2);
                    auto colon = entry.message.find(": ");
                    if (colon != std::string::npos) {
                        ImGui::TextUnformatted(
                            entry.message.c_str(),
                            entry.message.c_str() + colon);
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(
                            entry.message.c_str() + colon + 2);
                    } else {
                        ImGui::TextUnformatted(entry.message.c_str());
                    }
                }

                if (app->log_auto_scroll &&
                    ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                    ImGui::SetScrollHereY(1.0f);

                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    // --- Downloads panel ---
    download_tick(app);

    if (app->show_downloads) {
        ImGui::SetNextWindowSize(ImVec2(500, 300), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Downloads", &app->show_downloads)) {
            int dl_pause_id = 0, dl_remove_id = 0;
            {
                std::lock_guard lock(app->download_mutex);
                if (app->downloads.empty()) {
                    ImGui::TextDisabled("No active downloads.");
                } else if (ImGui::BeginTable("downloads", 5,
                               ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_Resizable |
                               ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("##ctl",
                        ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("%%",
                        ImGuiTableColumnFlags_WidthFixed, 60.0f);
                    ImGui::TableSetupColumn("Size",
                        ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableSetupColumn("Name",
                        ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("##err",
                        ImGuiTableColumnFlags_WidthFixed, 50.0f);

                    for (auto &d : app->downloads) {
                        ImGui::PushID(d.id);
                        ImGui::TableNextRow();
                        bool is_active = (d.id == app->download_active_id);

                        // Buttons
                        ImGui::TableSetColumnIndex(0);
                        if (ImGui::SmallButton(d.paused ? "Resume" : "Pause"))
                            dl_pause_id = d.id;
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove"))
                            dl_remove_id = d.id;

                        // Percent
                        ImGui::TableSetColumnIndex(1);
                        if (is_active)
                            ImGui::TextColored(
                                ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                                "%s", d.progress.empty()
                                    ? "..." : d.progress.c_str());
                        else if (d.paused)
                            ImGui::TextDisabled("paused");
                        else
                            ImGui::TextDisabled("queued");

                        // Size
                        ImGui::TableSetColumnIndex(2);
                        if (!d.total.empty())
                            ImGui::TextDisabled("%s", d.total.c_str());

                        // Name
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(d.title.empty()
                            ? d.url.c_str() : d.title.c_str());

                        // Failures
                        ImGui::TableSetColumnIndex(4);
                        if (d.failures > 0)
                            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1),
                                               "x%d", d.failures);

                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }
            // Act on buttons outside the lock
            if (dl_pause_id)  download_pause(app, dl_pause_id);
            if (dl_remove_id) download_remove(app, dl_remove_id);
            if (app->download_active_id != 0)
                app->needs_redraw = true;
        }
        ImGui::End();
    }
}

void elfeed_shutdown(Elfeed *app)
{
    fetch_stop(app);
    download_stop(app);
    db_save_ui_state(app, "show_log", app->show_log ? "1" : "0");
    db_save_ui_state(app, "show_downloads", app->show_downloads ? "1" : "0");
    db_close(app);
}
