#include "elfeed.hpp"

#include <cstdio>
#include <cstring>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#endif

static void process_event(Elfeed *app, SDL_Event *event)
{
    ImGui_ImplSDL3_ProcessEvent(event);
    switch (event->type) {
    case SDL_EVENT_QUIT:
        app->want_quit = true;
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        app->want_quit = true;
        break;
    default:
        if (event->type == app->wake_event_type) {
            // Worker thread posted a wake event
        }
        break;
    }
    app->needs_redraw = true;
}

int main(int, char **)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "elfeed: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    // GL 3.2 Core + GLSL 150 (required on macOS, works everywhere)
    const char *glsl_version = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,
                        SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    Elfeed app = {};

    // Register custom event type for worker thread wake-ups
    app.wake_event_type = SDL_RegisterEvents(1);

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
        SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    app.window = SDL_CreateWindow("Elfeed", app.win_w, app.win_h,
                                  window_flags);
    if (!app.window) {
        fprintf(stderr, "elfeed: SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(app.window);
    if (!gl_context) {
        fprintf(stderr, "elfeed: SDL_GL_CreateContext: %s\n", SDL_GetError());
        SDL_DestroyWindow(app.window);
        SDL_Quit();
        return 1;
    }

    SDL_GL_MakeCurrent(app.window, gl_context);
    SDL_GL_SetSwapInterval(1);
    SDL_SetWindowPosition(app.window,
                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(app.window);

    // Dear ImGui setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();

    float main_scale = SDL_GetDisplayContentScale(
        SDL_GetDisplayForWindow(app.window));
    app.dpi_scale = main_scale;
    ImGuiStyle &style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;
    io.ConfigDpiScaleFonts = true;

    ImGui_ImplSDL3_InitForOpenGL(app.window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    elfeed_init(&app);

    // Load saved window geometry
    {
        std::string s;
        s = db_load_ui_state(&app, "win_x");
        if (!s.empty()) app.win_x = atoi(s.c_str());
        s = db_load_ui_state(&app, "win_y");
        if (!s.empty()) app.win_y = atoi(s.c_str());
        s = db_load_ui_state(&app, "win_w");
        if (!s.empty()) app.win_w = atoi(s.c_str());
        s = db_load_ui_state(&app, "win_h");
        if (!s.empty()) app.win_h = atoi(s.c_str());
        if (app.win_w > 0 && app.win_h > 0)
            SDL_SetWindowSize(app.window, app.win_w, app.win_h);
        if (app.win_x >= 0 && app.win_y >= 0)
            SDL_SetWindowPosition(app.window, app.win_x, app.win_y);
    }

    // ImGui layout persistence
    app.ini_path = xdg_config_home() + "/elfeed2/imgui.ini";
    io.IniFilename = app.ini_path.c_str();

    // Main loop with passive rendering
    while (!app.want_quit) {
        SDL_Event event;
        if (app.needs_redraw) {
            while (SDL_PollEvent(&event))
                process_event(&app, &event);
        } else {
            if (SDL_WaitEvent(&event))
                process_event(&app, &event);
            while (SDL_PollEvent(&event))
                process_event(&app, &event);
        }

        if (SDL_GetWindowFlags(app.window) & SDL_WINDOW_MINIMIZED) {
            app.needs_redraw = false;
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        elfeed_frame(&app);

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(app.window);

        app.needs_redraw = false;
    }

    // Save window geometry
    {
        int x, y, w, h;
        SDL_GetWindowPosition(app.window, &x, &y);
        SDL_GetWindowSize(app.window, &w, &h);
        db_save_ui_state(&app, "win_x", std::to_string(x).c_str());
        db_save_ui_state(&app, "win_y", std::to_string(y).c_str());
        db_save_ui_state(&app, "win_w", std::to_string(w).c_str());
        db_save_ui_state(&app, "win_h", std::to_string(h).c_str());
    }

    elfeed_shutdown(&app);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}
