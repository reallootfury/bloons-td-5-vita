/*
 * BTD5 4.7 nativeTick indirect engine-call bridge.
 *
 * The verified instruction sequence at SO+0x00500994 is:
 *   ldr r1, [r0]
 *   ldr r1, [r1, #0x18]
 *   blx r1
 *   ldr r0, [r5, #0x20]
 *
 * patch.c replaces those eight bytes with an absolute jump here. This bridge
 * reproduces the sequence, surrounds only the virtual call with the sampled
 * timer, reloads r0 as the original code did, and continues at
 * SO+0x0050099C. The unsampled path preserves r0-r3 exactly as the original
 * call site would present them to the virtual function and performs no timer
 * syscall.
 */

.syntax unified
.thumb
.text
.align 2
#ifdef BTD5_NATIVE_PHASE_PROFILER
.global btd5_engine_callsite_trampoline
.type btd5_engine_callsite_trampoline, %function
.thumb_func
btd5_engine_callsite_trampoline:
    ldr     r1, [r0]
    ldr     r1, [r1, #0x18]

    /* r12 is call-clobbered. Use it for the profiler flag so r0-r3 still
     * match the original call site when the virtual function is entered. */
    ldr     r12, =btd5_engine_profile_sample_active
    ldr     r12, [r12]
    cmp     r12, #0
    bne     1f

    /* Unsampled fast path. r5 is nativeTick's callee-saved global pointer. */
    blx     r1
    ldr     r0, [r5, #0x20]
    ldr     r12, =btd5_engine_callsite_continue
    ldr     r12, [r12]
    bx      r12

1:
    /* Preserve nativeTick's live callee-saved state plus the original r2/r3
     * values. Six pushed registers and the 8-byte timestamp slot keep SP
     * 8-byte aligned across every C/virtual call. */
    push    {r4-r8, lr}
    sub     sp, sp, #8
    mov     r4, r0              /* engine / original r0 */
    mov     r5, r1              /* virtual target / original r1 */
    mov     r6, r2              /* original r2 */
    mov     r7, r3              /* original r3 */

    mov     r0, r4
    mov     r1, r5
    bl      btd5_engine_call_profile_enter
    strd    r0, r1, [sp]

    /* Recreate the exact argument-register state at the original BLX. */
    mov     r0, r4
    mov     r1, r5
    mov     r2, r6
    mov     r3, r7
    blx     r5

    ldrd    r0, r1, [sp]
    mov     r2, r5
    bl      btd5_engine_call_profile_exit

    add     sp, sp, #8
    pop     {r4-r8, lr}

    /* r5 has now been restored to nativeTick's original global pointer. */
    ldr     r0, [r5, #0x20]
    ldr     r12, =btd5_engine_callsite_continue
    ldr     r12, [r12]
    bx      r12

.size btd5_engine_callsite_trampoline, .-btd5_engine_callsite_trampoline

/*
 * Nested game/screen update bridge inside the verified outer engine function
 * SO+0x0020ACC4. The original sequence at SO+0x0020AD6C is:
 *   ldr r1, [r0]
 *   ldr r2, [r1, #0x18]
 *   mov r1, r5
 *   blx r2
 *
 * r0 is the active screen/game object, r5 is the frame context, and r2 is the
 * dynamic update target. The target's r0 return value is immediately consumed
 * by the direct call at SO+0x0020AD74, so the sampled path explicitly preserves
 * that value across the C timing callback.
 */
.align 2
.global btd5_inner_update_callsite_trampoline
.type btd5_inner_update_callsite_trampoline, %function
.thumb_func
btd5_inner_update_callsite_trampoline:
    ldr     r1, [r0]
    ldr     r2, [r1, #0x18]
    mov     r1, r5

    ldr     r12, =btd5_engine_profile_sample_active
    ldr     r12, [r12]
    cmp     r12, #0
    bne     2f

    /* Unsampled fast path reproduces the original four instructions. */
    blx     r2
    ldr     r12, =btd5_inner_update_callsite_continue
    ldr     r12, [r12]
    bx      r12

2:
    /* Six registers (24 bytes) plus 16 bytes local storage preserve the
     * required 8-byte stack alignment. */
    push    {r4-r8, lr}
    sub     sp, sp, #16
    mov     r4, r0              /* active screen/game object */
    mov     r6, r2              /* dynamic inner update target */
    mov     r7, r3              /* original r3 */
    /* r5 already contains the original frame context and is callee-saved. */

    mov     r0, r4
    mov     r1, r5
    mov     r2, r6
    bl      btd5_inner_update_profile_enter
    strd    r0, r1, [sp]

    mov     r0, r4
    mov     r1, r5
    mov     r2, r6
    mov     r3, r7
    blx     r6
    str     r0, [sp, #8]        /* preserve target return value */

    ldrd    r0, r1, [sp]
    mov     r2, r6
    bl      btd5_inner_update_profile_exit

    ldr     r0, [sp, #8]
    add     sp, sp, #16
    pop     {r4-r8, lr}
    ldr     r12, =btd5_inner_update_callsite_continue
    ldr     r12, [r12]
    bx      r12

.size btd5_inner_update_callsite_trampoline, .-btd5_inner_update_callsite_trampoline

#endif /* BTD5_NATIVE_PHASE_PROFILER */

#ifdef BTD5_FRAME_DEBT_CLAMP
/*
 * Experimental BTD5 4.7 frame-debt clamp.
 *
 * The exact sequence beginning at SO+0x0020AD56 is:
 *   ldr.w r0, [r4, #0x124]
 *   ldr   r5, [r6]
 *   ldr.w r0, [r0, #0xd0]
 *   ldr   r1, [r5]
 *   bl    SO+0x004E7FFC
 *
 * The first eight bytes are replaced with an absolute jump here. We recreate
 * the complete sequence, but cap a positive, finite frame delta above 1/30 s
 * to 1/30 s before both the prepare call and the following game update read
 * it through r5. The game writes the next frame delta again on the next tick.
 */
.align 2
.global btd5_frame_debt_callsite_trampoline
.type btd5_frame_debt_callsite_trampoline, %function
.thumb_func
btd5_frame_debt_callsite_trampoline:
    ldr.w   r0, [r4, #0x124]
    ldr     r5, [r6]
    ldr.w   r0, [r0, #0xd0]
    ldr     r1, [r5]

    /* Positive IEEE-754 values compare in integer order. Do not touch zero,
     * negative, NaN, infinity, or implausible >=1 second values. r2/r3 are
     * preserved until the original direct call because its exact signature
     * is not known. */
    cmp     r1, #0
    bmi     2f
    ldr     r12, =0x3f800000       /* 1.0f upper sanity bound */
    cmp     r1, r12
    bhs     2f
    ldr     r12, =0x3d088889       /* approximately 1.0f / 30.0f */
    cmp     r1, r12
    bls     2f
    str     r12, [r5]
    mov     r1, r12

    ldr     r12, =btd5_frame_debt_prepare_target
    ldr     r12, [r12]
    blx     r12

    /* The original call may clobber r0-r3, so count only after it returns. */
    ldr     r2, =btd5_frame_debt_clamp_count
    ldr     r3, [r2]
    adds    r3, #1
    str     r3, [r2]
    b       3f

2:
    ldr     r12, =btd5_frame_debt_prepare_target
    ldr     r12, [r12]
    blx     r12

3:
    ldr     r12, =btd5_frame_debt_callsite_continue
    ldr     r12, [r12]
    bx      r12

.size btd5_frame_debt_callsite_trampoline, .-btd5_frame_debt_callsite_trampoline
#endif /* BTD5_FRAME_DEBT_CLAMP */
