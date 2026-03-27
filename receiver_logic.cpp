#include "host_logic.h"
#include "receiver_logic.h"
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <SDL2/SDL.h>
#include <vector>
#include <string>
#include <atomic>
#include <cstring>
#include <cctype>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

#pragma comment(lib, "Ws2_32.lib")

static std::atomic<bool> g_inputAllowed(true);

struct GlyphDef { char ch; uint8_t rows[7]; };

static const GlyphDef g_glyphs[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x00}},
    {'B', {0x1E,0x11,0x11,0x1E,0x11,0x1E,0x00}},
    {'C', {0x0E,0x11,0x10,0x10,0x11,0x0E,0x00}},
    {'D', {0x1E,0x11,0x11,0x11,0x11,0x1E,0x00}},
    {'E', {0x1F,0x10,0x10,0x1E,0x10,0x1F,0x00}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x00}},
    {'G', {0x0E,0x11,0x10,0x17,0x11,0x0F,0x00}},
    {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x00}},
    {'I', {0x0E,0x04,0x04,0x04,0x04,0x0E,0x00}},
    {'J', {0x07,0x02,0x02,0x02,0x12,0x0C,0x00}},
    {'K', {0x11,0x12,0x14,0x18,0x14,0x13,0x00}},
    {'L', {0x10,0x10,0x10,0x10,0x10,0x1F,0x00}},
    {'M', {0x11,0x1B,0x15,0x11,0x11,0x11,0x00}},
    {'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x00}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x0E,0x00}},
    {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x00}},
    {'Q', {0x0E,0x11,0x11,0x15,0x12,0x0D,0x00}},
    {'R', {0x1E,0x11,0x11,0x1E,0x14,0x13,0x00}},
    {'S', {0x0F,0x10,0x10,0x0E,0x01,0x1E,0x00}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x00}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x0E,0x00}},
    {'V', {0x11,0x11,0x11,0x11,0x0A,0x04,0x00}},
    {'W', {0x11,0x11,0x11,0x15,0x1B,0x11,0x00}},
    {'X', {0x11,0x0A,0x04,0x04,0x0A,0x11,0x00}},
    {'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x00}},
    {'Z', {0x1F,0x01,0x02,0x04,0x08,0x1F,0x00}},
    {'0', {0x0E,0x13,0x15,0x19,0x11,0x0E,0x00}},
    {'1', {0x04,0x0C,0x04,0x04,0x04,0x0E,0x00}},
    {'2', {0x0E,0x11,0x01,0x06,0x08,0x1F,0x00}},
    {'3', {0x1F,0x02,0x04,0x02,0x11,0x0E,0x00}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x00}},
    {'5', {0x1F,0x10,0x1E,0x01,0x11,0x0E,0x00}},
    {'6', {0x06,0x08,0x10,0x1E,0x11,0x0E,0x00}},
    {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x00}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x0E,0x00}},
    {'9', {0x0E,0x11,0x11,0x0F,0x01,0x0E,0x00}},
    {':', {0x00,0x04,0x00,0x00,0x04,0x00,0x00}},
    {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'.', {0x00,0x00,0x00,0x00,0x00,0x04,0x00}},
    {'/', {0x01,0x02,0x04,0x08,0x10,0x00,0x00}},
    {'_', {0x00,0x00,0x00,0x00,0x00,0x1F,0x00}},
    {'|', {0x04,0x04,0x04,0x04,0x04,0x04,0x04}},
};
static const int g_glyphCount = (int)(sizeof(g_glyphs) / sizeof(g_glyphs[0]));

static const uint8_t* GetGlyph(char c) {
    c = (char)toupper((unsigned char)c);
    for (int i = 0; i < g_glyphCount; i++)
        if (g_glyphs[i].ch == c) return g_glyphs[i].rows;
    return g_glyphs[0].rows;
}

static void DrawChar(SDL_Renderer* ren, char c, int x, int y, int scale, SDL_Color col) {
    const uint8_t* rows = GetGlyph(c);
    SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, col.a);
    for (int row = 0; row < 7; row++) {
        for (int bit = 0; bit < 5; bit++) {
            if (rows[row] & (0x10 >> bit)) {
                SDL_Rect px = { x + bit * scale, y + row * scale, scale, scale };
                SDL_RenderFillRect(ren, &px);
            }
        }
    }
}

static void BitmapText(SDL_Renderer* ren, const char* text, int x, int y, int scale, SDL_Color col) {
    for (int i = 0; text[i]; i++)
        DrawChar(ren, text[i], x + i * (5 + 1) * scale, y, scale, col);
}

static int BitmapTextWidth(const char* text, int scale) {
    return (int)strlen(text) * (5 + 1) * scale;
}

static SDL_Rect MakeRect(int x, int y, int w, int h) {
    SDL_Rect r; r.x = x; r.y = y; r.w = w; r.h = h; return r;
}

static bool PointInRect(int px, int py, SDL_Rect r) {
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

static void DrawBox(SDL_Renderer* ren, SDL_Rect r, SDL_Color fill, SDL_Color border) {
    SDL_SetRenderDrawColor(ren, fill.r, fill.g, fill.b, fill.a);
    SDL_RenderFillRect(ren, &r);
    SDL_SetRenderDrawColor(ren, border.r, border.g, border.b, border.a);
    SDL_RenderDrawRect(ren, &r);
    SDL_Rect inner = { r.x+1, r.y+1, r.w-2, r.h-2 };
    SDL_RenderDrawRect(ren, &inner);
}

static void DrawButton(SDL_Renderer* ren, SDL_Rect r, const char* label, bool active, bool hovered) {
    SDL_Color bg     = active  ? SDL_Color{0,0,0,255}       : SDL_Color{255,255,255,255};
    SDL_Color border = hovered ? SDL_Color{90,90,90,255}    : SDL_Color{0,0,0,255};
    SDL_Color fg     = active  ? SDL_Color{255,255,255,255} : SDL_Color{0,0,0,255};
    SDL_Rect shadow  = { r.x+4, r.y+4, r.w, r.h };
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 160);
    SDL_RenderFillRect(ren, &shadow);
    DrawBox(ren, r, bg, border);
    int scale = 2;
    int tw = BitmapTextWidth(label, scale);
    int th = 7 * scale;
    BitmapText(ren, label, r.x + (r.w - tw) / 2, r.y + (r.h - th) / 2, scale, fg);
}

void RunReceiverLogic(std::string targetCode) {
    g_sessionActive = true;
    g_inputAllowed  = true;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    std::string targetIP = CodeToIP(targetCode);

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);

    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(12345);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(s, (sockaddr*)&addr, sizeof(addr));
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);

    sockaddr_in hostAddr = {};
    hostAddr.sin_family      = AF_INET;
    hostAddr.sin_port        = htons(12345);
    hostAddr.sin_addr.s_addr = inet_addr(targetIP.c_str());

    SOCKET ctrlS = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in ctrlBind = {};
    ctrlBind.sin_family      = AF_INET;
    ctrlBind.sin_port        = htons(12347);
    ctrlBind.sin_addr.s_addr = INADDR_ANY;
    bind(ctrlS, (sockaddr*)&ctrlBind, sizeof(ctrlBind));
    u_long cm = 1;
    ioctlsocket(ctrlS, FIONBIO, &cm);

    InputData hello = { 0 };
    sendto(s, (char*)&hello, sizeof(hello), 0, (sockaddr*)&hostAddr, sizeof(hostAddr));

    const AVCodec* cdc = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    AVCodecContext* ctx = avcodec_alloc_context3(cdc);
    avcodec_open2(ctx, cdc, NULL);

    AVFrame* frm = av_frame_alloc();

    SDL_Window*   win = nullptr;
    SDL_Renderer* ren = nullptr;
    SDL_Texture*  tex = nullptr;

    const int TOOLBAR_H = 48;

    bool  toolbarVisible   = true;
    float toolbarHideTimer = 0.0f;
    Uint32 lastTick        = SDL_GetTicks();
    bool  isFullscreen     = false;

    std::vector<uint8_t> frameBuf;
    uint32_t lastFIdx = 0;

    std::string statusMsg = "CONNECTING";
    Uint32 fpsTimer  = SDL_GetTicks();
    int fpsDisplay   = 0;
    int fpsCount     = 0;

    while (g_sessionActive) {
        Uint32 now = SDL_GetTicks();
        float dt   = (now - lastTick) / 1000.0f;
        lastTick   = now;

        int mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);

        SDL_Rect inputBtn = MakeRect(10,  6, 180, 36);
        SDL_Rect discBtn  = MakeRect(200, 6, 170, 36);
        SDL_Rect fullBtn  = MakeRect(380, 6, 160, 36);

        if (toolbarVisible) {
            toolbarHideTimer += dt;
            if (toolbarHideTimer > 4.0f && my > TOOLBAR_H + 20)
                toolbarVisible = false;
            if (my <= TOOLBAR_H + 20)
                toolbarHideTimer = 0.0f;
        } else {
            if (my <= 6) { toolbarVisible = true; toolbarHideTimer = 0.0f; }
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                g_sessionActive = false;

            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F11) {
                isFullscreen = !isFullscreen;
                if (win) SDL_SetWindowFullscreen(win, isFullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            }

            if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT && toolbarVisible) {
                int cx = ev.button.x, cy = ev.button.y;
                if (PointInRect(cx, cy, inputBtn)) {
                    g_inputAllowed = !g_inputAllowed;
                    statusMsg = g_inputAllowed ? "INPUT ON" : "INPUT OFF";
                }
                if (PointInRect(cx, cy, discBtn))
                    g_sessionActive = false;
                if (PointInRect(cx, cy, fullBtn)) {
                    isFullscreen = !isFullscreen;
                    if (win) SDL_SetWindowFullscreen(win, isFullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                }
            }

            if (!toolbarVisible || my > TOOLBAR_H) {
                InputData input = { 0 };
                if (ev.type == SDL_MOUSEMOTION && g_inputAllowed) {
                    input = { 1, ev.motion.x, ev.motion.y - (toolbarVisible ? TOOLBAR_H : 0), 0, 0 };
                } else if ((ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) && g_inputAllowed) {
                    input = { 2, ev.button.x, ev.button.y - (toolbarVisible ? TOOLBAR_H : 0),
                              (ev.button.button == SDL_BUTTON_LEFT ? 1 : 2), 0 };
                } else if (ev.type == SDL_KEYDOWN && g_inputAllowed) {
                    input = { 3, 0, 0, 0, (int)ev.key.keysym.sym };
                }
                if (input.type != 0)
                    sendto(s, (char*)&input, sizeof(input), 0, (sockaddr*)&hostAddr, sizeof(hostAddr));
            }
        }

        {
            char cb[sizeof(ControlPacket)];
            sockaddr_in cr; int cl = sizeof(cr);
            if (recvfrom(ctrlS, cb, sizeof(cb), 0, (sockaddr*)&cr, &cl) == sizeof(ControlPacket)) {
                ControlPacket* cp = (ControlPacket*)cb;
                if (cp->magic == CTRL_MAGIC) {
                    if (cp->cmd == CTRL_INPUT_ON)   { g_inputAllowed = true;  statusMsg = "INPUT ON"; }
                    if (cp->cmd == CTRL_INPUT_OFF)  { g_inputAllowed = false; statusMsg = "INPUT OFF"; }
                    if (cp->cmd == CTRL_DISCONNECT) { g_sessionActive = false; }
                }
            }
        }

        char udpBuf[2048];
        int r = recv(s, udpBuf, 2048, 0);
        if (r > (int)sizeof(FrameHeader)) {
            FrameHeader* h = (FrameHeader*)udpBuf;
            uint8_t* payload = (uint8_t*)(udpBuf + sizeof(FrameHeader));
            int pSize = r - (int)sizeof(FrameHeader);

            if (h->fIdx > lastFIdx) { frameBuf.clear(); lastFIdx = h->fIdx; }

            if (h->fIdx == lastFIdx) {
                frameBuf.insert(frameBuf.end(), payload, payload + pSize);

                if (h->isLast) {
                    uint8_t* pktBuf = (uint8_t*)av_malloc(frameBuf.size() + AV_INPUT_BUFFER_PADDING_SIZE);
                    memcpy(pktBuf, frameBuf.data(), frameBuf.size());
                    memset(pktBuf + frameBuf.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
                    AVPacket* pkt = av_packet_alloc();
                    av_packet_from_data(pkt, pktBuf, (int)frameBuf.size());

                    if (avcodec_send_packet(ctx, pkt) == 0) {
                        while (avcodec_receive_frame(ctx, frm) == 0) {
                            if (!win) {
                                int winW = h->width;
                                int winH = h->height + TOOLBAR_H;
                                win = SDL_CreateWindow("STASIS | LINK RECEIVER",
                                    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    winW, winH, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
                                ren = SDL_CreateRenderer(win, -1,
                                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
                                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_NV12,
                                    SDL_TEXTUREACCESS_STREAMING, h->width, h->height);
                                statusMsg = "LINKED";
                            }

                            SDL_UpdateNVTexture(tex, NULL,
                                frm->data[0], frm->linesize[0],
                                frm->data[1], frm->linesize[1]);

                            int curW, curH;
                            SDL_GetWindowSize(win, &curW, &curH);
                            int vidY = toolbarVisible ? TOOLBAR_H : 0;
                            int vidH = curH - vidY;

                            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
                            SDL_RenderClear(ren);

                            SDL_Rect vidDst = { 0, vidY, curW, vidH };
                            SDL_RenderCopy(ren, tex, NULL, &vidDst);

                            if (toolbarVisible) {
                                SDL_Rect bar = { 0, 0, curW, TOOLBAR_H };
                                SDL_SetRenderDrawColor(ren, 240, 240, 240, 255);
                                SDL_RenderFillRect(ren, &bar);
                                SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
                                SDL_RenderDrawLine(ren, 0, TOOLBAR_H-1, curW, TOOLBAR_H-1);
                                SDL_RenderDrawLine(ren, 0, TOOLBAR_H-2, curW, TOOLBAR_H-2);

                                bool iHov = PointInRect(mx, my, inputBtn);
                                bool dHov = PointInRect(mx, my, discBtn);
                                bool fHov = PointInRect(mx, my, fullBtn);

                                DrawButton(ren, inputBtn, g_inputAllowed ? "INPUT: ON" : "INPUT: OFF", g_inputAllowed, iHov);
                                DrawButton(ren, discBtn,  "DISCONNECT", false, dHov);
                                DrawButton(ren, fullBtn,  isFullscreen ? "WINDOWED" : "FULLSCREEN", false, fHov);

                                std::string stat = targetCode + "  " + statusMsg + "  " + std::to_string(fpsDisplay) + " FPS";
                                SDL_Color statCol = { 30, 30, 30, 255 };
                                int statX = curW - BitmapTextWidth(stat.c_str(), 2) - 12;
                                BitmapText(ren, stat.c_str(), statX, 17, 2, statCol);
                            }

                            SDL_RenderPresent(ren);

                            fpsCount++;
                            if (now - fpsTimer >= 1000) {
                                fpsDisplay = fpsCount;
                                fpsCount   = 0;
                                fpsTimer   = now;
                            }
                        }
                    }
                    av_packet_free(&pkt);
                    frameBuf.clear();
                }
            }
        } else {
            SDL_Delay(1);
        }
    }

    av_frame_free(&frm);
    avcodec_free_context(&ctx);
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    closesocket(ctrlS);
    closesocket(s);
    WSACleanup();
    SDL_Quit();
}
