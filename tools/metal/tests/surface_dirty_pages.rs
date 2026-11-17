//! Host checks of the actual injected SurfaceDirtyPages implementation. The runner extracts that
//! implementation and the pinned runtime's DirtyPages trait. Only CF/IOSurface and the diagnostic
//! device snapshot are mocked; publication logic and exported counter writes are production code.
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Barrier};

#[export_name = "IOSurfaceGetID"]
unsafe extern "C" fn mock_id(surface: *const c_void) -> u32 {
    surface as usize as u32
}

#[export_name = "IOSurfaceGetSeed"]
unsafe extern "C" fn mock_seed(surface: *const c_void) -> u32 {
    unsafe { &*surface.cast::<AtomicU32>() }.load(Ordering::Acquire)
}

#[export_name = "CFRetain"]
extern "C" fn mock_retain(surface: *const c_void) -> *const c_void { surface }

#[export_name = "CFRelease"]
extern "C" fn mock_release(_surface: *const c_void) {}

fn log(seed: &AtomicU32, bytes: u64) -> Arc<SurfaceDirtyPages> {
    let log = Arc::new(SurfaceDirtyPages::new((seed as *const AtomicU32).cast(), bytes));
    assert!(log.self_weak.set(Arc::downgrade(&log)).is_ok());
    log
}

fn changed(log: &SurfaceDirtyPages, since: DirtyStamp) -> Vec<Range<u64>> {
    let mut out = Vec::new();
    log.changed(since, &mut out);
    out
}

#[test]
fn whole_publication_is_already_in_an_equal_stamp() {
    let seed = AtomicU32::new(7);
    let log = log(&seed, 8192);
    let _watch = log.watch(0..8192);
    let held = log.request();
    assert!(changed(&log, held).is_empty());
    seed.store(8, Ordering::Release);
    assert_eq!(changed(&log, held), vec![0..8192]);
    assert!(changed(&log, log.request()).is_empty());
}

#[test]
fn runtime_write_is_cut_and_invalid_ranges_do_not_publish() {
    let seed = AtomicU32::new(1);
    let log = log(&seed, 8192);
    let _watch = log.watch(0..8192);
    let held = log.request();
    log.wrote(100..100);
    log.wrote(200..100);
    log.wrote(8192..9000);
    assert_eq!(log.request(), held);
    log.wrote(4096..9000);
    assert_eq!(changed(&log, held), vec![4096..8192]);
    assert!(changed(&log, log.request()).is_empty());
}

#[test]
fn unwatched_and_restarted_watches_cannot_reuse_an_old_hold() {
    let seed = AtomicU32::new(1);
    let log = log(&seed, 8192);
    let watch = log.watch(0..8192);
    let held = log.request();
    drop(watch);
    assert_eq!(changed(&log, held), vec![0..8192]);
    let _watch = log.watch(4096..8192);
    assert_eq!(changed(&log, held), vec![0..8192]);
    assert!(changed(&log, log.request()).is_empty());
    assert_eq!(changed(&log, DirtyStamp(u64::MAX)), vec![0..8192]);
}

#[test]
fn bounded_log_compaction_preserves_the_write_that_overflowed() {
    let seed = AtomicU32::new(1);
    let log = log(&seed, 8192);
    let _watch = log.watch(0..8192);
    let held = log.request();
    for _ in 0..=SURFACE_DIRTY_RECORDS { log.wrote(1..2); }
    assert_eq!(changed(&log, held), vec![0..8192]);
    assert!(log.runs.lock().unwrap().len() <= SURFACE_DIRTY_RECORDS);
    assert!(changed(&log, log.request()).is_empty());
}

#[test]
fn cpu_seed_cannot_authorize_skipping_external_gpu_writes() {
    let seed = AtomicU32::new(1);
    let log = log(&seed, 8192);
    let _watch = log.watch(0..8192);
    // No seed move does not establish that another process's GPU left these pages alone.
    assert!(!log.wait(log.request()));
}

#[test]
fn different_import_lengths_do_not_share_truncated_logs() {
    let seed = AtomicU32::new(1);
    let surface = (&seed as *const AtomicU32).cast();
    let log = surface_dirty_log(surface, 4096).unwrap();
    assert!(Arc::ptr_eq(&log, &surface_dirty_log(surface, 4096).unwrap()));
    assert!(surface_dirty_log(surface, 8192).is_none());
}

#[test]
fn request_cannot_publish_a_record_still_blocked_on_the_log_lock() {
    let seed = AtomicU32::new(1);
    let log = log(&seed, 8192);
    let _watch = log.watch(0..8192);
    let held = log.request();
    let locked = log.runs.lock().unwrap();
    let gate = Arc::new(Barrier::new(2));
    let worker = {
        let log = log.clone();
        let gate = gate.clone();
        std::thread::spawn(move || { gate.wait(); log.wrote(0..4096); })
    };
    gate.wait();
    // A writer waiting for this lock must not yet have advanced the publication stamp.
    // A timeout here gives the blocked writer a chance to expose an early stamp publication.
    let end = std::time::Instant::now() + std::time::Duration::from_millis(20);
    while std::time::Instant::now() < end {
        assert_eq!(log.stamp.load(Ordering::Acquire), held.0);
        std::thread::yield_now();
    }
    drop(locked);
    worker.join().unwrap();
    assert_eq!(changed(&log, held), vec![0..4096]);
}

#[test]
fn overlapping_writers_keep_all_records_after_the_hold() {
    let seed = AtomicU32::new(1);
    let log = log(&seed, 16384);
    let _watch = log.watch(0..16384);
    let held = log.request();
    std::thread::scope(|scope| {
        for page in 0..4 {
            let log = &log;
            scope.spawn(move || {
                for _ in 0..32 { log.wrote(page * 4096..(page + 1) * 4096); }
            });
        }
    });
    let out = changed(&log, held);
    for page in 0..4 { assert!(out.contains(&(page * 4096..(page + 1) * 4096))); }
    assert!(changed(&log, log.request()).is_empty());
}

#[test]
fn original_counter_abi_writes_only_twelve_words() {
    let mut out = [u64::MAX; 13];
    assert!(nmb_device_dirty_counts(std::ptr::null(), out.as_mut_ptr()));
    assert_eq!(&out[..8], &[0; 8]);
    assert_eq!(out[12], u64::MAX);
    assert!(!nmb_device_dirty_counts(std::ptr::null(), std::ptr::null_mut()));
}

#[test]
#[cfg(surface_dirty_v2)]
fn extended_counter_abi_keeps_cpu_and_shadow_bytes_separate() {
    let device = NmbDevice(MockDevice);
    let expected = device.0.work_counts();
    let mut out = [u64::MAX; 17];
    assert!(nmb_device_dirty_counts_v2(&device, out.as_mut_ptr()));
    assert_eq!(out[0], expected.copied_copy_in_bytes);
    assert_eq!(out[1], expected.copied_copy_in_bytes_skipped);
    assert_eq!(out[4], expected.dirty_copy_in_bytes_copied);
    assert_eq!(out[12], expected.dirty_copy_in_bytes_whole);
    assert_eq!(out[13], expected.dirty_copies_in_skipped);
    assert_eq!(out[14], expected.dirty_wait_nanos);
    assert_eq!(out[15], expected.dirty_holds_withheld);
    assert_eq!(out[16], u64::MAX);
}
