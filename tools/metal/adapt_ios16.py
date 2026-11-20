#!/usr/bin/env python3
"""Adapt UniXNU AdrenoMetal (pinned 73d582fd4) to iOS 16.7 on the Adreno 840 (data-a-live).
Stage-2 v1 (2026-11-15): edits 25-30 give every IOSurface import its own dirty-page log
(DirtyPages, HostMemory::with_dirty_pages): the kernel SweetSurface per-surface seed
(a write-unlock moves it; IOSurfaceGetSeed reads it without a trap) and the runtime
reported writes, so each copy of the pages into memory of the device own copies in
only what changed since its last copy (WorkCounts::copied_copy_in_bytes_skipped).


Exact, asserted text edits in a NativeMetal checkout; run once on a fresh tree.

  adapt_ios16.py NATIVEMETAL_DIR

Stage-1 rewrite (2026-11-14, run 14): the pinned tree already carries most of
what this script used to add - DroidVM linear no-copy texture layouts (every
ASTC block format), b3a8 plane computation, MPS heap sizing and aliasing,
allocatedSize/hazardTrackingMode/isDrawable/swizzle, resolve attachments,
protocol conformance, the XR10/PosterBoard answer, IOSurface use counting,
the host-page texture policy and refuse_rebinds - and its own TU_DEBUG and
diagnostics. Re-applying those edits would duplicate upstream definitions and
break the build, and the data-a-live probes (astc probe/replay, capture,
watch, visibility, bridge timing, work-count text report, surface observation,
frame-trace directories) are diagnostics this stage does not carry.

What remains is the iOS 16 client path the pinned tree has no equivalent for:
1. NativeAdrenoMetalPlugin.mm: without NativeAdrenoMetal.publish the plugin
   used to start its stage-1 self-test (raw ABI and PM4 tests written for the
   A740) in every process that runs Metal discovery. Here the clientDevice
   path answers the client device to processes named in NativeAdrenoMetal.clients
   and nil to all others, backboardd included; nothing else runs. TU_DEBUG is
   set from the boot-arg pandora_tu_debug= when given, else nocb (Mesa's own
   default on this chip); a value already in the environment wins.
2. NativeTurnipSmoke.h: Turnip next to the plugin binary, else
   /usr/lib/libvulkan_freedreno.dylib (data-a-live installs it there; no second
   17 MB copy inside the bundle).
3. bridge/NativeMetalObjects.rs: nmb_command_buffer_commit submits with
   commit_and_submit when metal-flow bit 1 asks for it (the pinned runtime's
   own split; the pinned tree already carries flow_bits).
4. NativeBridgeMetalFrontend.mm: every bridge refusal is logged (nbError).
   (The 3-wide function-constant sizes the old script added are already in
   the pinned tree's constantBytes - re-adding them is a duplicate-case
   build error, so that edit is gone.)
5. The pinned tree's NBObject keeps no per-object trace id or mutable
   dictionary; PridePosterExtension aborted on both (post29/post30), so every
   bridge object answers globalTraceObjectID 0 and keeps a userDictionary.
6. NativeAdrenoMetalPlugin.mm: every client loads the iOS 16 private-ABI
   adapter (NezhaStatusBar in the plugin bundle) before its first native
   device - wallpaper extension hosts do not inherit the daemon environment
   that injects it - and the AegirPoster/WeatherPoster wallpaper clients get
   their location reporter wired.
7. The backboardd telemetry thread (refused surfaces, last problems, every
   30 s for 10 rounds) is kept: native_bridge_metal_telemetry is the pinned
   frontend's own function.
8. NativeAdrenoMetalPlugin.mm: the open itself (Vulkan instance and device,
   the warm pass, whatever it compiles then) runs on the registration thread,
   which for a client's first Metal device is a default 512 KiB pthread. The
   runtime's own guard (device/compiles.rs compiles_here) only covers calls
   that may compile, not the open: MobileSafari, PosterBoard and MapKit all
   died in the stack's guard page under _MTLRegisterDevices (the frameless
   part of the open's chain consumed ~358 KiB below the last frame pointer,
   ~90 levels; the plugin's own primer comment says the translator recurses
   deeply). The open now runs on a pthread with the primer's 8 MiB stack and
   the discovery thread only waits for the device.
"""
import pathlib
import sys

ROOT = pathlib.Path(sys.argv[1])


def edit(name, old, new):
    p = ROOT / name
    s = p.read_text(encoding='utf-8')
    assert s.count(old) == 1, (name, old[:70])
    p.write_text(s.replace(old, new), encoding='utf-8')
    print('edited', name)


edit('NativeAdrenoMetalPlugin.mm',
     """#include <pthread.h>
""",
     """#include <pthread.h>
#include <sys/sysctl.h>
""")

edit('NativeAdrenoMetalPlugin.mm',
     """    if (publish::requested()) return publish::deviceForPort(port);
    static bool attempted;""",
     """    if (publish::requested()) return publish::deviceForPort(port);
    // data-a-live (iOS 16, Adreno 840): no stage-1 self-test, it drives the A740's raw ABI. Every Metal client
    // takes its own device through the client path below (the pinned tree keeps no client list); the attempted
    // gate behind this return is never reached.
    {   // data-a-live: TU_DEBUG from the boot-arg pandora_tu_debug=a,b,c when given (bb37), else nocb; a value
        // already in the environment wins either way (mtltest picks its own per launch). Without sysmem Turnip
        // chooses per render pass between on-chip tiles (GMEM) and memory (its autotune), its default on other
        // Adrenos; nocb keeps concurrent binning off (gen8 BV/BR stalls seen elsewhere, not yet understood here).
        static char chosen[128];
        const char *flags = "nocb";
        char args[1024] = {};
        size_t size = sizeof args - 1;
        if (sysctlbyname("kern.bootargs", args, &size, nullptr, 0) == 0)
            if (const char *at = strstr(args, "pandora_tu_debug=")) {
                snprintf(chosen, sizeof chosen, "%.*s", (int)strcspn(at + 17, " "), at + 17);
                flags = chosen;
            }
        setenv("TU_DEBUG", flags, 0);
        static bool told;
        if (!told) {
            told = true;
            NSLog(@"pandora-metal TU_DEBUG=%s", getenv("TU_DEBUG"));
        }
    }
    if (port) return publish::clientDevice(port);
    static bool attempted;""")

edit('bridge/NativeMetalObjects.rs',
     """#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_commit(command_buffer: *mut NmbCommandBuffer) -> bool {
    guard(false, || {
        // The committing thread submits where no other pass is under way, with no wake of the scheduler thread between commit and GPU.
        let command_buffer = &get(command_buffer)?.0;
        if flow_bits() & 2 != 0 {
            command_buffer.commit_and_submit().map_err(e("commit"))?;
        } else {
            command_buffer.commit().map_err(e("commit"))?;
        }
        Ok(true)
    })
}""",
     """// data-a-live: what a completed command buffer's execution status reported beyond a clean run, through syslog (the
// unified log), the first 64 per process: resident-set misses (a stored pointer that reached no resident allocation,
// or null), a read of an argument function constants removed, a device replay fault, a clamped indirect count, a
// cubic argument sampler. post18's hang followed two GPU reads of address 0 by the Astronomy poster.
unsafe extern "C" {
    fn syslog(priority: i32, format: *const c_char, ...);
}

fn nmb_report_execution(done: &CommandBuffer) {
    use std::sync::atomic::{AtomicU32, Ordering};
    static REPORTED: AtomicU32 = AtomicU32::new(0);
    let Some(report) = done.execution_report() else { return };
    if !(report.resident_miss || report.absent_argument_read || report.replay_fault || report.overflow
        || report.cubic_argument_sampler)
    {
        return;
    }
    if REPORTED.fetch_add(1, Ordering::Relaxed) >= 64 {
        return;
    }
    let text = format!(
        "pandora-metal: execution report: resident misses {} ({}), absent argument read {}, replay fault {} ({:#x}), \\
         clamped {}, cubic argument sampler {}\\0",
        report.resident_misses, report.resident_miss, report.absent_argument_read, report.replay_fault,
        report.replay_faults.0, report.overflow, report.cubic_argument_sampler
    );
    unsafe { syslog(3, c"%s".as_ptr(), text.as_ptr() as *const c_char) };
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_commit(command_buffer: *mut NmbCommandBuffer) -> bool {
    guard(false, || {
        // The committing thread submits where no other pass is under way, with no wake of the scheduler thread between commit and GPU.
        let command_buffer = &get(command_buffer)?.0;
        let _ = command_buffer.add_completed_handler(Box::new(nmb_report_execution));
        if flow_bits() & 2 != 0 {
            command_buffer.commit_and_submit().map_err(e("commit"))?;
        } else {
            command_buffer.commit().map_err(e("commit"))?;
        }
        Ok(true)
    })
}""")

edit('NativeAdrenoMetalPlugin.mm',
     """        if (native_bridge_metal_first_problem(problem, sizeof problem) && problem[0]) clientFailure(port, "problem", problem);
    }
    return device;""",
     """        if (native_bridge_metal_first_problem(problem, sizeof problem) && problem[0]) clientFailure(port, "problem", problem);
    }
    if (device && !strcmp(getprogname(), "backboardd")) {
        // data-a-live: the render server as a client (no publish mode here): what it reads for the deep
        // framebuffer, and the bridge's telemetry (refused surfaces, last problems) every 30 s.
        CFTypeRef deep = CFPreferencesCopyAppValue(CFSTR("CAEnableDeepFramebuffer"), kCFPreferencesCurrentApplication);
        NSLog(@"pandora-metal: backboardd has a device; CAEnableDeepFramebuffer %@", (__bridge id)deep);
        if (deep) CFRelease(deep);
        pthread_t thread;
        if (pthread_create(&thread, nullptr, [](void *) -> void * {
                static char table[6144];
                for (unsigned round = 0; round < 10; ++round) {
                    sleep(30);
                    native_bridge_metal_telemetry(table, sizeof table);
                    for (char *line = table, *end; *line; line = end) {
                        end = strchr(line, '\\n');
                        if (end) *end++ = 0; else end = line + strlen(line);
                        NSLog(@"pandora-metal T%u %s", round, line);
                    }
                }
                return nullptr;
            }, nullptr) == 0) pthread_detach(thread);
    }
    return device;""")

edit('NativeBridgeMetalFrontend.mm',
     """static NSError *nbError(NSString *why) {
 return""",
     """static NSError *nbError(NSString *why) {
 NSLog(@"pandora-metal refused: %@", why);   // data-a-live: every refusal in the log (backboardd: specialize, bb7)
 return""")

edit('NativeTurnipSmoke.h',
     """    snprintf(slash + 1, size - size_t(slash + 1 - path), "libvulkan_freedreno.dylib");
    return true;""",
     """    snprintf(slash + 1, size - size_t(slash + 1 - path), "libvulkan_freedreno.dylib");
    if (access(path, R_OK) != 0) snprintf(path, size, "%s", "/usr/lib/libvulkan_freedreno.dylib");   // data-a-live
    return true;""")

edit('NativeBridgeMetalFrontend.mm',
     """@implementation NBObject
- (id<MTLDevice>)device {return gDeviceObject;}
""",
     """@implementation NBObject
- (id<MTLDevice>)device {return gDeviceObject;}
- (uint64_t)globalTraceObjectID {return 0;}
// post30: PridePosterExtension then stopped on -[NBCommandBuffer userDictionary] (Apple's command buffers keep a
// private mutable dictionary for their clients); each bridge object keeps one, made on first use.
- (NSMutableDictionary *)userDictionary {
 @synchronized(self) {
  NSMutableDictionary *d=objc_getAssociatedObject(self,@selector(userDictionary));
  if(!d){d=[NSMutableDictionary dictionary];objc_setAssociatedObject(self,@selector(userDictionary),d,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
  return d;
 }
}
""")

edit('NativeBridgeMetalFrontend.mm',
     """#include <unordered_map>
""",
     """#include <unordered_map>
#include <unordered_set>
#include <atomic>
""")

edit('NativeBridgeMetalFrontend.mm',
     """@implementation NBObject
""",
     """// data-a-live: a question a client asked a bridge object and got NO for (respondsToSelector: and the like), logged once per
// (question, class, name) into the unified log, at most 400 a process (tools/metal/adapt_ios16.py, conformance step 1).
static void pandoraAsked(unsigned how,Class cls,const void *what,const char *name) {
 static const char *const hows[]={"responds","conforms","methodFor"};
 static std::mutex lock;
 static std::unordered_set<uint64_t> seen;
 static std::atomic<bool> full{false};
 if(full.load(std::memory_order_relaxed))return;
 const uint64_t key=((uint64_t)(uintptr_t)cls*1000003u)^((uint64_t)(uintptr_t)what<<2)^how;
 {std::lock_guard<std::mutex> hold(lock);
  if(!seen.insert(key).second)return;
  if(seen.size()>=400)full=true;}
 NSLog(@"pandora-metal asked: %s %s %s -> NO",hows[how],class_getName(cls),name);
}
static BOOL pandoraConforms(Class cls,Protocol *protocol) {
 for(Class c=cls;c;c=class_getSuperclass(c))if(class_conformsToProtocol(c,protocol))return YES;
 return NO;
}
// A method entry whose IMP is NULL answers methodForSelector: with NULL, and a caller that
// caches IMPs and calls directly (RenderBox) dies at pc 0 - the inherited _MTLDevice on this
// OS carries such entries (the pinned tree already special-cases targetDeviceInfo for the
// same reason). Forward them instead: missingSignature/answerMissing answer nil and note it.
extern "C" void _objc_msgForward(void);
#define PANDORA_ASKED_METHODS \\
- (BOOL)respondsToSelector:(SEL)sel { \\
 if(class_respondsToSelector(object_getClass(self),sel))return YES; \\
 pandoraAsked(0,object_getClass(self),sel,sel_getName(sel));return NO; \\
} \\
- (BOOL)conformsToProtocol:(Protocol *)protocol { \\
 if(pandoraConforms(object_getClass(self),protocol))return YES; \\
 pandoraAsked(1,object_getClass(self),(__bridge const void *)protocol,protocol_getName(protocol));return NO; \\
} \\
- (IMP)methodForSelector:(SEL)sel { \\
 IMP imp=class_getMethodImplementation(object_getClass(self),sel); \\
 if(!imp)pandoraAsked(2,object_getClass(self),sel,sel_getName(sel)); \\
 return imp?imp:(IMP)_objc_msgForward; \\
}
@implementation NBObject
PANDORA_ASKED_METHODS
+ (BOOL)instancesRespondToSelector:(SEL)sel {
 if(class_respondsToSelector(self,sel))return YES;
 pandoraAsked(0,self,sel,sel_getName(sel));return NO;
}
""")

edit('NativeBridgeMetalFrontend.mm',
     """@implementation NBDeviceMethods
- (NSString *)name {return gName;}
""",
     """@implementation NBDeviceMethods
// data-a-live: the device class logs the questions its own overrides answer (the pinned tree
// already overrides respondsToSelector: here); methodForSelector: forwards a NULL IMP instead
// of handing one to a caller that calls it directly.
- (BOOL)conformsToProtocol:(Protocol *)protocol {
 if(pandoraConforms(object_getClass(self),protocol))return YES;
 pandoraAsked(1,object_getClass(self),(__bridge const void *)protocol,protocol_getName(protocol));return NO;
}
- (IMP)methodForSelector:(SEL)sel {
 IMP imp=class_getMethodImplementation(object_getClass(self),sel);
 if(!imp)pandoraAsked(2,object_getClass(self),sel,sel_getName(sel));
 return imp?imp:(IMP)_objc_msgForward;
}
- (NSString *)name {return gName;}
""")

edit('NativeBridgeMetalFrontend.mm',
     """free(methods);installAnswers(cls);class_addProtocol(cls,@protocol(MTLDevice));""",
     """free(methods);
 // data-a-live (iOS 16): the inherited _MTLDevice carries method entries whose IMP is NULL on
 // this OS (the pinned tree already special-cases targetDeviceInfo for one of them, and its own
 // ABI check names 17's shape). objc_msgSend jumps to the entry's IMP directly, so any client
 // that sends one dies at pc 0 (SpringBoard, every launch); methodForSelector: never sees it,
 // which is why the asked journal stayed empty. Add a forwarding implementation on the subclass
 // for every NULL-IMP entry of the base: the message then reaches missingSignature and
 // answerMissing, which answer it nil with the true signature and note the selector once.
 {Method *baseMethods=class_copyMethodList(base,&count);
  unsigned forwarded=0;
  for(unsigned i=0;i<count&&i<4096;i++) {
   if(method_getImplementation(baseMethods[i]))continue;
   class_addMethod(cls,method_getName(baseMethods[i]),(IMP)_objc_msgForward,method_getTypeEncoding(baseMethods[i]));
   if(forwarded<64)NSLog(@"pandora-metal null-imp forwarded: %s %s",sel_getName(method_getName(baseMethods[i])),method_getTypeEncoding(baseMethods[i]));
   ++forwarded;
  }
  free(baseMethods);
  NSLog(@"pandora-metal null-imp entries forwarded: %u of %u",forwarded,count);
 }
 installAnswers(cls);class_addProtocol(cls,@protocol(MTLDevice));""")

edit('NativeBridgeMetalFrontend.mm',
     """ if(Protocol *spi=objc_getProtocol("MTLDeviceSPI"))class_addProtocol(cls,spi);
""",
     """ if(Protocol *spi=objc_getProtocol("MTLDeviceSPI"))class_addProtocol(cls,spi);
 // data-a-live: every other bridge class adopts its public Metal protocol too (tools/metal/adapt_ios16.py, conformance step 1).
 const struct {const char *cls;Protocol *protocol;} adopted[]={
  {"NBQueue",@protocol(MTLCommandQueue)},{"NBCommandBuffer",@protocol(MTLCommandBuffer)},
  {"NBRenderEncoder",@protocol(MTLRenderCommandEncoder)},{"NBComputeEncoder",@protocol(MTLComputeCommandEncoder)},
  {"NBBlitEncoder",@protocol(MTLBlitCommandEncoder)},{"NBTexture",@protocol(MTLTexture)},{"NBBuffer",@protocol(MTLBuffer)},
  {"NBHeap",@protocol(MTLHeap)},{"NBLibrary",@protocol(MTLLibrary)},{"NBFunction",@protocol(MTLFunction)},
  {"NBRenderPipeline",@protocol(MTLRenderPipelineState)},{"NBComputePipeline",@protocol(MTLComputePipelineState)},
  {"NBSampler",@protocol(MTLSamplerState)},{"NBDepthStencil",@protocol(MTLDepthStencilState)},{"NBEvent",@protocol(MTLEvent)},
  {"NBSharedEvent",@protocol(MTLSharedEvent)},{"NBFence",@protocol(MTLFence)},{"NBBinaryArchive",@protocol(MTLBinaryArchive)}};
 for(const auto &a:adopted)if(Class c=objc_getClass(a.cls))class_addProtocol(c,a.protocol);
""")

edit('NativeBridgeMetalFrontend.mm',
     """ if(!base || class_getInstanceSize(base)!=536){NBLOG(@"native-metal: unexpected _MTLDevice ABI");return nil;}""",
     """ // data-a-live: _MTLDevice is 528 bytes on iOS 16.7 (536 on iPadOS 17.7.11, the only size the
 // pinned check accepted - this return-nil was the whole crash: every client's open answered nil,
 // backboardd rendered nothing and SpringBoard died in Metal's nil-device path at pc 0). The
 // dynamic subclass adds no instance variables and reads none of the base's, so both sizes hold.
 const size_t pandoraAbi=class_getInstanceSize(base);
 if(!base || (pandoraAbi!=536 && pandoraAbi!=528)){NBLOG(@"native-metal: unexpected _MTLDevice ABI %zu",(size_t)pandoraAbi);return nil;}""")

edit('NativeAdrenoMetalPlugin.mm',
     """static id clientDevice(unsigned port) {
    char turnip[1024];""",
     """static id clientDevice(unsigned port) {
    NSLog(@"pandora-metal client0 %s enter", getprogname());
    char turnip[1024];""")

edit('NativeAdrenoMetalPlugin.mm',
     """    char turnip[1024];
    if (!turnip::libraryPath""",
     """    // Every native client needs the iOS private-ABI adapter, including ImageIO's
    // texture layouts and Core Image stitching. Extension hosts do not inherit
    // the daemon environment that injects it into SpringBoard/backboardd.
    static dispatch_once_t compatibilityOnce;
    static void *compatibility;
    dispatch_once(&compatibilityOnce, ^{
        compatibility = dlopen("/System/Library/Extensions/NativeAdrenoMetal.bundle/NezhaStatusBar", RTLD_NOW | RTLD_LOCAL);
        if (compatibility) NSLog(@"NEZHA Metal: client compatibility ready for %s", getprogname());
    });
    if (!compatibility) {
        const char *detail = dlerror();
        char error[256];
        snprintf(error, sizeof error, "%s", detail ? detail : "unavailable");
        NSLog(@"NEZHA Metal: client compatibility load failed: %s", error);
        clientFailure(port, "no-ios16-shim", error);
        return nil;
    }
    if (!strcmp(getprogname(), "AegirPoster")) {
        gClientPort = port;
        static dispatch_once_t reporterOnce;
        dispatch_once(&reporterOnce, ^{
            if (auto setReporter = (void (*)(void (*)(const char *)))dlsym(compatibility, "nezha_wallpaper_location_set_reporter"))
                setReporter([](const char *text) {
                    if (Gpu *gpu = makeGpu(gClientPort)) { streamText(*gpu, text); delete gpu; }
                });
        });
    }
    char turnip[1024];
    if (!turnip::libraryPath""")

edit('NativeAdrenoMetalPlugin.mm',
     '    if (!strcmp(getprogname(), "AegirPoster")) {\n        gClientPort = port;\n',
     '    if (!strcmp(getprogname(), "AegirPoster") || !strcmp(getprogname(), "WeatherPoster")) {\n        gClientPort = port;\n')

# ---- data-a-live: step log through the client open path (the pc-0 hunt) --------
# Three trials died at the same pc 0 with the same backtrace, and neither the asked
# journal nor the NULL-IMP enumeration ever printed: the crash is inside
# native_bridge_metal_open (or before it), under objc_msgSend's reach. Log every step;
# backboardd takes the same clientDevice path and survives, so the SpringBoard vs
# backboardd step traces name the exact fork point.
edit('NativeAdrenoMetalPlugin.mm',
     """        @try {
            device = native_bridge_metal_open(turnip, candidates[i], registryID, nullptr);
        } @catch (NSException *exception) {""",
     """        NSLog(@"pandora-metal client2 open dir %u", i);
        @try {
            device = native_bridge_metal_open(turnip, candidates[i], registryID, nullptr);
            NSLog(@"pandora-metal client3 open returned %@", device ? @"device" : @"nil");
        } @catch (NSException *exception) {""")
edit('NativeBridgeMetalFrontend.mm',
     """extern "C" id native_bridge_metal_open(const char *loader,const char *cache,uint64_t registryID,void (*missing)(const char *,const char *)) {
 if(gDeviceObject)return gDeviceObject;
 gMissing=missing;gInFlight=[NSMutableSet set];""",
     """extern "C" id native_bridge_metal_open(const char *loader,const char *cache,uint64_t registryID,void (*missing)(const char *,const char *)) {
 if(gDeviceObject)return gDeviceObject;
 gMissing=missing;gInFlight=[NSMutableSet set];
 NSLog(@"pandora-metal open0 %s enter",getprogname());""")
edit('NativeBridgeMetalFrontend.mm',
     """ gDevice=nmb_device_open(loader,cache);""",
     """ NSLog(@"pandora-metal open1 nmb_device_open begin");
 gDevice=nmb_device_open(loader,cache);
 NSLog(@"pandora-metal open2 device=%p",(void *)gDevice);""")
edit('NativeBridgeMetalFrontend.mm',
     """ Class cls=objc_allocateClassPair(base,"NativeBridgeMetalDevice",0);if(!cls)return nil;""",
     """ NSLog(@"pandora-metal open3 subclass begin");
 Class cls=objc_allocateClassPair(base,"NativeBridgeMetalDevice",0);if(!cls)return nil;""")

# ---- data-a-live: the open on a big stack (the client cold-start stack overflow) --------
# The 8b1d557 bundle's first device of a cold client process crashed in the stack's guard page
# under _MTLRegisterDevices (MobileSafari 3x, PosterBoard, MapKit 2x, identical 9-frame plugin
# backtrace): the discovery thread is a default 512 KiB pthread and the open's own chain
# recurses deeper through frameless functions (358 KiB below the last frame pointer). The
# runtime hands compile calls over from small threads (compiles.rs) but nothing guards the
# open itself. Run it on the primer's 8 MiB stack (PrimeShaders.h precedent) and wait.
edit('NativeAdrenoMetalPlugin.mm',
     """    gpu->port = port;
    return gpu;
}
""",
     """    gpu->port = port;
    return gpu;
}

// data-a-live (iOS 16, Adreno 840): the open itself runs the runtime's whole device bring-up inline -
// Vulkan instance and device, the warm pass, whatever it compiles then - and that chain recurses deeply
// through frameless functions, below the last frame pointer: on the default 512 KiB pthread that a
// client's Metal discovery runs on it reached the stack's guard page (MobileSafari, PosterBoard and
// MapKit all died there under _MTLRegisterDevices, every first device of a cold process). The primer
// already runs the same translator on an 8 MiB stack; the open runs on one too, and the discovery
// thread only waits and takes the device back.
static id openOnBigStack(const char *turnip, const char *cache, uint64_t registryID) {
    struct Open { const char *turnip; const char *cache; uint64_t registryID; id device; };
    Open open = {turnip, cache, registryID, nil};
    pthread_t thread;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 8u << 20);
    const int made = pthread_create(&thread, &attributes, [](void *arg) -> void * {
        Open *o = static_cast<Open *>(arg);
        @autoreleasepool {
            @try { o->device = native_bridge_metal_open(o->turnip, o->cache, o->registryID, nullptr); }
            @catch (NSException *exception) { o->device = nil; }
        }
        return nullptr;
    }, &open);
    pthread_attr_destroy(&attributes);
    if (made != 0) {
        @autoreleasepool {
            @try { return native_bridge_metal_open(turnip, cache, registryID, nullptr); }
            @catch (NSException *exception) { return nil; }
        }
    }
    (void)pthread_join(thread, nullptr);
    return open.device;
}
""")

edit('NativeAdrenoMetalPlugin.mm',
     """        @try {
            device = native_bridge_metal_open(turnip, candidates[i], registryID, nullptr);
            NSLog(@"pandora-metal client3 open returned %@", device ? @"device" : @"nil");
        } @catch (NSException *exception) {""",
     """        @try {
            // data-a-live: on the 8 MiB stack (the 512 KiB discovery thread reached its guard page).
            device = openOnBigStack(turnip, candidates[i], registryID);
            NSLog(@"pandora-metal client3 open returned %@", device ? @"device" : @"nil");
        } @catch (NSException *exception) {""")

edit('NativeAdrenoMetalPlugin.mm',
     """        @try {
            device = native_bridge_metal_open(turnip, path, registryID, nullptr);
        } @catch (NSException *exception) {
            device = nil;
            lastError = "exception";
        }
""",
     """        @try {
            device = openOnBigStack(turnip, path, registryID);
        } @catch (NSException *exception) {
            device = nil;
            lastError = "exception";
        }
""")

edit('NativeAdrenoMetalPlugin.mm',
     """            @try {
                device = native_bridge_metal_open(turnip, candidates[i], registryID, nullptr);
            } @catch (NSException *exception) {
                device = nil;
            }
""",
     """            @try {
                device = openOnBigStack(turnip, candidates[i], registryID);
            } @catch (NSException *exception) {
                device = nil;
            }
""")

# Stage-2 v1 (2026-11-15): the IOSurface dirty-page log, edits 25-30.
# Edit 31 (2026-11-15): the DP telemetry row - the log's counters ride the existing 30 s table.
# 1. The trait and its stamp, for the log the surface import attaches.
edit('bridge/NativeMetalObjects.rs',
     "    ComputePipelineState, Deallocator, DebugMessage, DepthStencilState, Device, DeviceOptions, MessageSeverity, MessageSink, Event, EventRef, Fence, Function, Heap, HeapDescriptor, HeapType, HostMemory, IoSurfacePlane, IoSurfaceYCbCr, Library, LoadAction,",
     "    ComputePipelineState, Deallocator, DebugMessage, DepthStencilState, Device, DeviceOptions, MessageSeverity, MessageSink, DirtyPages, DirtyStamp, Event, EventRef, Fence, Function, Heap, HeapDescriptor, HeapType, HostMemory, IoSurfacePlane, IoSurfaceYCbCr, Library, LoadAction,")

# 2. The dirty-page log itself, the no-copy entry's surface parameter, and the attachment.
edit('bridge/NativeMetalObjects.rs',
     """unsafe fn no_copy(device: *mut NmbDevice, pointer: *mut c_void, length: u64, options_word: u64, deallocator: Option<Deallocator>)
                  -> *mut NmbBuffer {
    guard(std::ptr::null_mut(), || {
        let device = get(device)?;
        // SAFETY: the caller's contract.
        let base = NonNull::new(pointer as *mut u8).ok_or("null memory")?;
        // SAFETY: the caller's contract.
        let memory = unsafe { HostMemory::new(base) };""",
     """// ---- data-a-live: the dirty-page log of an imported IOSurface (HostMemory::with_dirty_pages) ----

// The kernel's SweetSurface keeps a per-surface seed in a state page every importer maps read-only
// (the lookup result's +0x08); a write-unlock (IOSurfaceUnlock without kIOSurfaceLockReadOnly)
// increments it in the kernel, and IOSurfaceGetSeed reads it back without a trap. The runtime
// reports its own writes into the pages (a shadow's copy back, a merge, a replaceRegion) through
// DirtyPages::wrote_runs, so a log over an IOSurface's pages = the seed's moves and those reports.
// A process writing its mapped pages without the lock protocol is seen by neither: the periodic
// whole-range answer covers it (the runtime then copies every byte of that surface once more).
unsafe extern "C" {
    fn IOSurfaceGetID(surface: *const c_void) -> u32;
    fn IOSurfaceGetSeed(surface: *const c_void) -> u32;
    fn CFRetain(cf: *const c_void) -> *const c_void;
    fn CFRelease(cf: *const c_void);
}

/// How often a whole-range answer is made regardless of the records (a bounded window over CPU
/// writes that touched the mapped pages with no lock protocol).
const SURFACE_DIRTY_VALVE: u64 = 64;
/// Records kept past a whole-range answer: the runtime's reported writes between two of them.
const SURFACE_DIRTY_RECORDS: usize = 256;

/// The bridge's IOSurface dirty-page answers: logs made, whole-range answers (a seed move, a watch
/// restart, records outgrown), the runtime's recorded writes, and records consolidated away.
static SURFACE_DIRTY: [std::sync::atomic::AtomicU64; 4] = [const { std::sync::atomic::AtomicU64::new(0) }; 4];

struct SurfaceDirtyPages {
    /// The retained IOSurfaceRef: the seed's state page and the kernel's surface stay alive for it.
    surface: *const c_void,
    /// The bytes the log's offsets count (the import's length).
    bytes: u64,
    /// Every whole-range answer and every record carries the next publication's stamp.
    stamp: std::sync::atomic::AtomicU64,
    /// The kernel seed last observed: a move from it is a whole-range write.
    seed_last: std::sync::atomic::AtomicU32,
    /// The stamp of the last whole-range answer: a query at or before it takes the whole range.
    watermark: std::sync::atomic::AtomicU64,
    /// (start, end, stamp): the runtime's reported writes, every one later than the watermark.
    runs: std::sync::Mutex<Vec<(u64, u64, u64)>>,
    /// Answers of changed() made: every SURFACE_DIRTY_VALVE-th is whole-range.
    changed_calls: std::sync::atomic::AtomicU64,
    /// Live watches: a rise from zero restarts the continuous watch at a fresh watermark.
    watches: std::sync::atomic::AtomicU64,
    /// The log's own weak handle, for its watches.
    self_weak: std::sync::OnceLock<std::sync::Weak<SurfaceDirtyPages>>,
}

// SAFETY: the surface pointer is a retained CF object the log's calls only read (its seed), and
// every field's own locks and orderings carry the rest.
unsafe impl Send for SurfaceDirtyPages {}
unsafe impl Sync for SurfaceDirtyPages {}

impl SurfaceDirtyPages {
    fn new(surface: *const c_void, bytes: u64) -> SurfaceDirtyPages {
        SurfaceDirtyPages {
            surface,
            bytes,
            stamp: std::sync::atomic::AtomicU64::new(0),
            seed_last: std::sync::atomic::AtomicU32::new(u32::MAX),
            watermark: std::sync::atomic::AtomicU64::new(0),
            runs: std::sync::Mutex::new(Vec::new()),
            changed_calls: std::sync::atomic::AtomicU64::new(0),
            watches: std::sync::atomic::AtomicU64::new(0),
            self_weak: std::sync::OnceLock::new(),
        }
    }

    /// A whole-range write: the watermark moves to a fresh stamp and the records begin again, so
    /// the log's memory stays bounded no matter how long the pages live.
    fn whole_written(&self) {
        let stamp = self.stamp.fetch_add(1, std::sync::atomic::Ordering::AcqRel) + 1;
        self.watermark.store(stamp, std::sync::atomic::Ordering::Release);
        self.runs.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).clear();
        SURFACE_DIRTY[1].fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    }

    /// A write the runtime reported: a record after the watermark, cut to the log's bytes.
    fn record(&self, range: std::ops::Range<u64>) {
        if range.start >= self.bytes {
            return;
        }
        let end = range.end.min(self.bytes);
        let stamp = self.stamp.fetch_add(1, std::sync::atomic::Ordering::AcqRel) + 1;
        {
            let mut runs = self.runs.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            if runs.len() < SURFACE_DIRTY_RECORDS {
                runs.push((range.start, end, stamp));
                SURFACE_DIRTY[2].fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                return;
            }
        }
        // The records outgrew their window: one whole-range answer stands for them all.
        self.whole_written();
        SURFACE_DIRTY[3].fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    }

    /// The kernel's seed, observed: a move from the last value seen is a whole-range write.
    fn sync_seed(&self) {
        // SAFETY: the surface is retained for the log's life; the seed is a plain read of it.
        let seed = unsafe { IOSurfaceGetSeed(self.surface) };
        let last = self.seed_last.swap(seed, std::sync::atomic::Ordering::AcqRel);
        if seed != last {
            self.whole_written();
        }
    }
}

impl Drop for SurfaceDirtyPages {
    fn drop(&mut self) {
        // SAFETY: the log's own retain, taken in surface_dirty_log.
        unsafe { CFRelease(self.surface) };
    }
}

/// A watch of the log's pages, ended by its drop.
struct SurfaceWatch(Option<std::sync::Arc<SurfaceDirtyPages>>);

impl Drop for SurfaceWatch {
    fn drop(&mut self) {
        if let Some(log) = self.0.as_ref() {
            log.watches.fetch_sub(1, std::sync::atomic::Ordering::AcqRel);
        }
    }
}

impl DirtyPages for SurfaceDirtyPages {
    type Watch = SurfaceWatch;

    fn watch(&self, _range: std::ops::Range<u64>) -> SurfaceWatch {
        if self.watches.fetch_add(1, std::sync::atomic::Ordering::AcqRel) == 0 {
            // A continuous watch restarts: every stamp handed before its watermark answers whole.
            self.whole_written();
        }
        SurfaceWatch(self.self_weak.get().and_then(std::sync::Weak::upgrade))
    }

    fn request(&self) -> DirtyStamp {
        self.sync_seed();
        DirtyStamp(self.stamp.load(std::sync::atomic::Ordering::Acquire))
    }

    fn wait(&self, _stamp: DirtyStamp) -> bool {
        // Publications are synchronous (the seed is shared memory and the records are made in the
        // call), so every stamp handed out is already published.
        true
    }

    fn changed(&self, since: DirtyStamp, out: &mut Vec<std::ops::Range<u64>>) {
        self.sync_seed();
        let calls = self.changed_calls.fetch_add(1, std::sync::atomic::Ordering::AcqRel);
        if calls % SURFACE_DIRTY_VALVE == SURFACE_DIRTY_VALVE - 1
            || since.0 <= self.watermark.load(std::sync::atomic::Ordering::Acquire) {
            out.push(0..self.bytes);
            return;
        }
        let runs = self.runs.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        for &(start, end, stamp) in runs.iter() {
            if stamp > since.0 {
                out.push(start..end);
            }
        }
    }

    fn wrote(&self, range: std::ops::Range<u64>) {
        self.record(range);
    }
}

/// The bridge's IOSurface dirty-page logs, by the kernel's surface id: one log per surface, alive
/// while any import of its pages is (the registry holds it weakly, so its stamps end with it).
fn surface_dirty_log(surface: *const c_void, bytes: u64) -> Option<std::sync::Arc<SurfaceDirtyPages>> {
    static LOGS: std::sync::OnceLock<
        std::sync::Mutex<std::collections::HashMap<u32, std::sync::Weak<SurfaceDirtyPages>>>,
    > = std::sync::OnceLock::new();
    // SAFETY: the frontend's surface, valid for this call.
    let id = unsafe { IOSurfaceGetID(surface) };
    if id == 0 {
        return None;    // no registry identity (the frontend's own import key checks the same)
    }
    let logs = LOGS.get_or_init(|| std::sync::Mutex::new(std::collections::HashMap::new()));
    let mut logs = logs.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    if let Some(log) = logs.get(&id).and_then(std::sync::Weak::upgrade) {
        return Some(log);
    }
    // SAFETY: the log's own reference, released in its drop (the import's deallocator has its own).
    unsafe { CFRetain(surface) };
    let log = std::sync::Arc::new(SurfaceDirtyPages::new(surface, bytes));
    if log.self_weak.set(std::sync::Arc::downgrade(&log)).is_err() {
        // SAFETY: undo the log's own retain (an empty OnceLock never fails, but stay correct).
        unsafe { CFRelease(surface) };
        return None;
    }
    logs.insert(id, std::sync::Arc::downgrade(&log));
    SURFACE_DIRTY[0].fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    Some(log)
}

/// The dirty-page counters into the caller's twelve words: the runtime's copy-in bytes and the
/// bytes a log's answer skipped, the copies gated by a log, answered whole and their bytes copied,
/// stamps asked, waits and waits unanswered, then the bridge's own (logs made, whole-range
/// answers, records, records consolidated away).
#[unsafe(no_mangle)]
pub extern "C" fn nmb_device_dirty_counts(device: *const NmbDevice, out: *mut u64) -> bool {
    use std::sync::atomic::Ordering::Relaxed;
    guard(false, || {
        if out.is_null() {
            return Err("null counts".into());
        }
        // SAFETY: the header's twelve words.
        let out = unsafe { std::slice::from_raw_parts_mut(out, 12) };
        out.fill(0);
        if !device.is_null() {
            let w = get(device)?.0.work_counts();
            let words = [w.copied_copy_in_bytes, w.copied_copy_in_bytes_skipped, w.dirty_copies_in_gated,
                         w.dirty_copies_in_whole, w.dirty_copy_in_bytes_copied, w.dirty_stamps_asked,
                         w.dirty_waits, w.dirty_waits_unanswered];
            out[..words.len()].copy_from_slice(&words);
        }
        out[8] = SURFACE_DIRTY[0].load(Relaxed);
        out[9] = SURFACE_DIRTY[1].load(Relaxed);
        out[10] = SURFACE_DIRTY[2].load(Relaxed);
        out[11] = SURFACE_DIRTY[3].load(Relaxed);
        Ok(true)
    })
}

/// `nmb_buffer_new_no_copy_releasing` over an IOSurface, with the surface's own dirty-page log
/// attached: the kernel's per-surface seed and the runtime's reported writes, by which every copy
/// of these pages into memory of the device's own copies in only what changed since its last copy.
///
/// # Safety
/// `surface` is an IOSurfaceRef the caller keeps alive until `release` runs; `pointer` as
/// `nmb_buffer_new_no_copy_releasing`'s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_buffer_new_no_copy_surface_releasing(device: *mut NmbDevice, pointer: *mut c_void,
                                                                  length: u64, options_word: u64, release: Option<NmbRelease>,
                                                                  context: *mut c_void, surface: *const c_void) -> *mut NmbBuffer {
    let deallocator = release.map(|release| {
        let context = ReleaseContext(context);
        Box::new(move |pointer: NonNull<u8>, length: u64| {
            let context = context;
            // SAFETY: the caller's contract.
            unsafe { release(context.0, pointer.as_ptr() as *mut c_void, length) }
        }) as Deallocator
    });
    let surface = if surface.is_null() { None } else { Some(surface) };
    // SAFETY: the caller's contract.
    unsafe { no_copy(device, pointer, length, options_word, deallocator, surface) }
}

unsafe fn no_copy(device: *mut NmbDevice, pointer: *mut c_void, length: u64, options_word: u64,
                  deallocator: Option<Deallocator>, surface: Option<*const c_void>)
                  -> *mut NmbBuffer {
    guard(std::ptr::null_mut(), || {
        let device = get(device)?;
        // SAFETY: the caller's contract.
        let base = NonNull::new(pointer as *mut u8).ok_or("null memory")?;
        // SAFETY: the caller's contract. An IOSurface's import carries the surface's own
        // dirty-page log, so every copy of its pages into memory of the device's own copies in
        // only what changed since its last copy.
        let memory = match surface.and_then(|surface| surface_dirty_log(surface, length)) {
            Some(log) => unsafe { HostMemory::new(base).with_dirty_pages(log) },
            None => unsafe { HostMemory::new(base) },
        };""")

# 3. The plain no-copy entry keeps no log.
edit('bridge/NativeMetalObjects.rs',
     "    unsafe { no_copy(device, pointer, length, options_word, None) }",
     "    unsafe { no_copy(device, pointer, length, options_word, None, None) }")

# 4. The deallocator entry of the plain frontend path keeps no log either.
edit('bridge/NativeMetalObjects.rs',
     "    unsafe { no_copy(device, pointer, length, options_word, deallocator) }",
     "    unsafe { no_copy(device, pointer, length, options_word, deallocator, None) }")

# 5. The C ABI: the surface variant and the counters.
edit('bridge/NativeMetalObjects.h',
     """NmbBuffer *nmb_buffer_new_no_copy_releasing(NmbDevice *, void *pointer, uint64_t length, uint64_t options,
                                            void (*release)(void *context, void *pointer, uint64_t length), void *context);""",
     """NmbBuffer *nmb_buffer_new_no_copy_releasing(NmbDevice *, void *pointer, uint64_t length, uint64_t options,
                                            void (*release)(void *context, void *pointer, uint64_t length), void *context);
/* data-a-live: the same, over an IOSurface: the pages carry the surface's own dirty-page log (the kernel's
 * per-surface seed and the runtime's reported writes), so every copy of them into memory of the device's
 * own copies in only what changed since its last copy. `surface` stays alive until the import is freed. */
NmbBuffer *nmb_buffer_new_no_copy_surface_releasing(NmbDevice *, void *pointer, uint64_t length, uint64_t options,
                                                    void (*release)(void *context, void *pointer, uint64_t length), void *context,
                                                    const void *surface);
/* data-a-live: dirty-page counters, twelve words: copy-in bytes and bytes skipped by a log's answer,
 * copies gated by a log, answered whole and their bytes copied, stamps asked, waits, waits unanswered,
 * then logs made, whole-range answers, recorded writes, records consolidated away. */
bool nmb_device_dirty_counts(const NmbDevice *, uint64_t out[12]);""")

# 6. The frontend's surface import carries the surface for the log.
edit('NativeBridgeMetalFrontend.mm',
     " NmbBuffer *handle=nmb_buffer_new_no_copy_releasing(gDevice,base,bytes,MTLResourceStorageModeShared,releaseSurface,(void *)CFRetain(surface));",
     " NmbBuffer *handle=nmb_buffer_new_no_copy_surface_releasing(gDevice,base,bytes,MTLResourceStorageModeShared,releaseSurface,(void *)CFRetain(surface),(const void *)surface);")
edit('NativeBridgeMetalFrontend.mm',
     """  uint64_t tc[8]={};
  if(nmb_device_texture_counts(gDevice,tc))
   row("L inp %llu/%llu rf %llu/%llu sh %llu cp %llu bk %llu rt %llu",(ull)tc[0],(ull)tc[1],(ull)tc[2],
    (ull)tc[3],(ull)tc[4],(ull)tc[5],(ull)tc[6],(ull)tc[7]);""",
     """  uint64_t tc[8]={};
  if(nmb_device_texture_counts(gDevice,tc))
   row("L inp %llu/%llu rf %llu/%llu sh %llu cp %llu bk %llu rt %llu",(ull)tc[0],(ull)tc[1],(ull)tc[2],
    (ull)tc[3],(ull)tc[4],(ull)tc[5],(ull)tc[6],(ull)tc[7]);
  // data-a-live: the IOSurface dirty-page log's answers (nmb_device_dirty_counts): copy-in bytes the
  // log skipped vs copied, dirty-gated copies and whole answers, stamps and waits; the bridge's own
  // logs, whole-range answers, records and consolidations.
  uint64_t dp[12]={};
  if(nmb_device_dirty_counts(gDevice,dp))
   row("DP cb %llu skip %llu gated %llu whole %llu dbc %llu stamps %llu waits %llu/%llu | logs %llu wr %llu rec %llu cons %llu",
    (ull)dp[0],(ull)dp[1],(ull)dp[2],(ull)dp[3],(ull)dp[4],(ull)dp[5],(ull)dp[6],(ull)dp[7],
    (ull)dp[8],(ull)dp[9],(ull)dp[10],(ull)dp[11]);""")

# Stage-3 (2026-11-20): GPU command-buffer timestamp measurement lever.
# boot arg pandora_m2v_gputimes=host -> env -> options.gpu_times_from_host.
# Off by default (the env is unset unless the boot arg is present), so this is a
# zero-change default path. Turning it on spares each command buffer its two GPU
# timestamp writes (and avoids the wait on drivers whose timestamps wait for every
# queue), but it forgoes queue sharing -- so it is an A/B measurement lever, not a
# silent default. Both edits are simulated against the adapted export tree before
# the CI compile; the Rust field is defined in runtime create.rs (gpu_times_from_host).
edit('NativeAdrenoMetalPlugin.mm',
     '        if (bootArgSet("wallpaper-freeze=1")) setenv("NATIVE_METAL_FREEZE_WALLPAPER", "1", 1);',
     '        // data-a-live: GPU command-buffer timestamps from the host clock (measurement lever,\n'
     '        // off by default). Sparing the two GPU timestamp writes per command buffer; on a\n'
     '        // driver whose timestamps wait for every queue this also avoids that stall.\n'
     '        if (bootArgSet("pandora_m2v_gputimes=host")) setenv("PANDORA_M2V_GPUTIMES", "host", 1);\n'
     '        else unsetenv("PANDORA_M2V_GPUTIMES");\n'
     '        if (bootArgSet("wallpaper-freeze=1")) setenv("NATIVE_METAL_FREEZE_WALLPAPER", "1", 1);')
edit('bridge/NativeMetalObjects.rs',
     '        options.messages = Some(MessageSink::new(observe_message));',
     '        options.messages = Some(MessageSink::new(observe_message));\n'
     '        // data-a-live: pandora_m2v_gputimes=host -> host-clocked GPU times (measurement\n'
     '        // lever, default Metal timestamps). Note this forgoes queue sharing; measure A/B.\n'
     '        options.gpu_times_from_host = std::env::var("PANDORA_M2V_GPUTIMES").as_deref() == Ok("host");')

# Stage-4 (2026-11-20): restore the per-pass RP telemetry report (diagnostic only).
# The CI turnip build already injects tu_pandora_render_stats (turnip.yml runs
# tools/turnip/pandora_rpstats.py), but the CI frontend never grew the report block
# the local rp-stats build carried, so RP lines vanished from the CI bundle. Re-add
# it after the RM block of native_bridge_metal_telemetry: dlsym(RTLD_DEFAULT) finds
# the driver however the runtime loaded it (the loader-path dlopen above binds
# nothing in some processes, leaving TUC/RM empty there). Counting/reporting only;
# no rendering change. Anchor verified unique in the adapted export tree.
edit('NativeBridgeMetalFrontend.mm',
     """    row("RM gmem %llu %lluM | sys auto %llu %lluM other %llu %lluM | exact %llu",(ull)rm[0],(ull)(rm[1]/1000),(ull)rm[2],
     (ull)(rm[3]/1000),(ull)rm[4],(ull)(rm[5]/1000),(ull)rm[6]);
   }
  }""",
     """    row("RM gmem %llu %lluM | sys auto %llu %lluM other %llu %lluM | exact %llu",(ull)rm[0],(ull)(rm[1]/1000),(ull)rm[2],
     (ull)(rm[3]/1000),(ull)rm[4],(ull)(rm[5]/1000),(ull)rm[6]);
   }
  }
  // data-a-live: how Turnip drew each render pass (tu_pandora_render_stats, tools/turnip/pandora_rpstats.py):
  // GMEM passes vs sysmem and every gmem_disable_reason. dlsym(RTLD_DEFAULT) finds the driver however the
  // runtime loaded it; the loader-path dlopen above binds nothing in some processes (TUC/RM stay empty).
  {
   static void (*rpStats)(char *,size_t);static bool looked=false;
   if(!looked){looked=true;rpStats=(void (*)(char *,size_t))dlsym(RTLD_DEFAULT,"tu_pandora_render_stats");}
   if(rpStats) {
    char text[512]={};rpStats(text,sizeof text);
    for(const char *at=text;*at;) {
     size_t left=strlen(at);const char *end;
     if(left<=84)end=at+left;else {end=at+84;while(end>at && *end!=' ')--end;if(end==at)end=at+84;}
     char save=*(char *)end;*(char *)end=0;row("RP %s",at);*(char *)end=save;
     at=(*end?end+1:end);
    }
   }
  }""")

# Stage-5 (2026-11-20): fix the diagnostic gates' initialization-order hazard.
# diagOn()/gpuTraceOn()/traceWanted() cache their getenv answer in a static const
# on first call. When the first call lands before deviceForPort sets SHENG_DIAG /
# SHENG_GPU_TRACE from the boot args, the cache stays false for the process's
# life: tracePush drops every record and traceStartWriter never starts, so
# gpu-trace=1 boots write no frames.trace (observed 2026-11-20: K rows prove
# completed() runs, yet all three trace paths kept the previous boot's file).
# Re-read the env until it answers yes; the env is set once at the first device
# open and never unset afterwards, so the cached-true form is unchanged.
# Diagnostic-only; no rendering change. Anchors verified unique in the export tree.
edit('NativeBridgeMetalFrontend.mm',
     'static bool gpuTraceOn(){static const bool on=getenv("SHENG_GPU_TRACE")!=nullptr;return on;}',
     'static bool gpuTraceOn(){static bool on=false;if(!on)on=getenv("SHENG_GPU_TRACE")!=nullptr;return on;}')
edit('NativeBridgeMetalFrontend.mm',
     'static bool diagOn() {static const bool on=getenv("SHENG_DIAG")!=nullptr;return on;}',
     'static bool diagOn() {static bool on=false;if(!on)on=getenv("SHENG_DIAG")!=nullptr;return on;}')
edit('NativeBridgeMetalFrontend.mm',
     'static bool traceWanted() {static const bool on=diagOn() && nativeIsRenderServer();return on;}',
     'static bool traceWanted() {static bool on=false;if(!on)on=diagOn() && nativeIsRenderServer();return on;}')

# Stage-6 (2026-11-20): diagnostic-only NSLog to locate why the frame-trace writer never writes in
# backboardd even with Stage-5 applied and SHENG_DIAG set by publish::deviceForPort. Observed
# 2026-11-20 flicker-trace3 (epoch 1795163371): gatefix metal installed (cdhash 7d22c767), yet the only
# frames.trace on device stayed the previous boot's (sha 8fac7091, type0 wall 2026-11-03 pid57); the
# writer thread never opened/truncated any of the 3 candidate paths. The static-cache root-cause model
# is disproven; these logs reveal the actual gate state inside the render server. No rendering change.
# Anchors are single lines verified unique in the export tree.
edit('NativeBridgeMetalFrontend.mm',
     ' if(!diagOn() || !render)return;',
     ' {static std::atomic<uint32_t> tswN{0};uint32_t k=tswN.fetch_add(1);if(k<5||k%2000==0)NSLog(@"pandora-metal TSW n=%u diag=%d render=%d DIAG=%s GPUTRACE=%s started=%d",(unsigned)k,diagOn()?1:0,render?1:0,getenv("SHENG_DIAG")?:"null",getenv("SHENG_GPU_TRACE")?:"null",gTraceStarted.load()?1:0);}\n if(!diagOn() || !render)return;')
edit('NativeBridgeMetalFrontend.mm',
     '   if(render && fd<0)for(const char *path:paths){fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0644);if(fd>=0)break;}',
     '   if(render && fd<0)for(const char *path:paths){fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0644);if(fd>=0){NSLog(@"pandora-metal TSW writer opened fd=%d path=%s",fd,path);break;}}')
edit('NativeBridgeMetalFrontend.mm',
     '   if(head<written+4)continue;',
     '   {static uint32_t lp=0;if(++lp%40==1)NSLog(@"pandora-metal TSW loop head=%u written=%u fd=%d",(unsigned)head,(unsigned)written,fd);}\n   if(head<written+4)continue;')

# Stage-7 (2026-11-20): diagnostic-only NSLog inside bootArgSet. Stage-6 proved SHENG_DIAG/null in
# backboardd+SpringBoard; bootArgSet("diag=1") false despite full 598-byte bootargs at launchd.
# Suspect sysctlbyname("kern.bootargs") fails in guest. Log rc/errno/size/arglen/tail. No render change.
edit('NativeAdrenoMetalPlugin.mm',
     '    if (sysctlbyname("kern.bootargs", args, &size, nullptr, 0) != 0) return false;',
     '    int baRc = sysctlbyname("kern.bootargs", args, &size, nullptr, 0);\n    {static uint32_t baN=0;uint32_t k=baN++;if(k<10)NSLog(@"pandora-metal BA key=%s rc=%d errno=%d size=%zu arglen=%zu tail=%.50s",key,baRc,errno,size,strlen(args),args+(strlen(args)>50?strlen(args)-50:0));}\n    if (baRc != 0) return false;')
