#define pushp(a, b) stp a, b, [sp, #-0x10]!
#define popp(a, b) ldp a, b, [sp], #0x10 

/* `exception_vector.S` send all traps here. */
.global trap_entry
trap_entry:
// TODO: save UserContext
pushp(x0, x1)
pushp(x29, lr)
pushp(x27, x28)
pushp(x25, x26)
pushp(x23, x24)
pushp(x21, x22)
pushp(x19, x20)
bl trap_global_handler

.global trap_return
trap_return:
// TODO: restore UserContext

eret
