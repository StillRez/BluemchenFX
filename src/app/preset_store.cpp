#include "app/preset_store.h"
#include "ff_gen_drv.h"
#include "dev/sdram.h"
#include <cstddef>
#include <cstdio>
#include <cstring>

using namespace daisy;

// libDaisy globals used for recovery / diagnostics
extern SD_HandleTypeDef hsd1;      // per/sdmmc.cpp
extern "C" Disk_drvTypeDef disk;   // FatFs ff_gen_drv.c

namespace app
{
static constexpr const char *kDir = "BLUEMCHN";

/**
 * libDaisy's SD driver DMA-reads whole 512-byte sectors straight into FatFS's
 * sector buffers and then invalidates the D-cache over that range, rounded
 * to 32-byte cache lines. If a buffer isn't 32-byte aligned, the first/last
 * cache line also holds unrelated variables, and their pending writes are
 * thrown away. These wrappers place each object so that its sector buffer
 * starts on a cache-line boundary (and therefore also ends on one).
 */
template <typename T, size_t kBufOffset>
struct alignas(32) DmaSafe
{
    static constexpr size_t kPad = (32 - kBufOffset % 32) % 32;
    uint8_t                 pad[kPad == 0 ? 32 : kPad];
    T                       obj;
};

using SafeFs  = DmaSafe<FATFS, offsetof(FATFS, win)>;
using SafeFil = DmaSafe<FIL, offsetof(FIL, buf)>;

// Everything the SDMMC1 DMA touches lives in SDRAM: in a bootloader
// (BOOT_SRAM) build ordinary globals land in DTCM, which SDMMC1's DMA can't
// reach. SDRAM (FMC) is reachable in every build type. It's cached, which
// is why the cache-line alignment above matters. Not zeroed at boot; FatFS
// initialises these objects itself (f_mount / f_open).
static SafeFs DSY_SDRAM_BSS           s_fs;
static SafeFil DSY_SDRAM_BSS          s_file;
alignas(32) static char DSY_SDRAM_BSS s_buf[2048];

// Each object is a whole number of cache lines, so nothing else shares one
static_assert(sizeof(SafeFs) % 32 == 0 && sizeof(SafeFil) % 32 == 0,
              "keep DMA objects whole cache lines");

static_assert((offsetof(SafeFs, obj) + offsetof(FATFS, win)) % 32 == 0,
              "FATFS window must be cache-line aligned");
static_assert((offsetof(SafeFil, obj) + offsetof(FIL, buf)) % 32 == 0,
              "FIL buffer must be cache-line aligned");

#define FS (s_fs.obj)
#define FILE_ (s_file.obj)

/** Bus configurations tried in order (starting from the last good one).
 *  Presets are tiny, so speed doesn't matter; robustness does.
 *
 *  On the Bluemchen, 4-bit mode never gets data back from the card. A
 *  failed data transfer waits out the STM32 HAL's fixed hardware timeout
 *  of 2^32 SD clock cycles (~172 s at 25 MHz) before giving up, so a 4-bit
 *  attempt costs ~3 minutes before falling back. Only 1-bit is used.
 *  To experiment with 4-bit, add
 *    {SdmmcHandler::Speed::STANDARD, SdmmcHandler::BusWidth::BITS_4, "4b"}
 *  to this list -- but put it *after* the 1-bit entry. */
struct BusCfg
{
    SdmmcHandler::Speed    speed;
    SdmmcHandler::BusWidth width;
    const char            *tag;
};
static const BusCfg kBusCfgs[] = {
    {SdmmcHandler::Speed::MEDIUM_SLOW, SdmmcHandler::BusWidth::BITS_1, "1b"},
};
static constexpr int kNumBusCfgs = sizeof(kBusCfgs) / sizeof(kBusCfgs[0]);

// ------------------------------------------------------------- mounting
void PresetStore::Init(const fx::Effect *const *effects, size_t num_effects)
{
    effects_     = effects;
    num_effects_ = num_effects;
    // Registers the SD driver with FatFS and sets the "0:/" path.
    // Does not touch the card.
    fsi_.Init(FatFSInterface::Config::MEDIA_SD);
}

bool PresetStore::TryMount(int idx)
{
    const char *path = fsi_.GetSDPath();
    const int   pdrv = path[0] - '0';

    // 1. forget any previous volume
    f_mount(nullptr, path, 0);
    // 2. reset the SDMMC peripheral + HAL handle (required after a failed
    //    init or a removed card, see SdmmcHandler::Reset())
    sd_.Reset();
    // 3. libDaisy's FatFS glue latches "initialised" even when init fails,
    //    which would block every later attempt until reboot. Clear it.
    if(pdrv >= 0 && pdrv < _VOLUMES)
        disk.is_initialized[pdrv] = 0;
    // 4. configure the bus for this attempt
    SdmmcHandler::Config cfg;
    cfg.Defaults();
    cfg.speed = kBusCfgs[idx].speed;
    cfg.width = kBusCfgs[idx].width;
    sd_.Init(cfg);

    FRESULT fr = f_mount(&FS, path, 1); // 1 = mount now (talks to the card)
    if(fr == FR_OK)
        return true;
    RecordError(fr, idx);
    return false;
}

bool PresetStore::EnsureMounted()
{
    if(mounted_)
        return true;
    for(int i = 0; i < kNumBusCfgs; i++)
    {
        int idx = (bus_cfg_ + i) % kNumBusCfgs;
        if(TryMount(idx))
        {
            bus_cfg_       = idx;
            mounted_       = true;
            last_error_[0] = 0;
            return true;
        }
        // No response at all to commands = no card; don't try other modes
        if(last_sd_error_ & HAL_SD_ERROR_CMD_RSP_TIMEOUT)
            break;
    }
    return false;
}

void PresetStore::Unmount()
{
    f_mount(nullptr, fsi_.GetSDPath(), 0);
    mounted_ = false;
}

void PresetStore::RecordError(int fresult, int bus_cfg)
{
    last_sd_error_ = hsd1.ErrorCode;
    snprintf(last_error_,
             sizeof(last_error_),
             "F%d E%lX %s",
             fresult,
             (unsigned long)last_sd_error_,
             kBusCfgs[bus_cfg].tag);
}

PresetStore::Status PresetStore::MountFailureStatus() const
{
    // Commands get no reply -> nothing in the slot
    if(last_sd_error_ & HAL_SD_ERROR_CMD_RSP_TIMEOUT)
        return Status::NO_CARD;
    return Status::IO_ERROR;
}

// ------------------------------------------------------------ file I/O
void PresetStore::SlotPath(int slot, char *buf, size_t len) const
{
    snprintf(buf, len, "%s%s/PRESET%02d.TXT", fsi_.GetSDPath(), kDir, slot);
}

PresetStore::Status PresetStore::ReadSlot(int slot, size_t &len)
{
    len = 0;
    // Two attempts: the second one remounts, in case the card was swapped
    for(int attempt = 0; attempt < 2; attempt++)
    {
        if(!EnsureMounted())
            return MountFailureStatus();

        char path[48];
        SlotPath(slot, path, sizeof(path));
        FRESULT fr = f_open(&FILE_, path, FA_READ | FA_OPEN_EXISTING);
        if(fr == FR_NO_FILE || fr == FR_NO_PATH)
        {
            last_error_[0] = 0;
            return Status::NO_FILE;
        }
        if(fr != FR_OK)
        {
            RecordError(fr, bus_cfg_);
            Unmount();
            continue;
        }
        UINT br = 0;
        fr      = f_read(&FILE_, s_buf, sizeof(s_buf) - 1, &br);
        f_close(&FILE_);
        if(fr != FR_OK)
        {
            RecordError(fr, bus_cfg_);
            Unmount();
            continue;
        }
        s_buf[br]      = 0;
        len            = br;
        last_error_[0] = 0;
        return Status::OK;
    }
    return Status::IO_ERROR;
}

PresetStore::Status PresetStore::Peek(int    slot,
                                      char  *name,
                                      size_t len,
                                      char  *effect,
                                      size_t effect_len)
{
    name[0]   = 0;
    effect[0] = 0;
    size_t n;
    Status st = ReadSlot(slot, n);
    if(st != Status::OK)
        return st;
    EffectState scratch;
    uint8_t     scratch_input = 0;
    std::memset(&scratch, 0, sizeof(scratch));
    ParseResult res = ParsePreset(*effects_[0], s_buf, n, scratch, scratch_input);
    strncpy(name, res.name, len - 1);
    name[len - 1] = 0;
    strncpy(effect, res.effect, effect_len - 1);
    effect[effect_len - 1] = 0;
    return Status::OK;
}

PresetStore::Status
PresetStore::Load(int slot, uint8_t &effect, EffectState &state, uint8_t &input_mode)
{
    size_t n;
    Status st = ReadSlot(slot, n);
    if(st != Status::OK)
        return st;

    int idx = FindPresetEffect(effects_, num_effects_, s_buf, n);
    if(idx == kPresetBadEffect)
        return Status::BAD_EFFECT;
    if(idx == kPresetNoEffect)
        idx = effect; // old/hand-written file: current effect
    if(idx < 0 || idx >= (int)num_effects_)
        return Status::BAD_EFFECT;

    // A file for another effect starts from that effect's defaults
    EffectState tmp  = state;
    uint8_t     mode = input_mode;
    if(idx != effect)
    {
        for(size_t i = 0; i < kMaxParams; i++)
            tmp.norm[i] = i < effects_[idx]->NumParams() ? effects_[idx]->Spec(i).def
                                                         : 0.f;
        for(int r = 0; r < SRC_LAST; r++)
        {
            int p        = effects_[idx]->DefaultRoute(r);
            tmp.route[r] = (int8_t)(p >= 0 ? p : kRouteNone);
        }
    }
    ParseResult res = ParsePreset(*effects_[idx], s_buf, n, tmp, mode);
    if(res.applied == 0)
        return Status::PARSE_ERROR;
    effect     = (uint8_t)idx;
    state      = tmp;
    input_mode = mode;
    return Status::OK;
}

PresetStore::Status PresetStore::Save(int                slot,
                                      uint8_t            effect,
                                      const EffectState &state,
                                      uint8_t            input_mode)
{
    if(effect >= num_effects_)
        return Status::BAD_EFFECT;
    // Keep a name the user typed into the file
    char   name[kPresetNameLen];
    char   unused[kEffectNameLen];
    Status peek = Peek(slot, name, sizeof(name), unused, sizeof(unused));
    if(peek == Status::NO_CARD || peek == Status::IO_ERROR)
        return peek;
    if(peek != Status::OK || !name[0])
        snprintf(name, sizeof(name), "PRESET %02d", slot);

    for(int attempt = 0; attempt < 2; attempt++)
    {
        if(!EnsureMounted())
            return MountFailureStatus();

        char dir[24];
        snprintf(dir, sizeof(dir), "%s%s", fsi_.GetSDPath(), kDir);
        FRESULT fr = f_mkdir(dir);
        if(fr != FR_OK && fr != FR_EXIST)
        {
            RecordError(fr, bus_cfg_);
            Unmount();
            continue;
        }

        size_t len = SerializePreset(*effects_[effect],
                                     state,
                                     input_mode,
                                     name,
                                     s_buf,
                                     sizeof(s_buf));
        if(len == 0)
            return Status::IO_ERROR;

        char path[48];
        SlotPath(slot, path, sizeof(path));
        fr = f_open(&FILE_, path, FA_WRITE | FA_CREATE_ALWAYS);
        if(fr != FR_OK)
        {
            RecordError(fr, bus_cfg_);
            Unmount();
            continue;
        }
        UINT    bw = 0;
        fr         = f_write(&FILE_, s_buf, len, &bw);
        FRESULT fc = f_close(&FILE_);
        if(fr != FR_OK || fc != FR_OK || bw != len)
        {
            RecordError(fr != FR_OK ? fr : fc, bus_cfg_);
            Unmount();
            return Status::IO_ERROR;
        }
        last_error_[0] = 0;
        return Status::OK;
    }
    return Status::IO_ERROR;
}

const char *PresetStore::StatusText(Status s)
{
    switch(s)
    {
        case Status::OK: return "OK";
        case Status::NO_CARD: return "NO CARD";
        case Status::NO_FILE: return "EMPTY";
        case Status::IO_ERROR: return "SD ERROR";
        case Status::PARSE_ERROR: return "BAD FILE";
        case Status::BAD_EFFECT: return "BAD FX";
    }
    return "?";
}

} // namespace app
