#pragma once
#include <cstdint>

// Flat Steamworks calls used by the Steam link. The game's own steam_api.dll (SDK with
// SteamNetworking005 / SteamFriends015 / SteamUser018) exports them as __cdecl functions taking the
// interface pointer first; a CSteamID travels as a plain uint64 (checked by disassembling the
// exports: 8 bytes on the stack, returned in EDX:EAX). steam_mock.cpp fills the same table.
#pragma pack(push, 8)
struct FriendGameInfo {
    uint64_t game_id; // AppID in the low 24 bits
    uint32_t ip;
    uint16_t port, query_port;
    uint64_t lobby;
};
#pragma pack(pop)

struct SteamFns {
    void *net = nullptr, *user = nullptr, *friends = nullptr;
    uint64_t (*GetSteamID)(void *user) = nullptr;
    bool (*BLoggedOn)(void *user) = nullptr;
    bool (*SendP2PPacket)(void *net, uint64_t to, const void *data, uint32_t size, int type, int channel) = nullptr;
    bool (*IsP2PPacketAvailable)(void *net, uint32_t *size, int channel) = nullptr;
    bool (*ReadP2PPacket)(void *net, void *dest, uint32_t cap, uint32_t *size, uint64_t *from, int channel) = nullptr;
    bool (*AcceptP2PSessionWithUser)(void *net, uint64_t user) = nullptr;
    int (*GetFriendCount)(void *friends, int flags) = nullptr;
    uint64_t (*GetFriendByIndex)(void *friends, int index, int flags) = nullptr;
    bool (*GetFriendGamePlayed)(void *friends, uint64_t user, FriendGameInfo *info) = nullptr;
    const char *(*GetFriendRichPresence)(void *friends, uint64_t user, const char *key) = nullptr;
    bool (*SetRichPresence)(void *friends, const char *key, const char *value) = nullptr;
};

// steam_mock.cpp: tests only. Two copies on one PC with fake accounts, packets over localhost UDP.
bool steam_mock_fill(SteamFns &f);
