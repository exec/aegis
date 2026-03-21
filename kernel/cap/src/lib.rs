#![no_std]

extern "C" {
    // C declaration: void serial_write_string(const char *s)
    // On x86-64 with GCC, `char` is signed 8-bit and `u8` is unsigned 8-bit.
    // Both are 1 byte with identical ABI representation — safe to call with
    // a Rust byte-string literal (*const u8) on this target.
    fn serial_write_string(s: *const u8);
}

#[repr(C)]
pub struct CapSlot {
    pub kind:   u32,   /* CAP_KIND_* — 0 means empty */
    pub rights: u32,   /* CAP_RIGHTS_* bitfield */
}

const ENOCAP: u32 = 130;

/// Initialize the capability subsystem.
///
/// Phase 11: prints status line, returns immediately.
/// cap_grant and cap_check are now live.
///
/// Note: writes directly to serial rather than through printk because no
/// `printk` Rust FFI wrapper exists yet. This means CAP output does not
/// appear on VGA. Revisit when a printk wrapper is designed.
#[no_mangle]
pub extern "C" fn cap_init() {
    // SAFETY: serial_init() is called in arch_init() before cap_init() is
    // called in kernel_main, so the serial port is fully initialized.
    // serial_write_string is a simple polling write with no shared mutable
    // state and no re-entrancy concerns at this point in boot.
    // The pointer is to a valid C string literal (null-terminated) in read-only data.
    // `char` and `u8` have identical 8-bit ABI representation on x86-64/GCC.
    unsafe {
        serial_write_string(
            c"[CAP] OK: capability subsystem initialized\n".as_ptr() as *const u8
        );
    }
}

/// Write (kind, rights) into the first empty slot of table[0..n).
///
/// Returns the slot index on success, or -(ENOCAP) if all slots are occupied.
#[no_mangle]
pub extern "C" fn cap_grant(
    table: *mut CapSlot,
    n: u32,
    kind: u32,
    rights: u32,
) -> i32 {
    // SAFETY: `table` points to `n` CapSlot entries in the caller's PCB.
    // Called only from proc_spawn with proc->caps and CAP_TABLE_SIZE.
    // The PCB lives for the duration of the process; no concurrent mutation
    // occurs because proc_spawn runs before the task is added to the run queue.
    // The `caps` array is at a 4-byte-aligned offset within the page-aligned
    // PCB allocation (`kva_alloc_pages(1)` returns a page-aligned VA), so the
    // pointer meets the alignment requirement for `CapSlot` (align = 4).
    let slots = unsafe { core::slice::from_raw_parts_mut(table, n as usize) };
    for (i, slot) in slots.iter_mut().enumerate() {
        if slot.kind == 0 {
            slot.kind   = kind;
            slot.rights = rights;
            return i as i32;
        }
    }
    -(ENOCAP as i32)
}

/// Return 0 if table[0..n) contains a slot with matching kind and at least
/// the requested rights; return -(ENOCAP) otherwise.
#[no_mangle]
pub extern "C" fn cap_check(
    table: *const CapSlot,
    n: u32,
    kind: u32,
    rights: u32,
) -> i32 {
    // SAFETY: `table` points to `n` CapSlot entries in the caller's PCB.
    // Called from syscall handlers with proc->caps and CAP_TABLE_SIZE.
    // The PCB is valid for the lifetime of the process; syscalls run on the
    // process's kernel stack with the process's PCB pointer from s_current.
    let slots = unsafe { core::slice::from_raw_parts(table, n as usize) };
    for slot in slots {
        if slot.kind == kind && (slot.rights & rights) == rights {
            return 0;
        }
    }
    -(ENOCAP as i32)
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
