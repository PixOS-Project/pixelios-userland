/* eagl_private.h: the private EAGLContext selectors QuartzCore sends in the render server, declared by hand (the iPhoneOS SDK has
 * no header for them; no Apple file is copied here). This declaration is only a typo guard for the @selector() names and a record of
 * the signatures; we never import the SDK's EAGL.h, so this @interface does not clash with it.
 * Selector names and call sites: research 33 s.2.1 [obs-RE]. Signatures: reference ports iOS-sailfish eagl.c:1705-1860 and
 * hackios-xnu-userland eagl.c:3815-4011, which both run on hardware with these types [src]; neither checked against the iOS 16.7
 * OpenGLES binary here, so every type below is [guess] until reversed (see eagl_objc.m header). */
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>

@interface EAGLContext : NSObject
- (id)initWithAPI:(NSUInteger)api properties:(id)properties;                           /* [guess] api is NSUInteger (both refs) */
- (NSUInteger)API;                                                                     /* [guess] NSUInteger return (sailfish) */
- (BOOL)setParameter:(unsigned)pname to:(const int *)value;                            /* [guess] BOOL return, int* value (R33: &0) */
- (void *)getMacroContextPrivate;                                                      /* [guess] pointer to { rctx, disp[1036] } (R33, sailfish) */
/* the first argument is a GL TARGET (GL_TEXTURE_2D), the texture is whichever is bound: both refs treat it so, R33 s.2.1 wrongly says "tex" */
- (BOOL)attachImage:(NSUInteger)target toCoreSurface:(IOSurfaceRef)surface invertedRender:(BOOL)inverted;   /* [guess] */
- (BOOL)texImageIOSurface:(IOSurfaceRef)surface target:(NSUInteger)target internalFormat:(NSUInteger)internalFormat
                    width:(uint32_t)width height:(uint32_t)height format:(NSUInteger)format type:(NSUInteger)type
                    plane:(uint32_t)plane invert:(BOOL)invert;                         /* [guess] types per sailfish; 3 last args on the stack */
+ (BOOL)setCurrentContext:(EAGLContext *)context;                                      /* [guess] BOOL return (both refs) */
+ (EAGLContext *)currentContext;
@end
