#define pushp(a, b) stp a, b, [sp, #-0x10]!
#define popp(a, b) ldp a, b, [sp], #0x10 

/* `exception_vector.S` send all traps here. */
// Reference: https://github.com/rcore-os/trapframe-rs/blob/master/src/arch/aarch64/trap.S
.global trap_entry
trap_entry:
// TODO: save UserContext

// Save general-purpose registers (x0 is error type, don't need to save)
pushp(x29, x30)
pushp(x27, x28)
pushp(x25, x26)
pushp(x23, x24)
pushp(x21, x22)
pushp(x19, x20)
pushp(x17, x18)
pushp(x15, x16)
pushp(x13, x14)
pushp(x11, x12)
pushp(x9, x10)
pushp(x7, x8)
pushp(x5, x6)
pushp(x3, x4)
pushp(x1, x2)

// Now use the general registers to transfer system registers
mrs x1, spsr_el1
mrs x2, elr_el1
pushp(x1, x2)
// Reserve space for `sp` and `tpidr`
add sp, sp, #-0x10

// Pass `sp` as the first param for trap handler
mov x0, sp
bl trap_global_handler

.global trap_return
trap_return:
// TODO: restore UserContext

// Reserve space for `sp` and `tpidr`
add sp, sp, #0x10
// Pop system registers
popp(x1, x2)
msr spsr_el1, x1
msr elr_el1, x2

// Restore general-purpose registers
popp(x1, x2)
popp(x3, x4)
popp(x5, x6)
popp(x7, x8)
popp(x9, x10)
popp(x11, x12)
popp(x13, x14)
popp(x15, x16)
popp(x17, x18)
popp(x19, x20)
popp(x21, x22)
popp(x23, x24)
popp(x25, x26)
popp(x27, x28)
popp(x29, x30)
eret
