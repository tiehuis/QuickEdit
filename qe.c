/*
 * QuickEdit (qe) - v0.2.0
 * =======================
 *
 * A tiny editor intended for quick edits of gigantic files.
 *
 * ----------------------------------------------------------------------------
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Marc Tiehuis
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

enum edit_mode {
    MODE_NORMAL = 0,
    MODE_INSERT,
    MODE_SEARCH,
};

static const char *edit_mode_string[] = {
    "NORMAL",
    "INSERT",
    "SEARCH",
};

static struct {
    // Filename of <file>. Points to argv.
    const char *filename;

    // File descriptor of <file>.
    int fd;

    // Stat info of <file>.
    struct stat file;

    // Whether view matches editor state.
    volatile sig_atomic_t dirty;

    // dirty implies the following but the converse is not true. These flags
    // are for more efficient partial redraws.
    int dirty_cursor;
    int dirty_status;

    // Whether the file should be mapped in read-only mode. If read-only, then
    // insert mode is completely disabled.
    int read_only;

    // Whether edits should be synced to the page instantly or batched until
    // explicitly requested by the user.
    int batched_save;

    // Whether we are in wrapping mode (default: no wrap)
    int wrap;

    // Messages on bottom of screen.
    char status_buffer[64];

    // Virtual memory-mapped pages of <file>.
    uint8_t *page;

    // Byte offset into <page>. Always on new-line boundary.
    //
    // TODO: Not always on new-line boundary if wrapping extends over an entire
    // view page itself. In this case we want to adjust page_offset to a
    // non-boundary and proceed as normal, changes how page movement occurs
    // depending on the mode, though.
    //
    // We cannot use a size_t for our offset since mapped file may be larger
    // than the address space in say a 32-bit system.
    //
    // 63-bits should be sufficient and makes some compares a bit nicer.
    int64_t page_offset;

    // X-axis offset from <page_offset>.
    int64_t page_offset_x;

    // Position of cursor in editor (0-indexed, relative to window).
    uint16_t cursor_x;
    uint16_t cursor_y;

    // Current active edit mode.
    enum edit_mode mode;

    // Buffer for search string.
    char search_buf[64];

    // Length of current search term.
    size_t search_len;
} editor;

static struct {
    // Terminal dimensions.
    int16_t width;
    int16_t height;

    // Whether a terminal resize event occurred.
    volatile sig_atomic_t resized;

    // Original terminal settings prior to program start.
    struct termios original_settings;

    // Whether we have entered raw mode yet. Used only if we encounter an
    // error before the terminal setup was complete.
    int raw_mode;
} terminal;

static void qe_terminal_cleanup(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminal.original_settings);

    // restore screen content
    printf("\x1b[?47l");
}

__attribute__((noreturn))
static void fatal(const char *msg)
{
    if (terminal.raw_mode) {
        qe_terminal_cleanup();
    }

    printf("%s", msg);
    if (errno) {
        printf(" - %s", strerror(errno));
    }
    printf("\n");
    exit(1);
}

// TODO: Print \x08 byte value and return character printed count
// (don't exceed end).
static void print_char(char c)
{
    if (isprint(c)) {
        putchar(c);
    } else {
        printf("\x1b[2m");  // dim
        putchar('@');
        printf("\x1b[0m");  // reset color
    }
}

static int qe_draw_wrap(void)
{
    int64_t offset = editor.page_offset;
    int y;
    for (y = 0; y < terminal.height - 1; ++y) {
        for (int x = 0; x < terminal.width - 1; ++x) {
            const char c = editor.page[offset];
            offset += 1;

            if (offset >= editor.file.st_size) {
                goto file_end;
            }

            if (c == '\n') {
                goto next_row;
            } else {
                print_char(c);
            }
        }

next_row:
        printf("\x1b[E");
    }

    return y;

file_end:
    printf("\x1b[E");
    return y;
}

static int qe_draw_nowrap(void)
{
    int64_t offset = editor.page_offset;
    int y;
    for (y = 0; y < terminal.height - 1; ++y) {
        // clip start of lines
        if (offset + editor.page_offset_x >= editor.file.st_size) {
            goto file_end;
        }
        uint8_t *beg = memchr(editor.page + offset, '\n', editor.page_offset_x);
        if (beg) {
            // new-line encountered so early break
            goto next_row;
        }
        offset += editor.page_offset_x;

        int x;
        for (x = 0; x < terminal.width; ++x) {
            const char c = editor.page[offset];
            offset += 1;

            if (offset >= editor.file.st_size) {
                goto file_end;
            }

            if (c == '\n') {
                goto next_row;
            } else {
                print_char(c);
            }
        }

        // Clip line endings (scan for next).
        // This needs to be fast to handle files with very long single lines.
        // So we prefer memchr vs. a simple loop.
        uint8_t *end = memchr(editor.page + offset, '\n', editor.file.st_size - offset);
        if (!end) {
            goto file_end;
        }

        offset = end - &editor.page[offset];

    next_row:
        printf("\x1b[E");
    }

    return y;

file_end:
    printf("\x1b[E");
    return y;
}

static void qe_draw_cursor(void)
{
    // move cursor to x,y
    printf("\x1b[%d;%dH", editor.cursor_y + 1, editor.cursor_x + 1);
    editor.dirty_cursor = 0;
}

static void qe_draw_status(void)
{
    printf("\x1b[2;7m");  // invert color, dim

    int at_end = 0;
    for (int i = 0; i < terminal.width; ++i) {
        if (editor.status_buffer[i] == 0) {
            at_end = 1;
        }
        putchar(!at_end ? editor.status_buffer[i] : ' ');
    }

    printf("\x1b[0m");  // reset color
    editor.dirty_status = 0;
}

// Draw entire editor content to terminal. No delta is computed so a complete
// redraw is always performed. Only required when editor.dirty is true.
static void qe_draw(void)
{
    // hide cursor, clear screen, move cursor to 0,0
    printf("\x1b[?25l\x1b[2J\x1b[H");

    int y = editor.wrap ? qe_draw_wrap() : qe_draw_nowrap();

    // end of file markers
    for (; y < terminal.height - 1; ++y) {
        printf("~\x1b[E");
    }

    qe_draw_status();

    qe_draw_cursor();

    // show cursor
    printf("\x1b[?25h");

    qe_draw_cursor();

    fflush(stdout);

    editor.dirty = 0;
}

static void qe_terminal_init(void)
{
    struct termios raw_settings;

    if (!isatty(STDIN_FILENO)) {
        fatal("not a tty");
    }

    if (tcgetattr(STDIN_FILENO, &terminal.original_settings) < 0) {
        fatal("could not get terminal settings");
    }

    raw_settings = terminal.original_settings;
    raw_settings.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw_settings.c_oflag &= ~OPOST;
    raw_settings.c_cflag |= CS8;
    raw_settings.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw_settings.c_cc[VMIN] = 0;
    raw_settings.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_settings) < 0) {
        fatal("failed to set new terminal settings");
    }

    // save screen content (for restore)
    printf("\x1b[?47h");
    terminal.raw_mode = 1;
    atexit(qe_terminal_cleanup);
}

static void qe_winsize(void)
{
    struct winsize w;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1 || w.ws_col == 0) {
        fatal("failed to get terminal size");
    }

    terminal.width = w.ws_col;
    terminal.height = w.ws_row;
    terminal.resized = 0;
}

// Called if the window is resized.
//
// When the `was_resized` value is set the blocking read will return and give
// the program the chance to update the screen again.
//
// TODO: Does not catch the case of stacking <-> tiling in i3.
static void qe_winsize_sighandler(int signo)
{
    (void) signo;
    terminal.resized = 1;
    editor.dirty = 1;
}

static void qe_winsize_sighandler_init(void)
{
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = qe_winsize_sighandler;

    if (sigaction(SIGWINCH, &sa, NULL) == -1) {
        fatal("failed to setup sigwinch handler");
    }
}

static void qe_init(void)
{
    memset(&editor, 0, sizeof(editor));
    editor.filename = NULL;
    editor.dirty = 1;
    editor.fd = -1;
    editor.page = NULL;

    memset(&terminal, 0, sizeof(terminal));
}

static void qe_init_terminal()
{
    qe_terminal_init();
    qe_winsize();
    qe_winsize_sighandler_init();
}

// glibc defines this macro, but other libc's such as musl do not.
#undef CTRL
#define CTRL(k) ((k & 0x1f))

enum keycode {
    TAB = 9,
    ENTER = 13,
    ESC = 27,
    BACKSPACE = 127,

    HOME = 0x1ff,
    END,
    DEL,
    PGUP,
    PGDN,
    ARROW_UP,
    ARROW_DOWN,
    ARROW_RIGHT,
    ARROW_LEFT
};

static void qe_args(int argc, char **argv)
{
    const char *help =
        "usage: qe [options] filename\n"
        "\n"
        "   -ro   read-only\n"
        "   -s    no automatic save/sync (unimplemented)\n"
        "   -w    wrap\n"
        "   -h    print help"
        ;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (a[0] == '-') {
            if (!strcmp(a, "-ro")) {
                editor.read_only = 1;
            } else if (!strcmp(a, "-s")) {
                editor.batched_save = 1;
            } else if (!strcmp(a, "-w")) {
                editor.wrap = 1;
            } else if (!strcmp(a, "-h")) {
                fatal(help);
            } else {
                fatal("unknown argument");
            }
        } else {
            if (editor.filename != NULL) {
                fatal("only one filename is allowed");
            }

            editor.filename = a;
        }
    }

    if (editor.filename == NULL) {
        fatal(help);
    }
}

static void qe_open(void)
{
    int open_flags = editor.read_only ? O_RDONLY : O_RDWR;
    editor.fd = open(editor.filename, open_flags);
    if (editor.fd == -1) {
        fatal("failed to open file");
    }

    if (fstat(editor.fd, &editor.file) < 0) {
        fatal("failed to fstat file");
    }

    int mmap_flags = editor.read_only ? PROT_READ : PROT_WRITE | PROT_READ;
    editor.page = mmap(NULL, editor.file.st_size, mmap_flags, MAP_SHARED, editor.fd, 0);
    if (editor.page == MAP_FAILED) {
        fatal("failed to mmap file");
    }
}

// Update the status buffer with the current file status.
//
// This keeps track of the filename and current file position. It must be called
// if page_offset is modified.
//
// TODO: Handle page_offset_x movement.
static void qe_update_status_buffer(void)
{
    int64_t through = 0;
    if (editor.page_offset != 0) {
        through = 100ll * editor.page_offset / editor.file.st_size;
    }

    snprintf(editor.status_buffer, sizeof(editor.status_buffer),
             "%s: %3"PRId64"%% - %.32s (+%"PRId64") (%"PRId64"/%"PRId64")",
             edit_mode_string[editor.mode],
             through, editor.filename, editor.page_offset_x,
             editor.page_offset + editor.page_offset_x, editor.file.st_size);
}

// Scan from the current page offset past `n` newlines and set the new page
// offset. A negative value indicates reverse traversal.
//
// Stops if the edge of a file is reached.
static void qe_move_window_y(int32_t n)
{
    const int step = n > 0 ? 1 : -1;
    const int32_t an = n > 0 ? n : -n;

    int32_t i = 0;
    while (1) {
        if (editor.page_offset < 0) {
            editor.page_offset = 0;
            qe_update_status_buffer();
            return;
        }
        if (editor.page_offset >= editor.file.st_size) {
            editor.page_offset = editor.file.st_size - 1;
            qe_update_status_buffer();
            return;
        }

        if (editor.page[editor.page_offset] == '\n') {
            if (++i > an) {
                editor.page_offset += step;

                // if moving backwards, we are at the end of the line, move
                // to the start (or start of file).
                if (step < 0) {
                    while (editor.page_offset != 0 && editor.page[editor.page_offset] != '\n') {
                        editor.page_offset -= 1;
                    }

                    if (editor.page_offset != 0) {
                        editor.page_offset += 1;
                    }
                }

                break;
            }
        }

        editor.page_offset += step;
    }

    qe_update_status_buffer();
    editor.dirty = 1;
}

// Shift the buffer view left or right.
//
// TODO: Stop shifting if we have buffer is empty.
static void qe_move_window_x(int32_t n)
{
    editor.page_offset_x += n;

    if (editor.page_offset_x < 0) {
        editor.page_offset_x = 0;
        qe_update_status_buffer();
        return;
    }

    qe_update_status_buffer();
    editor.dirty = 1;
}

// Return the current byte position offset of the cursor.
//
// TODO: This needs to be fast as we use it on every cursor movement to check
// if are at the end of a line. Cache new-lines to make this faster.
//
// TODO: Incorrect on non-first-line.
static int64_t qe_get_cursor_byte_position(void)
{
    // scan forward past n new lines
    int64_t offset = editor.page_offset;
    for (int i = 0; i < editor.cursor_y; ++i) {
        uint8_t *s = memchr(editor.page + offset, '\n', editor.file.st_size - offset);
        if (!s) {
            // eof, return last byte
            return editor.file.st_size - 1;
        }

        offset = s - &editor.page[offset];
        offset += 1;
    }

    offset += editor.page_offset_x + editor.cursor_x;

    // could be passed the end of the file, cap
    if (offset >= editor.file.st_size) {
        offset = editor.file.st_size - 1;
    }

    return offset;
}

// Search at byte offset for the entry in the search-buffer. Returns -1
// on no match (EOF) else returns the offset which the search term was found at.
static int64_t qe_search(int64_t offset)
{
    // TODO: GNU specific
    uint8_t *p = memmem(editor.page + offset, editor.file.st_size - offset,
                        editor.search_buf, editor.search_len);
    if (p == NULL) {
        return -1;
    }

    return p - editor.page;
}

// Move the cursor, possibly moving the viewport if we exceed screen space.
static void qe_move_cursor_x(int32_t x)
{
    assert(x != 0);

    // TODO: Cursor cannot move right any more if next character is a newline
    // get_byte_position and check next character.

    int32_t new_x = editor.page_offset_x + editor.cursor_x + x;
    // all the way to the left
    if (new_x < 0) {
        editor.page_offset_x = 0;
        editor.cursor_x = editor.cursor_x == 0 ? 0 : terminal.width - 1;

        qe_update_status_buffer();
        editor.dirty = 1;
    }
    // off the right of the current view
    else if (new_x >= terminal.width) {
        // move the page offset in chunks (could adjust)
        editor.page_offset_x = new_x - (new_x % terminal.width);
        editor.cursor_x = new_x % terminal.width;

        qe_update_status_buffer();
        editor.dirty = 1;
    }
    // still in the current view
    else {
        if (x > 0) {
            uint64_t off = qe_get_cursor_byte_position();
            if (editor.page[off + 1] == '\n') {
                // TODO: Allow y movement here (separate into different functions)
                return;
            }
        }

        editor.cursor_x += x;
    }

    editor.dirty_cursor = 1;
}

static void qe_move_cursor_y(int32_t y)
{
    assert(y != 0);

    // TODO: on y cursor movement, truncate x cursor to max newline if needed
    // but retain the current column as well on the next line. If an x movement
    // occurs however then the virtual cursor is released.

    int32_t new_y = editor.cursor_y + y;
    // off the top
    if (new_y < 0) {
        // cannot scan cursor off top of first page
        if (editor.page_offset == 0) {
            return;
        }

        // scan back a page
        qe_move_window_y(-terminal.height / 2);
        editor.cursor_y = terminal.height - 2;
        editor.dirty = 1;
    }
    // off the bottom
    else if (new_y >= terminal.height - 1) {
        qe_move_window_y(terminal.height / 2);
        editor.cursor_y = 0;
        editor.dirty = 1;
    }
    // still in current view
    else {
        editor.cursor_y += y;
    }

    editor.dirty_cursor = 1;
}

// Read a single input key.
//
// This blocks until user input is received OR a signal occurs.
static int qe_readkey(void)
{
    ssize_t n;
    char c;

    errno = 0;
    while ((n = read(STDIN_FILENO, &c, 1)) == 0) {
        sched_yield();
    }

    if (n == -1) {
        // If a signal occurs during the above read call then it will fail
        // with the error code EINTR since we did not use SA_RESTART.
        //
        // In this case, we want to stop reading immediately to give the
        // program the chance to immediately update the buffer content.
        if (errno == EINTR) {
            return 0;
        }

        fatal("failed to read input key");
    }

    if (c == '\x1b') {
        char c1, c2, c3;

        if (read(STDIN_FILENO, &c1, 1) != 1 || read(STDIN_FILENO, &c2, 1) != 1) {
            return '\x1b';
        }

        if (c1 == '[') {
            if (c2 >= '0' && c2 <= '9') {
                if (read(STDIN_FILENO, &c3, 1) != 1) {
                    return '\x1b';
                }

                if (c3 == '~') {
                    switch (c2) {
                        case '1':
                        case '7':
                            return HOME;
                        case '4':
                        case '8':
                            return END;
                        case '3':
                            return DEL;
                        case '5':
                            return PGUP;
                        case '6':
                            return PGDN;
                        default:
                            break;
                    }
                }
            } else {
                switch (c2) {
                    case 'A':
                        return ARROW_UP;
                    case 'B':
                        return ARROW_DOWN;
                    case 'C':
                        return ARROW_RIGHT;
                    case 'D':
                        return ARROW_LEFT;
                    case 'H':
                        return HOME;
                    case 'F':
                        return END;
                    default:
                        break;
                }
            }
        } else if (c1 == 'O') {
            switch (c2) {
                case 'H':
                    return HOME;
                case 'F':
                    return END;
                default:
                    break;
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

static void qe_process_key(int c)
{
    switch (editor.mode) {
        case MODE_NORMAL:
        {
            // normal mode
            switch (c) {
                // TODO: Are you sure on quit.
                case 'q':
                case CTRL('c'):
                    exit(0);

                case 'i':
                    if (!editor.read_only) {
                        editor.mode = MODE_INSERT;
                    }
                    break;

                case '/':
                    editor.mode = MODE_SEARCH;

                    editor.status_buffer[0] = '/';
                    editor.status_buffer[1] = 0;

                    editor.search_buf[0] = 0;
                    editor.search_len = 0;

                    editor.dirty = 1;
                    break;

                case 'r':
                    // Force a refresh, useful if multiple editors at once on
                    // the same file.
                    editor.dirty = 1;
                    break;

                // TODO: Wrapping is currently very WIP so don't expect much.
                case 'w':
                    editor.wrap = !editor.wrap;
                    editor.dirty = 1;
                    break;

                case PGDN:
                case CTRL('d'):
                    qe_move_window_y(terminal.height - 1);
                    break;

                case PGUP:
                case CTRL('u'):
                    qe_move_window_y(-(terminal.height - 1));
                    break;

                case ARROW_DOWN:
                case 'j':
                    qe_move_cursor_y(1);
                    break;

                case ARROW_UP:
                case 'k':
                    qe_move_cursor_y(-1);
                    break;

                case ARROW_LEFT:
                case 'h':
                    qe_move_cursor_x(-1);
                    break;

                case ARROW_RIGHT:
                case 'l':
                    qe_move_cursor_x(1);
                    break;

                case CTRL('h'):
                    qe_move_window_x(-terminal.width / 2);
                    break;

                case CTRL('l'):
                    qe_move_window_x(terminal.width / 2);
                    break;

                default:
                    break;
            }
        }
        break;

        case MODE_INSERT:
        {
            switch (c) {
                case CTRL('c'):
                    exit(0);

                case ESC:
                    editor.mode = MODE_NORMAL;
                    break;

                case PGDN:
                case CTRL('d'):
                    qe_move_window_y(terminal.height - 1);
                    break;

                case PGUP:
                case CTRL('u'):
                    qe_move_window_y(-(terminal.height - 1));
                    break;

                case ARROW_DOWN:
                    qe_move_cursor_y(1);
                    break;

                case ARROW_UP:
                    qe_move_cursor_y(-1);
                    break;

                case ARROW_LEFT:
                    qe_move_cursor_x(-1);
                    break;

                case ARROW_RIGHT:
                    qe_move_cursor_x(1);
                    break;

                case CTRL('h'):
                    qe_move_window_x(-terminal.width / 2);
                    break;

                case CTRL('l'):
                    qe_move_window_x(terminal.width / 2);
                    break;

                default:
                {
                    static size_t page_size = 0;
                    if (page_size == 0) {
                        page_size = (size_t) sysconf(_SC_PAGESIZE);
                    }

                    // TODO: Ringbuffer storing a sequence of edits. Head is the
                    // first pending edit, tail is the latest edit to apply.
                    //
                    // Saving applies all edits to the file and keeps the edits
                    // in the buffer as a history set.
                    //
                    // Need a sentinel value to signal incomplete buffer.
                    //
                    // This should actually be an option to require hitting
                    // an explicit save. Still keep the undo buffer, though.
                    int64_t off = qe_get_cursor_byte_position();
                    editor.page[off] = c;

                    // align to page
                    uint8_t *page_addr = editor.page + off - (off % page_size);
                    int r = msync(page_addr, page_size, MS_SYNC | MS_INVALIDATE);
                    if (r == -1) {
                        fatal("failed to msync");
                    }

                    // advance the cursor, possibly moving to the next line
                    if (editor.page[off + 1] == '\n') {
                        editor.cursor_x = 0;
                        editor.cursor_y += 1;
                        editor.dirty_cursor = 1;
                    } else {
                        qe_move_cursor_x(1);
                    }

                    // TODO: Partial dirty state would be good
                    editor.dirty = 1;
                }
                break;
            }
        }
        break;

        case MODE_SEARCH:
        {
            switch (c) {
                case CTRL('c'):
                    exit(0);

                case ESC:
                    editor.mode = MODE_NORMAL;
                    break;

                case ENTER:
                {
                    // Search always switches mode
                    editor.mode = MODE_NORMAL;

                    // TODO: Add a flag to highlight the last match in the
                    // buffer. Empty search highlighting?

                    // Search from the current location forward.

                    int64_t off = qe_get_cursor_byte_position();
                    int64_t actual_addr = qe_search(off + 1);
                    if (actual_addr == -1) {
                        break;
                    }

                    int64_t addr = actual_addr;

                    // Go to the address, then, go to the start of the line
                    // where the entry occurred.
                    while (addr > 0 && editor.page[addr] != '\n') {
                        addr -= 1;
                    }
                    if (addr != 0) {
                        addr += 1;
                    }

                    // TODO: Shift the page_offset_x in one computation directly.
                    // based on terminal width.
                    editor.page_offset = addr;

                    // TODO: Round page_offset_x to editor terminal size and
                    // set cursor based on this.
                    editor.page_offset_x = actual_addr - addr;

                    editor.dirty = 1;
                }
                break;

                default:
                    // cap search term to 64 characters
                    if (editor.search_len >= sizeof(editor.search_buf) - 1) {
                        break;
                    }

                    if (c != BACKSPACE) {
                        editor.search_buf[editor.search_len++] = c;
                        editor.search_buf[editor.search_len] = 0;
                    } else {
                        editor.search_len -= 1;
                        if (editor.search_len == 0) {
                            editor.search_len = 0;
                        }

                        editor.search_buf[editor.search_len] = 0;
                    }

                    // fill the status buffer so we can see what is being
                    editor.status_buffer[0] = '/';
                    editor.status_buffer[1] = 0;
                    strncpy(editor.status_buffer + 1, editor.search_buf, sizeof(editor.status_buffer) - 1);
                    editor.dirty = 1;

                    break;
            }
        }
        break;
    }
}

int main(int argc, char **argv)
{
    qe_init();
    qe_args(argc, argv);
    qe_open();
    qe_init_terminal();
    qe_update_status_buffer();

    while (1) {
        if (terminal.resized) {
            qe_winsize();
        }

        // TODO: Move to bottom of screen and perform.
        // if (editor.dirty_status) {
        //     qe_draw_status();
        // }

        if (editor.dirty_cursor) {
            qe_draw_cursor();
            fflush(stdout);
        }

        if (editor.dirty) {
            qe_draw();
        }

        int c = qe_readkey();

        // if the mode changes, update the buffer
        enum edit_mode mode = editor.mode;
        qe_process_key(c);
        // TODO: Change how we update the buffer
        if (mode != editor.mode && editor.mode != MODE_SEARCH) {
            qe_update_status_buffer();
            editor.dirty = 1;
        }
    }
}
