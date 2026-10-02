// The launcher window: Dear ImGui on SDL2's software renderer (the emulator
// AppImage ships no GL stack).

#include "gui.h"

#include "cli.h"
#include "core/download.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

#include <SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

namespace cl {

namespace {

const int kWidth = 660, kHeight = 440;
const ImVec4 kWarning(1.0f, 0.78f, 0.35f, 1.0f);
const ImVec4 kError(1.0f, 0.45f, 0.40f, 1.0f);
const ImVec4 kDim(0.62f, 0.62f, 0.66f, 1.0f);

// Index refreshes and downloads run on one worker thread; the UI polls
// `finished` and reads `progress`.
struct Worker {
    enum class Job { None, Refresh, Fetch };

    std::thread thread;
    Job job = Job::None;
    std::string id;           // image being fetched
    bool boot_after = false;  // boot it once downloaded
    FetchProgress progress;
    FetchResult result;
    std::atomic<bool> finished{false};
    Uint32 wake_event = 0;

    bool busy() const { return job != Job::None; }
    bool fetching(const std::string& image) const { return job == Job::Fetch && id == image; }

    void wake() {
        SDL_Event event;
        SDL_zero(event);
        event.type = wake_event;
        SDL_PushEvent(&event);
    }
};

enum class Confirm { None, Delete, Reset, Update };

struct Ui {
    Session& session;
    Worker worker;
    std::vector<CatalogRow> rows;
    std::string selected;  // image id
    bool scroll_to_selected = false;
    bool refreshed = false;  // the first index refresh has finished

    Confirm confirm = Confirm::None;
    std::string confirm_id;
    bool open_confirm = false;

    std::string message;
    bool message_is_error = false;

    bool ssh_enabled = true;
    char ssh_port[8] = "2222";

    bool done = false;
    int exit_code = kExitClosed;

    explicit Ui(Session& s) : session(s) {}

    const CatalogRow* row(const std::string& id) const {
        for (const auto& r : rows)
            if (r.entry.id == id) return &r;
        return nullptr;
    }
    const CatalogRow* current() const { return row(selected); }

    void say(const std::string& text, bool error = false) {
        message = text;
        message_is_error = error;
    }

    // Rebuild the list from the session and the state directory. Not while
    // a refresh is running: the worker is replacing session.index.
    void reload() {
        rows = session.rows();
        if (!row(selected)) {
            Settings settings = session.state.load_settings();
            int pick = pick_default(rows, settings.last_id, session.config.default_id, session.index.latest_release);
            selected = pick >= 0 ? rows[pick].entry.id : "";
            scroll_to_selected = true;
        }
    }

    void move_selection(int delta) {
        if (rows.empty()) return;
        int at = 0;
        for (size_t i = 0; i < rows.size(); i++)
            if (rows[i].entry.id == selected) at = (int)i;
        at += delta;
        if (at < 0) at = 0;
        if (at >= (int)rows.size()) at = (int)rows.size() - 1;
        selected = rows[at].entry.id;
        scroll_to_selected = true;
    }

    void start_refresh() {
        if (worker.busy()) return;
        worker.job = Worker::Job::Refresh;
        worker.finished = false;
        worker.thread = std::thread([this] {
            session.refresh();
            worker.finished = true;
            worker.wake();
        });
    }

    void start_fetch(const CatalogRow& target, bool boot_after) {
        if (worker.busy()) return;
        worker.job = Worker::Job::Fetch;
        worker.id = target.entry.id;
        worker.boot_after = boot_after;
        worker.finished = false;
        worker.progress.phase = FetchPhase::Checking;
        worker.progress.transfer.now = 0;
        worker.progress.transfer.total = target.entry.size;
        worker.progress.transfer.cancel = false;
        say("");
        IndexEntry entry = target.entry;
        worker.thread = std::thread([this, entry] {
            worker.result = fetch_image(session.state, entry, worker.progress, session.http);
            worker.finished = true;
            worker.wake();
        });
    }

    // Called on the UI thread once the worker has finished its job.
    void job_finished() {
        worker.thread.join();
        Worker::Job job = worker.job;
        worker.job = Worker::Job::None;

        if (job == Worker::Job::Refresh) {
            refreshed = true;
            reload();
            return;
        }

        reload();
        const FetchResult& result = worker.result;
        if (result.ok()) {
            say(result.replaced ? "Image updated; its saved changes were discarded." : "Download complete.");
            if (worker.boot_after) boot(worker.id);
        } else if (result.error == FetchError::Cancelled) {
            say("Download cancelled. It will resume where it stopped.");
        } else {
            say(result.message, true);
        }
    }

    bool ssh_port_value(std::string& value) {
        if (!ssh_enabled) {
            value = "off";
            return true;
        }
        int port = atoi(ssh_port);
        if (port < 1 || port > 65535) return false;
        value = std::to_string(port);
        return true;
    }

    void boot(const std::string& id) {
        const CatalogRow* target = row(id);
        if (!target) return;
        std::string port;
        if (!ssh_port_value(port)) {
            say("The SSH port must be between 1 and 65535.", true);
            return;
        }
        if (!target->downloaded()) {
            start_fetch(*target, true);
            return;
        }
        Settings settings = session.state.load_settings();
        settings.last_id = id;
        settings.ssh_port = port;
        session.state.save_settings(settings);
        printf("id=%s\nssh_port=%s\n", id.c_str(), port.c_str());
        fflush(stdout);
        exit_code = kExitOk;
        done = true;
    }

    void ask(Confirm what, const std::string& id) {
        confirm = what;
        confirm_id = id;
        open_confirm = true;
    }

    // The Download / Update button.
    void download(const CatalogRow& target) {
        if (target.status == ImageStatus::UpdateAvailable && target.has_state)
            ask(Confirm::Update, target.entry.id);
        else
            start_fetch(target, false);
    }

    void confirmed() {
        std::string error;
        const CatalogRow* target = row(confirm_id);
        switch (confirm) {
            case Confirm::Delete:
                if (session.state.remove_image(confirm_id, error)) say("Image deleted.");
                else say(error, true);
                reload();
                break;
            case Confirm::Reset:
                if (session.state.reset_image(confirm_id, error)) say("Saved changes discarded.");
                else say(error, true);
                reload();
                break;
            case Confirm::Update:
                if (target) start_fetch(*target, false);
                break;
            case Confirm::None:
                break;
        }
        confirm = Confirm::None;
    }

    void draw();
    void draw_table(float height);
    void draw_buttons();
    void draw_confirm();
    void handle_keys();
};

void Ui::draw_table(float height) {
    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerH |
                            ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("images", 4, flags, ImVec2(0, height))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Image", ImGuiTableColumnFlags_WidthStretch, 1.1f);
    ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch, 1.3f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000 MB").x);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableHeadersRow();

    for (const auto& r : rows) {
        const std::string& id = r.entry.id;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGuiSelectableFlags sel = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
                                   ImGuiSelectableFlags_AllowOverlap;
        ImGui::PushID(id.c_str());
        if (ImGui::Selectable(r.entry.label.c_str(), selected == id, sel)) {
            selected = id;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !worker.busy()) boot(id);
        }
        if (selected == id && scroll_to_selected) {
            ImGui::SetScrollHereY();
            scroll_to_selected = false;
        }
        ImGui::PopID();

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r.entry.version.empty() ? "-" : r.entry.version.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r.entry.size ? human_size(r.entry.size).c_str() : "-");

        ImGui::TableNextColumn();
        if (worker.fetching(id)) {
            FetchPhase phase = worker.progress.phase;
            uint64_t now = worker.progress.transfer.now, total = worker.progress.transfer.total;
            float fraction = total ? (float)((double)now / (double)total) : 0.0f;
            char overlay[64];
            if (phase == FetchPhase::Verifying)
                snprintf(overlay, sizeof overlay, "Verifying %d%%", (int)(fraction * 100));
            else if (phase == FetchPhase::Downloading && total)
                snprintf(overlay, sizeof overlay, "%d%%  %s / %s", (int)(fraction * 100), human_size(now).c_str(),
                         human_size(total).c_str());
            else if (phase == FetchPhase::Downloading)
                snprintf(overlay, sizeof overlay, "%s", human_size(now).c_str());
            else
                snprintf(overlay, sizeof overlay, "Connecting...");
            ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), overlay);
        } else if (r.status == ImageStatus::UpdateAvailable) {
            ImGui::TextColored(kWarning, "%s", status_text(r.status));
        } else if (r.status == ImageStatus::NotDownloaded && r.partial) {
            ImGui::TextColored(kDim, "Download paused (%s)", human_size(r.partial).c_str());
        } else if (r.status == ImageStatus::NotDownloaded || r.status == ImageStatus::Unpublished) {
            ImGui::TextColored(kDim, "%s", status_text(r.status));
        } else {
            ImGui::TextUnformatted(r.has_state ? "Downloaded (changed)" : "Downloaded");
        }
    }
    ImGui::EndTable();
}

void Ui::draw_buttons() {
    const CatalogRow* target = current();
    bool fetching = worker.job == Worker::Job::Fetch;
    bool fetching_this = target && worker.fetching(target->entry.id);

    ImGui::BeginDisabled(!target || worker.busy());
    if (ImGui::Button(target && !target->downloaded() ? "Download & boot" : "Boot") && target) boot(target->entry.id);
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (fetching) {
        if (ImGui::Button("Cancel download")) worker.progress.transfer.cancel = true;
    } else {
        bool can = target && target->status != ImageStatus::Downloaded && target->status != ImageStatus::Unpublished;
        ImGui::BeginDisabled(!can || worker.busy());
        if (ImGui::Button(target && target->status == ImageStatus::UpdateAvailable ? "Update" : "Download") && target)
            download(*target);
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!target || !target->has_state || fetching_this);
    if (ImGui::Button("Discard changes") && target) ask(Confirm::Reset, target->entry.id);
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!target || !(target->downloaded() || target->partial) || fetching_this);
    if (ImGui::Button("Delete") && target) ask(Confirm::Delete, target->entry.id);
    ImGui::EndDisabled();

    float quit_width = ImGui::CalcTextSize("Quit").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - quit_width);
    if (ImGui::Button("Quit")) done = true;

    ImGui::Checkbox("SSH to the guest on localhost port", &ssh_enabled);
    ImGui::SameLine();
    ImGui::BeginDisabled(!ssh_enabled);
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("000000").x + ImGui::GetStyle().FramePadding.x * 2);
    ImGui::InputText("##ssh_port", ssh_port, sizeof ssh_port, ImGuiInputTextFlags_CharsDecimal);
    ImGui::EndDisabled();
}

void Ui::draw_confirm() {
    if (open_confirm) {
        ImGui::OpenPopup("Confirm");
        open_confirm = false;
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::BeginPopupModal("Confirm", nullptr, flags)) return;

    const CatalogRow* target = row(confirm_id);
    std::string name = target ? target->entry.label : confirm_id;
    const char* action = "OK";
    switch (confirm) {
        case Confirm::Delete:
            ImGui::Text("Delete \"%s\"?", name.c_str());
            ImGui::TextUnformatted(target && target->has_state
                                       ? "The image and the changes you made in it will be removed."
                                       : target && !target->downloaded()
                                             ? "The unfinished download will be removed."
                                             : "The downloaded image will be removed.");
            action = "Delete";
            break;
        case Confirm::Reset:
            ImGui::Text("Discard the changes made in \"%s\"?", name.c_str());
            ImGui::TextUnformatted("The image goes back to how it was downloaded.");
            action = "Discard";
            break;
        case Confirm::Update:
            ImGui::Text("Update \"%s\"?", name.c_str());
            ImGui::TextUnformatted("The changes you made in this image will be discarded.");
            action = "Update";
            break;
        case Confirm::None:
            break;
    }
    ImGui::Spacing();
    bool yes = ImGui::Button(action) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    ImGui::SameLine();
    bool no = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (yes) confirmed();
    if (yes || no) {
        confirm = Confirm::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Ui::handle_keys() {
    if (ImGui::GetIO().WantTextInput) return;
    const CatalogRow* target = current();
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move_selection(-1);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move_selection(1);
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) move_selection(-(int)rows.size());
    if (ImGui::IsKeyPressed(ImGuiKey_End)) move_selection((int)rows.size());
    if ((ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) && target &&
        !worker.busy())
        boot(target->entry.id);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && target && (target->downloaded() || target->partial) &&
        !worker.fetching(target->entry.id))
        ask(Confirm::Delete, target->entry.id);
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) start_refresh();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) done = true;
}

void Ui::draw() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("launcher", nullptr, flags);

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Calculinux Emulator %s", session.config.app_version.c_str());
    float refresh_width = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - refresh_width);
    ImGui::BeginDisabled(worker.busy());
    if (ImGui::Button("Refresh")) start_refresh();
    ImGui::EndDisabled();

    if (worker.job == Worker::Job::Refresh)
        ImGui::TextColored(kDim, "Checking for published images...");
    else if (!session.online && refreshed)
        ImGui::TextColored(kWarning, "Offline: the image list could not be fetched. Showing downloaded images.");
    else
        ImGui::TextColored(kDim, "Choose a system image to boot. Enter boots the selected one.");

    // Table fills what the two button rows and the message line leave.
    float line = ImGui::GetFrameHeightWithSpacing();
    float footer = line * 2 + ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    float height = ImGui::GetContentRegionAvail().y - footer;
    if (height < line * 3) height = line * 3;
    if (rows.empty() && worker.job != Worker::Job::Refresh) {
        ImGui::BeginChild("empty", ImVec2(0, height), ImGuiChildFlags_Borders);
        ImGui::TextWrapped(session.online ? "No images have been published yet."
                                          : "No images are downloaded, and the image list could not be fetched. "
                                            "Check the network connection and press Refresh.");
        ImGui::EndChild();
    } else {
        draw_table(height);
    }

    draw_buttons();
    if (!message.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(message_is_error ? kError : kDim, "%s", message.c_str());
        ImGui::PopTextWrapPos();
    }

    // A key that answers the dialog must not also act on the list.
    bool dialog_open = confirm != Confirm::None;
    draw_confirm();
    if (!dialog_open) handle_keys();
    ImGui::End();
}

void save_screenshot(SDL_Renderer* renderer, const char* path) {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!shot) return;
    if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, shot->pixels, shot->pitch) == 0)
        SDL_SaveBMP(shot, path);
    SDL_FreeSurface(shot);
}

// Test hook: CALCULINUX_LAUNCHER_SMOKE=N draws N frames and closes the
// window; CALCULINUX_LAUNCHER_SMOKE_KEYS=down,enter,... presses those keys
// first, one at a time, each once the worker is idle again.
struct Smoke {
    int frames = 0;
    std::vector<SDL_Keycode> keys;
    size_t next_key = 0;
    int cooldown = 0;

    bool active() const { return frames > 0; }
};

Smoke read_smoke() {
    Smoke smoke;
    if (const char* env = getenv("CALCULINUX_LAUNCHER_SMOKE")) smoke.frames = atoi(env);
    const char* keys = getenv("CALCULINUX_LAUNCHER_SMOKE_KEYS");
    std::string name;
    for (const char* c = keys ? keys : ""; smoke.active(); c++) {
        if (*c && *c != ',') {
            name += *c;
            continue;
        }
        if (name == "up") smoke.keys.push_back(SDLK_UP);
        else if (name == "down") smoke.keys.push_back(SDLK_DOWN);
        else if (name == "enter") smoke.keys.push_back(SDLK_RETURN);
        else if (name == "delete") smoke.keys.push_back(SDLK_DELETE);
        else if (name == "escape") smoke.keys.push_back(SDLK_ESCAPE);
        else if (name == "f5") smoke.keys.push_back(SDLK_F5);
        name.clear();
        if (!*c) break;
    }
    return smoke;
}

void press_key(SDL_Window* window, SDL_Keycode key) {
    SDL_Event event;
    SDL_zero(event);
    event.key.windowID = SDL_GetWindowID(window);
    event.key.keysym.sym = key;
    event.key.keysym.scancode = SDL_GetScancodeFromKey(key);
    event.type = SDL_KEYDOWN;
    event.key.state = SDL_PRESSED;
    SDL_PushEvent(&event);
    event.type = SDL_KEYUP;
    event.key.state = SDL_RELEASED;
    SDL_PushEvent(&event);
}

float ui_scale() {
    if (const char* env = getenv("CALCULINUX_EMULATOR_LAUNCHER_SCALE")) {
        float scale = (float)atof(env);
        if (scale >= 0.5f && scale <= 4.0f) return scale;
    }
    float scale = ImGui_ImplSDL2_GetContentScaleForDisplay(0);
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 3.0f) scale = 3.0f;
    return scale;
}

void apply_style(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    // Square corners and no anti-aliasing keep the geometry to axis-aligned
    // rectangles, which the software renderer fills without rasterising
    // triangles.
    style.WindowRounding = style.ChildRounding = style.FrameRounding = style.PopupRounding = 0.0f;
    style.ScrollbarRounding = style.GrabRounding = style.TabRounding = 0.0f;
    style.AntiAliasedLines = false;
    style.AntiAliasedLinesUseTex = false;
    style.AntiAliasedFill = false;
    style.WindowBorderSize = 0.0f;
    style.WindowPadding = ImVec2(12, 10);
    style.FramePadding = ImVec2(10, 5);
    style.ItemSpacing = ImVec2(8, 7);
    style.CellPadding = ImVec2(8, 5);
    style.ScaleAllSizes(scale);
    style.FontSizeBase = 15.0f;
    style.FontScaleDpi = scale;
}

}  // namespace

int run_gui(Session& session, const std::vector<std::string>& args) {
    std::string ssh_arg;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--ssh-port" && i + 1 < args.size()) {
            ssh_arg = args[++i];
        } else {
            fprintf(stderr, "calculinux-emulator: gui: unknown option %s\n", args[i].c_str());
            return kExitUsage;
        }
    }

    Smoke smoke = read_smoke();

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "calculinux-emulator: cannot open a window: %s\n", SDL_GetError());
        return kExitNoDisplay;
    }
    float scale = ui_scale();
    std::string title = "Calculinux Emulator " + session.config.app_version;
    SDL_Window* window =
        SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int)(kWidth * scale),
                         (int)(kHeight * scale), SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE) : nullptr;
    if (!renderer) {
        fprintf(stderr, "calculinux-emulator: cannot open a window: %s\n", SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return kExitNoDisplay;
    }
    SDL_SetWindowMinimumSize(window, (int)(520 * scale), (int)(320 * scale));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // A blinking cursor would keep the window redrawing while idle.
    io.ConfigInputTextCursorBlink = false;
    io.Fonts->AddFontDefaultVector();
    apply_style(scale);
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    Ui ui(session);
    ui.worker.wake_event = SDL_RegisterEvents(1);
    Settings settings = session.state.load_settings();
    std::string port = !ssh_arg.empty() ? ssh_arg : settings.ssh_port;
    if (port == "off") {
        ui.ssh_enabled = false;
    } else if (!port.empty() && atoi(port.c_str()) > 0 && atoi(port.c_str()) <= 65535) {
        snprintf(ui.ssh_port, sizeof ui.ssh_port, "%d", atoi(port.c_str()));
    }

    // Show what is already on disk at once; the index arrives in the background.
    session.load_cached();
    ui.reload();
    ui.start_refresh();

    // Redraw only when something happens: ImGui needs a few frames to
    // settle after an event, then the loop blocks until the next one.
    int frames_wanted = 3;
    while (!ui.done) {
        SDL_Event event;
        bool have_event;
        if (frames_wanted > 0) have_event = SDL_PollEvent(&event);
        else if (ui.worker.busy()) have_event = SDL_WaitEventTimeout(&event, 250);  // progress bar
        else if (smoke.active()) have_event = SDL_WaitEventTimeout(&event, 10);
        else have_event = SDL_WaitEvent(&event);
        while (have_event) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) ui.done = true;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window))
                ui.done = true;
            frames_wanted = 3;
            have_event = SDL_PollEvent(&event);
        }
        if (ui.worker.finished.exchange(false)) {
            ui.job_finished();
            frames_wanted = 3;
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ui.draw();
        ImGui::Render();
        SDL_RenderSetScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);

        if (frames_wanted > 0) frames_wanted--;
        if (smoke.active() && !ui.done) {
            // Wait for the worker (index refresh, download) before the next step.
            bool idle = !ui.worker.busy();
            if (idle && smoke.cooldown > 0) {
                smoke.cooldown--;
            } else if (idle && smoke.next_key < smoke.keys.size()) {
                press_key(window, smoke.keys[smoke.next_key++]);
                smoke.cooldown = 5;
            } else if (idle && --smoke.frames == 0) {
                if (const char* path = getenv("CALCULINUX_LAUNCHER_SCREENSHOT")) save_screenshot(renderer, path);
                ui.done = true;
            }
        }
        SDL_RenderPresent(renderer);
    }

    // A download in progress is abandoned (it resumes next time); do not
    // wait for the network to notice.
    ui.worker.progress.transfer.cancel = true;
    bool worker_running = ui.worker.busy() && !ui.worker.finished;
    if (ui.worker.thread.joinable() && !worker_running) ui.worker.thread.join();

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    if (worker_running) {
        fflush(stdout);
        fflush(stderr);
        _exit(ui.exit_code);
    }
    return ui.exit_code;
}

}  // namespace cl
