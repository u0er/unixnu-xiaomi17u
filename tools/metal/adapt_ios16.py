#!/usr/bin/env python3
"""Adapt UniXNU AdrenoMetal (pinned 73d582fd4) to iOS 16.7 on the Adreno 840 (data-a-live).
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

