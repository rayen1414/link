// AppMain.cpp
// Include order is critical:
//   1. Define NOUSER + NOGDI + WIN32_LEAN_AND_MEAN so windows.h skips winuser.h
//      (which is the source of all CloseWindow/ShowCursor/DrawText conflicts).
//   2. Include raylib (it now owns those names safely).
//   3. Clipboard uses clipboard_helper.cpp which includes full windows in its own TU.

#define NOUSER
#define NOGDI
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>   // must come before windows.h
#include <windows.h>

#include "raylib.h"
#include "raymath.h"
#include "host_logic.h"
#include "clipboard_helper.h"

#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <cmath>
#include <ctime>
#include <algorithm>

typedef enum { SCREEN_DASHBOARD, SCREEN_HOST_SETUP, SCREEN_RECEIVER_VIEW, SCREEN_STREAMING } AppScreen;

AppScreen currentScreen = SCREEN_DASHBOARD;
int targetBitrate = 12000;
std::string sessionCode = "";
std::string receiverInputCode = "";
Font mangaFont;

// ─── Glitch / FX State ──────────────────────────────────────────────────────
static float  g_appTimer      = 0.0f;
static bool   g_glitchActive  = false;
static float  g_glitchTimer   = 0.0f;
static Shader g_glitchShader;
static int    g_glitchTimeLoc = -1;
static RenderTexture2D g_canvas;
static bool   g_shadersReady  = false;

// ─── Session stats ───────────────────────────────────────────────────────────
static float g_sessionTimer   = 0.0f;
static bool  g_sessionRunning = false;

// ─── Notifications ───────────────────────────────────────────────────────────
struct Notif { std::string text; float life; float maxLife; Color col; };
static std::vector<Notif> g_notifs;
static void PushNotif(const std::string& txt, Color col = {20,20,20,255}, float dur = 2.5f) {
    g_notifs.push_back({txt, dur, dur, col});
}

// ─── Clipboard ───────────────────────────────────────────────────────────────
static void CopyToClipboard(const std::string& txt) {
    Clipboard_Set(txt.c_str());
}

static std::string PasteFromClipboard() {
    char* p = Clipboard_Get();
    if (!p) return {};
    std::string s(p);
    free(p);
    return s;
}

// ─── Misc helpers ────────────────────────────────────────────────────────────
static std::string FormatTime(float secs) {
    int s = (int)secs % 60;
    int m = ((int)secs / 60) % 60;
    int h = (int)secs / 3600;
    char buf[32]; snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    return buf;
}

// ─── UI helpers ──────────────────────────────────────────────────────────────
bool DrawMangaButton(Rectangle rect, const char* text, bool active = false) {
    Vector2 mouse = GetMousePosition();
    bool hovered = CheckCollisionPointRec(mouse, rect);
    DrawRectangleRec({rect.x + 6, rect.y + 6, rect.width, rect.height}, BLACK);
    DrawRectangleRec(rect, (hovered || active) ? BLACK : WHITE);
    DrawRectangleLinesEx(rect, 4, BLACK);
    Color txtColor = (hovered || active) ? WHITE : BLACK;
    Vector2 textSize = MeasureTextEx(mangaFont, text, 32, 2);
    DrawTextEx(mangaFont, text,
        {rect.x + (rect.width - textSize.x) / 2.f, rect.y + (rect.height - textSize.y) / 2.f},
        32, 2, txtColor);
    return (hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON));
}

void DrawStatusBadge(float x, float y, const char* label, Color col) {
    Vector2 sz = MeasureTextEx(mangaFont, label, 22, 2);
    DrawRectangle((int)x, (int)y, (int)sz.x + 20, 34, col);
    DrawRectangleLinesEx({x, y, sz.x + 20, 34}, 3, BLACK);
    DrawTextEx(mangaFont, label, {x + 10, y + 6}, 22, 2, WHITE);
}

// ─── Glitch scanline overlay ─────────────────────────────────────────────────
static void DrawGlitchBars() {
    for (int i = 0; i < 5; i++) {
        int y = GetRandomValue(0, 720);
        int h = GetRandomValue(2, 10);
        DrawRectangle(0, y, 1280, h, {255,255,255,(unsigned char)GetRandomValue(30,120)});
    }
    for (int i = 0; i < 2; i++) {
        int y = GetRandomValue(0, 720);
        DrawRectangle(0, y,   1280, GetRandomValue(1, 3), {255, 0,  60,  50});
        DrawRectangle(0, y+2, 1280, GetRandomValue(1, 2), {0, 200, 255,  40});
    }
}

// ─── Notification draw ───────────────────────────────────────────────────────
static void DrawNotifications() {
    float yOff = 668.0f;
    for (int i = (int)g_notifs.size() - 1; i >= 0; i--) {
        auto& n = g_notifs[i];
        float t    = n.life / n.maxLife;
        float alph = (t > 0.15f) ? 1.0f : (t / 0.15f);
        float slide = (1.0f - Clamp((n.maxLife - n.life) / 0.2f, 0.f, 1.f)) * 50.f;

        Vector2 sz = MeasureTextEx(mangaFont, n.text.c_str(), 22, 2);
        float bw = sz.x + 24, bh = 34;
        float bx = 1280.f - bw - 20 + slide;

        Color bg = n.col; bg.a = (unsigned char)(215 * alph);
        Color fg = WHITE;  fg.a = (unsigned char)(255 * alph);

        DrawRectangleRec({bx, yOff - bh, bw, bh}, bg);
        DrawRectangleLinesEx({bx, yOff - bh, bw, bh}, 2, {0,0,0,(unsigned char)(180*alph)});
        DrawTextEx(mangaFont, n.text.c_str(), {bx + 12, yOff - bh + 6}, 22, 2, fg);
        yOff -= bh + 5;
    }
}

// ─── Station code ─────────────────────────────────────────────────────────────
std::string FetchStationCode() {
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    char sz[255] = {};
    if (gethostname(sz, sizeof(sz)) != 0) return "LN-00-00";
    struct hostent* h = gethostbyname(sz);
    if (!h) return "LN-00-00";
    return IPToCode(inet_ntoa(*(struct in_addr*)*h->h_addr_list));
}

// ═════════════════════════════════════════════════════════════════════════════
int main() {
    SetConfigFlags(FLAG_MSAA_4X_HINT);
    InitWindow(1280, 720, "LINK | STASIS_P2P");
    SetTargetFPS(60);
    mangaFont = LoadFontEx("KOMIKAX_.ttf", 128, 0, 0);

    // Post-process glitch shader
    g_glitchShader  = LoadShader(nullptr, "glitch.fs");
    g_glitchTimeLoc = GetShaderLocation(g_glitchShader, "time");
    g_canvas        = LoadRenderTexture(1280, 720);
    g_shadersReady  = true;

    // GIF
    int animFrames = 0;
    Image gifImg     = LoadImageAnim("gif.gif", &animFrames);
    Texture2D gifTex = LoadTextureFromImage(gifImg);
    int   gifFrame   = 0;
    float gifTimer   = 0.0f;
    float gifCycle   = 0.0f;
    // States: FROZEN(25s) -> GLITCH(1.5s) -> PLAYING(2s) -> FROZEN(25s) -> ...
    // gifState: 0=frozen, 1=glitching+playing, 2=playing only
    int   gifState   = 0;      // starts frozen
    const float GIF_FREEZE_DUR = 15.0f;
    const float GLITCH_DUR     = 5.0f;
    const float GIF_PLAY_DUR   = 2.0f;

    sessionCode = FetchStationCode();
    SetRandomSeed((unsigned int)time(nullptr));

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        g_appTimer += dt;

        // ── GIF cycle: FROZEN(25s) → GLITCH+PLAY(2s) → FROZEN(25s) → ...
        gifCycle += dt;

        if (gifState == 0) {
            // FROZEN: wait 25s then trigger glitch+play
            if (gifCycle >= GIF_FREEZE_DUR) {
                gifCycle       = 0.0f;
                gifState       = 1;
                gifFrame       = 0;
                gifTimer       = 0.0f;
                g_glitchActive = true;
                g_glitchTimer  = 0.0f;
                PushNotif("!! SIGNAL DEGRADED !!", {200, 0, 30, 255}, 3.0f);
            }
        } else if (gifState == 1) {
            // GLITCH + PLAYING: run glitch for GLITCH_DUR then just play
            g_glitchTimer += dt;
            if (g_glitchTimer >= GLITCH_DUR) {
                g_glitchActive = false;
                g_glitchTimer  = 0.0f;
                gifState       = 2;
                PushNotif("SIGNAL RESTORED", {0, 140, 0, 255}, 2.0f);
            }
            // Advance GIF
            gifTimer += dt;
            if (gifTimer >= 0.08f && animFrames > 1) {
                gifTimer = 0.0f;
                gifFrame = (gifFrame + 1) % animFrames;
                UpdateTexture(gifTex,
                    ((unsigned char*)gifImg.data) +
                    (gifFrame * gifImg.width * gifImg.height * 4));
            }
        } else if (gifState == 2) {
            // PLAYING only: play for GIF_PLAY_DUR then freeze again
            if (gifCycle >= GIF_PLAY_DUR) {
                gifCycle = 0.0f;
                gifState = 0;
                gifFrame = 0;   // reset to frame 0 when freezing
                gifTimer = 0.0f;
                UpdateTexture(gifTex,
                    ((unsigned char*)gifImg.data) + 0); // show frame 0
            }
            gifTimer += dt;
            if (gifTimer >= 0.08f && animFrames > 1) {
                gifTimer = 0.0f;
                gifFrame = (gifFrame + 1) % animFrames;
                UpdateTexture(gifTex,
                    ((unsigned char*)gifImg.data) +
                    (gifFrame * gifImg.width * gifImg.height * 4));
            }
        }

        // ── Session timer ─────────────────────────────────────────────────
        if (g_sessionRunning) g_sessionTimer += dt;

        // ── Notification lifecycle ────────────────────────────────────────
        for (auto& n : g_notifs) n.life -= dt;
        g_notifs.erase(
            std::remove_if(g_notifs.begin(), g_notifs.end(),
                [](const Notif& n){ return n.life <= 0.f; }),
            g_notifs.end());

        // ══ RENDER to offscreen canvas ════════════════════════════════════
        BeginTextureMode(g_canvas);
        ClearBackground(WHITE);

        if (g_glitchActive && GetRandomValue(0, 2) == 0) DrawGlitchBars();

        DrawRectangleLinesEx({15, 15, 1250, 690}, 10, BLACK);

        Vector2 mouse = GetMousePosition();

        // ─────────────────── SCREEN_DASHBOARD ────────────────────────────
        if (currentScreen == SCREEN_DASHBOARD) {
            float ox = g_glitchActive ? sinf(g_glitchTimer * 47.3f) * 4.f : 0.f;
            DrawTextEx(mangaFont, "LINK", {80 + ox, 60}, 180, 5, BLACK);
            if (g_glitchActive) {
                DrawTextEx(mangaFont, "LINK", {80 + ox + 4, 60}, 180, 5, {200,0,40,55});
                DrawTextEx(mangaFont, "LINK", {80 + ox - 4, 60}, 180, 5, {0,200,255,55});
            }

            if (DrawMangaButton({80, 300, 400, 80}, "HOST STATION"))
                currentScreen = SCREEN_HOST_SETUP;

            if (DrawMangaButton({80, 400, 400, 80}, "RECEIVE LINK")) {
                currentScreen = SCREEN_RECEIVER_VIEW;
                StartDiscoveryListener();
            }

            if (DrawMangaButton({80, 500, 400, 80}, "QUICK HOST")) {
                g_sessionTimer = 0.f; g_sessionRunning = true;
                currentScreen = SCREEN_STREAMING;
                std::thread(RunHostLogic, targetBitrate, std::ref(sessionCode)).detach();
                PushNotif("QUICK HOST  " + std::to_string(targetBitrate/1000) + " KBPS", {0,120,0,255});
            }

            if (DrawMangaButton({80, 610, 250, 60}, "TERMINATE")) break;

            // Station code badge — click to copy
            {
                std::string sc = "STATION  " + sessionCode;
                Vector2 sz = MeasureTextEx(mangaFont, sc.c_str(), 22, 2);
                float bx = 1280.f - sz.x - 40, by = 24;
                Rectangle badge = {bx, by, sz.x + 24, 36};
                DrawRectangleRec(badge, BLACK);
                DrawTextEx(mangaFont, sc.c_str(), {bx + 12, by + 8}, 22, 2, WHITE);
                if (CheckCollisionPointRec(mouse, badge)) {
                    DrawTextEx(mangaFont, "click to copy", {bx, by + 40}, 16, 1, GRAY);
                    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                        CopyToClipboard(sessionCode);
                        PushNotif("CODE COPIED!", {30,30,30,255}, 1.5f);
                    }
                }
            }

            float scale = 560.f / (float)gifTex.height;
            float gifW  = gifTex.width * scale;
            float gifX  = 1240.f - gifW;   // right-align to x=1240, away from badge
            DrawTextureEx(gifTex, {gifX, 80}, 0.f, scale, WHITE);
            if (gifState == 1) {
                DrawStatusBadge(gifX + 4, 140, "SIGNAL LOST", {180,0,0,255});
            }
        }

        // ─────────────────── SCREEN_HOST_SETUP ───────────────────────────
        else if (currentScreen == SCREEN_HOST_SETUP) {
            Rectangle panel = {390, 120, 500, 500};
            DrawRectangleRec({panel.x+6, panel.y+6, panel.width, panel.height}, {200,200,200,100});
            DrawRectangleLinesEx(panel, 6, BLACK);

            DrawTextEx(mangaFont, "STATION CODE", {panel.x+30, panel.y+24}, 28, 2, BLACK);
            DrawTextEx(mangaFont, sessionCode.c_str(), {panel.x+30, panel.y+62}, 80, 5, BLACK);

            if (DrawMangaButton({panel.x+30, panel.y+150, 190, 44}, "COPY CODE")) {
                CopyToClipboard(sessionCode);
                PushNotif("CODE COPIED!", {30,30,30,255}, 1.5f);
            }

            DrawText(TextFormat("BITRATE: %d Kbps", targetBitrate), panel.x+30, panel.y+212, 20, BLACK);
            DrawText("higher = sharper, more bandwidth", panel.x+30, panel.y+234, 14, GRAY);

            if (DrawMangaButton({panel.x+30,  panel.y+254, 90,  46}, "-1K"))
                targetBitrate = (targetBitrate > 1000) ? targetBitrate - 1000 : 1000;
            if (DrawMangaButton({panel.x+130, panel.y+254, 90,  46}, "+1K"))
                targetBitrate += 1000;
            if (DrawMangaButton({panel.x+230, panel.y+254, 90,  46}, "+5K"))
                targetBitrate += 5000;
            if (DrawMangaButton({panel.x+330, panel.y+254, 100, 46}, "RESET"))
                targetBitrate = 12000;

            // Bitrate bar
            {
                float pct = Clamp((float)targetBitrate / 50000.f, 0.f, 1.f);
                Color bc = (targetBitrate > 25000) ? RED : (targetBitrate > 12000 ? ORANGE : GREEN);
                DrawRectangle(panel.x+30, panel.y+312, 440, 12, LIGHTGRAY);
                DrawRectangle(panel.x+30, panel.y+312, (int)(440*pct), 12, bc);
                DrawRectangleLinesEx({(float)panel.x+30, (float)panel.y+312, 440, 12}, 2, BLACK);
            }

            if (DrawMangaButton({panel.x+30,  panel.y+342, 200, 68}, "INITIALIZE")) {
                g_sessionTimer = 0.f; g_sessionRunning = true;
                currentScreen = SCREEN_STREAMING;
                std::thread(RunHostLogic, targetBitrate, std::ref(sessionCode)).detach();
            }
            if (DrawMangaButton({panel.x+250, panel.y+342, 200, 68}, "BACK"))
                currentScreen = SCREEN_DASHBOARD;
        }

        // ─────────────────── SCREEN_RECEIVER_VIEW ────────────────────────
        else if (currentScreen == SCREEN_RECEIVER_VIEW) {
            DrawTextEx(mangaFont, "AVAILABLE LINKS", {80, 55}, 76, 2, BLACK);

            std::vector<std::string> hosts = GetDiscoveredHosts();
            if (hosts.empty()) {
                int dots = ((int)(g_appTimer * 2.5f)) % 4;
                std::string scan = "SCANNING";
                for (int d = 0; d < dots; d++) scan += ".";
                DrawTextEx(mangaFont, scan.c_str(), {80, 160}, 34, 2, GRAY);
            }
            for (int i = 0; i < (int)hosts.size() && i < 5; i++) {
                if (DrawMangaButton({80, 155.f + i * 72.f, 380, 62}, hosts[i].c_str())) {
                    receiverInputCode = hosts[i];
                    StopDiscoveryListener();
                    g_sessionTimer = 0.f; g_sessionRunning = true;
                    currentScreen = SCREEN_STREAMING;
                    std::thread(RunReceiverLogic, receiverInputCode).detach();
                }
            }

            DrawText("MANUAL CONNECTION:", 580, 145, 20, BLACK);
            Rectangle inputRect = {580, 180, 400, 78};
            DrawRectangleLinesEx(inputRect, 4, BLACK);

            // Auto-format LN-XX-XX as user types
            int key = GetCharPressed();
            while (key > 0) {
                if (receiverInputCode.size() < 9) {
                    char c = (char)toupper(key);
                    size_t len = receiverInputCode.size();
                    if ((len == 2 || len == 5) && c != '-') receiverInputCode += '-';
                    receiverInputCode += c;
                }
                key = GetCharPressed();
            }
            if (IsKeyPressed(KEY_BACKSPACE) && !receiverInputCode.empty())
                receiverInputCode.pop_back();

            DrawTextEx(mangaFont, receiverInputCode.c_str(),
                {inputRect.x + 18, inputRect.y + 14}, 48, 2, BLACK);

            if (DrawMangaButton({580, 270, 185, 48}, "PASTE CODE")) {
                std::string cl = PasteFromClipboard();
                for (auto& c : cl) c = (char)toupper((unsigned char)c);
                if (cl.size() >= 9 && cl.substr(0,3) == "LN-")
                    receiverInputCode = cl.substr(0, 9);
                PushNotif("CODE PASTED!", {30,30,30,255}, 1.5f);
            }
            if (DrawMangaButton({778, 270, 185, 48}, "CONNECT") ||
                (IsKeyPressed(KEY_ENTER) && !receiverInputCode.empty())) {
                StopDiscoveryListener();
                g_sessionTimer = 0.f; g_sessionRunning = true;
                currentScreen = SCREEN_STREAMING;
                std::thread(RunReceiverLogic, receiverInputCode).detach();
            }

            DrawText("or press ENTER", 580, 330, 16, GRAY);

            if (DrawMangaButton({80, 630, 200, 56}, "BACK")) {
                StopDiscoveryListener();
                currentScreen = SCREEN_DASHBOARD;
            }
        }

        // ─────────────────── SCREEN_STREAMING ────────────────────────────
        else if (currentScreen == SCREEN_STREAMING) {
            Rectangle panel = {330, 70, 620, 570};
            DrawRectangleRec({panel.x+6, panel.y+6, panel.width, panel.height}, {200,200,200,80});
            DrawRectangleLinesEx(panel, 6, BLACK);

            float ox = g_glitchActive ? sinf(g_glitchTimer * 63.f) * 3.f : 0.f;
            DrawTextEx(mangaFont, "SESSION ACTIVE", {panel.x+28+ox, panel.y+22}, 40, 2, BLACK);
            if (g_glitchActive) {
                DrawTextEx(mangaFont, "SESSION ACTIVE", {panel.x+28+ox+3, panel.y+22}, 40, 2, {200,0,40,50});
                DrawTextEx(mangaFont, "SESSION ACTIVE", {panel.x+28+ox-3, panel.y+22}, 40, 2, {0,200,255,50});
            }

            DrawRectangleLinesEx({panel.x+28, panel.y+76, 564, 2}, 1, BLACK);

            DrawTextEx(mangaFont, "STATION", {panel.x+28, panel.y+90}, 20, 2, GRAY);
            {
                Vector2 csz = MeasureTextEx(mangaFont, sessionCode.c_str(), 58, 4);
                Rectangle cr = {panel.x+28, panel.y+113, csz.x, csz.y};
                DrawTextEx(mangaFont, sessionCode.c_str(), {panel.x+28, panel.y+113}, 58, 4, BLACK);
                if (CheckCollisionPointRec(mouse, cr)) {
                    DrawTextEx(mangaFont, "click to copy", {panel.x+28, panel.y+183}, 17, 1, GRAY);
                    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                        CopyToClipboard(sessionCode);
                        PushNotif("CODE COPIED!", {30,30,30,255}, 1.5f);
                    }
                }
            }

            DrawRectangleLinesEx({panel.x+28, panel.y+208, 564, 2}, 1, BLACK);

            DrawTextEx(mangaFont, "SESSION TIME", {panel.x+28, panel.y+222}, 20, 2, GRAY);
            DrawTextEx(mangaFont, FormatTime(g_sessionTimer).c_str(),
                {panel.x+28, panel.y+246}, 50, 3, BLACK);

            DrawRectangleLinesEx({panel.x+28, panel.y+308, 564, 2}, 1, BLACK);

            DrawTextEx(mangaFont, "REMOTE INPUT", {panel.x+28, panel.y+322}, 20, 2, GRAY);
            bool inputOn = g_inputEnabled.load();
            DrawStatusBadge(panel.x+28, panel.y+350,
                inputOn ? "INPUT ENABLED" : "INPUT DISABLED",
                inputOn ? Color{20,120,20,255} : Color{180,0,0,255});

            if (DrawMangaButton({panel.x+28, panel.y+396, 238, 52},
                inputOn ? "DISABLE INPUT" : "ENABLE INPUT")) {
                SetInputEnabled(!inputOn);
                PushNotif(inputOn ? "INPUT DISABLED" : "INPUT ENABLED",
                    inputOn ? Color{180,0,0,255} : Color{20,120,20,255});
            }

            if (DrawMangaButton({panel.x+280, panel.y+396, 238, 52}, "COPY CODE")) {
                CopyToClipboard(sessionCode);
                PushNotif("CODE COPIED!", {30,30,30,255}, 1.5f);
            }

            DrawRectangleLinesEx({panel.x+28, panel.y+462, 564, 2}, 1, BLACK);

            DrawText(TextFormat("BITRATE  %d Kbps", targetBitrate),
                panel.x+28, panel.y+474, 18, GRAY);

            if (DrawMangaButton({panel.x+28, panel.y+504, 220, 50}, "DISCONNECT")) {
                StopHostLogic();
                g_sessionRunning = false; g_sessionTimer = 0.f;
                currentScreen = SCREEN_DASHBOARD;
                PushNotif("SESSION ENDED", {180,0,0,255});
            }
            DrawTextEx(mangaFont, "ESC to exit", {panel.x+272, panel.y+516}, 20, 2, GRAY);

            if (IsKeyPressed(KEY_ESCAPE) || !g_sessionActive) {
                StopHostLogic();
                g_sessionRunning = false; g_sessionTimer = 0.f;
                currentScreen = SCREEN_DASHBOARD;
            }
        }

        // ── Notifications overlay ─────────────────────────────────────────
        DrawNotifications();

        // ── Random corrupted text glitch ──────────────────────────────────
        if (g_glitchActive && GetRandomValue(0, 100) == 0) {
            const char* noise[] = {
                "ERR_0xDEAD","SIGNAL_LOST","//CORRUPT//","NULL_REF","X_X_X","LINK_FAIL"
            };
            DrawTextEx(mangaFont, noise[GetRandomValue(0,5)],
                {(float)GetRandomValue(50,1050),(float)GetRandomValue(50,670)},
                28, 2, {200,0,40,150});
        }

        EndTextureMode();

        // ══ POST-PROCESS: draw canvas through glitch shader ═══════════════
        BeginDrawing();
        ClearBackground(WHITE);

        if (g_glitchActive && g_shadersReady) {
            SetShaderValue(g_glitchShader, g_glitchTimeLoc,
                &g_glitchTimer, SHADER_UNIFORM_FLOAT);
            BeginShaderMode(g_glitchShader);
        }

        // RenderTexture is Y-flipped — flip it back
        DrawTextureRec(g_canvas.texture,
            {0, 0, (float)g_canvas.texture.width, -(float)g_canvas.texture.height},
            {0, 0}, WHITE);

        if (g_glitchActive && g_shadersReady)
            EndShaderMode();

        EndDrawing();
    }

    UnloadFont(mangaFont);
    UnloadImage(gifImg);
    UnloadTexture(gifTex);
    if (g_shadersReady) {
        UnloadShader(g_glitchShader);
        UnloadRenderTexture(g_canvas);
    }
    CloseWindow();
    return 0;
}
