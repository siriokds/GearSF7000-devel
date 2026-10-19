/* Pico9918 has a 64 KiB backing map while its legacy CPU port retains the
 * original 14-bit window and wrapping implemented by the upstream core. */
#define VR_EMU_TMS9918_STATIC
#define VRAM_STORAGE_SIZE (1 << 16)
#define VR_EMU_TMS9918_RENDER_LINES 480
#define VR_EMU_TMS9918_HOST_FIELDS bool extendedRows; bool realSpriteY; uint8_t tilePaletteBase; uint8_t spritePaletteBase; uint8_t maxScanlineSprites; uint8_t maxSprites; uint8_t tileEcm; uint16_t tileEcmOffset; uint8_t spriteEcm; uint16_t spriteEcmOffset; uint8_t* debugBackground; uint8_t* debugSpriteMask;
#define VR_EMU_TMS9918_EXTENDED_ROWS(core) ((core)->extendedRows)
#define VR_EMU_TMS9918_DOUBLE_ROWS(core) (((core)->registers[0] & 0x08) != 0)
#define VR_EMU_TMS9918_TILE_COLOR(core, color) ((uint8_t)((color) | (core)->tilePaletteBase))
#define VR_EMU_TMS9918_SPRITE_COLOR(core, color) ((uint8_t)((color) | (core)->spritePaletteBase))
#define VR_EMU_TMS9918_MAX_SCANLINE_SPRITES(core) ((core)->maxScanlineSprites)
#define VR_EMU_TMS9918_MAX_SPRITES(core) ((core)->maxSprites)
#define VR_EMU_TMS9918_REAL_SPRITE_Y(core) ((core)->realSpriteY)
#define VR_EMU_TMS9918_TILE_ECM(core) ((core)->tileEcm)
#define VR_EMU_TMS9918_TILE_ECM_OFFSET(core) ((core)->tileEcmOffset)
#define VR_EMU_TMS9918_TILE_ECM_COLOR(core, ecm, attr, pixel) ((uint8_t)(((ecm) == 1 ? ((core)->tilePaletteBase & 0x20) : 0) | (((attr) & ((ecm) == 3 ? 0x0e : 0x0f)) << ((ecm) == 3 ? 2 : (ecm))) | (pixel)))
#define VR_EMU_TMS9918_SPRITE_ECM(core) ((core)->spriteEcm)
#define VR_EMU_TMS9918_SPRITE_ECM_OFFSET(core) ((core)->spriteEcmOffset)
#define VR_EMU_TMS9918_SPRITE_ECM_COLOR(core, ecm, attr, pixel) ((uint8_t)(((ecm) == 1 ? ((core)->spritePaletteBase & 0x20) : 0) | (((attr) & ((ecm) == 3 ? 0x0e : 0x0f)) << ((ecm) == 3 ? 2 : (ecm))) | (pixel)))
#define VR_EMU_TMS9918_CAPTURE_BACKGROUND(core, pixels) do { if ((core)->debugBackground != NULL) memcpy((core)->debugBackground, (pixels), TMS9918_PIXELS_X); } while (0)
#define VR_EMU_TMS9918_CAPTURE_SPRITE_PIXEL(core, x) do { if ((core)->debugSpriteMask != NULL) (core)->debugSpriteMask[(x)] = 1; } while (0)

/* Compile the replaceable upstream implementation and keep the only required
 * host extension beside GearSF7000 code. The upstream source remains usable
 * as a simple file replacement. */
#include "../third_party/vrEmuTms9918/src/vrEmuTms9918.c"

#include "Pico9918LegacyCoreBridge.h"

size_t gcPico9918LegacyStateSize(void)
{
    return 8 + 1 + 2 + 1 + 1 + 1 + 1 + VRAM_STORAGE_SIZE +
        TMS9918_PIXELS_X;
}

void gcPico9918LegacyPowerOn(struct vrEmuTMS9918_s* core)
{
    if (core == NULL)
        return;
    memset(core->vram, 0, sizeof(core->vram));
    memset(core->rowSpriteBits, 0, sizeof(core->rowSpriteBits));
    core->extendedRows = false;
    core->realSpriteY = false;
    core->tilePaletteBase = 0;
    core->spritePaletteBase = 0;
    core->maxScanlineSprites = 4;
    core->maxSprites = 32;
    core->tileEcm = 0;
    core->tileEcmOffset = 0x800;
    core->spriteEcm = 0;
    core->spriteEcmOffset = 0x800;
    core->debugBackground = NULL;
    core->debugSpriteMask = NULL;
    vrEmuTms9918Reset(core);
}

void gcPico9918LegacySaveState(
    const struct vrEmuTMS9918_s* core, void* destination)
{
    if (core == NULL || destination == NULL)
        return;
    uint8_t* out = (uint8_t*)destination;
    memcpy(out, core->registers, 8); out += 8;
    *out++ = core->status;
    *out++ = (uint8_t)core->currentAddress;
    *out++ = (uint8_t)(core->currentAddress >> 8);
    *out++ = core->regWriteStage;
    *out++ = core->regWriteStage0Value;
    *out++ = core->readAheadBuffer;
    *out++ = (uint8_t)core->mode;
    memcpy(out, core->vram, sizeof(core->vram)); out += sizeof(core->vram);
    memcpy(out, core->rowSpriteBits, sizeof(core->rowSpriteBits));
}

bool gcPico9918LegacyLoadState(
    struct vrEmuTMS9918_s* core, const void* source, size_t size)
{
    const size_t prefixSize = 8 + 1 + 2 + 1 + 1 + 1 + 1;
    const size_t legacySize = prefixSize + (1 << 14) + TMS9918_PIXELS_X;
    if (core == NULL || source == NULL ||
        (size != gcPico9918LegacyStateSize() && size != legacySize))
        return false;
    const uint8_t* in = (const uint8_t*)source;
    const uint8_t* mode = in + 8 + 1 + 2 + 1 + 1 + 1;
    if (in[11] > 1 || *mode > TMS_MODE_MULTICOLOR)
        return false;
    memcpy(core->registers, in, 8); in += 8;
    core->status = *in++;
    core->currentAddress = in[0] | ((uint16_t)in[1] << 8); in += 2;
    core->regWriteStage = *in++;
    core->regWriteStage0Value = *in++;
    core->readAheadBuffer = *in++;
    core->mode = (vrEmuTms9918Mode)*in++;
    const size_t storedVramSize = size == legacySize ? (1 << 14) :
        sizeof(core->vram);
    memset(core->vram, 0, sizeof(core->vram));
    memcpy(core->vram, in, storedVramSize); in += storedVramSize;
    memcpy(core->rowSpriteBits, in, sizeof(core->rowSpriteBits));
    return true;
}

bool gcPico9918LegacyReadDebugState(
    const struct vrEmuTMS9918_s* core,
    GcPico9918LegacyDebugState* state)
{
    if (core == NULL || state == NULL)
        return false;
    state->address = core->currentAddress;
    state->status = core->status;
    state->control_latch_pending = core->regWriteStage != 0;
    state->control_latch_value = core->regWriteStage0Value;
    state->read_ahead_buffer = core->readAheadBuffer;
    return true;
}

bool gcPico9918LegacyWriteVram(
    struct vrEmuTMS9918_s* core, uint16_t address, uint8_t value)
{
    if (core == NULL)
        return false;
    core->vram[address] = value;
    return true;
}

uint8_t gcPico9918LegacyReadVram(
    const struct vrEmuTMS9918_s* core, uint16_t address)
{
    return core != NULL ? core->vram[address] : 0;
}

void gcPico9918LegacyCancelControlLatch(struct vrEmuTMS9918_s* core)
{
    if (core != NULL)
        core->regWriteStage = 0;
}

void gcPico9918LegacyWriteStatus(
    struct vrEmuTMS9918_s* core, uint8_t value)
{
    if (core != NULL)
        core->status = value;
}

void gcPico9918LegacyAdjustAddress(
    struct vrEmuTMS9918_s* core, int16_t delta)
{
    if (core != NULL)
        core->currentAddress = (uint16_t)(core->currentAddress + delta);
}

uint8_t gcPico9918LegacyBeginScanline(
    struct vrEmuTMS9918_s* core, uint16_t line, bool extended_rows,
    bool real_sprite_y,
    uint8_t tile_palette_base, uint8_t sprite_palette_base,
    uint8_t max_scanline_sprites, uint8_t max_sprites,
    uint8_t tile_ecm, uint16_t tile_ecm_offset,
    uint8_t sprite_ecm, uint16_t sprite_ecm_offset,
    uint8_t* debug_background, uint8_t* debug_sprite_mask)
{
    uint8_t preserved = 0;
    if (core == NULL)
        return 0;
    core->extendedRows = extended_rows;
    core->realSpriteY = real_sprite_y;
    core->tilePaletteBase = tile_palette_base;
    core->spritePaletteBase = sprite_palette_base;
    core->maxScanlineSprites = max_scanline_sprites;
    core->maxSprites = max_sprites;
    core->tileEcm = tile_ecm;
    core->tileEcmOffset = tile_ecm_offset;
    core->spriteEcm = sprite_ecm;
    core->spriteEcmOffset = sprite_ecm_offset;
    core->debugBackground = debug_background;
    core->debugSpriteMask = debug_sprite_mask;
    if (line == 0)
        preserved = (core->status & 0x20) != 0
            ? core->status : (uint8_t)(core->status & 0xE0);
    /* The replaceable legacy renderer ORs the candidate sprite index into
       S0. Hardware replaces bits 0-4 until the fifth-sprite flag latches. */
    if ((core->status & 0x20) == 0)
        core->status &= 0xE0;
    return preserved;
}

void gcPico9918LegacyEndScanline(
    struct vrEmuTMS9918_s* core, uint8_t preserved_status)
{
    if (core != NULL)
    {
        core->status |= preserved_status;
        core->debugBackground = NULL;
        core->debugSpriteMask = NULL;
    }
}
