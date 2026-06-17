// tests/toolchain/hello_rs/src/lib.rs
// no_std Rust unit — proves the Rust nightly bare-metal target builds and that
// its staticlib links cleanly with clang/NASM objects via ld.lld.

#![no_std]

use core::panic::PanicInfo;

#[no_mangle]
pub extern "C" fn tc_rust_value() -> i32 {
    7
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}
