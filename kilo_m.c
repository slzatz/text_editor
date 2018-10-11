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

enum Command {
  C_caw,
  C_cw,
  C_daw,
  C_dw,
  C_de,
  C_d$,
  C_dd,
  C_indent,
  C_unindent,
  C_c$,
  C_gg,
  C_yy
};

/*** data ***/

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
  erow *row; //(e)ditorrow stores a pointer to a contiguous collection of erow structures 
  int prev_numrows; // the number of rows of text so last text row is always row numrows
  erow *prev_row; //for undo purposes
  int dirty; //file changes since last save
  char *filename;
  char statusmsg[80]; //status msg is a character array max 80 char
  time_t statusmsg_time;
  struct termios orig_termios;
  int highlight[2];
  int mode;
  char command[20]; //needs to accomodate file name ?malloc heap array
  int repeat;
  int indent;
  int smartindent;
};

struct editorConfig E;

char search_string[30] = {'\0'}; //used for '*' and 'n' searches

// buffers below for yanking
char *line_buffer[20] = {NULL}; //yanking lines
char string_buffer[50] = {'\0'}; //yanking chars

/*below is for multi-character commands*/
typedef struct { char *key; int val; } t_symstruct;
static t_symstruct lookuptable[] = {
  {"caw", C_caw},
  {"cw", C_cw},
  {"daw", C_daw},
  {"dw", C_dw},
  {"de", C_de},
  {"dd", C_dd},
  {">>", C_indent},
  {"<<", C_unindent},
  {"gg", C_gg},
  {"yy", C_yy},
  {"d$", C_d$}
};

#define NKEYS ((int) (sizeof(lookuptable)/sizeof(lookuptable[0])))

/*** prototypes ***/

void editorSetMessage(const char *fmt, ...);
void editorRefreshScreen();
//char *editorPrompt(char *prompt);
void getcharundercursor();
void editorDecorateWord(int c);
void editorDecorateVisual(int c);
void editorDelWord();
void editorIndentRow();
void editorUnIndentRow();
int editorIndentAmount(int y);
void editorMoveCursor(int key);
void editorBackspace();
void editorDelChar();
void editorDeleteToEndOfLine();
void editorYankLine(int n);
void editorPasteLine();
void editorPasteString();
void editorYankString();
void editorMoveCursorEOL();
void editorMoveBeginningWord();
void editorMoveEndWord(); 
void editorMoveNextWord();
void editorMarkupLink();
void getWordUnderCursor();
void editorFindNextWord();
void editorChangeCase();
void editorRestoreSnapshot(); 
void editorCreateSnapshot(); 

int keyfromstring(char *key)
{
    int i;
    for (i=0; i <  NKEYS; i++) {
        if (strcmp(lookuptable[i].key, key) == 0)
          return lookuptable[i].val;
    }

    //nothing should match -1
    return -1;
}
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

  /* if the character read was an escape, need to figure out if it was
     a recognized escape sequence or an isolated escape to switch from
     insert mode to normal mode or to reset in normal mode
  */

  if (c == '\x1b') {
    char seq[3];
    //editorSetMessage("You pressed %d", c); //slz
    // the reads time out after 0.1 seconds
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    // Assumption is that seq[0] == '[' 
    if (seq[1] >= '0' && seq[1] <= '9') {
      if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b'; //need 4 bytes
      if (seq[2] == '~') {
        //editorSetMessage("You pressed %c%c%c", seq[0], seq[1], seq[2]); //slz
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
        //editorSetMessage("You pressed %c%c", seq[0], seq[1]); //slz
        switch (seq[1]) {
          case 'A': return ARROW_UP; //<esc>[A
          case 'B': return ARROW_DOWN; //<esc>[B
          case 'C': return ARROW_RIGHT; //<esc>[C
          case 'D': return ARROW_LEFT; //<esc>[D
          case 'H': return HOME_KEY; // <esc>[H - this one is being issued
          case 'F': return END_KEY;  // <esc>[F - this one is being issued
      }
    }

    return '\x1b'; // if it doesn't match a known escape sequence like ] ... or O ... just return escape
  
  } else {
      //editorSetMessage("You pressed %d", c); //slz
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

  /*E.row is a pointer to an array of erow structures
  The array of erows that E.row points to needs to have its memory enlarged when
  you add a row. Note that erow structues are just a size and a pointer*/

  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

  /*
  memmove(dest, source, number of bytes to move?)
  moves the line at at to at+1 and all the other erow structs until the end
  when you insert into the last row E.numrows==at then no memory is moved
  apparently ok if there is no E.row[at+1] if number of bytes = 0
  so below we are moving the row structure currently at *at* to x+1
  and all the rows below *at* to a new location to make room at *at*
  to create room for the line that we are inserting
  */

  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  // section below nice - creates an erow struct for the new row
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0'; //each line is made into a c-string (maybe for searching)
  E.numrows++;
  E.dirty++;
}

void editorFreeRow(erow *row) {
  free(row->chars);
}

void editorDelRow(int at) {
  //editorSetMessage("Row to delete = %d; E.numrows = %d", at, E.numrows); 
  //if (at < 0 || at >= E.numrows) return; /orig
  if (E.numrows == 0) return; // some calls may duplicate this guard
  editorFreeRow(&E.row[at]);
  if ( E.numrows != 1) {
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  } else {
    E.row = NULL;
  }
  E.numrows--;
  if (E.cy == E.numrows && E.cy > 0) E.cy--; 
  E.dirty++;
  editorSetMessage("Row to delete = %d; E.numrows = %d", at, E.numrows); 
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
  // is there any reason to realloc for one character?
  // row->chars = realloc(row->chars, row->size -1); 
  //have to realloc when adding but I guess no need to realloc for one character
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  E.dirty++;
}

/*** editor operations ***/
void editorInsertChar(int c) {
  if (E.cy == E.numrows) {
    editorInsertRow(E.numrows, "", 0); //editorInsertRow will also insert another '\0'
  }

  erow *row = &E.row[E.cy];
  if (E.cx < 0 || E.cx > row->size) E.cx = row->size; //can either of these be true? ie is check necessary?
  row->chars = realloc(row->chars, row->size + 1); //******* was size + 2

  /* moving all the chars at the current x cursor position on char
     farther down the char string to make room for the new character
     Maybe a clue from editorInsertRow - it's memmove is below
     memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
  */

  memmove(&row->chars[E.cx + 1], &row->chars[E.cx], row->size - E.cx); //****was E.cx + 1

  row->size++;
  row->chars[E.cx] = c;
  E.dirty++;
  E.cx++;
}

/* uses VLA */
void editorInsertNewline(int direction) {
  erow *row = &E.row[E.cy];
  int i;
  if (E.cx == 0 || E.cx == row->size) {
    if (E.smartindent) i = editorIndentAmount(E.cy);
    else i = 0;
    char spaces[i + 1]; //VLA
    for (int j=0; j<i; j++) {
      spaces[j] = ' ';
    }
    spaces[i] = '\0';
    E.cy+=direction;
    editorInsertRow(E.cy, spaces, i);
    E.cx = i;
    
      
  }
  else {
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    row = &E.row[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0';//wouldn't row->chars[E.cx] = 0 be better
    if (E.smartindent) i = editorIndentAmount(E.cy);
    else i = 0;
    E.cy++;
    row = &E.row[E.cy];
    E.cx = 0;
    for (;;){
      if (row->chars[0] != ' ') break;
      editorDelChar();
    }

  for ( int j=0; j < i; j++ ) editorInsertChar(' ');
  E.cx = i;
  }
}

void editorDelChar() {
  if (E.cy == E.numrows) return;
  erow *row = &E.row[E.cy];

  /* row size = 1 means there is 1 char; size 0 means 0 chars */
  /* Note that row->size does not count the terminating '\0' char*/
  if (E.cx == 0 && row->size == 0) return; 

    memmove(&row->chars[E.cx], &row->chars[E.cx + 1], row->size - E.cx);
    row->size--;
    E.dirty++;
}

void editorBackspace() {
  if (E.cx == 0 && E.cy == 0) return;
  erow *row = &E.row[E.cy];

  if (E.cx > 0) {

    //memmove(dest, source, number of bytes to move?)
    memmove(&row->chars[E.cx - 1], &row->chars[E.cx], row->size - E.cx + 1);
    row->size--;
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    editorDelRow(E.cy);
    if (E.cy != E.numrows - 1) E.cy--; //editorDelRow will decrement if last row
  }
  E.dirty++;
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
  if (E.filename == NULL) return;
  int len;
  char *buf = editorRowsToString(&len);

  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        E.dirty = 0;
        editorSetMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }

  free(buf);
  editorSetMessage("Can't save! I/O error: %s", strerror(errno));
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
    // below line doesn't work because sends you to else
    //if (filerow >= E.numrows && filerow > 0) { //slz to not draw ~ in first row

      //drawing '~' below: first escape is red and second erases rest of line
      abAppend(ab, "\x1b[31m~\x1b[K", 9); 

    } else {
      int len = E.row[filerow].size - E.coloff;
      //if (len < 0) len = 0; //how could this ever be true? - slz
      if (len > E.screencols) len = E.screencols;
      
      if (E.mode == 3 && filerow >= E.highlight[0] && filerow <= E.highlight[1]) {
          abAppend(ab, "\x1b[48;5;242m", 11);
          abAppend(ab, &E.row[filerow].chars[E.coloff], len);
          abAppend(ab, "\x1b[0m", 4); //slz return background to normal
        
      } else if (E.mode == 4 && filerow == E.cy) {
          abAppend(ab, &E.row[filerow].chars[0], E.highlight[0]);
          abAppend(ab, "\x1b[48;5;242m", 11);
          abAppend(ab, &E.row[filerow].chars[E.highlight[0]], E.highlight[1]-E.highlight[0]);
          abAppend(ab, "\x1b[0m", 4); //slz return background to normal
          abAppend(ab, &E.row[filerow].chars[E.highlight[1]], len - E.highlight[1]);
        
      } else abAppend(ab, &E.row[filerow].chars[E.coloff], len);
    
    //"\x1b[K" erases the part of the line to the right of the cursor in case the
    // new line i shorter than the old

    abAppend(ab, "\x1b[K", 3); 
    }
    abAppend(ab, "\r\n", 2);
    abAppend(ab, "\x1b[0m", 4); //slz return background to normal
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
  /*void editorSetMessage(const char *fmt, ...) is where the message is created/set*/

  //"\x1b[K" erases the part of the line to the right of the cursor in case the
  // new line i shorter than the old

  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 1000) //time
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
  if (E.mode != 2){
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
                                            (E.cx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));
}
  abAppend(&ab, "\x1b[?25h", 6); //shows the cursor

  write(STDOUT_FILENO, ab.b, ab.len);

  // could ab be memcopied into cd until mode changes from 1 to 0?
  // could cd then be freed when mode changed from

  abFree(&ab);
 // nn++;
}


/*va_list, va_start(), and va_end() come from <stdarg.h> and vsnprintf() is
from <stdio.h> and time() is from <time.h>.  stdarg.h allows functions to accept a
variable number of arguments and are declared with an ellipsis in place of the last parameter.*/

void editorSetMessage(const char *fmt, ...) {
  va_list ap; //type for iterating arguments
  va_start(ap, fmt); // start iterating arguments with a va_list


  /* vsnprint from <stdio.h> writes to the character string str
     vsnprint(char *str,size_t size, const char *format, va_list ap)*/

  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap); //free a va_list
  E.statusmsg_time = time(NULL);
}

/*** input ***/

/*

editorPrompt has some interesting code about buffers, keys, backspaces but not in use

char *editorPrompt(char *prompt) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);

  size_t buflen = 0;
  buf[0] = '\0';

  while (1) {
    editorSetMessage(prompt, buf);
    editorRefreshScreen();

    int c = editorReadKey();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) buf[--buflen] = '\0';
    } else if (c == '\x1b') {
      //editorSetMessage(""); //slz
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buflen != 0) {
        //editorSetMessage(""); //slz
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
*/

void editorMoveCursor(int key) {
  //erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy]; //orig - didn't thinke we needed check

  erow *row = &E.row[E.cy];

  switch (key) {
    case ARROW_LEFT:
    case 'h':
      if (E.cx != 0) E.cx--; //do need to check for row?
      break;

    case ARROW_RIGHT:
    case 'l':
      if (row && E.cx < row->size)  E.cx++;  //segfaults on opening if you arrow right w/o row
      break;

    case ARROW_UP:
    case 'k':
      if (E.cy != 0) E.cy--;
      break;

    case ARROW_DOWN:
    case 'j':
      if (E.cy < E.numrows-1) E.cy++; //slz change added -1 
      break;
  }

  /* Below deals with moving cursor up and down from longer rows to shorter rows 
     row has to be calculated again because this is the new row you've landed on */
  //row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy]; // I don't think this check is necessary

  row = &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (rowlen == 0) E.cx = 0;
  else if (E.mode == 1 && E.cx >= rowlen) E.cx = rowlen;
  else if (E.cx >= rowlen) E.cx = rowlen-1;

}
// higher level editor function depends on editorReadKey()
void editorProcessKeypress() {
  static int quit_times = KILO_QUIT_TIMES;
  int i, start, end;
  //char ex[10] = {':', '\0'};
  //char z[10];

  /* editorReadKey brings back one processed character that handles
     escape sequences for things like navigation keys
  */

  int c = editorReadKey();


/**************************************** 
 * This would be a place to create prev * 
 * row                                  *
 ***************************************/
  //if (c == 'u' || c == 'i' || c == '\x1b' || (E.mode == 0 && isdigit(c))) ;
  //else editorCreateSnapshot();

/*************************************** 
 * This is where you enter insert mode* 
 * E.mode = 1
 ***************************************/

  if (E.mode == 1){

  switch (c) {

    case '\r':
      editorCreateSnapshot();
      editorInsertNewline(1);
      break;

    case CTRL_KEY('q'):
      if (E.dirty && quit_times > 0) {
        editorSetMessage("WARNING!!! File has unsaved changes. "
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
      editorCreateSnapshot();
      editorBackspace();
      break;

    case DEL_KEY:
      editorCreateSnapshot();
      editorDelChar();
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      
      if (c == PAGE_UP) {
        E.cy = E.rowoff;
      } else if (c == PAGE_DOWN) {
        E.cy = E.rowoff + E.screenrows - 1;
        if (E.cy > E.numrows) E.cy = E.numrows;
      }

        int times = E.screenrows;
        while (times--){
          editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
          } 
      
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;

    case CTRL_KEY('b'):
    case CTRL_KEY('i'):
    case CTRL_KEY('e'):
      editorCreateSnapshot();
      editorDecorateWord(c);
      break;

    case CTRL_KEY('z'):
      E.smartindent = (E.smartindent == 1) ? 0 : 1;
      editorSetMessage("E.smartindent = %d", E.smartindent); 
      break;

    case '\x1b':
      E.mode = 0;
      if (E.cx > 0) E.cx--;
      int n = editorIndentAmount(E.cy);
      if (n == E.row[E.cy].size) {
        E.cx = 0;
        for (int i = 0; i < n; i++) {
          editorDelChar();
        }
      }
      editorSetMessage("");
      break;

    default:
      editorCreateSnapshot();
      editorInsertChar(c);
      break;
 
 } 
  quit_times = KILO_QUIT_TIMES;

/*************************************** 
 * This is where you enter normal mode* 
 * E.mode = 0
 ***************************************/

 } else if (E.mode == 0){
 
  /*leading digit is a multiplier*/
  if (isdigit(c)) { //equiv to if (c > 47 && c < 58) 
    if ( E.repeat == 0 ){

      if ( c != 48 ) { //if c = 48 = 0 then it falls through to 0 more to beginning of line
        E.repeat = c - 48;
        return;
      }  

    } else { 
      E.repeat = E.repeat*10 + c - 48;
      return;
    }
  }

  if ( E.repeat == 0 ) E.repeat = 1;

  switch (c) {

    case 'i':
      if (E.command[0] == '\0') { //This probably needs to be generalized but makes sure 'd$' works
        E.mode = 1;
        E.command[0] = '\0';
        E.repeat = 0;
        editorSetMessage("\x1b[1m-- INSERT --\x1b[0m");
      return;
      }
      break;

    case 's':
      editorCreateSnapshot();
      for (int i = 0; i < E.repeat; i++) editorDelChar();
      E.command[0] = '\0';
      E.repeat = 0;
      E.mode = 1;
      editorSetMessage("\x1b[1m-- INSERT --\x1b[0m"); //[1m=bold
      return;

    case 'x':
      editorCreateSnapshot();
      for (int i = 0; i < E.repeat; i++) editorDelChar();
      E.command[0] = '\0';
      E.repeat = 0;
      return;
    
    case 'r':
      E.mode = 5;
      return;

    case '~':
      editorCreateSnapshot();
      editorChangeCase();
      E.command[0] = '\0';
      E.repeat = 0;
      return;

    case 'a':
      if (E.command[0] == '\0') { //This probably needs to be generalized but makes sure 'd$' works
        E.mode = 1; //this has to go here for MoveCursor to work right at EOLs
        editorMoveCursor(ARROW_RIGHT);
        E.command[0] = '\0';
        E.repeat = 0;
        editorSetMessage("\x1b[1m-- INSERT --\x1b[0m");
      return;
      }
      break;

    case 'A':
      editorMoveCursorEOL();
      E.mode = 1; //needs to be here for movecursor to work at EOLs
      editorMoveCursor(ARROW_RIGHT);
      E.command[0] = '\0';
      E.repeat = 0;
      E.mode = 1;
      editorSetMessage("\x1b[1m-- INSERT --\x1b[0m");
      return;

    case 'w':
      if (E.command[0] == '\0') { //This probably needs to be generalized but makes sure 'dw' works
        editorMoveNextWord();
        E.command[0] = '\0';
        E.repeat = 0;
        return;
      }
      break;

    case 'b':
      editorMoveBeginningWord();
      E.command[0] = '\0';
      E.repeat = 0;
      return;

    case 'e':
      if (E.command[0] == '\0') { //This probably needs to be generalized but makes sure 'de' works
        editorMoveEndWord();
        E.command[0] = '\0';
        E.repeat = 0;
        return;
        }
      break;

    case '0':
      E.cx = 0;
      E.command[0] = '\0';
      E.repeat = 0;
      return;

    case '$':
      if (E.command[0] == '\0') { //This probably needs to be generalized but makes sure 'd$' works
        editorMoveCursorEOL();
        E.command[0] = '\0';
        E.repeat = 0;
        return;
      }
      break;

    case 'I':
      E.cx = editorIndentAmount(E.cy);
      E.mode = 1;
      E.command[0] = '\0';
      E.repeat = 0;
      editorSetMessage("\x1b[1m-- INSERT --\x1b[0m");
      return;

    case 'o':
      editorCreateSnapshot();
      E.cx = 0;
      editorInsertNewline(1);
      E.mode = 1;
      E.command[0] = '\0';
      E.repeat = 0;
      editorSetMessage("\x1b[1m-- INSERT --\x1b[0m");
      return;

    case 'O':
      editorCreateSnapshot();
      E.cx = 0;
      editorInsertNewline(0);
      E.mode = 1;
      E.command[0] = '\0';
      E.repeat = 0;
      editorSetMessage("\x1b[1m-- INSERT --\x1b[0m");
      return;

    case 'G':
      E.cx = 0;
      E.cy = E.numrows-1;
      E.command[0] = '\0';
      E.repeat = 0;
      return;
  
    case ':':
      E.mode = 2;
      E.command[0] = ':';
      E.command[1] = '\0';
      editorSetMessage(":"); 
      return;

    case 'V':
      E.mode = 3;
      E.command[0] = '\0';
      E.repeat = 0;
      E.highlight[0] = E.highlight[1] = E.cy;
      editorSetMessage("\x1b[1m-- VISUAL LINE --\x1b[0m");
      return;

    case 'v':
      E.mode = 4;
      E.command[0] = '\0';
      E.repeat = 0;
      E.highlight[0] = E.highlight[1] = E.cx;
      editorSetMessage("\x1b[1m-- VISUAL --\x1b[0m");
      return;

    case 'p':  
      editorCreateSnapshot();
      if (strlen(string_buffer)) editorPasteString();
      else editorPasteLine();
      E.command[0] = '\0';
      E.repeat = 0;
      return;

    case '*':  
      getWordUnderCursor();
      editorFindNextWord(); 
      return;

    case 'n':
      editorFindNextWord();
      return;

    case 'u':
      // set some flag that says set E.row --> E.prev_row
      editorRestoreSnapshot();
      return;

    case CTRL_KEY('z'):
      E.smartindent = (E.smartindent == 4) ? 0 : 4;
      editorSetMessage("E.smartindent = %d", E.smartindent); 
      return;

    case CTRL_KEY('b'):
    case CTRL_KEY('i'):
    case CTRL_KEY('e'):
      editorCreateSnapshot();
      editorDecorateWord(c);
      return;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
    case 'h':
    case 'j':
    case 'k':
    case 'l':
      editorMoveCursor(c);
      E.command[0] = '\0'; //arrow does reset command in vim although left/right arrow don't do anything = escape
      E.repeat = 0;
      return;

// for testing purposes I am using CTRL-h in normal mode
    case CTRL_KEY('h'):
      editorMarkupLink(); 
      return;

    case '\x1b':
    // Leave in E.mode = 0 -> normal mode
      E.command[0] = '\0';
      E.repeat = 0;
      return;
  }

  // don't want a default case just want it to fall through
  // if it doesn't match switch above
  // presumption is it's a multicharacter command

  int n = strlen(E.command);
  E.command[n] = c;
  E.command[n+1] = '\0';

  switch (keyfromstring(E.command)) {
    
    case C_daw:
      editorCreateSnapshot();
      for (int i = 0; i < E.repeat; i++) editorDelWord();
      E.command[0] = '\0';
      E.repeat = 0;
      return;

    case C_dw:
      // not right if on last letter of word (deletes next word) but probably won't fix
      editorCreateSnapshot();
      start = E.cx;
      editorMoveEndWord();
      end = E.cx;
      E.cx = start;
      for (int j = 0; j < end - start + 2; j++) editorDelChar();
      E.command[0] = '\0';
      E.repeat = 0;
      return;

    case C_de:
      editorCreateSnapshot();
      start = E.cx;
      editorMoveEndWord();
      end = E.cx;
      E.cx = start; 
      for (int j = 0; j < end - start + 1; j++) editorDelChar();
      E.cx = (start < E.row[E.cy].size) ? start : E.row[E.cy].size -1;
      E.command[0] = '\0';
      E.repeat = 0;
      return;

    case C_dd:
      if (E.numrows != 0) {
        int r = E.numrows - E.cy;
        E.repeat = (r >= E.repeat) ? E.repeat : r ;
        editorCreateSnapshot();
        editorYankLine(E.repeat);
        for (int i = 0; i < E.repeat ; i++) editorDelRow(E.cy);
      }
      E.cx = 0;
      E.command[0] = '\0';
      E.repeat = 0;
      return;

    case C_d$:
      editorDeleteToEndOfLine();
      return;

    case C_cw:
      editorCreateSnapshot();
      start = E.cx;
      editorMoveEndWord();
      end = E.cx;
      E.cx = start;
      for (int j = 0; j < end - start + 1; j++) editorDelChar();
      E.command[0] = '\0';
      E.repeat = 0;
      E.mode = 1;
      editorSetMessage("\x1b[1m-- INSERT --\x1b[0m");
      return;

    case C_caw:
      editorCreateSnapshot();
      for (int i = 0; i < E.repeat; i++) editorDelWord();
      E.command[0] = '\0';
      E.repeat = 0;
      E.mode = 1;
      editorSetMessage("\x1b[1m-- INSERT --\x1b[0m");
      return;

    case C_indent:
      editorCreateSnapshot();
      for ( i = 0; i < E.repeat; i++ ) {
        editorIndentRow();
        E.cy++;}
      E.cy-=i;
      E.command[0] = '\0';
      E.repeat = 0;
      return;

    case C_unindent:
      editorCreateSnapshot();
      for ( i = 0; i < E.repeat; i++ ) {
        editorUnIndentRow();
        E.cy++;}
      E.cy-=i;
      E.command[0] = '\0';
      E.repeat = 0;
      return;

    case C_gg:
     E.cx = 0;
     E.cy = E.repeat-1;
     E.command[0] = '\0';
     E.repeat = 0;
     return;

   case C_yy:  
     editorYankLine(E.repeat);
     E.command[0] = '\0';
     E.repeat = 0;
     return;

    default:
      return;

    } 

  /************************************   
   *command line mode below E.mode = 2*
   ************************************/

  } else if (E.mode == 2) {

    if (c == '\x1b') {
      E.mode = 0;
      E.command[0] = '\0';
      editorSetMessage(""); 
      return;}

      if (c == '\r') {
        if (E.command[1] == 'w') {
          if (strlen(E.command) > 3) {
            E.filename = strdup(&E.command[3]);
            editorSave();
            editorSetMessage("\"%s\" written", E.filename);
          }
          else if (E.filename != NULL) {
              editorSave();
              editorSetMessage("\"%s\" written", E.filename);
          }
          else editorSetMessage("No file name");

          E.mode = 0;
          E.command[0] = '\0';
        }

        if (E.command[1] == 'x') {
          if (strlen(E.command) > 3) {
            E.filename = strdup(&E.command[3]);
            editorSave();
            write(STDOUT_FILENO, "\x1b[2J", 4); //clears the screen
            write(STDOUT_FILENO, "\x1b[H", 3); //cursor goes home, which is to first char
            exit(0);
          }
          else if (E.filename != NULL) {
            editorSave();
            write(STDOUT_FILENO, "\x1b[2J", 4); //clears the screen
            write(STDOUT_FILENO, "\x1b[H", 3); //cursor goes home, which is to first char
            exit(0);
          }
          else editorSetMessage("No file name");

          E.mode = 0;
          E.command[0] = '\0';
        }

        if (E.command[1] == 'q') {
          if (E.dirty) {
            if (strlen(E.command) == 3 && E.command[2] == '!') {
              write(STDOUT_FILENO, "\x1b[2J", 4); //clears the screen
              write(STDOUT_FILENO, "\x1b[H", 3); //cursor goes home, which is to first char
              exit(0);
            }  
            else {
              E.mode = 0;
              E.command[0] = '\0';
              editorSetMessage("No write since last change");
            }
          }
        
          else {
            write(STDOUT_FILENO, "\x1b[2J", 4); //clears the screen
            write(STDOUT_FILENO, "\x1b[H", 3); //cursor goes home, which is to first char
            exit(0);
          }
        }
    }

    else {
      int n = strlen(E.command);
      if (c == DEL_KEY || c == BACKSPACE) {
        E.command[n-1] = '\0';
      } else {
        E.command[n] = c;
        E.command[n+1] = '\0';
      }
      editorSetMessage(E.command);
    }
  /********************************************
   * visual line mode E.mode = 3
   ********************************************/

  } else if (E.mode == 3) {


    switch (c) {

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
    case 'h':
    case 'j':
    case 'k':
    case 'l':
      editorMoveCursor(c);
      E.highlight[1] = E.cy;
      //E.command[0] = '\0'; //untested but I believe arrow should reset command
      return;

    case 'x':
      if (E.numrows != 0) {
        editorCreateSnapshot();
        E.repeat = E.highlight[1] - E.highlight[0] + 1;
        E.cy = E.highlight[0];
        editorYankLine(E.repeat);

        for (int i = 0; i < E.repeat; i++) editorDelRow(E.cy);
      }
      E.cx = 0;
      E.command[0] = '\0';
      E.repeat = 0;
      E.mode = 0;
      editorSetMessage("");
      return;

    case 'y':  
      E.repeat = E.highlight[1] - E.highlight[0] + 1;
      E.cy = E.highlight[0];
      editorYankLine(E.repeat);
      E.command[0] = '\0';
      E.repeat = 0;
      E.mode = 0;
      editorSetMessage("");
      return;

    case '>':
      editorCreateSnapshot();
      E.repeat = E.highlight[1] - E.highlight[0] + 1;
      E.cy = E.highlight[0];
      for ( i = 0; i < E.repeat; i++ ) {
        editorIndentRow();
        E.cy++;}
      E.cy-=i;
      E.command[0] = '\0';
      E.repeat = 0;
      E.mode = 0;
      editorSetMessage("");
      return;

    case '<':
      editorCreateSnapshot();
      E.repeat = E.highlight[1] - E.highlight[0] + 1;
      E.cy = E.highlight[0];
      for ( i = 0; i < E.repeat; i++ ) {
        editorUnIndentRow();
        E.cy++;}
      E.cy-=i;
      E.command[0] = '\0';
      E.repeat = 0;
      E.mode = 0;
      editorSetMessage("");
      return;

    case '\x1b':
      E.mode = 0;
      E.command[0] = '\0';
      E.repeat = 0;
      editorSetMessage("");
      return;

    default:
      return;
    }

  } else if (E.mode == 4) {

    switch (c) {

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
    case 'h':
    case 'j':
    case 'k':
    case 'l':
      editorMoveCursor(c);
      E.highlight[1] = E.cx;
      //E.command[0] = '\0'; //untested but I believe arrow should reset command
      return;

    case 'x':
      editorCreateSnapshot();
      E.repeat = E.highlight[1] - E.highlight[0] + 1;
      E.cx = E.highlight[0];
      editorYankString(); 

      for (int i = 0; i < E.repeat; i++) {
        editorDelChar(E.cx);
      }

      E.command[0] = '\0';
      E.repeat = 0;
      E.mode = 0;
      editorSetMessage("");
      return;

    case 'y':  
      E.repeat = E.highlight[1] - E.highlight[0] + 1;
      E.cx = E.highlight[0];
      editorYankString();
      E.command[0] = '\0';
      E.repeat = 0;
      E.mode = 0;
      editorSetMessage("");
      return;

    case CTRL_KEY('b'):
    case CTRL_KEY('i'):
    case CTRL_KEY('e'):
      editorCreateSnapshot();
      editorDecorateVisual(c);
      E.command[0] = '\0';
      E.repeat = 0;
      E.mode = 0;
      editorSetMessage("");
      return;

    case '\x1b':
      E.mode = 0;
      E.command[0] = '\0';
      E.repeat = 0;
      editorSetMessage("");
      return;

    default:
      return;
    }
  } else if (E.mode == 5) {
      editorCreateSnapshot();
      editorDelChar();
      editorInsertChar(c);
      E.repeat = 0;
      E.command[0] = '\0';
      E.mode = 0;
  }
}

/*** slz additions ***/

void editorCreateSnapshot() {
  for (int j = 0 ; j < E.prev_numrows ; j++ ) {
    free(E.prev_row[j].chars);
  }
  E.prev_row = realloc(E.prev_row, sizeof(erow) * E.numrows );
  for ( int i = 0 ; i < E.numrows ; i++ ) {
    int len = E.row[i].size;
    E.prev_row[i].chars = malloc(len + 1);
    E.prev_row[i].size = len;
    memcpy(E.prev_row[i].chars, E.row[i].chars, len);
    E.prev_row[i].chars[len] = '\0';
  }
  E.prev_numrows = E.numrows;
}

void editorRestoreSnapshot() {
  for (int j = 0 ; j < E.numrows ; j++ ) {
    free(E.row[j].chars);
  } 
  E.row = realloc(E.row, sizeof(erow) * E.prev_numrows );
  for (int i = 0 ; i < E.prev_numrows ; i++ ) {
    int len = E.prev_row[i].size;
    E.row[i].chars = malloc(len + 1);
    E.row[i].size = len;
    memcpy(E.row[i].chars, E.prev_row[i].chars, len);
    E.row[i].chars[len] = '\0';
  }
  E.numrows = E.prev_numrows;
}

void editorChangeCase() {
  erow *row = &E.row[E.cy];
  char d = row->chars[E.cx];
  if (d < 91 && d > 64) d = d + 32;
  else if (d > 96 && d < 123) d = d - 32;
  else {
    editorMoveCursor(ARROW_RIGHT);
    return;
  }
  editorDelChar();
  editorInsertChar(d);
}

void editorYankLine(int n){
  for (int i=0; i < 10; i++) {
    free(line_buffer[i]);
    line_buffer[i] = NULL;
    }

  for (int i=0; i < n; i++) {
    int len = E.row[E.cy+i].size;
    line_buffer[i] = malloc(len + 1);
    memcpy(line_buffer[i], E.row[E.cy+i].chars, len);
    line_buffer[i][len] = '\0';
  }
  // set string_buffer to "" to signal should paste line
  string_buffer[0] = '\0';
}

void editorYankString() {
  int n,x;
  erow *row = &E.row[E.cy];
  for (x = E.highlight[0], n = 0; x < E.highlight[1]+1; x++, n++) {
      string_buffer[n] = row->chars[x];
  }

  string_buffer[n] = '\0';
}

void editorPasteString() {
  if (E.cy == E.numrows) {
    editorInsertRow(E.numrows, "", 0); //editorInsertRow will also insert another '\0'
  }

  erow *row = &E.row[E.cy];
  if (E.cx < 0 || E.cx > row->size) E.cx = row->size;
  int len = strlen(string_buffer);
  row->chars = realloc(row->chars, row->size + len); 

  /* moving all the chars at the current x cursor position on char
     farther down the char string to make room for the new character
     Maybe a clue from editorInsertRow - it's memmove is below
     memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
  */

  memmove(&row->chars[E.cx + len], &row->chars[E.cx], row->size - E.cx); //****was E.cx + 1

  for (int i = 0; i < len; i++) {
    row->size++;
    row->chars[E.cx] = string_buffer[i];
    E.cx++;
  }
  E.dirty++;
}


void editorPasteLine(){
  for (int i=0; i < 10; i++) {
    if (line_buffer[i] == NULL) break;

    int len = strlen(line_buffer[i]);
    E.cy++;
    editorInsertRow(E.cy, line_buffer[i], len);
  }
}

void editorIndentRow() {
  erow *row = &E.row[E.cy];
  if (row->size == 0) return;
  //E.cx = 0;
  E.cx = editorIndentAmount(E.cy);
  for (int i = 0; i < E.indent; i++) editorInsertChar(' ');
  E.dirty++;
}

void editorUnIndentRow() {
  erow *row = &E.row[E.cy];
  if (row->size == 0) return;
  E.cx = 0;
  for (int i = 0; i < E.indent; i++) {
    if (row->chars[0] == ' ') {
      editorDelChar();
    }
  }
  E.dirty++;
}

int editorIndentAmount(int y) {
  int i;
  erow *row = &E.row[y];
  if ( !row || row->size == 0 ) return 0; //row is NULL if the row has been deleted or opening app

  for ( i = 0; i < row->size; i++) {
    if (row->chars[i] != ' ') break;}

  return i;
}

void editorDelWord() {
  erow *row = &E.row[E.cy];
  if (row->chars[E.cx] < 48) return;

  int i,j,x;
  for (i = E.cx; i > -1; i--){
    if (row->chars[i] < 48) break;
    }
  for (j = E.cx; j < row->size ; j++) {
    if (row->chars[j] < 48) break;
  }
  E.cx = i+1;

  for (x = 0 ; x < j-i; x++) {
      editorDelChar();
  }
  E.dirty++;
  //editorSetMessage("i = %d, j = %d", i, j ); 
}

void editorDeleteToEndOfLine() {
  erow *row = &E.row[E.cy];
  row->size = E.cx;
  //Arguably you don't have to reallocate when you reduce the length of chars
  row->chars = realloc(row->chars, E.cx + 1); //added 10042018 - before wasn't reallocating memory
  row->chars[E.cx] = '\0';
  }

void editorMoveCursorEOL() {
  E.cx = E.row[E.cy].size - 1; 
}

void editorMoveNextWord() {
  int j;
  erow row = E.row[E.cy];
  editorMoveEndWord();
  for (j = E.cx + 1; j < row.size ; j++) {
    if (row.chars[j] > 48) break;
  }
  E.cx = j;
}

void editorMoveBeginningWord() {
  erow *row = &E.row[E.cy];
  if (E.cx == 0) return;
  for (;;) {
    if (row->chars[E.cx - 1] < 48) E.cx--;
    else break;
    if (E.cx == 0) return;
  }

  int i;
  for (i = E.cx - 1; i > -1; i--){
    if (row->chars[i] < 48) break;
  }

  E.cx = i + 1;
}

void editorMoveEndWord() {
  erow *row = &E.row[E.cy];
  if (E.cx == row->size - 1) return;
  for (;;) {
    if (row->chars[E.cx + 1] < 48) E.cx++;
    else break;
    if (E.cx == row->size - 1) return;
  }

  int j;
  for (j = E.cx + 1; j < row->size ; j++) {
    if (row->chars[j] < 48) break;
  }

  E.cx = j - 1;
}

void editorDecorateWord(int c) {
  erow *row = &E.row[E.cy];
  char cc;
  if (row->chars[E.cx] < 48) return;

  int i, j;

  /*Note to catch ` would have to be row->chars[i] < 48 || row-chars[i] == 96 - may not be worth it*/

  for (i = E.cx - 1; i > -1; i--){
    if (row->chars[i] < 48) break;
  }

  for (j = E.cx + 1; j < row->size ; j++) {
    if (row->chars[j] < 48) break;
  }
  
  if (row->chars[i] != '*' && row->chars[i] != '`'){
    cc = (c == CTRL_KEY('b') || c ==CTRL_KEY('i')) ? '*' : '`';
    E.cx = i + 1;
    editorInsertChar(cc);
    E.cx = j + 1;
    editorInsertChar(cc);

    if (c == CTRL_KEY('b')) {
      E.cx = i + 1;
      editorInsertChar('*');
      E.cx = j + 2;
      editorInsertChar('*');
    }
  } else {
    E.cx = i;
    editorDelChar();
    E.cx = j-1;
    editorDelChar();

    if (c == CTRL_KEY('b')) {
      E.cx = i - 1;
      editorDelChar();
      E.cx = j - 2;
      editorDelChar();
    }
  }
}

void editorDecorateVisual(int c) {
  E.cx = E.highlight[0];
  if (c == CTRL_KEY('b')) {
    editorInsertChar('*');
    editorInsertChar('*');
    E.cx = E.highlight[1]+3;
    editorInsertChar('*');
    editorInsertChar('*');
  } else {
    char cc = (c ==CTRL_KEY('i')) ? '*' : '`';
    editorInsertChar(cc);
    E.cx = E.highlight[1]+2;
    editorInsertChar(cc);
  }
}

void getWordUnderCursor(){
  erow *row = &E.row[E.cy];
  if (row->chars[E.cx] < 48) return;

  int i,j,n,x;

  for (i = E.cx - 1; i > -1; i--){
    if (row->chars[i] < 48) break;
  }

  for (j = E.cx + 1; j < row->size ; j++) {
    if (row->chars[j] < 48) break;
  }

  for (x = i + 1, n = 0; x < j; x++, n++) {
      search_string[n] = row->chars[x];
  }

  search_string[n] = '\0';
  editorSetMessage("word under cursor: <%s>", search_string); 

}

void editorFindNextWord() {
  int y, x;
  char *z;
  y = E.cy;
  x = E.cx + 1; //in case sitting on beginning of the word
 
  /*n counter is for no matches for command 'n'*/
  for ( int n=0; n < E.numrows; n++ ) {
    erow *row = &E.row[y];
    z = strstr(&(row->chars[x]), search_string);
    if ( z != NULL ) {
      E.cy = y;
      E.cx = z - row->chars;
      break;
    }
    y++;
    x = 0;
    if ( y == E.numrows ) y = 0;
  }

    editorSetMessage("x = %d; y = %d", x, y); 
}

void editorMarkupLink() {
  int y, numrows, j, n, p;
  char *z;
  char *http = "http";
  char *bracket_http = "[http";
  numrows = E.numrows;
  n = 1;


  for ( n=1; E.row[numrows-n].chars[0] == '[' ; n++ );


  for (y=0; y<numrows; y++){
    erow *row = &E.row[y];
    if (row->chars[0] == '[') continue;
    if (strstr(row->chars, bracket_http)) continue;

    z = strstr(row->chars, http);
    if (z==NULL) continue;
    E.cy = y;
    p = z - row->chars;

    //url including http:// must be at least 10 chars you'd think
    for (j = p + 10; j < row->size ; j++) { 
      if (row->chars[j] == 32) break;
    }

    int len = j-p;
    char *zz = malloc(len + 1);
    memcpy(zz, z, len);
    zz[len] = '\0';

    E.cx = p;
    editorInsertChar('[');
    E.cx = j+1;
    editorInsertChar(']');
    editorInsertChar('[');
    editorInsertChar(48+n);
    editorInsertChar(']');

    if ( E.row[numrows-1].chars[0] != '[' ) {
      E.cy = E.numrows - 1; //check why need - 1 otherwise seg faults
      E.cx = 0;
      editorInsertNewline(1);
      }

    editorInsertRow(E.numrows, zz, len); 
    free(zz);
    E.cx = 0;
    E.cy = E.numrows - 1;
    editorInsertChar('[');
    editorInsertChar(48+n);
    editorInsertChar(']');
    editorInsertChar(':');
    editorInsertChar(' ');
    editorSetMessage("z = %u and beginning position = %d and end = %d and %u", z, p, j,zz); 
    n++;
  }
}

/*** slz testing stuff ***/

void getcharundercursor() {
  erow *row = &E.row[E.cy];
  char d = row->chars[E.cx];
  editorSetMessage("character under cursor at position %d of %d: %c", E.cx, row->size, d); 
}

/*** slz testing stuff (above) ***/

/*** init ***/

void initEditor() {
  E.cx = 0; //cursor x position
  E.cy = 0; //cursor y position
  E.rowoff = 0;  //row the user is currently scrolled to  
  E.coloff = 0;  //col the user is currently scrolled to  
  E.numrows = 0; //number of rows of text
  E.row = NULL; //pointer to the erow structure 'array'
  E.prev_numrows = 0; //number of rows of text in snapshot
  E.prev_row = NULL; //prev_row is pointer to snapshot for undoing
  E.dirty = 0; //has filed changed since last save
  E.filename = NULL;
  E.statusmsg[0] = '\0'; // inital statusmsg is ""
  E.statusmsg_time = 0;
  E.highlight[0] = E.highlight[1] = -1;
  E.mode = 0; //0=normal; 1=insert; 2=command line
  E.command[0] = '\0';
  E.repeat = 0; //number of times to repeat commands like x,s,yy also used for visual line mode x,y
  E.indent = 4;
  E.smartindent = 1; //CTRL-z toggles - don't want on what pasting from outside source

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
  E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  // for testing purposes added the else - inserts text for testing purposes 
  // when no file is being read
  else {
    editorInsertRow(E.numrows, "Hello, Steve!", 13); 
    E.cx = E.row[E.cy].size; //put cursor at end of line
    editorInsertNewline(1); 
    editorInsertRow(E.numrows, "http://www.webmd.com", 20); //testing url markup
    editorInsertRow(E.numrows, "abc def ghi", 11); 
    E.cy = 2; //puts cursor at end of line above
  }

  //editorSetMessage("HELP: Ctrl-S = save | Ctrl-Q = quit"); //slz commented this out
  editorSetMessage("rows: %d  cols: %d", E.screenrows, E.screencols); //for display screen dimens

  while (1) {
    editorRefreshScreen(); //screen is refreshed after every key press - need to be smarter
    editorProcessKeypress();
  }
  return 0;
}
