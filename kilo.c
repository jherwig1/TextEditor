/*** includes ***/
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

/*** defines ***/
#define CTRL_KEY(k) ((k) & 0x1f)

struct abuf {
	char *b;
	int len;
};

enum editorKey {
	ARROW_UP = 1000,
	ARROW_DOWN,
	ARROW_LEFT,
	ARROW_RIGHT,
	PAGE_UP,
	PAGE_DOWN
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
	if (ab == NULL) {
		return;
	}

	char *new = realloc(ab->b, ab->len + len);
	memcpy(&new[ab->len], s, len);

	ab->b = new;
	ab->len = ab->len + len;
}

void abFree(struct abuf *ab) {
	free (ab->b);
}


/*** data ***/
struct editorConfigs {
	struct termios orig_termios;
	int rows;
	int cols;
	int cx;
	int cy;
	void (* editorProcessKeys)();
};

struct editorConfigs E;

/*** terminal ***/
void die(const char *s) {
	write(STDOUT_FILENO, "\x1b[2J", 4); //clear screen
	write(STDOUT_FILENO, "\x1b[H", 3); //reposition cursor

	perror(s);
	exit(1);
}

void disableRawMode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
		die("tcsetattr");
	}
}

void enableRawMode() {
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
		die("tcgetattr");
	}

	atexit(disableRawMode);
	struct termios raw = E.orig_termios;
	raw.c_iflag &= ~(ICRNL | IXON | BRKINT | INPCK | ISTRIP);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);

	raw.c_cc[VMIN] = 0; // min chars for read
	raw.c_cc[VTIME] = 1; // timeout, tenths of second


	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
		die("tcsetattr");
	}
}

int editorReadKey() {
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) {
			die("read");
		}
	}

	if (c == '\x1b') {
		char seq[3];
		if (read(STDIN_FILENO, &seq[0], 1) != 1) {
			return '\x1b';
		}
		if (read(STDIN_FILENO, &seq[1], 1) != 1) {
			return '\x1b';
		}

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) {
					return '\x1b';
				}

				/* Check page up/down */
				if (seq[2] == '~') {
					switch(seq[1]) {
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
					}
				}

			} else {
				switch(seq[1]) {
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
				}
			}
		}
	}

	return c;
}

int getCursorPosition(int *rows, int *cols) {
	char buf[32];
	unsigned int i = 0;
	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) { 
		return -1;
	}

	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if (buf[i] == 'R') break;
		i++;
	}

	buf[i] = '\0';
	if (buf[0] != '\x1b' || buf[1] != '[') return -1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
	return 0;
}

int getWindowSize(int *rows, int *cols) {
	struct winsize ws;

	if (1 || ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
			return -1;
		}
		return getCursorPosition(rows, cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}


/*** output ***/
void editorDrawRows(struct abuf *ab) {
	/* Draw tildes */
	int y;
	for (y = 0; y < E.rows; y++) {
		if (y == E.rows / 3) {
			char welcome[80];
			int welcomlen = snprintf(welcome, sizeof(welcome), "Welcome to my editor!");
			if (welcomlen > E.cols) {
				welcomlen = E.cols;
			}

			int padding = (E.cols - welcomlen) / 2;
			if (padding) {
				abAppend(ab, "~", 1);
				padding--;
			}

			while (padding--) {
				abAppend(ab, " ", 1);
			}
			abAppend(ab, welcome, welcomlen);
		} else {
			abAppend(ab, "~", 1);
		}
		abAppend(ab, "\x1b[K", 3); //clear screen to right of cursor


		if (y < E.rows - 1) {
			abAppend(ab, "\r\n", 2);
		}
	}
}

void editorRefreshScreen() {
	struct abuf ab = ABUF_INIT;
	abAppend(&ab, "\x1b[?25l", 6); //hide cursor
	//abAppend(&ab, "\x1b[2J", 4); //clear screen
	abAppend(&ab, "\x1b[H", 3); //reposition cursor
	editorDrawRows(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
	abAppend(&ab, buf, strlen(buf)); //reposition cursor
	abAppend(&ab, "\x1b[?25h", 6); //show cursor
	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}


void editorMoveCursor(int c) {
	switch(c) {
		case ARROW_LEFT:
			if (E.cx != 0)
				E.cx--;
			break;
		case ARROW_DOWN:
			if (E.cy != (E.rows - 1))
				E.cy++;
			break;
		case ARROW_UP:
			if (E.cy != 0)
				E.cy--;
			break;
		case ARROW_RIGHT:
			if (E.cx != (E.cols -1))
				E.cx++;
			break;
	}
}

/*** input ***/
void editorProcessKeypress() { 
	int c = editorReadKey();
	switch (c) {
		case CTRL_KEY('q'): 
			write(STDOUT_FILENO, "\x1b[2J", 4); //clear screen
			write(STDOUT_FILENO, "\x1b[H", 3); //reposition cursor
			exit(0);
			break;
		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;
		case PAGE_UP:
		case PAGE_DOWN:
		{
			int times = E.rows;
			while (times--) {
				editorMoveCursor(c == PAGE_UP ? ARROW_UP: ARROW_DOWN);
			}
		}
	}
}

/*** input ***/
void editorProcessVimKeypress() { 
	int c = editorReadKey();
	switch (c) {
		case CTRL_KEY('q'): 
			write(STDOUT_FILENO, "\x1b[2J", 4); //clear screen
			write(STDOUT_FILENO, "\x1b[H", 3); //reposition cursor
			exit(0);
			break;
		case 'j':
			editorMoveCursor(ARROW_DOWN);
			break;
		case 'k':
			editorMoveCursor(ARROW_UP);
			break;
		case 'h':
			editorMoveCursor(ARROW_LEFT);
			break;
		case 'l':
			editorMoveCursor(ARROW_RIGHT);
			break;
		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;
		case PAGE_UP:
		case PAGE_DOWN:
		{
			int times = E.rows;
			while (times--) {
				editorMoveCursor(c == PAGE_UP ? ARROW_UP: ARROW_DOWN);
			}
		}
	}
}

void initEditor() {
	E.cx = 0;
	E.cy = 0;
	E.editorProcessKeys = editorProcessVimKeypress;
	if (getWindowSize(&E.rows, &E.cols) == -1) {
		die("getWindowSize");
	}
}

/*** init ***/
int main() {
	enableRawMode();
	initEditor();
	char c;

	while (1) {
		editorRefreshScreen();
		E.editorProcessKeys();
		//editorProcessKeypress();
	}
	return 0;
			E.cx = 0;
			E.cx = 0;
			E.cx = 0;
			E.cx = 0;
			E.cx = 0;
}
