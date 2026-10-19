#ifndef PICO9918_LEGACY_CORE_BRIDGE_H
#define PICO9918_LEGACY_CORE_BRIDGE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

struct vrEmuTMS9918_s;

typedef struct GcPico9918LegacyDebugState
{
    uint16_t address;
    uint8_t status;
    uint8_t control_latch_pending;
    uint8_t control_latch_value;
    uint8_t read_ahead_buffer;
} GcPico9918LegacyDebugState;

#ifdef __cplusplus
extern "C" {
#endif

size_t gcPico9918LegacyStateSize(void);
void gcPico9918LegacyPowerOn(struct vrEmuTMS9918_s* core);
void gcPico9918LegacySaveState(
    const struct vrEmuTMS9918_s* core, void* destination);
bool gcPico9918LegacyLoadState(
    struct vrEmuTMS9918_s* core, const void* source, size_t size);
bool gcPico9918LegacyReadDebugState(
    const struct vrEmuTMS9918_s* core,
    GcPico9918LegacyDebugState* state);
bool gcPico9918LegacyWriteVram(
    struct vrEmuTMS9918_s* core, uint16_t address, uint8_t value);
uint8_t gcPico9918LegacyReadVram(
    const struct vrEmuTMS9918_s* core, uint16_t address);
void gcPico9918LegacyCancelControlLatch(struct vrEmuTMS9918_s* core);
void gcPico9918LegacyWriteStatus(
    struct vrEmuTMS9918_s* core, uint8_t value);
void gcPico9918LegacyAdjustAddress(
    struct vrEmuTMS9918_s* core, int16_t delta);
uint8_t gcPico9918LegacyBeginScanline(
    struct vrEmuTMS9918_s* core, uint16_t line, bool extended_rows,
    bool real_sprite_y,
    uint8_t tile_palette_base, uint8_t sprite_palette_base,
    uint8_t max_scanline_sprites, uint8_t max_sprites,
    uint8_t tile_ecm, uint16_t tile_ecm_offset,
    uint8_t sprite_ecm, uint16_t sprite_ecm_offset,
    uint8_t* debug_background, uint8_t* debug_sprite_mask);
void gcPico9918LegacyEndScanline(
    struct vrEmuTMS9918_s* core, uint8_t preserved_status);

#ifdef __cplusplus
}
#endif

#endif
