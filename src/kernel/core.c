#include <aarch64/intrinsic.h>
#include <test/test.h>

NO_RETURN void idle_entry() {
    kalloc_test();
    arch_stop_cpu();
}
