#ifndef HOST_LOGIC_H
#define HOST_LOGIC_H

#include <string>
#include <vector>
#include <atomic>

#pragma pack(push, 1)
struct FrameHeader {
    uint32_t fIdx;
    uint32_t cIdx;
    uint32_t isLast;
    uint32_t width;
    uint32_t height;
};

struct InputData {
    int type;
    int x, y;
    int button;
    int key;
};

struct ControlPacket {
    uint32_t magic;
    uint32_t cmd;
};
#pragma pack(pop)

#define CTRL_MAGIC       0xDEADC0DE
#define CTRL_INPUT_ON    1
#define CTRL_INPUT_OFF   2
#define CTRL_DISCONNECT  3

extern std::atomic<bool> g_sessionActive;
extern std::atomic<bool> g_inputEnabled;

void RunHostLogic(int bitrateKbps, std::string& outCode);
void StopHostLogic();
void SetInputEnabled(bool enabled);

void RunReceiverLogic(std::string targetCode);

void StartDiscoveryListener();
void StopDiscoveryListener();
std::vector<std::string> GetDiscoveredHosts();

std::string IPToCode(std::string ip);
std::string CodeToIP(const std::string& code);

#endif
