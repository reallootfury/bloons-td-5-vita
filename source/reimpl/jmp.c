/*
 * Android/Bionic and Vita newlib have incompatible private jmp_buf layouts.
 * libnative.so allocates Android-sized storage, so save only the ARM state
 * that is safe to restore across the loader boundary.
 */

__attribute__((naked, returns_twice))
int bionic_setjmp(void *env)
{
    __asm__ volatile(
        "stmia r0, {r4-r11}\n"
        "str sp, [r0, #32]\n"
        "str lr, [r0, #36]\n"
        "add r1, r0, #40\n"
        "vstmia r1, {d8-d15}\n"
        "vmrs r1, fpscr\n"
        "str r1, [r0, #104]\n"
        "mov r0, #0\n"
        "bx lr\n"
    );
}

__attribute__((naked, noreturn))
void bionic_longjmp(void *env, int value)
{
    __asm__ volatile(
        "mov r2, r0\n"
        "cmp r1, #0\n"
        "bne 1f\n"
        "mov r1, #1\n"
        "1:\n"
        "add r3, r2, #40\n"
        "vldmia r3, {d8-d15}\n"
        "ldr r3, [r2, #104]\n"
        "vmsr fpscr, r3\n"
        "ldr sp, [r2, #32]\n"
        "ldr lr, [r2, #36]\n"
        "ldmia r2, {r4-r11}\n"
        "mov r0, r1\n"
        "bx lr\n"
    );
}

__attribute__((naked, returns_twice))
int bionic_sigsetjmp(void *env, int save_mask)
{
    (void)save_mask;
    __asm__ volatile("b bionic_setjmp\n");
}

__attribute__((naked, noreturn))
void bionic_siglongjmp(void *env, int value)
{
    (void)value;
    __asm__ volatile("b bionic_longjmp\n");
}
