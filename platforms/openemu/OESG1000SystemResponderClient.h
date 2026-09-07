/*
 * Interface shared with OpenEmu's built-in SG-1000 system plugin.
 * The numeric order is part of that plugin's responder ABI.
 */
#import <Foundation/Foundation.h>

@protocol OESystemResponderClient;

typedef NS_ENUM(NSUInteger, OESG1000Button)
{
    OESG1000ButtonUp,
    OESG1000ButtonDown,
    OESG1000ButtonLeft,
    OESG1000ButtonRight,
    OESG1000Button1,
    OESG1000Button2,
    OESG1000ButtonPause,
    OESG1000ButtonCount
};

@protocol OESG1000SystemResponderClient <OESystemResponderClient, NSObject>
- (oneway void)didPushSG1000Button:(OESG1000Button)button forPlayer:(NSUInteger)player;
- (oneway void)didReleaseSG1000Button:(OESG1000Button)button forPlayer:(NSUInteger)player;
@end
