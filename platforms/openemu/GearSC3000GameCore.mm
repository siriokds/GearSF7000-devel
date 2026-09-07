#import "GearSC3000GameCore.h"
#import "OESG1000SystemResponderClient.h"
#import <OpenEmuBase/OEAudioBuffer.h>
#import <OpenEmuBase/OEGameCoreDisplayModes.h>

#include <array>
#include <cstring>
#include <vector>

#include "../../src/GearSF7000Core.h"
#include "../../src/Video.h"

namespace
{
constexpr int kHostVideoWidth = GC_RESOLUTION_WIDTH_WITH_OVERSCAN;
constexpr int kHostVideoHeight = GC_RESOLUTION_HEIGHT_WITH_OVERSCAN;
constexpr NSString *kOverscanPreference = @"overscan";
constexpr NSString *kInternalPictureMode = @"Internal 256x192";
constexpr NSString *kBordersMode = @"Borders";

NSError *CoreError(OEGameCoreErrorCodes code, NSString *description)
{
    return [NSError errorWithDomain:OEGameCoreErrorDomain
                               code:code
                           userInfo:@{NSLocalizedDescriptionKey: description}];
}

bool IsExcludedExtension(NSString *path)
{
    NSString *extension = path.pathExtension.lowercaseString;
    return [extension isEqualToString:@"col"] || [extension isEqualToString:@"cv"];
}

GC_Keys ButtonKey(OESG1000Button button)
{
    static constexpr GC_Keys keys[] = {
        Key_Up, Key_Down, Key_Left, Key_Right,
        Key_Left_Button, Key_Right_Button
    };
    return button < OESG1000ButtonPause ? keys[button] : Key_Left_Button;
}
}

@interface GearSC3000GameCore () <OESG1000SystemResponderClient>
{
    GearSF7000Core *_core;
    Cartridge::ForceConfiguration _configuration;
    std::vector<uint8_t> _engineVideo;
    std::vector<uint8_t> _fallbackVideo;
    std::array<int16_t, GC_AUDIO_BUFFER_SIZE> _audio;
    void *_video;
    bool _showBorders;
}
@end

@implementation GearSC3000GameCore

- (instancetype)init
{
    self = [super init];
    if (self)
    {
        _core = new GearSF7000Core();
        _core->Init(GC_PIXEL_RGB565);
        _core->SetKeyboardMode(true);
        _core->GetVideo()->SetOverscan(Video::OverscanDisabled);
        const size_t maximumVideoBytes = kHostVideoWidth * kHostVideoHeight *
                                         sizeof(uint16_t);
        _engineVideo.resize(maximumVideoBytes);
        _fallbackVideo.resize(maximumVideoBytes);
        _video = _fallbackVideo.data();
        _showBorders = false;
    }
    return self;
}

- (void)dealloc
{
    delete _core;
}

- (BOOL)loadFileAtPath:(NSString *)path error:(NSError **)error
{
    if (IsExcludedExtension(path))
    {
        if (error)
            *error = CoreError(OEGameCoreCouldNotLoadROMError,
                               @"ColecoVision content is not part of GearSC3000.");
        return NO;
    }

    NSString *savedOverscan = self.displayModeInfo[kOverscanPreference];
    _showBorders = [savedOverscan isEqualToString:@"borders"];
    _core->GetVideo()->SetOverscan(_showBorders ? Video::OverscanFull284
                                                : Video::OverscanDisabled);

    if (!_core->LoadROM(path.fileSystemRepresentation, &_configuration))
    {
        if (error)
            *error = CoreError(OEGameCoreCouldNotLoadROMError,
                               @"The cartridge is invalid or unsupported.");
        return NO;
    }

    _core->SetKeyboardMode(true);
    return YES;
}

- (void)executeFrame
{
    int sampleCount = 0;
    _core->RunToVBlank(_engineVideo.data(), _audio.data(), &sampleCount);

    const GC_VideoFrameDescriptor frame = _core->GetVideoFrameDescriptor();
    const size_t sourceRowBytes = static_cast<size_t>(frame.stride_pixels) *
                                  sizeof(uint16_t);
    const size_t destinationRowBytes = kHostVideoWidth * sizeof(uint16_t);
    const uint8_t *source = _engineVideo.data();
    uint8_t *destination = static_cast<uint8_t *>(_video);
    for (int row = 0; row < frame.frame_height; ++row)
        std::memcpy(destination + row * destinationRowBytes,
                    source + row * sourceRowBytes,
                    sourceRowBytes);

    if (sampleCount > 0)
        [[self audioBufferAtIndex:0] write:_audio.data()
                              maxLength:static_cast<NSUInteger>(sampleCount) * sizeof(int16_t)];
}

- (void)resetEmulation
{
    _core->ResetROMPreservingRAM(&_configuration);
}

- (void)stopEmulation
{
    _core->EjectCartridge();
    [super stopEmulation];
}

- (NSTimeInterval)frameInterval
{
    return _core->GetNativeFrameRate();
}

- (const void *)getVideoBufferWithHint:(void *)hint
{
    _video = hint ? hint : _fallbackVideo.data();
    return _video;
}

- (OEIntSize)bufferSize
{
    // OpenEmu's bitmap renderer retains its first allocation. Keep the host
    // surface and row stride fixed while screenRect selects the active frame.
    return OEIntSizeMake(kHostVideoWidth, kHostVideoHeight);
}

- (OEIntRect)screenRect
{
    const GC_VideoRect visible = _core->GetVideoFrameDescriptor().visible_area;
    return OEIntRectMake(visible.x, visible.y, visible.width, visible.height);
}

- (OEIntSize)aspectSize
{
    // OpenEmu's official Genesis Plus core uses the SG-1000 8:7 pixel aspect.
    const GC_VideoFrameDescriptor frame = _core->GetVideoFrameDescriptor();
    return OEIntSizeMake(frame.frame_width * 8, frame.frame_height * 7);
}

- (uint32_t)pixelFormat
{
    return OEPixelFormat_RGB;
}

- (uint32_t)pixelType
{
    return OEPixelType_UNSIGNED_SHORT_5_6_5;
}

- (NSInteger)bytesPerRow
{
    return kHostVideoWidth * sizeof(uint16_t);
}

- (NSArray<NSDictionary<NSString *, id> *> *)displayModes
{
    return @[
        OEDisplayMode_OptionWithStateValue(kInternalPictureMode,
                                           kOverscanPreference,
                                           @(!_showBorders),
                                           @"internal"),
        OEDisplayMode_OptionWithStateValue(kBordersMode,
                                           kOverscanPreference,
                                           @(_showBorders),
                                           @"borders")
    ];
}

- (void)changeDisplayWithMode:(NSString *)displayMode
{
    bool showBorders;
    if ([displayMode isEqualToString:kBordersMode])
        showBorders = true;
    else if ([displayMode isEqualToString:kInternalPictureMode])
        showBorders = false;
    else
        return;

    if (_showBorders == showBorders)
        return;

    _showBorders = showBorders;
    _core->GetVideo()->SetOverscan(showBorders ? Video::OverscanFull284
                                               : Video::OverscanDisabled);

}

- (double)audioSampleRate
{
    return GC_AUDIO_SAMPLE_RATE;
}

- (NSUInteger)channelCount
{
    return 2;
}

- (NSData *)serializeStateWithError:(NSError **)error
{
    size_t size = 0;
    if (!_core->SaveState(nullptr, size) || size == 0)
    {
        if (error)
            *error = CoreError(OEGameCoreCouldNotSaveStateError,
                               @"GearSC3000 could not size the save state.");
        return nil;
    }

    NSMutableData *data = [NSMutableData dataWithLength:size];
    if (!_core->SaveState(static_cast<uint8_t *>(data.mutableBytes), size))
    {
        if (error)
            *error = CoreError(OEGameCoreCouldNotSaveStateError,
                               @"GearSC3000 could not create the save state.");
        return nil;
    }
    data.length = size;
    return data;
}

- (BOOL)deserializeState:(NSData *)state withError:(NSError **)error
{
    if (_core->LoadState(static_cast<const uint8_t *>(state.bytes), state.length))
        return YES;
    if (error)
        *error = CoreError(OEGameCoreCouldNotLoadStateError,
                           @"The save state is invalid or belongs to another cartridge/configuration.");
    return NO;
}

- (void)saveStateToFileAtPath:(NSString *)path
            completionHandler:(void (^)(BOOL, NSError *))completionHandler
{
    NSError *error = nil;
    NSData *state = [self serializeStateWithError:&error];
    const BOOL ok = state && [state writeToFile:path options:NSDataWritingAtomic error:&error];
    completionHandler(ok, error);
}

- (void)loadStateFromFileAtPath:(NSString *)path
              completionHandler:(void (^)(BOOL, NSError *))completionHandler
{
    NSError *error = nil;
    NSData *state = [NSData dataWithContentsOfFile:path options:0 error:&error];
    const BOOL ok = state && [self deserializeState:state withError:&error];
    completionHandler(ok, error);
}

- (oneway void)didPushSG1000Button:(OESG1000Button)button forPlayer:(NSUInteger)player
{
    if (button == OESG1000ButtonPause)
        _core->PauseKeyPressed();
    else if (player >= 1 && player <= 2)
        _core->JoystickPressed(static_cast<GC_Controllers>(player - 1), ButtonKey(button));
}

- (oneway void)didReleaseSG1000Button:(OESG1000Button)button forPlayer:(NSUInteger)player
{
    if (button != OESG1000ButtonPause && player >= 1 && player <= 2)
        _core->JoystickReleased(static_cast<GC_Controllers>(player - 1), ButtonKey(button));
}

@end
