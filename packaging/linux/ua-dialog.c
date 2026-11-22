/* UA Reforged - the Linux launcher's dialogs (packaging/linux/launcher.sh, downloads.sh).
 *
 * One small SDL2 + SDL2_ttf program, so the dialogs look the same on every desktop (GNOME, KDE, SteamOS, a bare
 * X11 or Wayland session) and need nothing the package does not bring. Mouse, keyboard (Tab / arrows, Enter,
 * Space, Esc) and game controller (d-pad, A = choose, B = close, X = tick the box) all work, for a TV setup.
 *
 *   ua-dialog ask --font F --title T --heading H --text X [--check LABEL --checked 0|1] [--default N] -- BUTTON...
 *       prints "<button>\t<check 0|1>"; closed or Esc prints "\t<check>" and exits 1
 *   ua-dialog progress --font F --title T --dir D
 *       shows D/status ("<percent>\t<text>[\t<step>]", re-read 5 times a second) as a bar and text;
 *       Cancel (or closing the window) creates D/cancel. The launcher ends it when the step is done.
 *
 * Text comes in as arguments and is only drawn; nothing in it is ever run. No file paths are shown by the
 * launcher (a player's paths carry their account name).
 */
#include <SDL.h>
#include <SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXB 6
#define MAXL 64

static SDL_Window *win;
static SDL_Renderer *ren;
static TTF_Font *font, *bold;
static int S = 1; /* UI scale (2 on a 4K TV) */

static const SDL_Color FG = {230, 232, 228, 255}, DIM = {160, 166, 160, 255}, ACC = {214, 160, 64, 255};

static void color(Uint8 r, Uint8 g, Uint8 b) { SDL_SetRenderDrawColor(ren, r, g, b, 255); }
static void rect(int x, int y, int w, int h) { SDL_Rect r = {x, y, w, h}; SDL_RenderFillRect(ren, &r); }
static void frame(int x, int y, int w, int h) { SDL_Rect r = {x, y, w, h}; SDL_RenderDrawRect(ren, &r); }

static int textw(TTF_Font *f, const char *s) { int w = 0, h = 0; TTF_SizeUTF8(f, s, &w, &h); return w; }

static void text(TTF_Font *f, const char *s, int x, int y, SDL_Color c)
{
    if ( !s || !*s ) return;
    SDL_Surface *sf = TTF_RenderUTF8_Blended(f, s, c);
    if ( !sf ) return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, sf);
    SDL_Rect d = {x, y, sf->w, sf->h};
    SDL_RenderCopy(ren, t, NULL, &d);
    SDL_DestroyTexture(t);
    SDL_FreeSurface(sf);
}

/* word wrap <s> into lines of at most <w> pixels; "\n" starts a new line, an empty line stays */
static int wrap(TTF_Font *f, const char *s, int w, char lines[][512], int max)
{
    int n = 0;
    char cur[512] = "";
    const char *p = s;
    while ( *p && n < max )
    {
        if ( *p == '\n' ) { snprintf(lines[n++], 512, "%s", cur); cur[0] = 0; p++; continue; }
        const char *e = p;
        while ( *e && *e != ' ' && *e != '\n' ) e++;
        char word[256];
        int len = (int)(e - p) < 255 ? (int)(e - p) : 255;
        memcpy(word, p, len); word[len] = 0;
        char test[512];
        snprintf(test, sizeof(test), "%s%s%s", cur, cur[0] ? " " : "", word);
        if ( cur[0] && textw(f, test) > w ) { snprintf(lines[n++], 512, "%s", cur); snprintf(cur, sizeof(cur), "%s", word); }
        else snprintf(cur, sizeof(cur), "%s", test);
        p = e;
        if ( *p == ' ' ) p++;
    }
    if ( n < max && (cur[0] || n == 0) ) snprintf(lines[n++], 512, "%s", cur);
    return n;
}

static const char *arg(int argc, char **argv, const char *key, const char *def)
{
    for ( int i = 2; i + 1 < argc; i++ )
    {
        if ( !strcmp(argv[i], "--") ) break;
        if ( !strcmp(argv[i], key) ) return argv[i + 1];
    }
    return def;
}

static int setup(const char *title, const char *fontPath, int w, int h)
{
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_SetHint(SDL_HINT_APP_NAME, "UA Reforged");
    /* the launcher ends the progress window with SIGTERM: let it end the program (SDL would turn it into a quit
       event, which the progress window takes as Cancel) */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if ( SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0 && SDL_Init(SDL_INIT_VIDEO) != 0 )
    {
        fprintf(stderr, "ua-dialog: %s\n", SDL_GetError());
        return 0;
    }
    SDL_DisplayMode dm;
    if ( SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.h >= 2000 ) S = 2;
    if ( TTF_Init() != 0 ) return 0;
    font = TTF_OpenFont(fontPath, 16 * S);
    bold = TTF_OpenFont(fontPath, 20 * S);
    if ( !font || !bold ) { fprintf(stderr, "ua-dialog: font: %s\n", TTF_GetError()); return 0; }
    win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w * S, h * S, SDL_WINDOW_HIDDEN);
    if ( !win ) { fprintf(stderr, "ua-dialog: %s\n", SDL_GetError()); return 0; }
    /* a dialog needs no GPU: the software renderer avoids driver trouble before the game has even started
       (seen: an accelerated renderer on Mesa d3d12 under Xvfb never showed the window) */
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if ( !ren ) ren = SDL_CreateRenderer(win, -1, 0);
    if ( !ren ) { fprintf(stderr, "ua-dialog: %s\n", SDL_GetError()); return 0; }
    for ( int i = 0; i < SDL_NumJoysticks(); i++ )
        if ( SDL_IsGameController(i) ) SDL_GameControllerOpen(i);
    return 1;
}

static void button(const char *label, int x, int y, int w, int h, int focus, int primary)
{
    if ( primary ) color(150, 104, 32); else color(58, 62, 58);
    rect(x, y, w, h);
    if ( focus ) { color(240, 200, 120); frame(x - 2 * S, y - 2 * S, w + 4 * S, h + 4 * S); frame(x - S, y - S, w + 2 * S, h + 2 * S); }
    text(font, label, x + (w - textw(font, label)) / 2, y + (h - TTF_FontHeight(font)) / 2, FG);
}

/* ------------------------------------------------------------------------------------------------ ask */
static int ask(int argc, char **argv)
{
    const char *title = arg(argc, argv, "--title", "UA Reforged");
    const char *heading = arg(argc, argv, "--heading", "");
    const char *body = arg(argc, argv, "--text", "");
    const char *check = arg(argc, argv, "--check", "");
    int checked = atoi(arg(argc, argv, "--checked", "0"));
    const char *btn[MAXB];
    int nb = 0;
    for ( int i = 2; i < argc; i++ )
        if ( !strcmp(argv[i], "--") ) { for ( int j = i + 1; j < argc && nb < MAXB; j++ ) btn[nb++] = argv[j]; break; }
    if ( nb == 0 ) btn[nb++] = "OK";
    int def = atoi(arg(argc, argv, "--default", "0"));
    if ( def < 0 || def >= nb ) def = 0;

    const int W = 620, P = 24;
    if ( !setup(title, arg(argc, argv, "--font", ""), W, 200) ) return 2;
    static char lines[MAXL][512];
    int nl = wrap(font, body, (W - 2 * P) * S, lines, MAXL);
    int lh = TTF_FontHeight(font) + 3 * S, hh = heading[0] ? TTF_FontHeight(bold) + 12 * S : 0;
    int bh = 36 * S, gap = 10 * S, rowY, checkY = 0;
    /* buttons: one row when they fit, else one per row (wide labels, small windows) */
    int bw[MAXB], total = 0;
    for ( int i = 0; i < nb; i++ ) { bw[i] = textw(font, btn[i]) + 32 * S; if ( bw[i] < 110 * S ) bw[i] = 110 * S; total += bw[i] + gap; }
    int oneRow = total - gap <= (W - 2 * P) * S;
    int H = P * S + hh + nl * lh + 18 * S;
    if ( check[0] ) { checkY = H; H += 34 * S; }
    rowY = H;
    H += oneRow ? bh : nb * (bh + gap) - gap;
    H += P * S;
    SDL_SetWindowSize(win, W * S, H);
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(win);
    SDL_RaiseWindow(win);

    int nf = nb + (check[0] ? 1 : 0); /* focus order: buttons, then the box */
    int focus = def, result = -1, done = 0, mx0 = -1, my0 = 0, moved = 0;
    SDL_Rect br[MAXB], cr = {P * S, checkY, (W - 2 * P) * S, 26 * S};
    while ( !done )
    {
        /* layout (right-aligned row, default button first from the right like most desktops) */
        if ( oneRow )
        {
            int x = (W - P) * S;
            for ( int i = nb - 1; i >= 0; i-- ) { x -= bw[i]; br[i] = (SDL_Rect){x, rowY, bw[i], bh}; x -= gap; }
        }
        else
            for ( int i = 0; i < nb; i++ ) br[i] = (SDL_Rect){P * S, rowY + i * (bh + gap), (W - 2 * P) * S, bh};
        color(30, 33, 31); SDL_RenderClear(ren);
        color(214, 160, 64); rect(0, 0, W * S, 4 * S);
        int y = P * S;
        if ( heading[0] ) { text(bold, heading, P * S, y, ACC); y += hh; }
        for ( int i = 0; i < nl; i++, y += lh ) text(font, lines[i], P * S, y, FG);
        if ( check[0] )
        {
            int b = 20 * S, f = focus == nb;
            color(58, 62, 58); rect(cr.x, cr.y + 3 * S, b, b);
            if ( f ) { color(240, 200, 120); frame(cr.x - 2 * S, cr.y + S, b + 4 * S, b + 4 * S); }
            if ( checked ) { color(214, 160, 64); rect(cr.x + 5 * S, cr.y + 8 * S, b - 10 * S, b - 10 * S); }
            text(font, check, cr.x + b + 10 * S, cr.y + 3 * S, f ? FG : DIM);
        }
        for ( int i = 0; i < nb; i++ ) button(btn[i], br[i].x, br[i].y, br[i].w, br[i].h, focus == i, i == def);
        SDL_RenderPresent(ren);

        SDL_Event e;
        if ( !SDL_WaitEventTimeout(&e, 250) ) continue;
        do
        {
            switch ( e.type )
            {
            case SDL_QUIT: done = 1; break;
            case SDL_KEYDOWN:
                switch ( e.key.keysym.sym )
                {
                case SDLK_ESCAPE: done = 1; break;
                case SDLK_TAB: case SDLK_RIGHT: case SDLK_DOWN: focus = (focus + 1) % nf; break;
                case SDLK_LEFT: case SDLK_UP: focus = (focus + nf - 1) % nf; break;
                case SDLK_SPACE: case SDLK_RETURN: case SDLK_KP_ENTER:
                    if ( focus == nb ) checked = !checked; else { result = focus; done = 1; }
                    break;
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                switch ( e.cbutton.button )
                {
                case SDL_CONTROLLER_BUTTON_B: done = 1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: case SDL_CONTROLLER_BUTTON_DPAD_DOWN: focus = (focus + 1) % nf; break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: case SDL_CONTROLLER_BUTTON_DPAD_UP: focus = (focus + nf - 1) % nf; break;
                case SDL_CONTROLLER_BUTTON_X: if ( check[0] ) checked = !checked; break;
                case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_START:
                    if ( focus == nb ) checked = !checked; else { result = focus; done = 1; }
                    break;
                }
                break;
            case SDL_CONTROLLERDEVICEADDED: SDL_GameControllerOpen(e.cdevice.which); break;
            case SDL_MOUSEMOTION:
                /* hover moves the focus only once the pointer really moves: a pointer that happens to sit where
                   the window opens must not take the focus off the default button (Enter would pick another) */
                if ( mx0 < 0 ) { mx0 = e.motion.x; my0 = e.motion.y; break; }
                if ( !moved && abs(e.motion.x - mx0) + abs(e.motion.y - my0) < 4 * S ) break;
                moved = 1;
                for ( int i = 0; i < nb; i++ ) { SDL_Point pt = {e.motion.x, e.motion.y}; if ( SDL_PointInRect(&pt, &br[i]) ) focus = i; }
                break;
            case SDL_MOUSEBUTTONUP:
            {
                SDL_Point pt = {e.button.x, e.button.y};
                for ( int i = 0; i < nb; i++ ) if ( SDL_PointInRect(&pt, &br[i]) ) { result = i; done = 1; }
                if ( check[0] && SDL_PointInRect(&pt, &cr) ) { checked = !checked; focus = nb; }
                break;
            }
            }
        } while ( !done && SDL_PollEvent(&e) );
    }
    printf("%s\t%d\n", result >= 0 ? btn[result] : "", checked);
    fflush(stdout);
    return result >= 0 ? 0 : 1;
}

/* ------------------------------------------------------------------------------------------------ progress */
static int progress(int argc, char **argv)
{
    const char *title = arg(argc, argv, "--title", "UA Reforged");
    const char *dir = arg(argc, argv, "--dir", ".");
    const int W = 560, P = 24;
    if ( !setup(title, arg(argc, argv, "--font", ""), W, 170) ) return 2;
    SDL_ShowWindow(win);
    char status[1024], cancel[1024];
    snprintf(status, sizeof(status), "%s/status", dir);
    snprintf(cancel, sizeof(cancel), "%s/cancel", dir);
    int pct = 0, focus = 1, cancelled = 0, seen = 0;
    Uint32 gone = 0;
    char line[1024] = "", msg[512] = "Checking the download link...", step[512] = "";
    for ( ;; )
    {
        FILE *f = fopen(status, "r");
        /* the launcher removes the status file when the step is over: go too (also if it could not stop us) */
        if ( !f && seen )
        {
            if ( !gone ) gone = SDL_GetTicks();
            else if ( SDL_GetTicks() - gone > 3000 ) break;
        }
        if ( f )
        {
            seen = 1;
            gone = 0;
            if ( fgets(line, sizeof(line), f) )
            {
                line[strcspn(line, "\r\n")] = 0;
                char *a = strtok(line, "\t"), *b = strtok(NULL, "\t"), *c = strtok(NULL, "\t");
                if ( a ) pct = atoi(a);
                if ( b ) snprintf(msg, sizeof(msg), "%s", b);
                snprintf(step, sizeof(step), "%s", c ? c : "");
            }
            fclose(f);
        }
        if ( pct < 0 ) pct = 0;
        if ( pct > 100 ) pct = 100;
        SDL_Rect cb = {(W - P - 110) * S, 120 * S, 110 * S, 34 * S};
        color(30, 33, 31); SDL_RenderClear(ren);
        color(214, 160, 64); rect(0, 0, W * S, 4 * S);
        text(bold, title, P * S, 18 * S, ACC);
        text(font, step[0] ? step : msg, P * S, 50 * S, FG);
        color(58, 62, 58); rect(P * S, 80 * S, (W - 2 * P) * S, 18 * S);
        color(214, 160, 64); rect(P * S, 80 * S, (W - 2 * P) * S * pct / 100, 18 * S);
        if ( step[0] ) text(font, msg, P * S, 104 * S, DIM);
        if ( !cancelled ) button("Cancel", cb.x, cb.y, cb.w, cb.h, focus, 0);
        else text(font, "Stopping...", cb.x, cb.y + 8 * S, DIM);
        SDL_RenderPresent(ren);
        SDL_Event e;
        int hit = 0;
        while ( SDL_WaitEventTimeout(&e, 200) )
        {
            if ( e.type == SDL_QUIT ) hit = 1;
            else if ( e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE ||
                      (focus && (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_SPACE))) ) hit = 1;
            else if ( e.type == SDL_CONTROLLERBUTTONDOWN && (e.cbutton.button == SDL_CONTROLLER_BUTTON_B ||
                      e.cbutton.button == SDL_CONTROLLER_BUTTON_A) ) hit = 1;
            else if ( e.type == SDL_CONTROLLERDEVICEADDED ) SDL_GameControllerOpen(e.cdevice.which);
            else if ( e.type == SDL_MOUSEBUTTONUP ) { SDL_Point pt = {e.button.x, e.button.y}; if ( SDL_PointInRect(&pt, &cb) ) hit = 1; }
            if ( hit ) break;
            if ( !SDL_PollEvent(NULL) ) break;
        }
        if ( hit && !cancelled )
        {
            FILE *c = fopen(cancel, "w");
            if ( c ) fclose(c);
            cancelled = 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if ( argc >= 2 && !strcmp(argv[1], "ask") ) return ask(argc, argv);
    if ( argc >= 2 && !strcmp(argv[1], "progress") ) return progress(argc, argv);
    fprintf(stderr, "usage: ua-dialog ask|progress ... (see the source)\n");
    return 2;
}
