/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_QUIT_TIMES 1

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

/*** data ***/

// apparently some c experts say you shouldn't typedef structs
// The use of typedef for erow means you don't have to use struct again
// This typedef erow is crucial
typedef struct erow {
  int size; //the number of characters in the line
  char *chars; //points at the character array of a row - mem assigned by malloc
} erow;

struct editorConfig {
  int cx, cy; //cursor x and y position
  int rx; //index into the render field - only nec b/o tabs
  int rowoff; //row the user is currently scrolled to
  int coloff; //column user is currently scrolled to
  int screenrows; //number of rows and columns in the display
  int screencols;  //number of rows and columns in the display
  int numrows; // the number of rows of text so last text row is always row numrows
  erow *row; //(e)ditorrow stores a pointer to an erow structure 
  int dirty;
  char *filename;
  char statusmsg[80]; //status msg is a character array max 80 char
  time_t statusmsg_time;
  struct termios orig_termios;
};

struct editorConfig E;

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt);
void  getwordundercursor();

/*** terminal ***/

void die(const char *s) {
  // write is from <unistd.h> 
  //ssize_t write(int fildes, const void *buf, size_t nbytes);
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0; // minimum data to receive?
  raw.c_cc[VTIME] = 1; // timeout for read will return 0 if no bytes read

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
  int nread;
  char c;

  /* read is from <unistd.h> - not sure why read is used and not getchar <stdio.h>
   prototype is: ssize_t read(int fd, void *buf, size_t count); 
   On success, the number of bytes read is returned (zero indicates end of file)
   So the while loop below just keeps cycling until a byte is read
   it does check to see if there was an error (nread == -1)*/

   /*Note that ctrl-key maps to ctrl-A=1, ctrl-b=2 etc.*/

  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }

  // if the character read was an escape, need to figure out if it was an escape sequence
  if (c == '\x1b') {
    char seq[3];
    //editorSetStatusMessage("You pressed %d", c); //slz
    // the reads time out after 0.1 seconds
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b'; //need 4 bytes
        if (seq[2] == '~') {
          //editorSetStatusMessage("You pressed %c%c%c", seq[0], seq[1], seq[2]); //slz
          switch (seq[1]) {
            case '1': return HOME_KEY; //not being issued
            case '3': return DEL_KEY; //<esc>[3~
            case '4': return END_KEY;  //not being issued
            case '5': return PAGE_UP; //<esc>[5~
            case '6': return PAGE_DOWN;  //<esc>[6~
            case '7': return HOME_KEY; //not being issued
            case '8': return END_KEY;  //not being issued
          }
        }
      } else {
          //editorSetStatusMessage("You pressed %c%c", seq[0], seq[1]); //slz
          switch (seq[1]) {
            case 'A': return ARROW_UP; //<esc>[A
            case 'B': return ARROW_DOWN; //<esc>[B
            case 'C': return ARROW_RIGHT; //<esc>[C
            case 'D': return ARROW_LEFT; //<esc>[D
            case 'H': return HOME_KEY; // <esc>[H - this one is being issued
            case 'F': return END_KEY;  // <esc>[F - this one is being issued
        }
      }
    } else if (seq[0] == 'O') { 
      switch (seq[1]) {
        case 'H': return HOME_KEY; //<esc>OH - not being used
        case 'F': return END_KEY; //<esc>OF - not being used
      }
    }

    return '\x1b'; // if it doesn't match a known escape sequence like ] ... or O ... just return escape

  } else {
      //editorSetStatusMessage("You pressed %d", c); //slz
      return c;
  }
}

int getWindowSize(int *rows, int *cols) {

//TIOCGWINSZ = fill in the winsize structure
/*struct winsize
{
  unsigned short ws_row;	 rows, in characters 
  unsigned short ws_col;	 columns, in characters 
  unsigned short ws_xpixel;	 horizontal size, pixels 
  unsigned short ws_ypixel;	 vertical size, pixels 
};*/

// ioctl(), TIOCGWINXZ and struct windsize come from <sys/ioctl.h>
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    //if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1; \\not needed because ioctl works on arch
    //return getCursorPosition(rows, cols);
    return -1;
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;

    return 0;
  }
}

/*** row operations ***/

//at is the row number of the row to insert
void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows) return;

  //E.row is a pointer to an array of erow structures
  //The array of erows that E.row points to needs to have its memory enlarged when you add a row
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  // memmove(dest, source, number of bytes to move?)
  // moves the line at at to at+1 and all the other erow structs until the end
  // when you insert into the last row E.numrows==at then no memory is moved
  // apparently ok if there is no E.row[at+1] if number of bytes = 0
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  // section below nice - creates an erow struct for the new row
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.numrows++;
  E.dirty++;
}

void editorFreeRow(erow *row) {
  free(row->chars);
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows) return;
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
  E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  E.dirty++;
}

/*** editor operations ***/
void editorInsertChar(int c) {
  //E.cy is the cursor y position
  // inserts a row if at the end of the file
  if (E.cy == E.numrows) {
    editorInsertRow(E.numrows, "", 0);
  }

  erow *row = &E.row[E.cy];
  if (E.cx < 0 || E.cx > row->size) E.cx = row->size;
  // we have to insert a char so we need more memory
  // why 2 more bytes??
  // we add 2 because we also have to make room for the null byte??????
  row->chars = realloc(row->chars, row->size + 1); //******* 2
  memmove(&row->chars[E.cx + 1], &row->chars[E.cx], row->size - E.cx + 1);
  row->size++;
  row->chars[E.cx] = c;
  E.dirty++;
  E.cx++;
}

void editorInsertNewline() {
  if (E.cx == 0) {
    editorInsertRow(E.cy, "", 0);
  } else {
    erow *row = &E.row[E.cy];
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    row = &E.row[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0';
  }
  E.cy++;
  E.cx = 0;
}

void editorDelChar() {
  if (E.cy == E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;

  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    if (E.cx < 1 || E.cx >= 1+row->size) return;
    memmove(&row->chars[E.cx - 1], &row->chars[E.cx], row->size - E.cx + 1);
    row->size--;
    E.dirty++;
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

/*** file i/o ***/

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + 1;
  *buflen = totlen;

  char *buf = malloc(totlen);
  char *p = buf;
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }

  return buf;
}

void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' ||
                           line[linelen - 1] == '\r'))
      linelen--;
    editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

void editorSave() {
  if (E.filename == NULL) {
    E.filename = editorPrompt("Save as: %s (ESC to cancel)");
    if (E.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
  }

  int len;
  char *buf = editorRowsToString(&len);

  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        E.dirty = 0;
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }

  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** append buffer ***/

struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {

  /*
     initally abuf consists of *b = NULL pointer and int len = 0
     abuf.b holds the pointer to the memory that holds the string
     the below creates a new memory pointer that reallocates
     memory to something 
  */

  /*realloc's first argument is the current pointer to memory and the second argumment is the size_t needed*/
  char *new = realloc(ab->b, ab->len + len); 

  if (new == NULL) return;
  /*new is the new pointer to the string
    new[len] is a value and memcpy needs a pointer
    to the location in the string where s is being copied*/

  //copy s on to the end of whatever string was there
  memcpy(&new[ab->len], s, len); 

  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}

/*** output ***/

void editorScroll() {

  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  if (E.cx < E.coloff) {
    E.coloff = E.cx;
  }
  if (E.cx >= E.coloff + E.screencols) {
    E.coloff = E.cx - E.screencols + 1;
  }
}
// "drawing" rows really means updating the ab buffer
void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
          "Kilo editor -- version %s", KILO_VERSION);
        if (welcomelen > E.screencols) welcomelen = E.screencols;
        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--) abAppend(ab, " ", 1);
        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int len = E.row[filerow].size - E.coloff;
      if (len < 0) len = 0;
      if (len > E.screencols) len = E.screencols;
      abAppend(ab, &E.row[filerow].chars[E.coloff], len);
    }

    abAppend(ab, "\x1b[K", 3); //erases the part of the line to the right of the cursor in case the new line is shorter than the old
    abAppend(ab, "\r\n", 2);
  }
}
//status bar has inverted colors
void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4); //switches to inverted colors
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
    E.filename ? E.filename : "[No Name]", E.numrows,
    E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "Status bar %d/%d",
    E.cy + 1, E.numrows);
  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);
  
  /* pretty clever - add spaces until you just have enough room
     left to print the rstatus message  */

  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3); //switches back to normal formatting
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  /*void editorSetStatusMessage(const char *fmt, ...) is where the message is created/set*/
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

// this is continuously called by main
//int nn = 0;
void editorRefreshScreen() {
  editorScroll();

  /*  struct abuf {
      char *b;
      int len;
    };*/

  struct abuf ab = ABUF_INIT; //abuf *b = NULL and int len = 0
  //"\x1b[2J" clears the screen
  abAppend(&ab, "\x1b[?25l", 6); //b is a new pointer to "\x1b[?25l" - hides the cursor
  abAppend(&ab, "\x1b[H", 3);  //b is a new pointer to "\x1.. + "\x1b[H=sends the cursor home


  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  /*abAppend(&ab, " -> ", 4);
  char str[10];
  sprintf(str, "%d", nn); //str is a pointer to the char array
  abAppend(&ab, str, 3);*/

  // the lines below position the cursor where it should go
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
                                            (E.cx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6); //shows the cursor

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
 // nn++;
}
/*va_list, va_start(), and va_end() come from <stdarg.h> and vsnprintf() is
from <stdio.h> and time() is from <time.h>.  stdarg.h allows functions to accept a
variable number of arguments and are declared with an ellipsis in place of the last parameter.*/

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap; //type for iterating arguments
  va_start(ap, fmt); // start iterating arguments with a va_list


  // vsnprint from <stdio.h> writes to the character string str
  //vsnprint(char *str,size_t size, const char *format, va_list ap)
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap); //free a va_list
  E.statusmsg_time = time(NULL);
}

/*** input ***/

char *editorPrompt(char *prompt) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);

  size_t buflen = 0;
  buf[0] = '\0';

  while (1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();

    int c = editorReadKey();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) buf[--buflen] = '\0';
    } else if (c == '\x1b') {
      //editorSetStatusMessage(""); //slz
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buflen != 0) {
        //editorSetStatusMessage(""); //slz
        return buf;
      }
    // <ctype> library function void iscntrl(int c) ctrl-m is interpreted as '\r' above
    } else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }
  }
}

void editorMoveCursor(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
    case ARROW_LEFT:
      if (E.cx != 0) {
        E.cx--;
      } else if (E.cy > 0) {
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cx < row->size) {
        E.cx++;
      } else if (row && E.cx == row->size) {
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
      if (E.cy != 0) {
        E.cy--;
      }
      break;
    case ARROW_DOWN:
      if (E.cy < E.numrows) {
        E.cy++;
      }
      break;
  }

  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}
// higher level editor function depends on editorReadKey()
void editorProcessKeypress() {
  static int quit_times = KILO_QUIT_TIMES;

  int c = editorReadKey(); //this is the money shot that brings back a c that has been processed

  switch (c) {
    case '\r':
      editorInsertNewline();
      break;

    case '\t':
      //c = '*';
      editorInsertChar(' ');
      editorInsertChar(' ');
      editorInsertChar(' ');
      editorInsertChar(' ');
      break;

    case CTRL_KEY('q'):
      if (E.dirty && quit_times > 0) {
        editorSetStatusMessage("WARNING!!! File has unsaved changes. "
          "Press Ctrl-Q %d more times to quit.", quit_times);
        quit_times--;
        return;
      }
      write(STDOUT_FILENO, "\x1b[2J", 4); //clears the screen
      write(STDOUT_FILENO, "\x1b[H", 3); //cursor goes home, which is to first char
      exit(0);
      break;

    case CTRL_KEY('s'):
      editorSave();
      break;

    case HOME_KEY:
      E.cx = 0;
      break;

    case END_KEY:
      if (E.cy < E.numrows)
        E.cx = E.row[E.cy].size;
      break;

    case BACKSPACE:
    //case CTRL_KEY('h'): //slz - didn't think needed ctrl-h for backspace
    case DEL_KEY:
      if (c == DEL_KEY) {
        editorMoveCursor(ARROW_RIGHT);}
      editorDelChar();
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        if (c == PAGE_UP) {
          E.cy = E.rowoff;
        } else if (c == PAGE_DOWN) {
          E.cy = E.rowoff + E.screenrows - 1;
          if (E.cy > E.numrows) E.cy = E.numrows;
        }

        int times = E.screenrows;
        while (times--){
          editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
          } //editorSetStatusMessage("You pressed %d", c);} //sz addition
      }
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;

    case CTRL_KEY('l'):
    case '\x1b':
      break;

   // below is slz testing
    case CTRL_KEY('h'):
      //editorInsertChar('*');
      getwordundercursor();
      break;

    default:
      editorInsertChar(c);
      break;
  }

  quit_times = KILO_QUIT_TIMES;
}
/*** slz testing stuff ***/
void getwordundercursor() {
  erow *row = &E.row[E.cy];
  char d = row->chars[E.cx];
  editorSetStatusMessage("character under cursor: %c", d); 
}
/*** init ***/

void initEditor() {
  E.cx = 0; //cursor x position
  E.cy = 0; //cursor y position
  E.rowoff = 0;  //row the user is currently scrolled to  
  E.coloff = 0;  //col the user is currently scrolled to  
  E.numrows = 0; //number of rows of text
  E.row = NULL; //pointer to the erow structure 'array'
  E.dirty = 0;
  E.filename = NULL;
  E.statusmsg[0] = '\0'; // inital statusmsg is ""
  E.statusmsg_time = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
  E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }
  //I added the else - inserts text when no file is being read
  else {
    editorInsertRow(E.numrows, "Hello, Steve!", 13); //slz adds
    editorInsertRow(E.numrows, "Goodbye, world!", 15); //slz adds
  }

  //editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit"); //slz commented this out
  editorSetStatusMessage("rows: %d  cols: %d", E.screenrows, E.screencols); //this works prints rows: 43  cols: 159

  while (1) {
    editorRefreshScreen(); //screen is refreshed after every key press
    editorProcessKeypress();
    editorSetStatusMessage("row: %d  col: %d", E.cy, E.cx); //shows row and column
  }

  return 0;
}
