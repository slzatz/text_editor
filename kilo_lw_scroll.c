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
  int screenrows; //number of rows in the display
  int screencols;  //number of columns in the display
  int filerows; // the number of rows(lines) of text delineated by /n if written out to a file
  erow *row; //(e)ditorrow stores a pointer to a contiguous collection of erow structures 
  int prev_filerows; // the number of rows of text so last text row is always row numrows
  erow *prev_row; //for undo purposes
  int dirty; //file changes since last save
  char *filename;
  char statusmsg[120]; //status msg is a character array max 80 char
  //time_t statusmsg_time;
  struct termios orig_termios;
  int highlight[2];
  int mode;
  char command[20]; //needs to accomodate file name ?malloc heap array
  int repeat;
  int indent;
  int smartindent;
  int continuation;
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
void editorRefreshScreen(void);
void getcharundercursor(void);
void editorDecorateWord(int c);
void editorDecorateVisual(int c);
void editorDelWord(void);
void editorIndentRow(void);
void editorUnIndentRow(void);
int editorIndentAmount(int y);
void editorMoveCursor(int key);
void editorBackspace(void);
void editorDelChar(void);
void editorDeleteToEndOfLine(void);
void editorYankLine(int n);
void editorPasteLine(void);
void editorPasteString(void);
void editorYankString(void);
void editorMoveCursorEOL(void);
void editorMoveCursorBOL(void);
void editorMoveBeginningWord(void);
void editorMoveEndWord(void); 
void editorMoveEndWord2(void); //not 'e' but just moves to end of word even if on last letter
void editorMoveNextWord(void);
void editorMarkupLink(void);
void getWordUnderCursor(void);
void editorFindNextWord(void);
void editorChangeCase(void);
void editorRestoreSnapshot(void); 
void editorCreateSnapshot(void); 
int get_filecol(void);
int get_filerow_by_line (int y);
int get_filerow(void);
int get_line_char_count (void); 
int get_screenline_from_filerow(int fr);

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

void disableRawMode(void) {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode(void) {
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

int editorReadKey(void) {
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
    return -1;
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;

    return 0;
  }
}

/*** row operations ***/

//fr is the row number of the row to insert
void editorInsertRow(int fr, char *s, size_t len) {

  /*E.row is a pointer to an array of erow structures
  The array of erows that E.row points to needs to have its memory enlarged when
  you add a row. Note that erow structues are just a size and a char pointer*/

  E.row = realloc(E.row, sizeof(erow) * (E.filerows + 1));

  /*
  memmove(dest, source, number of bytes to move?)
  moves the line at fr to fr+1 and all the other erow structs until the end
  when you insert into the last row E.filerows==fr then no memory is moved
  apparently ok if there is no E.row[fr+1] if number of bytes = 0
  so below we are moving the row structure currently at *fr* to x+1
  and all the rows below *fr* to a new location to make room at *fr*
  to create room for the line that we are inserting
  */

  memmove(&E.row[fr + 1], &E.row[fr], sizeof(erow) * (E.filerows - fr));

  // section below creates an erow struct for the new row
  E.row[fr].size = len;
  E.row[fr].chars = malloc(len + 1);
  memcpy(E.row[fr].chars, s, len);
  E.row[fr].chars[len] = '\0'; //each line is made into a c-string (maybe for searching)
  E.filerows++;
  E.dirty++;
}

void editorFreeRow(erow *row) {
  free(row->chars);
}

void editorDelRow(int fr) {
  //editorSetMessage("Row to delete = %d; E.filerows = %d", fr, E.filerows); 
  if (E.filerows == 0) return; // some calls may duplicate this guard
  int fc = get_filecol();
  editorFreeRow(&E.row[fr]); 
  memmove(&E.row[fr], &E.row[fr + 1], sizeof(erow) * (E.filerows - fr - 1));
  E.filerows--; 
  if (E.filerows == 0) {
    E.row = NULL;
    E.cy = 0;
    E.cx = 0;
  } else if (E.cy > 0) {
    int lines = fc/E.screencols;
    E.cy = E.cy - lines;
    if (fr == E.filerows) E.cy--;
  }
  E.dirty++;
  //editorSetMessage("Row deleted = %d; E.filerows after deletion = %d E.cx = %d E.row[fr].size = %d", fr, E.filerows, E.cx, E.row[fr].size); 
}
// only used by editorBackspace
void editorRowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  E.dirty++;
}

/* not in use right now
void editorRowDelChar(erow *row, int fr) {
  if (fr < 0 || fr >= row->size) return;
  // is there any reason to realloc for one character?
  // row->chars = realloc(row->chars, row->size -1); 
  //have to realloc when adding but I guess no need to realloc for one character
  memmove(&row->chars[fr], &row->chars[fr + 1], row->size - fr);
  row->size--;
  E.dirty++;
}
*/

/*** editor operations ***/
void editorInsertChar(int c) {

  // E.cy == E.filerows == 0 when you start program or delete all lines
  if ( E.filerows == 0 ) {
    editorInsertRow(0, "", 0); //editorInsertRow will insert '\0'
  }

  erow *row = &E.row[get_filerow()];
  int fc = get_filecol();


  //if (E.cx < 0 || E.cx > row->size) E.cx = row->size; //can either of these be true? ie is check necessary?
  row->chars = realloc(row->chars, row->size + 1); //******* was size + 2

  /* moving all the chars fr the current x cursor position on char
     farther down the char string to make room for the new character
     Maybe a clue from editorInsertRow - it's memmove is below
     memmove(&E.row[fr + 1], &E.row[fr], sizeof(erow) * (E.filerows - fr));
  */

  memmove(&row->chars[fc + 1], &row->chars[fc], row->size - fc); //****was E.cx + 1

  row->size++;
  row->chars[fc] = c;
  E.dirty++;

  if (E.cx >= E.screencols) {
    E.cy++; 
    E.cx = 0;
  }
  E.cx++;
}

/* uses VLA */
void editorInsertNewline(int direction) {
  /* note this func does position cursor*/
  if (E.filerows == 0) {
    editorInsertRow(0, "", 0);
    return;
  }

  if (get_filerow() == 0 && direction == 0) {
    editorInsertRow(0, "", 0);
    E.cx = 0;
    E.cy = 0;
    return;
  }
    
  erow *row = &E.row[get_filerow()];
  int i;
  if (E.cx == 0 || E.cx == row->size) {
    if (E.smartindent) i = editorIndentAmount(get_filerow());
    else i = 0;
    char spaces[i + 1]; //VLA
    for (int j=0; j<i; j++) {
      spaces[j] = ' ';
    }
    spaces[i] = '\0';
    int fr = get_filerow();
    int y = E.cy;
    editorInsertRow(get_filerow()+direction, spaces, i);
    if (direction) {
      for (;;) {
        if (get_filerow_by_line(y) > fr) break;   
        y++;
      }
    }
    else {

      for (;;) {
        if (get_filerow_by_line(y) < fr) break;   
        y--;
      }
    }
    E.cy = y;
    if (direction == 0) E.cy++;
    //if (E.cy == E.screenrows) {
    //  E.rowoff++;
    //  E.cy--;
   // }
    E.cx = i;
  }
  else {
    editorInsertRow(get_filerow() + 1, &row->chars[get_filecol()], row->size - get_filecol());
    row = &E.row[get_filerow()];
    row->size = get_filecol();
    row->chars[row->size] = '\0';
    if (E.smartindent) i = editorIndentAmount(E.cy);
    else i = 0;

    E.cy++;
    //if (E.cy == E.screenrows) {
    //  E.rowoff++;
    //  E.cy--;
    //}

    E.cx = 0;
    for (;;){
      if (row->chars[0] != ' ') break;
      editorDelChar();
    }

  for ( int j=0; j < i; j++ ) editorInsertChar(' ');
  E.cx = i;
  }
}

void editorDelChar(void) {
  erow *row = &E.row[get_filerow()];

  /* row size = 1 means there is 1 char; size 0 means 0 chars */
  /* Note that row->size does not count the terminating '\0' char*/
  // note below order important because row->size undefined if E.filerows = 0 because E.row is NULL
  if (E.filerows == 0 || row->size == 0 ) return; 

  memmove(&row->chars[get_filecol()], &row->chars[get_filecol() + 1], row->size - get_filecol());
  row->size--;

  if (E.filerows == 1 && row->size == 0) {
    E.filerows = 0;
    free(E.row);
    //editorFreeRow(&E.row[fr]);
    E.row = NULL;
  }
  else if (E.cx == row->size && E.cx) E.cx = row->size - 1;  // not sure what to do about this

  E.dirty++;
}

void editorBackspace(void) {
  if (E.cx == 0 && E.cy == 0) return;
  int fc = get_filecol();
  int fr = get_filerow();
  erow *row = &E.row[fr];

  if (E.cx > 0) {
    //memmove(dest, source, number of bytes to move?)
    memmove(&row->chars[fc - 1], &row->chars[fc], row->size - fc + 1);
    row->size--;
    if (E.cx == 1 && row->size/E.screencols && fc > row->size) E.continuation = 1; //right now only backspace in multi-line
    E.cx--;
  } else { //else E.cx == 0 and could be multiline
    if (fc > 0) { //this means it's a multiline row and we're not at the top
      memmove(&row->chars[fc - 1], &row->chars[fc], row->size - fc + 1);
      row->size--;
      E.cx = E.screencols - 1;
      E.cy--;
      E.continuation = 0;
    } else {// this means we're at fc == 0 so we're in the first filecolumn
      E.cx = (E.row[fr - 1].size/E.screencols) ? E.screencols : E.row[fr - 1].size ;
      //if (E.cx < 0) E.cx = 0; //don't think this guard is necessary but we'll see
      editorRowAppendString(&E.row[fr - 1], row->chars, row->size); //only use of this function
      editorFreeRow(&E.row[fr]);
      memmove(&E.row[fr], &E.row[fr + 1], sizeof(erow) * (E.filerows - fr - 1));
      E.filerows--;
      E.cy--;
    }
  }
  E.dirty++;
}

/*** file i/o ***/

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  for (j = 0; j < E.filerows; j++)
    totlen += E.row[j].size + 1;
  *buflen = totlen;

  char *buf = malloc(totlen);
  char *p = buf;
  for (j = 0; j < E.filerows; j++) {
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
    editorInsertRow(E.filerows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

void editorSave(void) {
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
/* cursor can be move negative or beyond screen lines and also in wrong x and
this function deals with that */
void editorScroll(void) {
  int lines =  E.row[get_filerow()].size/E.screencols + 1;
  if (E.row[get_filerow()].size%E.screencols == 0) lines--;
  //if (E.cy >= E.screenrows) {
  if (E.cy + lines - 1 >= E.screenrows) {
    int first_row_lines = E.row[get_filerow_by_line(0)].size/E.screencols + 1; //****
    if (E.row[get_filerow_by_line(0)].size && E.row[get_filerow_by_line(0)].size%E.screencols == 0) first_row_lines--;
    int lines =  E.row[get_filerow()].size/E.screencols + 1;
    if (E.row[get_filerow()].size%E.screencols == 0) lines--;
    int delta = E.cy + lines - E.screenrows; //////
    delta = (delta > first_row_lines) ? delta : first_row_lines; //
    E.rowoff += delta;
    E.cy-=delta;
  }
  if (E.cy < 0) {
     E.rowoff+=E.cy;
     E.cy = 0;
  }

  /*if (E.cy < E.rowoff) {
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
  }*/
}
// "drawing" rows really means updating the ab buffer
// filerow conceptually is the row/column of the written to file text
// NOTE: when you can't display a whole file line in a multiline you go to the next file line: not implemented yet!!
void editorDrawRows(struct abuf *ab) {
  int y = 0;
  int len, n;
  //int filerow = 0;
  int filerow = get_filerow_by_line(0); //thought is find the first row given E.rowoff

  // if not displaying the 0th row of the 0th filerow than increment one filerow - this is what vim does
  // if (get_screenline_from_filerow != 0) filerow++; ? necessary ******************************

  for (;;){
    if (y >= E.screenrows) break; //somehow making this >= made all the difference
    if (filerow > E.filerows - 1) { 
      //if (y >= E.screenrows) break; //somehow making this >= made all the difference

      //drawing '~' below: first escape is red and second erases rest of line
      //may not be worth this if else to not draw ~ in first row
      //and probably there is a better way to do it
      if (y) abAppend(ab, "\x1b[31m~~\x1b[K", 10); 
      else abAppend(ab, "\x1b[K", 3); 
      abAppend(ab, "\r\n", 2);
      y++;

    } else {

      int lines = E.row[filerow].size/E.screencols;
      if (E.row[filerow].size%E.screencols) lines++;
      if (lines == 0) lines = 1;
      if ((y + lines) > E.screenrows) {
          for (n=0; n < (E.screenrows - y);n++) {
            abAppend(ab, "@", 2);
            abAppend(ab, "\x1b[K", 3); 
            abAppend(ab, "\r\n", 2); ///////////////////////////////////////////
          }
      break;
      }      

      for (n=0; n<lines;n++) {
        y++;
        int start = n*E.screencols;
        if ((E.row[filerow].size - n*E.screencols) > E.screencols) len = E.screencols;
        else len = E.row[filerow].size - n*E.screencols;

        if (E.mode == 3 && filerow >= E.highlight[0] && filerow <= E.highlight[1]) {
            abAppend(ab, "\x1b[48;5;242m", 11);
            abAppend(ab, &E.row[filerow].chars[start], len);
            abAppend(ab, "\x1b[0m", 4); //slz return background to normal
        
        } else if (E.mode == 4 && filerow == get_filerow()) {
            //if ((E.highlight[0] > start) && (E.highlight[0] < start + len)) {
            if ((E.highlight[0] >= start) && (E.highlight[0] < start + len)) {
            abAppend(ab, &E.row[filerow].chars[start], E.highlight[0] - start);
            abAppend(ab, "\x1b[48;5;242m", 11);
            abAppend(ab, &E.row[filerow].chars[E.highlight[0]], E.highlight[1]
                                                - E.highlight[0]);
            abAppend(ab, "\x1b[0m", 4); //slz return background to normal
            abAppend(ab, &E.row[filerow].chars[E.highlight[1]], start + len - E.highlight[1]);
            } else abAppend(ab, &E.row[filerow].chars[start], len);

        
        } else abAppend(ab, &E.row[filerow].chars[start], len);
    
      //"\x1b[K" erases the part of the line to the right of the cursor in case the
      // new line i shorter than the old

      abAppend(ab, "\x1b[K", 3); 
      abAppend(ab, "\r\n", 2); ///////////////////////////////////////////
      abAppend(ab, "\x1b[0m", 4); //slz return background to normal
      }

      filerow++;
    }
    //abAppend(ab, "\r\n", 2);
   abAppend(ab, "\x1b[0m", 4); //slz return background to normal
  }
}

//status bar has inverted colors
void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4); //switches to inverted colors
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
    E.filename ? E.filename : "[No Name]", E.filerows,
    E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "Status bar %d/%d",
    E.cy + 1, E.filerows);
  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);
  
  /* add spaces until you just have enough room
     left to print the status message  */

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
  //if (msglen && time(NULL) - E.statusmsg_time < 1000) //time
    abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen(void) {
  editorScroll(); ////////////////////////

  /*  struct abuf {
      char *b;
      int len;
    };*/
  if (E.row)
    editorSetMessage("length = %d, E.cx = %d, E.cy = %d, filerow = %d, filecol = %d, size = %d, E.filerows = %d, E.rowoff = %d, 0th = %d", get_line_char_count(), E.cx, E.cy, get_filerow(), get_filecol(), E.row[get_filerow()].size, E.filerows, E.rowoff, get_filerow_by_line(0)); 
  else
    editorSetMessage("E.row is NULL, E.cx = %d, E.cy = %d,  E.filerows = %d, E.rowoff = %d", E.cx, E.cy, E.filerows, E.rowoff); 

  struct abuf ab = ABUF_INIT; //abuf *b = NULL and int len = 0

  abAppend(&ab, "\x1b[?25l", 6); //hides the cursor
  abAppend(&ab, "\x1b[H", 3);  //sends the cursor home


  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  // the lines below position the cursor where it should go
  if (E.mode != 2){
  char buf[32];
  //snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
  //                                          (E.cx - E.coloff) + 1);
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
  abAppend(&ab, buf, strlen(buf));
}
  abAppend(&ab, "\x1b[?25h", 6); //shows the cursor

  write(STDOUT_FILENO, ab.b, ab.len);

  abFree(&ab);
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
  //E.statusmsg_time = time(NULL);
}

void editorMoveCursor(int key) {

  int fr = get_filerow();
  int lines;
  erow *row = &E.row[fr];

  switch (key) {
    case ARROW_LEFT:
    case 'h':
      if (E.cx == 0 && get_filecol() > 0) {
        E.cx = E.screencols - 1;
        E.cy--;
      }
      else if (E.cx != 0) E.cx--; //do need to check for row?
      break;

    case ARROW_RIGHT:
    case 'l':
      ;
      int fc = get_filecol();
      int row_size = E.row[fr].size;
      int line_in_row = 1 + fc/E.screencols; //counting from one
      int total_lines = row_size/E.screencols;
      if (row_size%E.screencols) total_lines++;
      if (total_lines > line_in_row && E.cx >= E.screencols-1) {
        E.cy++;
        E.cx = 0;
      } else E.cx++;
      break;

    case ARROW_UP:
    case 'k':
      if (fr > 0) {
        lines = get_filecol()/E.screencols;
        int more_lines = E.row[fr - 1].size/E.screencols;
        if (E.row[fr - 1].size%E.screencols) more_lines++;
        if (more_lines == 0) more_lines = 1;
        E.cy = E.cy - lines - more_lines;
        if (0){
        //if (E.cy < 0) {
          E.rowoff+=E.cy;
          E.cy = 0;
        }
      }
      break;

    case ARROW_DOWN:
    case 'j':
      ;
      // note that we are counting the initial line of a row as the 0th line
      int line = get_filecol()/E.screencols;
      
      //the below is one less than the number of lines
      lines =  row->size/E.screencols;
      if (row->size && row->size%E.screencols == 0) lines--;

      if (fr < E.filerows - 1) {
        int increment = lines - line + 1;
        E.cy += increment; 
      } 
      break;
  }
  /* Below deals with moving cursor up and down from longer rows to shorter rows 
     row has to be calculated again because this is the new row you've landed on 
     Also deals with trying to move cursor to right beyond length of line.
     E.mode == 1 is insert mode in the code below*/

  int line_char_count = get_line_char_count(); 
  if (line_char_count == 0) E.cx = 0;
  else if (E.mode == 1) {
    if (E.cx >= line_char_count) E.cx = line_char_count;
    }
  else if (E.cx >= line_char_count) E.cx = line_char_count - 1;
}
// higher level editor function depends on editorReadKey()
void editorProcessKeypress(void) {
  static int quit_times = KILO_QUIT_TIMES;
  int i, start, end;

  /* editorReadKey brings back one processed character that handles
     escape sequences for things like navigation keys */

  int c = editorReadKey();

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
      if (E.cy < E.filerows)
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
        if (E.cy > E.filerows) E.cy = E.filerows;
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
      E.continuation = 0; // right now used by backspace in multi-line filerow
      if (E.cx > 0) E.cx--;
      // below - if the indent amount == size of line then it's all blanks
      int n = editorIndentAmount(get_filerow());
      if (n == E.row[get_filerow()].size) {
        E.cx = 0;
        for (int i = 0; i < n; i++) {
          editorDelChar();
        }
      }
      editorSetMessage("");
      return;

    default:
      editorCreateSnapshot();
      editorInsertChar(c);
      return;
 
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

      //if c = 48 => 0 then it falls through to 0 move to beginning of line
      if ( c != 48 ) { 
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
      //This probably needs to be generalized when a letter is a single char command
      //but can also appear in multi-character commands too
      if (E.command[0] == '\0') { 
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
      for (int i = 0; i < E.repeat; i++) editorChangeCase();
      E.command[0] = '\0';
      E.repeat = 0;
      return;

    case 'a':
      if (E.command[0] == '\0') { 
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
      if (E.command[0] == '\0') { 
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
      if (E.command[0] == '\0') { 
        editorMoveEndWord();
        E.command[0] = '\0';
        E.repeat = 0;
        return;
        }
      break;

    case '0':
      //E.cx = 0;
      editorMoveCursorBOL();
      E.command[0] = '\0';
      E.repeat = 0;
      return;

    case '$':
      if (E.command[0] == '\0') { 
        editorMoveCursorEOL();
        E.command[0] = '\0';
        E.repeat = 0;
        return;
      }
      break;

    case 'I':
      editorMoveCursorBOL();
      //E.cx = editorIndentAmount(E.cy);
      E.cx = editorIndentAmount(get_filerow());
      E.mode = 1;
      E.command[0] = '\0';
      E.repeat = 0;
      editorSetMessage("\x1b[1m-- INSERT --\x1b[0m");
      return;

    case 'o':
      editorCreateSnapshot();
      E.cx = 0; //editorInsertNewline needs E.cx set to zero for 'o' and 'O' before calling it
      editorInsertNewline(1);
      E.mode = 1;
      E.command[0] = '\0';
      E.repeat = 0;
      editorSetMessage("\x1b[1m-- INSERT --\x1b[0m");
      return;

    case 'O':
      editorCreateSnapshot();
      E.cx = 0;  //editorInsertNewline needs E.cx set to zero for 'o' and 'O' before calling it
      editorInsertNewline(0);
      E.mode = 1;
      E.command[0] = '\0';
      E.repeat = 0;
      editorSetMessage("\x1b[1m-- INSERT --\x1b[0m");
      return;

    case 'G':
      E.cx = 0;
      E.cy = get_screenline_from_filerow(E.filerows-1);
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
      E.highlight[0] = E.highlight[1] = get_filerow();
      editorSetMessage("\x1b[1m-- VISUAL LINE --\x1b[0m");
      return;

    case 'v':
      E.mode = 4;
      E.command[0] = '\0';
      E.repeat = 0;
      E.highlight[0] = E.highlight[1] = get_filecol();
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
      editorCreateSnapshot();
      for (int j = 0; j < E.repeat; j++) {
        start = E.cx;
        editorMoveEndWord2();
        end = E.cx;
        E.cx = start;
        for (int j = 0; j < end - start + 2; j++) editorDelChar();
      }
      E.command[0] = '\0';
      E.repeat = 0;
      return;

    case C_de:
      editorCreateSnapshot();
      start = E.cx;
      editorMoveEndWord(); //correct one to use to emulate vim
      end = E.cx;
      E.cx = start; 
      for (int j = 0; j < end - start + 1; j++) editorDelChar();
      E.cx = (start < E.row[E.cy].size) ? start : E.row[E.cy].size -1;
      E.command[0] = '\0';
      E.repeat = 0;
      return;

    case C_dd:
      ;
      int fr = get_filerow();
      if (E.filerows != 0) {
        //int r = E.filerows - E.cy;
        int r = E.filerows - fr;
        E.repeat = (r >= E.repeat) ? E.repeat : r ;
        editorCreateSnapshot();
        editorYankLine(E.repeat);
        for (int i = 0; i < E.repeat ; i++) editorDelRow(fr);
      }
      E.cx = 0;
      E.command[0] = '\0';
      E.repeat = 0;
      return;

    case C_d$:
      editorCreateSnapshot();
      editorDeleteToEndOfLine();
      if (E.filerows != 0) {
        int r = E.filerows - E.cy;
        E.repeat--;
        E.repeat = (r >= E.repeat) ? E.repeat : r ;
        //editorYankLine(E.repeat); //b/o 2 step won't really work right
        for (int i = 0; i < E.repeat ; i++) editorDelRow(E.cy);
      }
      E.command[0] = '\0';
      E.repeat = 0;
      return;

    //tested with repeat on one line
    case C_cw:
      editorCreateSnapshot();
      for (int j = 0; j < E.repeat; j++) {
        start = E.cx;
        editorMoveEndWord();
        end = E.cx;
        E.cx = start;
        for (int j = 0; j < end - start + 1; j++) editorDelChar();
      }
      E.command[0] = '\0';
      E.repeat = 0;
      E.mode = 1;
      editorSetMessage("\x1b[1m-- INSERT --\x1b[0m");
      return;

    //tested with repeat on one line
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

      else if (E.command[1] == 'x') {
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

      else if (E.command[1] == 'q') {
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
      E.highlight[1] = get_filerow();
      return;

    case 'x':
      if (E.filerows != 0) {
        editorCreateSnapshot();
        E.repeat = E.highlight[1] - E.highlight[0] + 1;
        E.cy = E.highlight[0]; // this isn't right because E.highlight[0] and [1] are now rows
        editorYankLine(E.repeat);

        for (int i = 0; i < E.repeat; i++) editorDelRow(E.highlight[0]);
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

 // visual mode
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
      E.highlight[1] = get_filecol();
      return;

    case 'x':
      editorCreateSnapshot();
      E.repeat = E.highlight[1] - E.highlight[0] + 1;
      E.cx = E.highlight[0]%E.screencols; //need to position E.cx
      editorYankString(); 

      for (int i = 0; i < E.repeat; i++) {
        editorDelChar();
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
      for (int i = 0; i < E.repeat; i++) {
        editorDelChar();
        editorInsertChar(c);
      }
      E.repeat = 0;
      E.command[0] = '\0';
      E.mode = 0;
  }
}

/*** slz additions ***/
int get_filerow(void) {
  int screenrow = -1;
  int n = 0;
  int linerows;
  int y = E.cy + E.rowoff; ////////
  //if (E.cy == 0) return 0;
  if (y == 0) return 0;
  for (;;) {
    linerows = E.row[n].size/E.screencols;
    if (E.row[n].size%E.screencols) linerows++;
    if (linerows == 0) linerows = 1;
    screenrow+= linerows;
    //if (screenrow >= E.cy) break;
    if (screenrow >= y) break;
    n++;
  }
  // right now this is necesssary for backspacing in a multiline filerow
  // no longer seems necessary for insertchar
  if (E.continuation) n--;
  return n;
}

int get_filerow_by_line (int y){
  int screenrow = -1;
  int n = 0;
  int linerows;
  y+= E.rowoff;
  if (y == 0) return 0;
  for (;;) {
    linerows = E.row[n].size/E.screencols;
    if (E.row[n].size%E.screencols) linerows++;
    if (linerows == 0) linerows = 1; // a row with no characters still takes up a line may also deal with last line
    screenrow+= linerows;
    if (screenrow >= y) break;
    n++;
  }
  return n;
}

// returns E.cy for a given filerow - right now just used for 'G'
int get_screenline_from_filerow (int fr){
  int screenline = -1;
  int n = 0;
  int rowlines;
  if (fr == 0) return 0;
  for (n=0;n < fr + 1;n++) {
    rowlines = E.row[n].size/E.screencols;
    if (E.row[n].size%E.screencols) rowlines++;
    if (rowlines == 0) rowlines = 1; // a row with no characters still takes up a line may also deal with last line
    screenline+= rowlines;
  }
  return screenline - E.rowoff;

}

int get_filecol(void) {
  int n = 0;
  int y = E.cy;
  int fr = get_filerow();
  for (;;) {
    if (y == 0) break;
    y--;
    if (get_filerow_by_line(y) < fr) break;
    n++;
  }

  int col = E.cx + n*E.screencols; 
  return col;
}

int get_line_char_count(void) {

  int fc = get_filecol();
  int fr = get_filerow();
  int row_size = E.row[fr].size;
  if (row_size <= E.screencols) return row_size;
  int line_in_row = 1 + fc/E.screencols; //counting from one
  int total_lines = row_size/E.screencols;
  if (row_size%E.screencols) total_lines++;
  if (line_in_row == total_lines) return row_size%E.screencols;
  else return E.screencols;
}
void editorCreateSnapshot(void) {
  if ( E.filerows == 0 ) return; //don't create snapshot if there is no text
  for (int j = 0 ; j < E.prev_filerows ; j++ ) {
    free(E.prev_row[j].chars);
  }
  E.prev_row = realloc(E.prev_row, sizeof(erow) * E.filerows );
  for ( int i = 0 ; i < E.filerows ; i++ ) {
    int len = E.row[i].size;
    E.prev_row[i].chars = malloc(len + 1);
    E.prev_row[i].size = len;
    memcpy(E.prev_row[i].chars, E.row[i].chars, len);
    E.prev_row[i].chars[len] = '\0';
  }
  E.prev_filerows = E.filerows;
}

void editorRestoreSnapshot(void) {
  for (int j = 0 ; j < E.filerows ; j++ ) {
    free(E.row[j].chars);
  } 
  E.row = realloc(E.row, sizeof(erow) * E.prev_filerows );
  for (int i = 0 ; i < E.prev_filerows ; i++ ) {
    int len = E.prev_row[i].size;
    E.row[i].chars = malloc(len + 1);
    E.row[i].size = len;
    memcpy(E.row[i].chars, E.prev_row[i].chars, len);
    E.row[i].chars[len] = '\0';
  }
  E.filerows = E.prev_filerows;
}

void editorChangeCase(void) {
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

  int fr = get_filerow();
  for (int i=0; i < n; i++) {
    int len = E.row[fr + i].size;
    line_buffer[i] = malloc(len + 1);
    memcpy(line_buffer[i], E.row[fr + i].chars, len);
    line_buffer[i][len] = '\0';
  }
  // set string_buffer to "" to signal should paste line and not chars
  string_buffer[0] = '\0';
}

void editorYankString(void) {
  int n,x;
  int fr = get_filerow();
  erow *row = &E.row[fr];
  for (x = E.highlight[0], n = 0; x < E.highlight[1]+1; x++, n++) {
      string_buffer[n] = row->chars[x];
  }

  string_buffer[n] = '\0';
}

void editorPasteString(void) {
  if (E.cy == E.filerows) {
    editorInsertRow(E.filerows, "", 0); //editorInsertRow will also insert another '\0'
  }
  int fr = get_filerow();
  int fc = get_filecol();

  erow *row = &E.row[fr];
  //if (E.cx < 0 || E.cx > row->size) E.cx = row->size; 10-29-2018 ? is this necessary - not sure
  int len = strlen(string_buffer);
  row->chars = realloc(row->chars, row->size + len); 

  /* moving all the chars at the current x cursor position on char
     farther down the char string to make room for the new character
     Maybe a clue from editorInsertRow - it's memmove is below
     memmove(&E.row[fr + 1], &E.row[fr], sizeof(erow) * (E.filerows - fr));
  */

  memmove(&row->chars[fc + len], &row->chars[fc], row->size - fc); //****was E.cx + 1

  for (int i = 0; i < len; i++) {
    row->size++;
    row->chars[fc] = string_buffer[i];
    fc++;
  }
  E.cx = fc%E.screencols; //this can't work in all circumstances - might have to move E.cy too
  E.dirty++;
}

void editorPasteLine(void){
  if ( E.filerows == 0 ) editorInsertRow(0, "", 0);
  int fr = get_filerow();
  for (int i=0; i < 10; i++) {
    if (line_buffer[i] == NULL) break;

    int len = strlen(line_buffer[i]);
    fr++;
    editorInsertRow(fr, line_buffer[i], len);
    //need to set E.cy - need general fr to E.cy function 10-28-2018
  }
}

void editorIndentRow(void) {
  int fr = get_filerow();
  erow *row = &E.row[fr];
  if (row->size == 0) return;
  //E.cx = 0;
  E.cx = editorIndentAmount(fr);
  for (int i = 0; i < E.indent; i++) editorInsertChar(' ');
  E.dirty++;
}

void editorUnIndentRow(void) {
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

int editorIndentAmount(int fr) {
  int i;
  erow *row = &E.row[fr];
  if ( !row || row->size == 0 ) return 0; //row is NULL if the row has been deleted or opening app

  for ( i = 0; i < row->size; i++) {
    if (row->chars[i] != ' ') break;}

  return i;
}

void editorDelWord(void) {
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

void editorDeleteToEndOfLine(void) {
  erow *row = &E.row[E.cy];
  row->size = E.cx;
  //Arguably you don't have to reallocate when you reduce the length of chars
  row->chars = realloc(row->chars, E.cx + 1); //added 10042018 - before wasn't reallocating memory
  row->chars[E.cx] = '\0';
  }

void editorMoveCursorBOL(void) {
  E.cx = 0;
  int fr = get_filerow();
  if (fr == 0) {
    E.cy = 0;
    return;
  }
  int y = E.cy - 1;
  for (;;) {
    if (get_filerow_by_line(y) != fr) break;
    y--;
  }
  E.cy = y + 1;
}

void editorMoveCursorEOL(void) {
 // possibly should turn line in row and total lines into a function but use does vary a little so maybe not 
  int fc = get_filecol();
  int fr = get_filerow();
  int row_size = E.row[fr].size;
  int line_in_row = 1 + fc/E.screencols; //counting from one
  int total_lines = row_size/E.screencols;
  if (row_size%E.screencols) total_lines++;
  if (total_lines > line_in_row) E.cy = E.cy + total_lines - line_in_row;
  int char_in_line = get_line_char_count();
  if (char_in_line == 0) E.cx = 0; 
  else E.cx = char_in_line - 1;
}

// not same as 'e' but moves to end of word or stays put if already on end of word
// used by dw
void editorMoveEndWord2() {
  int j;
  int fr = get_filerow();
  int fc = get_filecol();
  erow row = E.row[fr];

  for (j = fc + 1; j < row.size ; j++) {
    if (row.chars[j] < 48) break;
  }

  fc = j - 1;
  E.cx = fc%E.screencols;
}

// used by 'w'
void editorMoveNextWord(void) {
// doesn't handle multiple white space characters at EOL
  int j;
  int fr = get_filerow();
  int fc = get_filecol();
  int line_in_row = fc/E.screencols; //counting from zero
  erow row = E.row[fr];

  if (row.chars[fc] < 48) j = fc;
  else {
    for (j = fc + 1; j < row.size; j++) { 
      if (row.chars[j] < 48) break;
    }
  } 
  if (j >= row.size - 1) { // at end of line was ==

    if (fr == E.filerows - 1) return; // no more rows
    
    for (;;) {
      fr++;
      E.cy++;
      row = E.row[fr];
      if (row.size == 0 && fr == E.filerows - 1) return;
      if (row.size) break;
      }

    line_in_row = 0;
    E.cx = 0;
    fc = 0;
    if (row.chars[0] >= 48) return;  //Since we went to a new row it must be beginning of a word if char in 0th position
  
  } else fc = j - 1;
  
  for (j = fc + 1; j < row.size ; j++) { //+1
    if (row.chars[j] > 48) break;
  }
  fc = j;
  E.cx = fc%E.screencols;
  E.cy+=fc/E.screencols - line_in_row;
}

void editorMoveBeginningWord(void) {
  int fr = get_filerow();
  int fc = get_filecol();
  erow *row = &E.row[fr];
  int line_in_row = fc/E.screencols; //counting from zero
  if (fc == 0){ 
    if (fr == 0) return;
      for (;;) {
        fr--;
        E.cy--;
        row = &E.row[fr];
        if (row->size == 0 && fr==0) return;
        if (row->size) break;
      }
    fc = row->size - 1;
    line_in_row = fc/E.screencols;
  }

  int j = fc;
  for (;;) {
    if (row->chars[j - 1] < 48) j--;
    else break;
    if (j == 0) return; 
  }

  int i;
  for (i = j - 1; i > -1; i--){
    if (row->chars[i] < 48) break;
  }
  fc = i + 1;

  E.cx = fc%E.screencols;
  E.cy = E.cy - line_in_row + fc/E.screencols;
}

void editorMoveEndWord(void) {
// doesn't handle whitespace at the end of a line
  int fr = get_filerow();
  int fc = get_filecol();
  int line_in_row = fc/E.screencols; //counting from zero
  erow *row = &E.row[fr];
  int j;

  if (fc >= row->size - 1) {
    if (fr == E.filerows - 1) return; // no more rows
    
    for (;;) {
      fr++;
      E.cy++;
      row = &E.row[fr];
      if (row->size == 0 && fr == E.filerows - 1) return;
      if (row->size) break;
      }
    line_in_row = 0;
    //E.cx = 0;
    fc = 0;
  }
  j = fc + 1;
  if (row->chars[j] < 48) {
 
    for (j = fc + 1; j < row->size ; j++) {
      if (row->chars[j] > 48) break;
    }
  }
  //for (j = E.cx + 1; j < row->size ; j++) {
  for (j++; j < row->size ; j++) {
    if (row->chars[j] < 48) break;
  }

  fc = j - 1;
  E.cx = fc%E.screencols;
  E.cy+=fc/E.screencols - line_in_row;
}

void editorDecorateWord(int c) {
  int fr = get_filerow();
  int fc = get_filecol();
  erow *row = &E.row[fr];
  char cc;
  if (row->chars[fc] < 48) return;

  int i, j;

  /*Note to catch ` would have to be row->chars[i] < 48 || row-chars[i] == 96 - may not be worth it*/

  for (i = fc - 1; i > -1; i--){
    if (row->chars[i] < 48) break;
  }

  for (j = fc + 1; j < row->size ; j++) {
    if (row->chars[j] < 48) break;
  }
  
  if (row->chars[i] != '*' && row->chars[i] != '`'){
    cc = (c == CTRL_KEY('b') || c ==CTRL_KEY('i')) ? '*' : '`';
    E.cx = i%E.screencols + 1;
    editorInsertChar(cc);
    E.cx = j%E.screencols+ 1;
    editorInsertChar(cc);

    if (c == CTRL_KEY('b')) {
      E.cx = i%E.screencols + 1;
      editorInsertChar('*');
      E.cx = j%E.screencols + 2;
      editorInsertChar('*');
    }
  } else {
    E.cx = i%E.screencols; 
    editorDelChar();
    E.cx = j%E.screencols-1;
    editorDelChar();

    if (c == CTRL_KEY('b')) {
      E.cx = i%E.screencols - 1;
      editorDelChar();
      E.cx = j%E.screencols - 2;
      editorDelChar();
    }
  }
}

void editorDecorateVisual(int c) {
 // E.cx = E.highlight[0];
    E.cx = E.highlight[0]%E.screencols;
  if (c == CTRL_KEY('b')) {
    editorInsertChar('*');
    editorInsertChar('*');
    //E.cx = E.highlight[1]+3;
    E.cx = (E.highlight[1]+3)%E.screencols;
    editorInsertChar('*');
    editorInsertChar('*');
  } else {
    char cc = (c ==CTRL_KEY('i')) ? '*' : '`';
    editorInsertChar(cc);
    //E.cx = E.highlight[1]+2;
    E.cx = (E.highlight[1]+2)%E.screencols;
    editorInsertChar(cc);
  }
}

void getWordUnderCursor(void){
  int fr = get_filerow();
  int fc = get_filecol();
  erow *row = &E.row[fr];
  if (row->chars[fc] < 48) return;

  int i,j,n,x;

  for (i = fc - 1; i > -1; i--){
    if (row->chars[i] < 48) break;
  }

  for (j = fc + 1; j < row->size ; j++) {
    if (row->chars[j] < 48) break;
  }

  for (x = i + 1, n = 0; x < j; x++, n++) {
      search_string[n] = row->chars[x];
  }

  search_string[n] = '\0';
  editorSetMessage("word under cursor: <%s>", search_string); 

}

void editorFindNextWord(void) {
  int y, x;
  char *z;
  int fc = get_filecol();
  int fr = get_filerow();
  y = fr;
  x = fc + 1;
  erow *row;
 
  /*n counter so we can exit for loop if there are  no matches for command 'n'*/
  for ( int n=0; n < E.filerows; n++ ) {
    row = &E.row[y];
    z = strstr(&(row->chars[x]), search_string);
    if ( z != NULL ) {
      break;
    }
    y++;
    x = 0;
    if ( y == E.filerows ) y = 0;
  }
  fc = z - row->chars;
  E.cx = fc%E.screencols;
  int line_in_row = 1 + fc/E.screencols; //counting from one
  int total_lines = row->size/E.screencols;
  if (row->size%E.screencols) total_lines++;
  E.cy = get_screenline_from_filerow(y) - (total_lines - line_in_row); //that is screen line of last row in multi-row

    editorSetMessage("x = %d; y = %d", x, y); 
}

void editorMarkupLink(void) {
  int y, numrows, j, n, p;
  char *z;
  char *http = "http";
  char *bracket_http = "[http";
  numrows = E.filerows;
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
      E.cy = E.filerows - 1; //check why need - 1 otherwise seg faults
      E.cx = 0;
      editorInsertNewline(1);
      }

    editorInsertRow(E.filerows, zz, len); 
    free(zz);
    E.cx = 0;
    E.cy = E.filerows - 1;
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

void getcharundercursor(void) {
  erow *row = &E.row[E.cy];
  char d = row->chars[E.cx];
  editorSetMessage("character under cursor at position %d of %d: %c", E.cx, row->size, d); 
}

/*** slz testing stuff (above) ***/

/*** init ***/

void initEditor(void) {
  E.cx = 0; //cursor x position
  E.cy = 0; //cursor y position
  E.rowoff = 0;  //row the user is currently scrolled to  
  E.coloff = 0;  //col the user is currently scrolled to  
  E.filerows = 0; //number of rows (lines) of text delineated by a return
  E.row = NULL; //pointer to the erow structure 'array'
  E.prev_filerows = 0; //number of rows of text in snapshot
  E.prev_row = NULL; //prev_row is pointer to snapshot for undoing
  E.dirty = 0; //has filed changed since last save
  E.filename = NULL;
  E.statusmsg[0] = '\0'; //very bottom of screen; ex. -- INSERT --
  //E.statusmsg_time = 0;
  E.highlight[0] = E.highlight[1] = -1;
  E.mode = 0; //0=normal; 1=insert; 2=command line; 3=visual line; 4=visual; 5='r' 
  E.command[0] = '\0';
  E.repeat = 0; //number of times to repeat commands like x,s,yy also used for visual line mode x,y
  E.indent = 4;
  E.smartindent = 1; //CTRL-z toggles - don't want on what pasting from outside source
  E.continuation = 0; //circumstance when a line wraps

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
  E.screenrows -= 2;
  E.screencols -=2;
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
    editorInsertRow(0, "Hello, Steve!", 13); 
    E.cx = E.row[0].size; //put cursor at end of line
    editorInsertNewline(1); 
    editorInsertRow(E.filerows, "http://www.webmd.com", 20); //testing url markup
    editorInsertRow(E.filerows, "The quick brown fox jumps over the lazy dog", 43); 
    E.cx = E.row[0].size - 1; //put cursor at end of line
    E.cy = 0;
  }

  //editorSetMessage("HELP: Ctrl-S = save | Ctrl-Q = quit"); //slz commented this out
  editorSetMessage("rows: %d  cols: %d", E.screenrows, E.screencols); //for display screen dimens

  while (1) {
    editorRefreshScreen(); 
    editorProcessKeypress();
/*
    if (E.row)
      editorSetMessage("length = %d, E.cx = %d, E.cy = %d, filerow = %d, filecol = %d, size = %d, E.filerows = %d, E.rowoff = %d, 0th = %d", get_line_char_count(), E.cx, E.cy, get_filerow(), get_filecol(), E.row[get_filerow()].size, E.filerows, E.rowoff, get_filerow_by_line(0)); 
    else
      editorSetMessage("E.row is NULL, E.cx = %d, E.cy = %d,  E.filerows = %d, E.rowoff = %d", E.cx, E.cy, E.filerows, E.rowoff); 
*/
    
  }
  return 0;
}
