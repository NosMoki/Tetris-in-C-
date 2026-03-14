
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#ifdef _WIN32
#  include <windows.h>
#  include <conio.h>
#else
#  include <unistd.h>
#  include <termios.h>
#  include <sys/time.h>
#endif


// TERMINAL

#ifdef _WIN32
static void term_restore(){ printf("\x1b[?25h\x1b[0m"); fflush(stdout); }
static void term_init(){
    HANDLE h=GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode=0; GetConsoleMode(h,&mode);
    SetConsoleMode(h,mode|ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    atexit(term_restore);
    printf("\x1b[?25l"); fflush(stdout);
}
static void sleep_ms(int ms){ Sleep(ms); }
#else
static termios orig_termios;
static void term_restore(){
    tcsetattr(STDIN_FILENO,TCSANOW,&orig_termios);
    printf("\x1b[?25h\x1b[0m"); fflush(stdout);
}
static void term_init(){
    tcgetattr(STDIN_FILENO,&orig_termios);
    atexit(term_restore);
    termios raw=orig_termios;
    raw.c_lflag &= ~(ICANON|ECHO);
    raw.c_cc[VMIN]=0; raw.c_cc[VTIME]=0;
    tcsetattr(STDIN_FILENO,TCSANOW,&raw);
    printf("\x1b[?25l"); fflush(stdout);
}
static void sleep_ms(int ms){ usleep(ms*1000); }
#endif


// BUFFERED OUTPUT  

static char outbuf[1<<17];
static int  outpos=0;

static void ob_flush(){
    fwrite(outbuf,1,outpos,stdout);
    fflush(stdout);
    outpos=0;
}
static void ob_puts(const char* s){
    int n=(int)strlen(s);
    if(outpos+n>=(int)sizeof(outbuf)) ob_flush();
    memcpy(outbuf+outpos,s,n); outpos+=n;
}
static void ob_printf(const char* fmt,...){
    char tmp[256]; va_list ap; va_start(ap,fmt);
    int n=vsnprintf(tmp,sizeof(tmp),fmt,ap); va_end(ap);
    if(outpos+n>=(int)sizeof(outbuf)) ob_flush();
    memcpy(outbuf+outpos,tmp,n); outpos+=n;
}

// cursor + reset via buffer
static void mv (int r,int c){ ob_printf("\x1b[%d;%dH",r+1,c+1); }
static void rst()            { ob_puts("\x1b[0m"); }

// DOUBLE-BUFFER 

static const int SCOLS=20, SROWS=28;

struct Slot {
    char    ch[2];
    uint8_t fr,fg,fb;   // fg color
    uint8_t br,bg,bb;   // bg color
    bool    bld;
    bool operator==(const Slot& o) const {
        return ch[0]==o.ch[0]&&ch[1]==o.ch[1]&&
               fr==o.fr&&fg==o.fg&&fb==o.fb&&
               br==o.br&&bg==o.bg&&bb==o.bb&&bld==o.bld;
    }
};
static Slot scr[SROWS][SCOLS];   // desired
static Slot prv[SROWS][SCOLS];   // last sent

static void screen_init(){
    for(int r=0;r<SROWS;r++) for(int c=0;c<SCOLS;c++){
        scr[r][c]={' ',' ',70,65,110,12,10,22,false};
        // poison prev so first frame redraws everything
        prv[r][c]={0,0,0,0,0,0,0,0,false};
    }
}

static void screen_flush(){
    for(int r=0;r<SROWS;r++) for(int c=0;c<SCOLS;c++){
        Slot& s=scr[r][c];
        if(s==prv[r][c]) continue;
        prv[r][c]=s;
        mv(r,c*2);
        ob_printf("\x1b[%dm\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm%c%c",
            s.bld?1:22,
            s.fr,s.fg,s.fb,
            s.br,s.bg,s.bb,
            s.ch[0],s.ch[1]);
    }
    rst(); ob_flush();
}


static void sp(int row,int col,char c0,char c1,
               uint8_t fr,uint8_t fg2,uint8_t fb,
               uint8_t br,uint8_t bg2,uint8_t bb,
               bool bld=false){
    if(row<0||row>=SROWS||col<0||col>=SCOLS) return;
    Slot& s=scr[row][col];
    s.ch[0]=c0; s.ch[1]=c1;
    s.fr=fr; s.fg=fg2; s.fb=fb;
    s.br=br; s.bg=bg2; s.bb=bb;
    s.bld=bld;
}

// Write a string 
static void st(int row,int col,const char* str,
               uint8_t fr,uint8_t fg2,uint8_t fb,
               uint8_t br,uint8_t bg2,uint8_t bb,
               bool bld=false){
    int len=(int)strlen(str);
    for(int i=0;i<len;i+=2){
        char c1=str[i], c2=(i+1<len)?str[i+1]:' ';
        sp(row,col+i/2,c1,c2,fr,fg2,fb,br,bg2,bb,bld);
    }
}


// TIME + RNG

static uint64_t now_ms(){
#ifdef _WIN32
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    return ((((uint64_t)ft.dwHighDateTime<<32)|ft.dwLowDateTime)-116444736000000000ULL)/10000;
#else
    struct timeval tv; gettimeofday(&tv,nullptr);
    return (uint64_t)tv.tv_sec*1000+tv.tv_usec/1000;
#endif
}
static uint64_t rng_st=1;
static void rng_seed(){ rng_st=now_ms()^0xdeadbeefcafe1337ULL; if(!rng_st)rng_st=1; }
static int rng_n(int n){
    rng_st^=rng_st<<13; rng_st^=rng_st>>7; rng_st^=rng_st<<17;
    return (int)(rng_st%(uint64_t)n);
}


// INPUT

enum Key { K_NONE,K_LEFT,K_RIGHT,K_UP,K_DOWN,K_SPACE,K_QUIT };
static Key read_key(){
#ifdef _WIN32
    if(!_kbhit()) return K_NONE;
    int c=_getch();
    switch(c){
        case 'q':case 'Q': return K_QUIT;
        case ' ':          return K_SPACE;
        case 'a':case 'A': return K_LEFT;
        case 'd':case 'D': return K_RIGHT;
        case 'w':case 'W': return K_UP;
        case 's':case 'S': return K_DOWN;
        case 0xE0: c=_getch();
            if(c==75) return K_LEFT;  if(c==77) return K_RIGHT;
            if(c==72) return K_UP;    if(c==80) return K_DOWN;
    }
    return K_NONE;
#else
    uint8_t c;
    if(read(STDIN_FILENO,&c,1)!=1) return K_NONE;
    switch(c){
        case 'q':case 'Q': return K_QUIT;
        case ' ':          return K_SPACE;
        case 'a':case 'A': return K_LEFT;
        case 'd':case 'D': return K_RIGHT;
        case 'w':case 'W': return K_UP;
        case 's':case 'S': return K_DOWN;
        case 0x1b:{
            uint8_t b1,b2;
            if(read(STDIN_FILENO,&b1,1)!=1) return K_QUIT;
            if(b1=='['&&read(STDIN_FILENO,&b2,1)==1){
                if(b2=='A') return K_UP;
                if(b2=='B') return K_DOWN;
                if(b2=='C') return K_RIGHT;
                if(b2=='D') return K_LEFT;
            }
            return K_NONE;
        }
    }
    return K_NONE;
#endif
}


// TETROMINO DATA

static const int BW=10, BH=20;
static uint8_t board[BH][BW];

static const int8_t PIECES[7][4][4][2]={
    {{{0,0},{0,1},{0,2},{0,3}},{{0,2},{1,2},{2,2},{3,2}},{{2,0},{2,1},{2,2},{2,3}},{{0,1},{1,1},{2,1},{3,1}}}, // I
    {{{0,0},{0,1},{1,0},{1,1}},{{0,0},{0,1},{1,0},{1,1}},{{0,0},{0,1},{1,0},{1,1}},{{0,0},{0,1},{1,0},{1,1}}}, // O
    {{{0,0},{0,1},{0,2},{1,1}},{{0,1},{1,0},{1,1},{2,1}},{{1,0},{1,1},{1,2},{0,1}},{{0,0},{1,0},{2,0},{1,1}}}, // T
    {{{0,1},{0,2},{1,0},{1,1}},{{0,0},{1,0},{1,1},{2,1}},{{0,1},{0,2},{1,0},{1,1}},{{0,0},{1,0},{1,1},{2,1}}}, // S
    {{{0,0},{0,1},{1,1},{1,2}},{{0,1},{1,0},{1,1},{2,0}},{{0,0},{0,1},{1,1},{1,2}},{{0,1},{1,0},{1,1},{2,0}}}, // Z
    {{{0,0},{1,0},{1,1},{1,2}},{{0,0},{0,1},{1,0},{2,0}},{{0,0},{0,1},{0,2},{1,2}},{{0,1},{1,1},{2,0},{2,1}}}, // J
    {{{0,2},{1,0},{1,1},{1,2}},{{0,0},{1,0},{2,0},{2,1}},{{0,0},{0,1},{0,2},{1,0}},{{0,0},{0,1},{1,1},{2,1}}}, // L
};

struct RGB{ uint8_t r,g,b; };
static const RGB COLORS[7]={
    {0,200,220},{220,190,0},{180,0,230},
    {0,210,80},{230,40,40},{30,110,240},{240,120,0},
};


// GAME LOGIC

struct Game{ int piece,rot,px,py,next; uint32_t score,lines,level; bool over; };
static Game G;

static bool fits(int row,int col,int p,int r){
    for(int i=0;i<4;i++){
        int rr=row+PIECES[p][r][i][0], cc=col+PIECES[p][r][i][1];
        if(rr<0||rr>=BH||cc<0||cc>=BW||board[rr][cc]) return false;
    }
    return true;
}
static void lock_piece(){
    uint8_t col=(uint8_t)(G.piece+1);
    for(int i=0;i<4;i++){
        int r=G.py+PIECES[G.piece][G.rot][i][0];
        int c=G.px+PIECES[G.piece][G.rot][i][1];
        if(r>=0&&r<BH&&c>=0&&c<BW) board[r][c]=col;
    }
}
static void clear_lines(){
    uint32_t cleared=0; uint8_t nb[BH][BW]={}; int wr=BH-1;
    for(int r=BH-1;r>=0;r--){
        bool full=true;
        for(int c=0;c<BW;c++) if(!board[r][c]){full=false;break;}
        if(full) cleared++; else memcpy(nb[wr--],board[r],BW);
    }
    memcpy(board,nb,sizeof(board));
    G.lines+=cleared; G.level=G.lines/10+1;
    static const uint32_t pts[]={0,100,300,500,800};
    if(cleared<=4) G.score+=pts[cleared]*G.level;
}
static void spawn(){
    G.piece=G.next; G.next=rng_n(7);
    G.rot=0; G.px=3; G.py=0;
    if(!fits(0,3,G.piece,0)) G.over=true;
}
static int ghost_row(){ int g=G.py; while(fits(g+1,G.px,G.piece,G.rot))g++; return g; }
static uint64_t drop_ms(){ uint64_t m=800-(G.level>1?(G.level-1)*70:0); return m<50?50:m; }
static void g_left()   { if(fits(G.py,G.px-1,G.piece,G.rot))G.px--; }
static void g_right()  { if(fits(G.py,G.px+1,G.piece,G.rot))G.px++; }
static void g_rotate() {
    int nr=(G.rot+1)%4, kicks[]={0,-1,1,-2,2};
    for(int k:kicks) if(fits(G.py,G.px+k,G.piece,nr)){G.rot=nr;G.px+=k;return;}
}
static bool g_soft(){ if(fits(G.py+1,G.px,G.piece,G.rot)){G.py++;return true;} return false; }
static void g_hard(){ while(fits(G.py+1,G.px,G.piece,G.rot)){G.py++;G.score+=2;} }
static void g_grav(){ if(!g_soft()){lock_piece();clear_lines();spawn();} }


static const int
    B_ROW = 1,   // board inner top 
    B_COL = 1,   // board inner left 
    SB    = 12;  // sidebar 

// Colors used everywhere
static const uint8_t
    // void bg
    VR=12,VG=10,VB=22,
    // board empty cell bg
    ER=18,EG=15,EB=32,
    // sidebar bg
    SR=16,SG=14,SB2=28;


static void draw_chrome(){
    // Fill all with void
    for(int r=0;r<SROWS;r++)
        for(int c=0;c<SCOLS;c++)
            sp(r,c,' ',' ',60,55,100,VR,VG,VB);

    // Board empty cells
    for(int r=0;r<BH;r++)
        for(int c=0;c<BW;c++)
            sp(B_ROW+r,B_COL+c,'.',' ',30,26,55,ER,EG,EB);

    // Board border
    uint8_t br=70,bg2=65,bb=120;
    // top row
    for(int c=B_COL;c<B_COL+BW;c++) sp(0,c,'-','-',br,bg2,bb,VR,VG,VB);
    sp(0,B_COL-1,'+','-',br,bg2,bb,VR,VG,VB);
    sp(0,B_COL+BW,'-','+',br,bg2,bb,VR,VG,VB);
    // bottom row
    for(int c=B_COL;c<B_COL+BW;c++) sp(BH+1,c,'-','-',br,bg2,bb,VR,VG,VB);
    sp(BH+1,B_COL-1,'+','-',br,bg2,bb,VR,VG,VB);
    sp(BH+1,B_COL+BW,'-','+',br,bg2,bb,VR,VG,VB);
    // sides
    for(int r=1;r<=BH;r++){
        sp(r,B_COL-1,'|',' ',br,bg2,bb,VR,VG,VB);
        sp(r,B_COL+BW,'|',' ',br,bg2,bb,VR,VG,VB);
    }


    sp(0,4,'[',' ',0,200,200,VR,VG,VB,true);
    st(0,5,"TETRIS",0,210,210,VR,VG,VB,true);
    sp(0,8,']',' ',0,200,200,VR,VG,VB,true);

    
    for(int r=0;r<SROWS;r++)
        for(int c=SB;c<SB+8;c++)
            sp(r,c,' ',' ',60,55,100,SR,SG,SB2);

    
    auto panel=[](int row, const char* lbl,
                  uint8_t lr, uint8_t lg, uint8_t lb){
        
        for(int c=SB;c<SB+8;c++){
            sp(row,  c,'-','-',lr,lg,lb,SR,SG,SB2);
            sp(row+2,c,'-','-',lr,lg,lb,SR,SG,SB2);
        }
        sp(row,  SB,'+','-',lr,lg,lb,SR,SG,SB2);
        sp(row,  SB+7,'-','+',lr,lg,lb,SR,SG,SB2);
        sp(row+2,SB,'+','-',lr,lg,lb,SR,SG,SB2);
        sp(row+2,SB+7,'-','+',lr,lg,lb,SR,SG,SB2);
        
        sp(row+1,SB,  '|',' ',lr,lg,lb,SR,SG,SB2);
        sp(row+1,SB+7,' ','|',lr,lg,lb,SR,SG,SB2);
        
        st(row+1,SB+1,lbl,lr,lg,lb,SR,SG,SB2);
    };
    panel(0, "SCORE", 100, 88,180);
    panel(3, "LINES",  65,160,100);
    panel(6, "LEVEL", 120, 90,200);

   
    for(int c=SB;c<SB+8;c++){
        sp(9, c,'-','-',80,75,140,SR,SG,SB2);
        sp(15,c,'-','-',80,75,140,SR,SG,SB2);
    }
    sp(9, SB,  '+','-',80,75,140,SR,SG,SB2);
    sp(9, SB+7,'-','+',80,75,140,SR,SG,SB2);
    sp(15,SB,  '+','-',80,75,140,SR,SG,SB2);
    sp(15,SB+7,'-','+',80,75,140,SR,SG,SB2);
    for(int r=10;r<15;r++){
        sp(r,SB,  '|',' ',80,75,140,SR,SG,SB2);
        sp(r,SB+7,' ','|',80,75,140,SR,SG,SB2);
    }
    st(9,SB+3,"NEXT",80,75,140,SR,SG,SB2);


    for(int c=SB;c<SB+8;c++){
        sp(17,c,'-','-',55,52,90,SR,SG,SB2);
        sp(24,c,'-','-',55,52,90,SR,SG,SB2);
    }
    sp(17,SB,  '+','-',55,52,90,SR,SG,SB2);
    sp(17,SB+7,'-','+',55,52,90,SR,SG,SB2);
    sp(24,SB,  '+','-',55,52,90,SR,SG,SB2);
    sp(24,SB+7,'-','+',55,52,90,SR,SG,SB2);
    for(int r=18;r<24;r++){
        sp(r,SB,  '|',' ',55,52,90,SR,SG,SB2);
        sp(r,SB+7,' ','|',55,52,90,SR,SG,SB2);
    }
    st(17,SB+3,"KEYS",55,52,90,SR,SG,SB2);

    struct { const char* k; const char* d; } hints[]={
        {"< >","Move"},{"^  ","Rot "},{"v  ","Soft"},{"SPC","Drop"},{"Q  ","Quit"}
    };
    for(int i=0;i<5;i++){
        st(18+i,SB+1,hints[i].k, 0,200,200,SR,SG,SB2,true);
        st(18+i,SB+4,hints[i].d,140,130,190,SR,SG,SB2);
    }
}


static void put_cell(int row,int col,RGB c,bool ghost){
    if(ghost){
        sp(B_ROW+row,B_COL+col,
           '[',']',
           (uint8_t)(c.r/4),(uint8_t)(c.g/4),(uint8_t)(c.b/4),
           (uint8_t)(ER+6),(uint8_t)(EG+4),(uint8_t)(EB+10));
    } else {
        
        uint8_t hr=(uint8_t)(c.r+(255-c.r)/3);
        uint8_t hg=(uint8_t)(c.g+(255-c.g)/3);
        uint8_t hb=(uint8_t)(c.b+(255-c.b)/3);
        sp(B_ROW+row,B_COL+col,'[',']',hr,hg,hb,c.r,c.g,c.b,true);
    }
}
static void put_empty(int row,int col){
    sp(B_ROW+row,B_COL+col,'.',' ',30,26,55,ER,EG,EB);
}

static void update_stat(int panel_row,uint32_t val,
                        uint8_t vr,uint8_t vg,uint8_t vb){
    // value right-aligned in the interior right half (slots SB+4..SB+6)
    char tmp[9]; snprintf(tmp,sizeof(tmp),"%8u",val);
    // pack 8 chars into 4 slots
    for(int i=0;i<4;i++)
        sp(panel_row+1,SB+3+i,tmp[i*2],tmp[i*2+1],vr,vg,vb,SR,SG,SB2,true);
}

static void update_next(){
    // clear interior rows 10-14 cols SB+1..SB+6
    for(int r=10;r<15;r++)
        for(int c=SB+1;c<SB+7;c++)
            sp(r,c,' ',' ',60,55,100,SR,SG,SB2);

    RGB nc=COLORS[G.next];
    // centre piece: 4 cols wide = 4 slots, interior = 6 slots -> offset 1
    int ox=SB+2, oy=11;
    for(int i=0;i<4;i++){
        int dr=PIECES[G.next][0][i][0], dc=PIECES[G.next][0][i][1];
        uint8_t hr=(uint8_t)(nc.r+(255-nc.r)/3);
        uint8_t hg=(uint8_t)(nc.g+(255-nc.g)/3);
        uint8_t hb=(uint8_t)(nc.b+(255-nc.b)/3);
        sp(oy+dr,ox+dc,'[',']',hr,hg,hb,nc.r,nc.g,nc.b,true);
    }
}

static void render(){
    // Board
    for(int r=0;r<BH;r++) for(int c=0;c<BW;c++){
        uint8_t v=board[r][c];
        if(v) put_cell(r,c,COLORS[v-1],false);
        else  put_empty(r,c);
    }
    // Ghost
    int gr=ghost_row();
    if(gr!=G.py) for(int i=0;i<4;i++){
        int r=gr+PIECES[G.piece][G.rot][i][0];
        int c=G.px+PIECES[G.piece][G.rot][i][1];
        if(r>=0&&r<BH&&c>=0&&c<BW&&!board[r][c])
            put_cell(r,c,COLORS[G.piece],true);
    }
    // Active piece
    for(int i=0;i<4;i++){
        int r=G.py+PIECES[G.piece][G.rot][i][0];
        int c=G.px+PIECES[G.piece][G.rot][i][1];
        if(r>=0&&r<BH&&c>=0&&c<BW)
            put_cell(r,c,COLORS[G.piece],false);
    }
    // Sidebar 
    update_stat(0,G.score,200,190,255);
    update_stat(3,G.lines,120,220,150);
    update_stat(6,G.level,210,180,255);
    update_next();

    screen_flush();
}

static void draw_gameover(){
    int oy=B_ROW+BH/2-3;
    // overlay box 
    for(int r=0;r<7;r++) for(int c=0;c<BW;c++)
        sp(oy+r,B_COL+c,' ',' ',60,50,100,22,8,42);
    // border
    for(int c=B_COL;c<B_COL+BW;c++){
        sp(oy,  c,'-','-',200,40,60,22,8,42);
        sp(oy+6,c,'-','-',200,40,60,22,8,42);
    }
    sp(oy,  B_COL,'+','-',200,40,60,22,8,42);
    sp(oy,  B_COL+BW-1,'-','+',200,40,60,22,8,42);
    sp(oy+6,B_COL,'+','-',200,40,60,22,8,42);
    sp(oy+6,B_COL+BW-1,'-','+',200,40,60,22,8,42);
    for(int r=1;r<6;r++){
        sp(oy+r,B_COL,      '|',' ',200,40,60,22,8,42);
        sp(oy+r,B_COL+BW-1,' ','|',200,40,60,22,8,42);
    }
    // text 
    st(oy+1,B_COL+1,"GAME OVER!  ",255,60,80,22,8,42,true);
    char tmp[17];
    snprintf(tmp,sizeof(tmp),"Score%7u",G.score);
    st(oy+2,B_COL+1,tmp,220,210,255,22,8,42);
    snprintf(tmp,sizeof(tmp),"Lines%4u Lv%2u",G.lines,G.level);
    st(oy+3,B_COL+1,tmp,170,165,210,22,8,42);
    st(oy+5,B_COL+2,"  Q to quit   ",90,85,140,22,8,42);
    sp(oy+5,B_COL+3,'Q',' ',0,220,220,22,8,42,true);
    screen_flush();
}


int main(){
    rng_seed();
    term_init();

    // Paint blank lines 
    printf("\x1b[H");
    for(int i=0;i<SROWS+2;i++) printf("\x1b[2K\n");
    printf("\x1b[H");
    fflush(stdout);

    memset(board,0,sizeof(board));
    G={rng_n(7),0,3,0,rng_n(7),0,0,1,false};
    if(!fits(0,3,G.piece,0)) G.over=true;

    screen_init();
    draw_chrome();
    render();

    uint64_t last=now_ms();
    for(;;){
        Key k=read_key();
        if(k==K_QUIT) break;
        if(!G.over){
            switch(k){
                case K_LEFT:  g_left();   break;
                case K_RIGHT: g_right();  break;
                case K_UP:    g_rotate(); break;
                case K_DOWN:  g_soft(); last=now_ms(); break;
                case K_SPACE:
                    g_hard(); lock_piece(); clear_lines(); spawn();
                    last=now_ms(); break;
                default: break;
            }
        }
        if(!G.over&&now_ms()-last>=drop_ms()){ g_grav(); last=now_ms(); }
        render();
        if(G.over){
            draw_gameover();
            for(;;){ if(read_key()==K_QUIT) goto done; sleep_ms(50); }
        }
        sleep_ms(16);
    }
    done:
    mv(SROWS,0); ob_puts("\n"); ob_flush();
    return 0;
}
