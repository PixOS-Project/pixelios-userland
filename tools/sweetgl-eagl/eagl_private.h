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
- (uint32_t)setParameter:(unsigned)pname to:(const int *)value;                        /* status, 0 = success (not BOOL) [obs-RE research 34 s.1.1, 0x1d3f52064] */
- (void *)getMacroContextPrivate;                                                      /* [guess] pointer to { rctx, disp[1036] } (R33, sailfish) */
/* the first argument is a GL TARGET (0xde1 or 0x8d41 GL_RENDERBUFFER per research 34; the shim accepts only 0xde1) (GL_TEXTURE_2D), the texture is whichever is bound: both refs treat it so, R33 s.2.1 wrongly says "tex" */
- (BOOL)attachImage:(NSUInteger)target toCoreSurface:(IOSurfaceRef)surface invertedRender:(BOOL)inverted;   /* [guess] */
- (BOOL)texImageIOSurface:(IOSurfaceRef)surface target:(NSUInteger)target internalFormat:(NSUInteger)internalFormat
                    width:(uint32_t)width height:(uint32_t)height format:(NSUInteger)format type:(NSUInteger)type
                    plane:(uint32_t)plane invert:(BOOL)invert;                         /* [guess] types per sailfish; 3 last args on the stack */
+ (BOOL)setCurrentContext:(EAGLContext *)context;                                      /* [guess] BOOL return (both refs) */
+ (EAGLContext *)currentContext;
@end

/* eagl_objc.m: install the EAGLContext swizzles; 0 or -errno. */
int sweet_eagl_install(void);
