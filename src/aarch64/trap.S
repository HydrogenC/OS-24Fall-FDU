#define pushp(a, b) stp a, b, [sp, #-0x10]!
#define popp(a, b) ldp a, b, [sp], #0x10 

/* `exception_vector.S` send all traps here. */
// Reference: https://github.com/rcore-os/trapframe-rs/blob/master/src/arch/aarch64/trap.S
.global trap_entry
trap_entry:
// TODO: save UserContext
// Save general-purpose registers
pushp(x28, x29)
pushp(x26, x27)
pushp(x24, x25)
pushp(x22, x23)
pushp(x20, x21)
pushp(x18, x19)
pushp(x16, x17)
pushp(x14, x15)
pushp(x12, x13)
pushp(x10, x11)
pushp(x8, x9)
pushp(x6, x7)
pushp(x4, x5)
pushp(x2, x3)
pushp(x0, x1)

// Push lr and reserve 8 bytes
pushp(lr, x0)

// Now use the general registers to transfer system registers
mrs x1, spsr_el1
mrs x2, elr_el1
mrs x3, tpidr_el0
mrs x4, sp_el0
pushp(x1, x2)
pushp(x3, x4)

// Return the address to the user context
mov x0, sp

bl trap_global_handler

.global trap_return
trap_return:
// TODO: restore UserContext
// Pop system registers
popp(x3, x4)
popp(x1, x2)
msr tpidr_el0, x3
msr sp_el0, x4
msr spsr_el1, x1
msr elr_el1, x2

// Pop lr and reserved 8 bytes
popp(lr, x0)

// Restore general-purpose registers
popp(x0, x1)
popp(x2, x3)
popp(x4, x5)
popp(x6, x7)
popp(x8, x9)
popp(x10, x11)
popp(x12, x13)
popp(x14, x15)
popp(x16, x17)
popp(x18, x19)
popp(x20, x21)
popp(x22, x23)
popp(x24, x25)
popp(x26, x27)
popp(x28, x29)

eret
