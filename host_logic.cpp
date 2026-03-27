#include "host_logic.h"
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <thread>
#include <mutex>
#include "win_adapter.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#pragma comment(lib, "Ws2_32.lib")

std::atomic<bool> g_sessionActive(false);
std::atomic<bool> g_discoveryActive(false);
std::atomic<bool> g_inputEnabled(true);
std::vector<std::string> g_hosts;
std::mutex g_hostMtx;

static SOCKET g_ctrlSocket = INVALID_SOCKET;
static sockaddr_in g_receiverAddr = {};
static bool g_receiverKnown = false;
static std::mutex g_receiverMtx;

static ID3D11Device* lDev = nullptr;
static ID3D11DeviceContext* lCtx = nullptr;
static IDXGIOutputDuplication* lDupl = nullptr;
static ID3D11Texture2D* lStg = nullptr;

std::string GetLocalIP() {
    char sz[255];
    if (gethostname(sz, 255) != 0) return "127.0.0.1";
    struct hostent* h = gethostbyname(sz);
    if (!h) return "127.0.0.1";
    return std::string(inet_ntoa(*(struct in_addr*)*h->h_addr_list));
}

std::string IPToCode(std::string ip) {
    std::stringstream ss(ip);
    std::string seg;
    std::vector<int> p;
    while (std::getline(ss, seg, '.')) p.push_back(std::stoi(seg));
    if (p.size() != 4) return "LN-00-00";
    std::stringstream res;
    res << "LN-" << std::uppercase << std::hex << std::setfill('0') << std::setw(2) << p[2] << "-" << std::setw(2) << p[3];
    return res.str();
}

std::string CodeToIP(const std::string& code) {
    if (code.size() < 9 || code.substr(0, 3) != "LN-") return "127.0.0.1";
    try {
        int oct3 = std::stoi(code.substr(3, 2), nullptr, 16);
        int oct4 = std::stoi(code.substr(6, 2), nullptr, 16);
        std::string localIP = GetLocalIP();
        std::stringstream ss(localIP);
        std::string seg;
        std::vector<int> parts;
        while (std::getline(ss, seg, '.')) parts.push_back(std::stoi(seg));
        if (parts.size() != 4) return "127.0.0.1";
        return std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "." +
               std::to_string(oct3) + "." + std::to_string(oct4);
    } catch (...) {
        return "127.0.0.1";
    }
}

void SetInputEnabled(bool enabled) {
    g_inputEnabled = enabled;
    std::lock_guard<std::mutex> lk(g_receiverMtx);
    if (g_ctrlSocket == INVALID_SOCKET || !g_receiverKnown) return;
    ControlPacket cp;
    cp.magic = CTRL_MAGIC;
    cp.cmd   = enabled ? CTRL_INPUT_ON : CTRL_INPUT_OFF;
    sendto(g_ctrlSocket, (char*)&cp, sizeof(cp), 0, (sockaddr*)&g_receiverAddr, sizeof(g_receiverAddr));
}

void HandleRemoteInput(InputData* i) {
    if (!g_inputEnabled) return;
    if (i->type == 1) {
        SetCursorPos(i->x, i->y);
    } else if (i->type == 2) {
        Win_SendMouseClick(i->x, i->y, (i->button == 1));
    } else if (i->type == 3) {
        INPUT in = { 0 };
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = (WORD)i->key;
        SendInput(1, &in, sizeof(INPUT));
    }
}

bool SetupDirectX(int w, int h) {
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &lDev, nullptr, &lCtx);
    IDXGIDevice* dxDev; lDev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxDev);
    IDXGIAdapter* adp; dxDev->GetParent(__uuidof(IDXGIAdapter), (void**)&adp);
    IDXGIOutput* out; adp->EnumOutputs(0, &out);
    IDXGIOutput1* out1; out->QueryInterface(__uuidof(IDXGIOutput1), (void**)&out1);
    HRESULT hr = out1->DuplicateOutput(lDev, &lDupl);
    D3D11_TEXTURE2D_DESC d = { (UINT)w, (UINT)h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1,0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0 };
    lDev->CreateTexture2D(&d, nullptr, &lStg);
    adp->Release(); out->Release(); out1->Release(); dxDev->Release();
    return SUCCEEDED(hr);
}

void StartDiscoveryListener() {
    g_discoveryActive = true;
    std::thread([]() {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons(12346);
        a.sin_addr.s_addr = INADDR_ANY;
        bind(s, (sockaddr*)&a, sizeof(a));
        u_long m = 1; ioctlsocket(s, FIONBIO, &m);
        while (g_discoveryActive) {
            char b[128] = {};
            sockaddr_in r;
            int l = sizeof(r);
            if (recvfrom(s, b, 127, 0, (sockaddr*)&r, &l) > 0) {
                std::string c(b);
                std::lock_guard<std::mutex> lock(g_hostMtx);
                bool found = false;
                for (auto& h : g_hosts) if (h == c) found = true;
                if (!found) g_hosts.push_back(c);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        closesocket(s);
    }).detach();
}

void StopDiscoveryListener() {
    g_discoveryActive = false;
    std::lock_guard<std::mutex> lock(g_hostMtx);
    g_hosts.clear();
}

std::vector<std::string> GetDiscoveredHosts() {
    std::lock_guard<std::mutex> lock(g_hostMtx);
    return g_hosts;
}

void RunHostLogic(int bitrateKbps, std::string& code) {
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    code = IPToCode(GetLocalIP());
    g_sessionActive = true;
    g_inputEnabled = true;

    SOCKET bcS = socket(AF_INET, SOCK_DGRAM, 0);
    BOOL opt = TRUE;
    setsockopt(bcS, SOL_SOCKET, SO_BROADCAST, (char*)&opt, sizeof(opt));
    sockaddr_in bcA = {};
    bcA.sin_family = AF_INET;
    bcA.sin_port = htons(12346);
    bcA.sin_addr.s_addr = INADDR_BROADCAST;

    std::thread([bcS, bcA, code]() mutable {
        while (g_sessionActive) {
            sendto(bcS, code.c_str(), (int)code.length(), 0, (sockaddr*)&bcA, sizeof(bcA));
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        closesocket(bcS);
    }).detach();

    SOCKET udpS = socket(AF_INET, SOCK_DGRAM, 0);
    {
        std::lock_guard<std::mutex> lk(g_receiverMtx);
        g_ctrlSocket = udpS;
    }
    sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(12345);
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bind(udpS, (sockaddr*)&bindAddr, sizeof(bindAddr));

    sockaddr_in streamTarget = {};
    streamTarget.sin_family = AF_INET;
    streamTarget.sin_port = htons(12345);
    bool targetFound = false;

    const int SRC_W = 1920, SRC_H = 1080;
    const int DST_W = 1280, DST_H = 720;
    if (!SetupDirectX(SRC_W, SRC_H)) return;

    const AVCodec* cdc = avcodec_find_encoder_by_name("hevc_nvenc");
    if (!cdc) cdc = avcodec_find_encoder(AV_CODEC_ID_HEVC);
    AVCodecContext* enc = avcodec_alloc_context3(cdc);
    enc->width = DST_W; enc->height = DST_H;
    enc->time_base = { 1, 60 }; enc->framerate = { 60, 1 };
    enc->pix_fmt = AV_PIX_FMT_NV12;
    enc->bit_rate = (int64_t)bitrateKbps * 1000;
    enc->gop_size = 60;
    enc->max_b_frames = 0;
    av_opt_set(enc->priv_data, "preset", "p1", 0);
    av_opt_set(enc->priv_data, "tune", "ull", 0);
    avcodec_open2(enc, cdc, NULL);

    SwsContext* sws = sws_getContext(SRC_W, SRC_H, AV_PIX_FMT_BGRA, DST_W, DST_H, AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, 0, 0, 0);
    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_NV12; frame->width = DST_W; frame->height = DST_H;
    av_frame_get_buffer(frame, 0);
    AVPacket* pkt = av_packet_alloc();
    uint32_t fCount = 0;

    u_long nbMode = 1;
    ioctlsocket(udpS, FIONBIO, &nbMode);

    while (g_sessionActive) {
        if (!targetFound) {
            char b[512];
            sockaddr_in r; int l = sizeof(r);
            int got = recvfrom(udpS, b, sizeof(b), 0, (sockaddr*)&r, &l);
            if (got > 0) {
                streamTarget.sin_addr = r.sin_addr;
                targetFound = true;
                {
                    std::lock_guard<std::mutex> lk(g_receiverMtx);
                    g_receiverAddr = r;
                    g_receiverAddr.sin_port = htons(12345);
                    g_receiverKnown = true;
                }
                if (got == sizeof(InputData))
                    HandleRemoteInput((InputData*)b);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        {
            char b[512];
            sockaddr_in r; int l = sizeof(r);
            int got;
            while ((got = recvfrom(udpS, b, sizeof(b), 0, (sockaddr*)&r, &l)) > 0) {
                if (got == sizeof(InputData))
                    HandleRemoteInput((InputData*)b);
            }
        }

        IDXGIResource* res = nullptr;
        DXGI_OUTDUPL_FRAME_INFO info;
        if (SUCCEEDED(lDupl->AcquireNextFrame(1, &info, &res))) {
            ID3D11Texture2D* tex;
            res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
            lCtx->CopyResource(lStg, tex);
            D3D11_MAPPED_SUBRESOURCE map;
            if (SUCCEEDED(lCtx->Map(lStg, 0, D3D11_MAP_READ, 0, &map))) {
                uint8_t* sd[] = { (uint8_t*)map.pData };
                int sl[] = { (int)map.RowPitch };
                sws_scale(sws, sd, sl, 0, SRC_H, frame->data, frame->linesize);
                if (avcodec_send_frame(enc, frame) == 0) {
                    while (avcodec_receive_packet(enc, pkt) == 0) {
                        fCount++;
                        int rem = pkt->size;
                        uint8_t* d = pkt->data;
                        uint32_t cIdx = 0;
                        while (rem > 0) {
                            int cur = (rem > 1300) ? 1300 : rem;
                            FrameHeader h = { fCount, cIdx++, (uint32_t)(rem <= 1300), (uint32_t)DST_W, (uint32_t)DST_H };
                            std::vector<char> p(sizeof(h) + cur);
                            memcpy(p.data(), &h, sizeof(h));
                            memcpy(p.data() + sizeof(h), d, cur);
                            sendto(udpS, p.data(), (int)p.size(), 0, (sockaddr*)&streamTarget, sizeof(streamTarget));
                            d += cur; rem -= cur;
                        }
                        av_packet_unref(pkt);
                    }
                }
                lCtx->Unmap(lStg, 0);
            }
            tex->Release(); res->Release(); lDupl->ReleaseFrame();
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_receiverMtx);
        g_ctrlSocket = INVALID_SOCKET;
        g_receiverKnown = false;
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&enc);
    sws_freeContext(sws);
    if (lStg) lStg->Release();
    if (lDupl) lDupl->Release();
    if (lCtx) lCtx->Release();
    if (lDev) lDev->Release();
    closesocket(udpS);
    WSACleanup();
}

void StopHostLogic() {
    g_sessionActive = false;
}
