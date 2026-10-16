// Balatro <-> GTA V passthrough shared-memory protocol.
//
// Dual-licensed under MIT / Unlicense.
#pragma once

#include <cstdint>

namespace balatro_gta::proto
{
    inline constexpr std::uint32_t kMagic = 0x42414C54; // "BALT"
    inline constexpr std::uint32_t kVersion = 1;
    inline constexpr wchar_t       kMappingName[] = L"Local\\BalatroGTA_v1";

    // ---- Region Offsets ----------------------------------------------------
    inline constexpr std::uint64_t kOffHeader         = 0x0;
    inline constexpr std::uint64_t kOffBalatroState   = 0x100;
    inline constexpr std::uint64_t kOffGtaState       = 0x180;
    inline constexpr std::uint64_t kOffOverlayCtl     = 0x200;
    inline constexpr std::uint64_t kOffOverlaySlotHdr = 0x240; // 3 x 0x40
    inline constexpr std::uint64_t kOffInputRing      = 0x1000;
    inline constexpr std::uint64_t kOffOverlayPixels  = 0x10000; // 64 KB alignment

    inline constexpr std::uint32_t kMaxOverlayW = 3840;
    inline constexpr std::uint32_t kMaxOverlayH = 2160;
    inline constexpr std::uint64_t kOverlaySlotBytes = std::uint64_t(kMaxOverlayW) * kMaxOverlayH * 4;
    inline constexpr std::uint32_t kOverlaySlots = 3;
    inline constexpr std::uint64_t kMappingBytes = kOffOverlayPixels + (kOverlaySlotBytes * kOverlaySlots);

    // ---- Header @0x0 -------------------------------------------------------
    struct Header
    {
        std::uint32_t magic;              // "BALT"
        std::uint32_t version;            // kVersion
        std::uint32_t gtaPid;
        std::uint32_t balatroPid;
        std::uint64_t gtaHeartbeatMs;     // GetTickCount64()
        std::uint64_t balatroHeartbeatMs; // GetTickCount64()
        std::uint32_t flags;              // bit 0: phone active, bit 1: casino active
        std::uint32_t pad;
    };
    static_assert(sizeof(Header) == 0x28);

    // ---- Balatro State @0x100 (seqlock: seq odd while writing) -------------
    enum BalatroFlags : std::uint32_t
    {
        kBalatroInRun      = 1u << 0,
        kBalatroInShop     = 1u << 1,
        kBalatroInBlind    = 1u << 2,
        kBalatroGameOver   = 1u << 3,
        kBalatroGameWon    = 1u << 4,
    };

    struct BalatroState
    {
        std::uint32_t seq;
        std::uint32_t flags;           // BalatroFlags
        std::int32_t  dollars;         // Balatro $
        std::uint32_t ante;
        std::uint32_t round;
        std::uint32_t handsLeft;
        std::uint32_t discardsLeft;
        std::uint32_t pad;
        std::uint64_t chipsCurrent;
        std::uint64_t chipsTarget;
    };
    static_assert(sizeof(BalatroState) <= 0x80);

    // ---- GTA State @0x180 (seqlock) ----------------------------------------
    enum GtaFlags : std::uint32_t
    {
        kGtaPhoneOpen   = 1u << 0,
        kGtaCasinoSeated = 1u << 1,
        kGtaInputFocused = 1u << 2,
    };

    struct GtaState
    {
        std::uint32_t seq;
        std::uint32_t flags;           // GtaFlags
        std::int64_t  playerCash;      // GTA$ wallet/bank
        std::uint32_t viewportW;       // GTA display or phone quad width
        std::uint32_t viewportH;       // GTA display or phone quad height
    };
    static_assert(sizeof(GtaState) <= 0x80);

    // ---- Overlay Triple Buffer @0x200 --------------------------------------
    inline constexpr std::uint32_t kOverlayDirty = 1u << 2;

    struct OverlayCtl
    {
        std::uint32_t state;
        std::uint32_t pad;
        std::uint64_t framesPublished;
    };

    struct OverlaySlotHdr
    {
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t stride;
        std::uint32_t flags;          // bit 0: bottom-up rows
        std::uint64_t frameId;
        std::uint64_t timestampNs;
        std::uint8_t  reserved[0x40 - 0x20];
    };
    static_assert(sizeof(OverlaySlotHdr) == 0x40);

    // ---- Input Ring @0x1000 (GTA produces, Balatro consumes) ---------------
    inline constexpr std::uint32_t kInputRingEntries = 512;
    inline constexpr std::uint64_t kInputRingHeadOff = 0x00;
    inline constexpr std::uint64_t kInputRingTailOff = 0x08;
    inline constexpr std::uint64_t kInputRingDataOff = 0x40;

    enum InputType : std::uint16_t
    {
        kInNone        = 0,
        kInMouseMove   = 1, // normX (0..1), normY (0..1)
        kInMouseButton = 2, // button (1=L, 2=R, 3=M), pressed (1/0)
        kInMouseWheel  = 3, // delta (-1 / +1)
        kInKey         = 4, // keycode, pressed (1/0)
        kInResetInput  = 5  // release all held buttons
    };

    struct InputEvent
    {
        std::uint16_t type;
        std::uint16_t code;
        float         x;
        float         y;
        std::uint32_t extra;
        std::uint32_t pad[2];
    };
    static_assert(sizeof(InputEvent) == 24);
}
