/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"

#define SIZEOFSEQ(x) (sizeof(x) - 1)
#define SEQ_ESCAPE "\x1b"
#define SEQ_HIDECURSOR "\x1b[?25l"
#define SEQ_SHOWCURSOR "\x1b[?25h"
#define SEQ_CLEARSCREEN "\x1b[2J"
/* erases the part of the line to the right of the cursor */
#define SEQ_ERASEINLINE0 "\x1b[K"
#define SEQ_ERASEINLINE SEQ_ERASEINLINE0
#define SEQ_MOVECURSORTOPLEFT "\x1b[H"
#define SEQ_MOVECURSORBTMRIGHT "\x1b[999C\x1b[999B"
#define SEQ_QUERY_CURSORLOC "\x1b[6n"

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey
{
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    PAGE_UP,
    PAGE_DOWN,
};

/*** data ***/

typedef struct editorConfig
{
    int cx, cy;
    int screenrows;
    int screencols;
    struct termios orig_termios;
} editorConfig;

editorConfig E;

/*** terminal ***/

void die(const char *reason)
{
    write(STDOUT_FILENO, SEQ_CLEARSCREEN, SIZEOFSEQ(SEQ_CLEARSCREEN));
    write(STDOUT_FILENO, SEQ_MOVECURSORTOPLEFT, SIZEOFSEQ(SEQ_MOVECURSORTOPLEFT));

    perror(reason);
    exit(1);
}

void disableRawMode(void)
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    {
        die("tcsetattr");
    }
}

void enableRawMode(void)
{
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    {
        die("tcgetattr");
    }
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;  // make read() instantly returns
    raw.c_cc[VTIME] = 1; // 100 ms

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    {
        die("tcsetattr");
    }
}

int editorReadKey(void)
{
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
    {
        if (nread == -1 && errno != EAGAIN)
        {
            die("read");
        }
    }

    if (c == '\x1b')
    {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            goto ERROR_RETURN;
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            goto ERROR_RETURN;

        if (seq[0] == '[')
        {
            if (seq[1] >= '0' && seq[1] <= '9')
            {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    goto ERROR_RETURN;
                if (seq[2] == '~')
                {
                    switch (seq[1])
                    {
                    case '5':
                        return PAGE_UP;
                    case '6':
                        return PAGE_DOWN;
                    }
                }
            }
            else
            {
                switch (seq[1])
                {
                case 'A':
                    return ARROW_UP;
                case 'B':
                    return ARROW_DOWN;
                case 'C':
                    return ARROW_RIGHT;
                case 'D':
                    return ARROW_LEFT;
                }
            }
        }

    ERROR_RETURN:
        return '\x1b';
    }

    return c;
}

int getCursorPosition(int *rows, int *cols)
{
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, SEQ_QUERY_CURSORLOC, SIZEOFSEQ(SEQ_QUERY_CURSORLOC)) != SIZEOFSEQ(SEQ_QUERY_CURSORLOC))
        return -1;

    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
        {
            break;
        }
        if (buf[i] == 'R')
        {
            break;
        }
        ++i;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        // move the cursor right(C) 999 and down(B) 999
        if (write(STDOUT_FILENO, SEQ_MOVECURSORBTMRIGHT, SIZEOFSEQ(SEQ_MOVECURSORBTMRIGHT)) !=
            SIZEOFSEQ(SEQ_MOVECURSORBTMRIGHT))
            return -1;
        return getCursorPosition(rows, cols);
    }
    else
    {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** append buffer ***/

typedef struct abuf
{
    char *b;
    int len;
} abuf;

// clang-format off
#define ABUF_INIT {NULL, 0}
// clang-format on

void abAppend(abuf *ab, const char *s, int len)
{
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL)
        return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(abuf *ab)
{
    free(ab->b);
}

/*** output ***/

void editorDrawRows(abuf *ab)
{
    int y;
    for (y = 0; y < E.screenrows; y++)
    {
        if (y == E.screenrows / 3)
        {
            char welcome[80];
            int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);
            if (welcomelen > E.screencols)
                welcomelen = E.screencols;
            int padding = (E.screencols - welcomelen) / 2;
            if (padding)
            {
                abAppend(ab, "~", 1);
                --padding;
            }
            while (padding--)
                abAppend(ab, " ", 1);
            abAppend(ab, welcome, welcomelen);
        }
        else
        {
            abAppend(ab, "~", 1);
        }

        abAppend(ab, SEQ_ERASEINLINE, SIZEOFSEQ(SEQ_ERASEINLINE));
        if (y < E.screenrows - 1)
        {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen(void)
{
    abuf ab = ABUF_INIT;

    abAppend(&ab, SEQ_HIDECURSOR, SIZEOFSEQ(SEQ_HIDECURSOR));
    abAppend(&ab, SEQ_MOVECURSORTOPLEFT, SIZEOFSEQ(SEQ_MOVECURSORTOPLEFT));

    editorDrawRows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, SEQ_SHOWCURSOR, SIZEOFSEQ(SEQ_SHOWCURSOR));

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/

void editorActionMoveCursor(int key)
{
    switch (key)
    {
    case ARROW_LEFT:
        if (E.cx != 0)
        {
            --E.cx;
        }
        break;
    case ARROW_RIGHT:
        if (E.cx != E.screencols - 1)
        {
            ++E.cx;
        }
        break;
    case ARROW_UP:
        if (E.cy != 0)
        {
            --E.cy;
        }
        break;
    case ARROW_DOWN:
        if (E.cy != E.screenrows - 1)
        {
            ++E.cy;
        }
        break;
    }
}

void editorProcessKeypress(void)
{
    int c = editorReadKey();

    switch (c)
    {
    case CTRL_KEY('q'):
        write(STDOUT_FILENO, SEQ_CLEARSCREEN, SIZEOFSEQ(SEQ_CLEARSCREEN));
        write(STDOUT_FILENO, SEQ_MOVECURSORTOPLEFT, SIZEOFSEQ(SEQ_MOVECURSORTOPLEFT));
        exit(0);
        break;

    case PAGE_UP:
    case PAGE_DOWN: {
        int times = E.screenrows;
        while (times--)
        {
            editorActionMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
    }
    break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorActionMoveCursor(c);
        break;
    default:
        break;
    }
}

/*** init ***/

void initEditor(void)
{
    E.cx = 0;
    E.cy = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    {
        die("getWindowSize");
    }
}

int main(void)
{
    enableRawMode();
    initEditor();

    while (1)
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
