/*
 * QuickEdit (qe) - v0.1.0
 * =======================
 *
 * A small editor intended for quick edits.
 *
 *  - Handles arbitrarily long lines
 *  - Allows partial file edits on huge files (?)
 *  - Simple viewer alternative to less
 *
 * How is it fast?
 * ===============
 *
 *  - Does not need to load complete files.
 *  - Minimises dealing/thinking with lines.
 *  - Does not use fancy editing structures such as ropes/skip-lists.
 *  - Doesn't do that much.
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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include <termios.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

struct {
    // Name of the file being edited. Points to argv.
    char *filename;

    // File descriptor of file being edited.
    int fd;

    // Information of the file being edited.
    struct stat file;

    // Whether the onscreen content matches the underlying editor context.
    volatile sig_atomic_t dirty;

    // Stores messages to be displayed at the bottom of the screen.
    char status_buffer[40];

    // Current page mapped into memory.
    //
    // TODO: This maps the whole file into memory. Is this the best approach?
    // Is extra manual management required for efficiency?
    uint8_t *page;

    // Byte offset into the current page. Should always be on new line boundary.
    int64_t page_offset;

    // Offset from start of line shifting the content left-right.
    int64_t page_offset_lr;
} editor;

struct {
    // Width of the terminal.
    int32_t cols;

    // Height of the terminal.
    int32_t rows;

    // Whether the terminal has been resized and requires synchronization.
    volatile sig_atomic_t was_resized;

    // Original settings of the terminal before the program was started.
    struct termios original_settings;

    // Whether we have entered raw mode in the terminal yet.
    //
    // This is required to perform a clean exit in case we fail before the
    // terminal has been correctly set up.
    int raw_mode;
} terminal;

void qe_terminal_cleanup(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminal.original_settings);
    printf("\x1b[?47l");
}

void fatal(const char *msg)
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

// Perform a complete redraw of the editor content.
//
// This performs no buffering and will always result in a complete screen
// refresh followed by printing of the current content.
//
// TODO: Double-buffer existing content where we can. Prefer pre-building the
// content to print and perform a single write call. The internal printf
// buffer may not be sufficient.
void qe_draw(void)
{
    int64_t offset = editor.page_offset;

    printf("\x1b[?25l\x1b[2J\x1b[H");
    int y;
    for (y = 0; y < terminal.rows - 1; ++y) {
        // Clip pre-buffer line content.
        for (int i = 0; i < editor.page_offset_lr; ++i) {
            const char ch = editor.page[offset];
            offset += 1;

            if (offset >= editor.file.st_size) {
                printf("\x1b[E");
                goto finish_draw;
            }

            if (ch == '\n') {
                goto next_row;
            }
        }

        int x;
        for (x = 0; x < terminal.cols; ++x) {
            const char ch = editor.page[offset];
            offset += 1;

            if (offset >= editor.file.st_size) {
                printf("\x1b[E");
                goto finish_draw;
            }

            if (ch == '\n') {
                goto next_row;
            } else {
                printf("%c", ch);
            }
        }

        // Clip post-buffer line content.
        if (x == terminal.cols) {
            while (editor.page[offset++] != '\n') {
                if (offset >= editor.file.st_size) {
                    printf("\x1b[E");
                    goto finish_draw;
                }
            }
        }

    next_row:
        printf("\x1b[E");
    }

finish_draw:
    // Show marker rows at end of file.
    for (; y < terminal.rows - 1; ++y) {
        printf("~\x1b[E");
    }

    printf("\x1b[7m");
    printf("%s", editor.status_buffer);
    printf("\x1b[0m");
    printf("\x1b[?25h");

    fflush(stdout);
    editor.dirty = 0;
}

void qe_terminal_init(void)
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

    printf("\x1b[?47h");
    terminal.raw_mode = 1;
    atexit(qe_terminal_cleanup);
}

void qe_winsize(void)
{
    struct winsize w;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1 || w.ws_col == 0) {
        fatal("failed to get terminal size");
    }

    terminal.cols = w.ws_col;
    terminal.rows = w.ws_row;
    terminal.was_resized = 0;
}

// Called if the window is resized.
//
// When the `was_resized` value is set the blocking read will return and give
// the program the chance to update the screen again.
//
// TODO: Does not catch the case of stacking <-> tiling in i3.
void qe_winsize_sighandler(int signo)
{
    (void) signo;
    terminal.was_resized = 1;
    editor.dirty = 1;
}

void qe_winsize_sighandler_init(void)
{
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = qe_winsize_sighandler;

    if (sigaction(SIGWINCH, &sa, NULL) == -1) {
        fatal("failed to setup sigwinch handler");
    }
}

void qe_init(void)
{
    memset(&editor, 0, sizeof(editor));
    editor.filename = NULL;
    editor.dirty = 1;
    editor.fd = -1;
    editor.page = NULL;

    memset(&terminal, 0, sizeof(terminal));

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

void qe_args(int argc, char **argv)
{
    if (argc != 2) {
        fatal("usage: qe <filename>");
    }

    editor.filename = argv[1];
}

void qe_open(void)
{
    // TODO: Check if we can optimize flags to open for mmap usage.
    editor.fd = open(editor.filename, O_RDONLY);
    if (editor.fd == -1) {
        fatal("failed to open file");
    }

    if (fstat(editor.fd, &editor.file) < 0) {
        fatal("failed to fstat file");
    }

    editor.page = mmap(NULL, editor.file.st_size, PROT_READ, MAP_SHARED,
                       editor.fd, 0);
    if (editor.page == MAP_FAILED) {
        fatal("failed to mmap file");
    }
}

// Update the status buffer with the current file status.
//
// This keeps track of the filename and current file position. It must be called
// if page_offset is modified.
//
// TODO: Handle page_offset_lr movement.
void qe_update_status_buffer(void)
{
    int through = 0;
    if (editor.page_offset != 0) {
        through = 100ll * editor.page_offset / editor.file.st_size;
    }

    snprintf(editor.status_buffer, sizeof(editor.status_buffer),
             "%3d%% - %.32s", through, editor.filename);
}

// Scan from the current page offset past `n` newlines and set the new page
// offset. A negative value indicates reverse traversal.
//
// Stops if the edge of a file is reached.
void qe_scan_past_newlines(int32_t n)
{
    const int step = n > 0 ? 1 : -1;
    const int32_t an = n > 0 ? n : -n;

    int32_t i = 0;
    while (1) {
        if (editor.page_offset < 0) {
            editor.page_offset = 0;
            break;
        }
        if (editor.page_offset >= editor.file.st_size) {
            editor.page_offset = editor.file.st_size - 1;
            break;
        }

        if (editor.page[editor.page_offset] == '\n') {
            if (++i > an) {
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
void qe_scan_left_right(int32_t n)
{
    editor.page_offset_lr += n;

    if (editor.page_offset_lr < 0) {
        editor.page_offset_lr = 0;
    }

    qe_update_status_buffer();
    editor.dirty = 1;
}

// Read a single input key.
//
// This blocks until user input is received OR a signal occurs.
int qe_readkey(void)
{
    int n;
    char c;

    errno = 0;
    while ((n = read(STDIN_FILENO, &c, 1)) == 0) {}
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
                        return ARROW_LEFT;
                    case 'D':
                        return ARROW_RIGHT;
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

void qe_process_key(int c)
{
    switch (c) {
        case 'q':
        case CTRL('c'):
            exit(0);
            break;

        case ARROW_DOWN:
        case 'j':
            qe_scan_past_newlines(1);
            break;

        case ARROW_UP:
        case 'k':
            qe_scan_past_newlines(-1);
            break;

        case PGDN:
        case CTRL('d'):
            qe_scan_past_newlines(terminal.rows - 1);
            break;

        case PGUP:
        case CTRL('u'):
            qe_scan_past_newlines(-(terminal.rows - 1));
            break;

        case ARROW_LEFT:
        case 'l':
            qe_scan_left_right(1);
            break;

        case ARROW_RIGHT:
        case 'h':
            qe_scan_left_right(-1);
            break;

        case CTRL('l'):
            qe_scan_left_right(terminal.cols / 2);
            break;

        case CTRL('h'):
            qe_scan_left_right(-terminal.cols / 2);
            break;

        default:
            break;
    }
}

int main(int argc, char **argv)
{
    qe_init();
    qe_args(argc, argv);
    qe_open();
    qe_update_status_buffer();

    while (1) {
        if (terminal.was_resized) {
            qe_winsize();
        }

        if (editor.dirty) {
            qe_draw();
        }

        int c = qe_readkey();
        qe_process_key(c);
    }
}
