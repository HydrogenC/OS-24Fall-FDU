#include <kernel/console.h>
#include <aarch64/intrinsic.h>
#include <kernel/sched.h>
#include <driver/uart.h>

#define CTRL(c) (c - '@')

struct console cons;

void console_init()
{
    /* (Final) TODO BEGIN */
    init_spinlock(&cons.lock);
    init_sem(&cons.sem, 0);

    // Set this to `IBUF_SIZE - 1` so that it could read index 0 after increment
    cons.read_idx = 0;
    cons.write_idx = 0;
    cons.edit_idx = 0;
    /* (Final) TODO END */
}

/**
 * console_write - write to uart from the console buffer.
 * @ip: the pointer to the inode
 * @buf: the buffer
 * @n: number of bytes to write
 */
isize console_write(Inode *ip, char *buf, isize n)
{
    /* (Final) TODO BEGIN */
    acquire_spinlock(&cons.lock);
    for (isize i = 0; i < n; i++) {
        uart_put_char(buf[i]);
    }
    release_spinlock(&cons.lock);
    /* (Final) TODO END */
}

/**
 * console_read - read to the destination from the buffer
 * @ip: the pointer to the inode
 * @dst: the destination
 * @n: number of bytes to read
 */
isize console_read(Inode *ip, char *dst, isize n)
{
    /* (Final) TODO BEGIN */
    // Remaining length to read
    isize len = n;
    acquire_spinlock(&cons.lock);

    while (len > 0) {
        // Nothing new to read
        while (cons.read_idx == cons.write_idx) {
            release_spinlock(&cons.lock);
            if (!wait_sem(&cons.sem)) {
                // Process already killed
                return -1;
            }
            acquire_spinlock(&cons.lock);
        }

        char c = cons.buf[cons.read_idx];
        cons.read_idx++;
        cons.read_idx %= IBUF_SIZE;

        // EOF
        if (c == CTRL('D')) {
            if (len < n) {
                // From xv6:
                // Save ^D for next time, to make sure caller gets a 0-byte result.
                if (cons.read_idx == 0) {
                    cons.read_idx += IBUF_SIZE;
                }
                cons.read_idx--;
            }
            break;
        }

        *dst = c;
        dst++;
        len--;

        // Endl
        if (c == '\n') {
            break;
        }
    }
    release_spinlock(&cons.lock);
    return n - len;
    /* (Final) TODO END */
}

void console_intr(char c)
{
    /* (Final) TODO BEGIN */
    acquire_spinlock(&cons.lock);
    // Special characters
    switch (c) {
    // Backspace
    case '\x7f':
        if (cons.edit_idx != cons.write_idx) {
            if (cons.edit_idx == 0) {
                cons.edit_idx += IBUF_SIZE;
            }
            cons.edit_idx--;
            uart_put_char('\b');
            uart_put_char(' ');
            uart_put_char('\b');
        }
        break;
    case CTRL('U'):
        while (cons.edit_idx != cons.write_idx &&
               cons.buf[cons.edit_idx - 1] != '\n') {
            if (cons.edit_idx == 1) {
                cons.edit_idx += IBUF_SIZE;
            }
            cons.edit_idx--;
            // Clear console
            uart_put_char('\b');
            uart_put_char(' ');
            uart_put_char('\b');
        }
        break;
    case CTRL('D'):
        cons.write_idx = cons.edit_idx;
        __attribute__((fallthrough));
    default:
        // Handle enter key as new line
        if (c == '\r') {
            c = '\n';
        }

        cons.buf[cons.edit_idx++] = c;
        cons.edit_idx %= IBUF_SIZE;
        uart_put_char(c);

        // Flush
        if (c == '\n' || c == CTRL('D')) {
            cons.write_idx = cons.edit_idx;
            post_all_sem(&cons.sem);
        }
        break;
    }
    release_spinlock(&cons.lock);
    /* (Final) TODO END */
}