/* third_party/musl-overlay/__set_thread_area.s — PROC-D, ADR-023 §D7.2.
 *
 * Replaces musl's src/thread/x86_64/__set_thread_area.s (which issues
 * arch_prctl(ARCH_SET_FS, p)). PRADYOS has no arch_prctl; the thread pointer is
 * set with SYS_SET_TLS (NSI 27), which takes the FS base in arg1 (RDI) — exactly
 * the register musl's __set_thread_area(void *p) receives p in. So just load the
 * NSI number and syscall; the kernel programs IA32_FS_BASE and returns 0. */
.text
.global __set_thread_area
.hidden __set_thread_area
.type __set_thread_area,@function
__set_thread_area:
	movl $27, %eax          /* SYS_SET_TLS; p is already in %rdi */
	syscall
	ret
