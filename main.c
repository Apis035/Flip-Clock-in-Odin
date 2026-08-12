/**
 * @file main.c
 * @brief Flip clock: IST at full size with a smaller Pacific-time clock beneath.
 */
#include "raylib.h"
#include "rlgl.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_CARDS 6

/**
 * @brief Seconds one card takes to flip.
 *
 * Nearly fills the one-second tick, so a card stays in motion instead of
 * snapping and then sitting still until the next change. Override at build
 * time to inspect the motion, e.g. -DFLIP_SECONDS=2.0f.
 */
#ifndef FLIP_SECONDS
#define FLIP_SECONDS 0.90f
#endif

#define FLIP_IDLE       (-1.0)
#define PHASE_SETTLED   (-1.0f)

#define SUPERSAMPLE     2
#define PERSPECTIVE     0.09f
#define FOLD_SHADE      118.0f
#define CARD_ASPECT     1.30f
#define CARD_ROUNDNESS  0.24f

static const float kWindowHeightFraction = 0.17f;
static const float kRowHalfWidthInEms    = 4.7f;
static const float kMiniScale            = 0.42f;

static const Color COL_BG    = { 17, 17, 18, 255 };
static const Color COL_CARD  = { 32, 32, 35, 255 };
static const Color COL_SEAM  = { 12, 12, 13, 255 };
static const Color COL_INK   = { 214, 214, 214, 255 };
static const Color COL_MUTED = { 176, 176, 180, 255 };
static const Color COL_FAINT = { 92, 92, 97, 255 };
static const Color COL_DOT   = { 74, 74, 78, 255 };

enum { F_DATE, F_MERIDIEM_BIG, F_MERIDIEM_MINI, F_LABEL, F_COUNT };

typedef struct {
    const char *tz;
    int cardCount;              /**< 6 for HH:MM:SS, 4 for HH:MM. */
    int digit[MAX_CARDS];
    int prev[MAX_CARDS];        /**< Value being flipped away from. */
    double flipAt[MAX_CARDS];   /**< When the flip began, FLIP_IDLE when none. */
    float phase[MAX_CARDS];     /**< Progress 0..1, PHASE_SETTLED when at rest. */
    char meridiem[8];
    char date[64];
    char zone[64];
} Clock;

/** @brief One pre-rendered card face per digit. */
typedef struct {
    RenderTexture2D face[10];
    bool built;
} Deck;

/** @brief Card geometry, all of it derived from the digit em. */
typedef struct {
    float size, cardW, cardH, tight, gap;
} Metrics;

static Metrics MetricsFor(float size)
{
    return (Metrics){ size, size, size*CARD_ASPECT, size*0.10f, size*0.40f };
}

/**
 * @brief Broken-down local time for an arbitrary zone.
 *
 * Swaps $TZ around localtime_r instead of doing offset arithmetic, so DST and
 * the zone abbreviation both come from the system database.
 *
 * @param tz IANA zone name, e.g. "Asia/Kolkata".
 */
static struct tm TimeIn(const char *tz, time_t stamp)
{
    char saved[64] = "";
    const char *old = getenv("TZ");
    bool hadTZ = (old != NULL);
    if (hadTZ) strncpy(saved, old, sizeof(saved) - 1);

    setenv("TZ", tz, 1);
    tzset();
    struct tm out;
    localtime_r(&stamp, &out);

    if (hadTZ) setenv("TZ", saved, 1);
    else unsetenv("TZ");
    tzset();
    return out;
}

/**
 * @brief Latches the time and starts a flip on every digit that changed.
 *
 * @param startedAt Monotonic instant the second turned over, which is not the
 *        instant it was noticed. Anchoring there keeps successive flips exactly
 *        one second apart whichever frame the change lands on. Pass FLIP_IDLE
 *        to latch without animating.
 */
static void ClockSample(Clock *c, time_t stamp, double startedAt)
{
    struct tm t = TimeIn(c->tz, stamp);

    int hour12 = t.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;

    int want[MAX_CARDS] = {
        hour12/10, hour12%10,
        t.tm_min/10, t.tm_min%10,
        t.tm_sec/10, t.tm_sec%10,
    };

    for (int i = 0; i < c->cardCount; i++) {
        if (want[i] == c->digit[i]) continue;
        c->prev[i] = c->digit[i];
        c->digit[i] = want[i];
        c->flipAt[i] = startedAt;
    }

    strcpy(c->meridiem, t.tm_hour < 12 ? "AM" : "PM");
    strftime(c->date, sizeof(c->date), "%A, %B %-d %Y", &t);
    strftime(c->zone, sizeof(c->zone), "%Z", &t);
}

/**
 * @brief Recomputes each card's flip progress.
 *
 * Taken from @p now rather than accumulated frame deltas, so a hitched frame
 * skips ahead instead of stretching the animation.
 */
static void ClockAdvance(Clock *c, double now)
{
    for (int i = 0; i < c->cardCount; i++) {
        if (c->flipAt[i] < 0.0) { c->phase[i] = PHASE_SETTLED; continue; }
        double progress = (now - c->flipAt[i])/FLIP_SECONDS;
        if (progress >= 1.0) {
            c->flipAt[i] = FLIP_IDLE;
            c->phase[i] = PHASE_SETTLED;
        } else {
            c->phase[i] = (float)(progress < 0.0 ? 0.0 : progress);
        }
    }
}

/**
 * @brief First installed face of the requested weight.
 *
 * @return Path, or NULL when nothing at all is installed. A missing regular
 *         weight falls back to bold rather than returning nothing.
 */
static const char *FontPath(bool bold)
{
    static const char *boldFaces[] = {
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Bold.ttf",
        "/usr/share/fonts/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/TTF/Inter-Bold.ttf",
    };
    static const char *regularFaces[] = {
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/Inter-Regular.ttf",
    };
    const char **faces = bold ? boldFaces : regularFaces;
    for (int i = 0; i < 7; i++) {
        if (FileExists(faces[i])) return faces[i];
    }
    return bold ? NULL : FontPath(true);
}

/**
 * @brief Bakes a face at an exact pixel size.
 *
 * Every string is drawn at the size its atlas was baked at, so glyphs are never
 * resampled.
 *
 * @return The shared default font when no face can be loaded.
 */
static Font BakeFont(float px, bool bold)
{
    const char *path = FontPath(bold);
    if (!path) return GetFontDefault();
    Font f = LoadFontEx(path, (int)(px + 0.5f), NULL, 0);
    if (f.texture.id == 0) return GetFontDefault();
    SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
    return f;
}

/** @brief Unloads a baked font, skipping the shared default BakeFont may return. */
static void UnloadBaked(Font f)
{
    if (f.texture.id != 0 && f.texture.id != GetFontDefault().texture.id) UnloadFont(f);
}

static void UnloadFonts(Font *fonts)
{
    for (int i = 0; i < F_COUNT; i++) UnloadBaked(fonts[i]);
}

static void DrawTextAt(Font f, const char *s, float x, float y, float sp, Color col)
{
    DrawTextEx(f, s, (Vector2){ floorf(x + 0.5f), floorf(y + 0.5f) }, (float)f.baseSize, sp, col);
}

static void DrawCentered(Font f, const char *s, float cx, float y, float sp, Color col)
{
    Vector2 m = MeasureTextEx(f, s, (float)f.baseSize, sp);
    DrawTextAt(f, s, cx - m.x*0.5f, y, sp, col);
}

static void DeckFree(Deck *d)
{
    if (!d->built) return;
    for (int i = 0; i < 10; i++) UnloadRenderTexture(d->face[i]);
    d->built = false;
}

/**
 * @brief Pre-renders the ten card faces at SUPERSAMPLE scale.
 *
 * Drawing a card then costs one textured quad and the glyphs come out
 * supersampled. Digits are centred on the glyph box rather than the line box,
 * which carries descender padding the digits never use.
 */
static void DeckBuild(Deck *d, Metrics m)
{
    DeckFree(d);

    int texW = (int)(m.cardW*SUPERSAMPLE + 0.5f), texH = (int)(m.cardH*SUPERSAMPLE + 0.5f);
    Font digitFont = BakeFont(texH/CARD_ASPECT, true);
    GlyphInfo zeroGlyph = GetGlyphInfo(digitFont, '0');
    float glyphH = (float)zeroGlyph.image.height;

    for (int n = 0; n < 10; n++) {
        d->face[n] = LoadRenderTexture(texW, texH);
        BeginTextureMode(d->face[n]);
        ClearBackground(BLANK);

        DrawRectangleRounded((Rectangle){ 0, 0, (float)texW, (float)texH },
                             CARD_ROUNDNESS, 20, COL_CARD);
        DrawRectangle(0, texH/2 - SUPERSAMPLE/2, texW, SUPERSAMPLE, COL_SEAM);

        const char *s = TextFormat("%d", n);
        Vector2 digitSize = MeasureTextEx(digitFont, s, (float)digitFont.baseSize, 0);
        float y = (texH - glyphH)*0.5f - zeroGlyph.offsetY;
        DrawTextEx(digitFont, s, (Vector2){ (texW - digitSize.x)*0.5f, y },
                   (float)digitFont.baseSize, 0, COL_INK);

        EndTextureMode();
        SetTextureFilter(d->face[n].texture, TEXTURE_FILTER_BILINEAR);
    }

    UnloadBaked(digitFont);
    d->built = true;
}

/**
 * @brief Maps an image-space rect onto the source rect DrawTexturePro expects.
 *
 * Render textures are stored bottom-up, and a negative height samples flipped.
 */
static Rectangle FlippedSource(float texH, float x, float y, float w, float h)
{
    return (Rectangle){ x, texH - y - h, w, -h };
}

/**
 * @brief Draws the folding half of a card as a textured quad.
 *
 * The seam edge stays pinned while the free edge lifts toward the viewer, so
 * the leaf foreshortens vertically and widens slightly.
 */
static void DrawLeaf(Texture2D tex, float texH, float srcY, float srcH,
                     float cx, float seamY, float w, float leafH,
                     float grow, bool upper, Color tint)
{
    float halfSeam = w*0.5f;
    float halfFree = halfSeam*(1.0f + grow);
    float freeY = upper ? seamY - leafH : seamY + leafH;

    float vTop = (texH - srcY)/texH;
    float vBot = (texH - (srcY + srcH))/texH;

    Vector2 tl, bl, br, tr;
    if (upper) {
        tl = (Vector2){ cx - halfFree, freeY };
        tr = (Vector2){ cx + halfFree, freeY };
        bl = (Vector2){ cx - halfSeam, seamY };
        br = (Vector2){ cx + halfSeam, seamY };
    } else {
        tl = (Vector2){ cx - halfSeam, seamY };
        tr = (Vector2){ cx + halfSeam, seamY };
        bl = (Vector2){ cx - halfFree, freeY };
        br = (Vector2){ cx + halfFree, freeY };
    }

    rlSetTexture(tex.id);
    rlBegin(RL_QUADS);
    rlColor4ub(tint.r, tint.g, tint.b, tint.a);
    rlNormal3f(0.0f, 0.0f, 1.0f);

    rlTexCoord2f(0.0f, vTop); rlVertex2f(tl.x, tl.y);
    rlTexCoord2f(0.0f, vBot); rlVertex2f(bl.x, bl.y);
    rlTexCoord2f(1.0f, vBot); rlVertex2f(br.x, br.y);
    rlTexCoord2f(1.0f, vTop); rlVertex2f(tr.x, tr.y);

    rlEnd();
    rlSetTexture(0);
}

/**
 * @brief Draws one card, settled or mid-flip.
 *
 * The incoming digit sits on the top half and the outgoing one below, both
 * whole faces with the lower clipped; neither moves, so scissor rounding costs
 * nothing. The fold angle then sweeps at a constant rate: easing it would stall
 * the leaf at both ends, and the visible height already goes as cos, which is
 * the fast-through-the-middle motion a real leaf has.
 *
 * @param phase Progress 0..1, PHASE_SETTLED when at rest.
 */
static void DrawCard(const Deck *d, Rectangle r, int digit, int prev, float phase)
{
    Texture2D texNew = d->face[digit].texture;
    float texW = (float)texNew.width, texH = (float)texNew.height;
    Rectangle wholeFace = FlippedSource(texH, 0, 0, texW, texH);
    Vector2 origin = { 0, 0 };

    if (phase < 0.0f) {
        DrawTexturePro(texNew, wholeFace, r, origin, 0.0f, WHITE);
        return;
    }

    Texture2D texOld = d->face[prev].texture;
    float halfFace = texH*0.5f, halfCard = r.height*0.5f;
    float seamY = r.y + halfCard, cx = r.x + r.width*0.5f;

    DrawTexturePro(texNew, wholeFace, r, origin, 0.0f, WHITE);
    BeginScissorMode((int)r.x, (int)(seamY + 0.5f), (int)(r.width + 0.5f), (int)(halfCard + 1.0f));
    DrawTexturePro(texOld, wholeFace, r, origin, 0.0f, WHITE);
    EndScissorMode();

    float foldAngle = PI*phase;
    float foreshorten = fabsf(cosf(foldAngle)), lift = sinf(foldAngle);
    float leafH = halfCard*foreshorten;
    float grow = PERSPECTIVE*lift;

    unsigned char shade = (unsigned char)(255.0f - FOLD_SHADE*lift);
    Color tint = { shade, shade, shade, 255 };

    if (foldAngle < PI*0.5f) {
        DrawLeaf(texOld, texH, 0, halfFace, cx, seamY, r.width, leafH, grow, true, tint);
    } else {
        DrawLeaf(texNew, texH, halfFace, halfFace, cx, seamY, r.width, leafH, grow, false, tint);
    }

    DrawRectangle((int)r.x, (int)(seamY - 1), (int)r.width, 2, COL_SEAM);
}

static void DrawColon(float cx, float cy, float size)
{
    float rad = size*0.05f;
    DrawCircleV((Vector2){ cx, cy - size*0.19f }, rad, COL_DOT);
    DrawCircleV((Vector2){ cx, cy + size*0.19f }, rad, COL_DOT);
}

/**
 * @brief Width of the card row, excluding the meridiem.
 *
 * Excluded so the cards stay centred under the date while AM/PM overhangs to
 * the right, as it does on a real unit.
 */
static float RowWidth(const Clock *c, Metrics m)
{
    int groups = c->cardCount/2;
    return groups*(2*m.cardW + m.tight) + (groups - 1)*m.gap;
}

static void DrawClock(const Clock *c, const Deck *deck, float centerX, float topY,
                      Metrics m, Font meridiemFont)
{
    float x = centerX - RowWidth(c, m)*0.5f;
    for (int i = 0; i < c->cardCount; i++) {
        DrawCard(deck, (Rectangle){ floorf(x), floorf(topY), m.cardW, m.cardH },
                 c->digit[i], c->prev[i], c->phase[i]);
        x += m.cardW;
        if (i % 2 == 0) {
            x += m.tight;
        } else if (i + 1 < c->cardCount) {
            DrawColon(x + m.gap*0.5f, topY + m.cardH*0.5f, m.size);
            x += m.gap;
        }
    }

    Vector2 t = MeasureTextEx(meridiemFont, c->meridiem, (float)meridiemFont.baseSize, 0);
    DrawTextAt(meridiemFont, c->meridiem, x + m.gap, topY + (m.cardH - t.y)*0.5f, 0, COL_INK);
}

/**
 * @brief Sizes the layout to the window, then draws both clocks each frame.
 *
 * Escape is disabled so the window is not dismissed by a stray keypress; set
 * SetExitKey back to KEY_ESCAPE to allow it. Faces and font atlases are rebaked
 * only once a resize has settled, since baking is expensive.
 */
int main(void)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(1280, 720, "Flip Clock - IST");
    SetExitKey(KEY_NULL);

    Clock ist = { .tz = "Asia/Kolkata",        .cardCount = 6 };
    Clock pst = { .tz = "America/Los_Angeles", .cardCount = 4 };
    Clock *clocks[] = { &ist, &pst };

    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < MAX_CARDS; j++) clocks[i]->flipAt[j] = FLIP_IDLE;
        ClockSample(clocks[i], time(NULL), FLIP_IDLE);
    }

    Deck deckBig = { 0 }, deckMini = { 0 };
    Font fonts[F_COUNT] = { 0 };
    float bakedForSize = 0.0f;
    float settleSeconds = 0.0f;

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_F11) || IsKeyPressed(KEY_F)) ToggleFullscreen();

        float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
        float size = fminf(H*kWindowHeightFraction, (W*0.5f)/kRowHalfWidthInEms);
        Metrics big = MetricsFor(size), mini = MetricsFor(size*kMiniScale);

        if (fabsf(size - bakedForSize) > 0.5f) settleSeconds += GetFrameTime();
        else settleSeconds = 0.0f;

        if (bakedForSize == 0.0f || settleSeconds > 0.20f) {
            DeckBuild(&deckBig, big);
            DeckBuild(&deckMini, mini);
            UnloadFonts(fonts);
            fonts[F_DATE] = BakeFont(size*0.175f, false);
            fonts[F_MERIDIEM_BIG] = BakeFont(size*0.22f, true);
            fonts[F_MERIDIEM_MINI] = BakeFont(mini.size*0.32f, true);
            fonts[F_LABEL] = BakeFont(size*0.13f, false);
            bakedForSize = size;
            settleSeconds = 0.0f;
        }

        struct timespec wall;
        clock_gettime(CLOCK_REALTIME, &wall);
        double monotonicNow = GetTime();
        double secondTurnedOver = monotonicNow - (double)wall.tv_nsec/1e9;

        for (int i = 0; i < 2; i++) {
            ClockSample(clocks[i], wall.tv_sec, secondTurnedOver);
            ClockAdvance(clocks[i], monotonicNow);
        }

        float blockH = big.cardH + size*0.90f + mini.cardH + size*0.55f;
        float mainTop = (H - blockH)*0.5f + size*0.55f;
        float miniTop = mainTop + big.cardH + size*0.90f;
        float labelTracking = size*0.13f*0.16f;

        BeginDrawing();
        ClearBackground(COL_BG);

        DrawCentered(fonts[F_DATE], ist.date, W*0.5f, mainTop - size*0.52f, 0, COL_MUTED);
        DrawClock(&ist, &deckBig, W*0.5f, mainTop, big, fonts[F_MERIDIEM_BIG]);
        DrawCentered(fonts[F_LABEL], TextFormat("%s   -   INDIA STANDARD TIME", ist.zone),
                     W*0.5f, mainTop + big.cardH + size*0.22f, labelTracking, COL_FAINT);

        DrawClock(&pst, &deckMini, W*0.5f, miniTop, mini, fonts[F_MERIDIEM_MINI]);
        DrawCentered(fonts[F_LABEL], TextFormat("%s   -   PACIFIC TIME", pst.zone),
                     W*0.5f, miniTop + mini.cardH + mini.size*0.35f, labelTracking, COL_FAINT);

        EndDrawing();
    }

    DeckFree(&deckBig);
    DeckFree(&deckMini);
    UnloadFonts(fonts);
    CloseWindow();
    return 0;
}
