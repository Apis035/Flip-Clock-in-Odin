#+vet explicit-allocators shadowing
package main

import "core:fmt"
import "core:math"
import "core:time"
import "core:time/timezone"
import rl "vendor:raylib"
import gl "vendor:raylib/rlgl"

MAX_CARDS      :: 6
FLIP_SECONDS   :: 0.9
FLIP_IDLE      :: -1
PHASE_SETTLED  :: -1
SUPERSAMPLE    :: 2
PERSPECTIVE    :: 0.09
FOLD_SHADE     :: 118
CARD_ASPECT    :: 1.3
CARD_ROUNDNESS :: 0.24

WINDOW_HEIGHT_FRACTION :: 0.17
ROW_HALF_WIDTH_IN_EMS  :: 4.7
MINI_SCALE             :: 0.42

COL_BG    :: rl.Color { 17, 17, 18, 255 }
COL_CARD  :: rl.Color { 32, 32, 35, 255 }
COL_SEAM  :: rl.Color { 12, 12, 13, 255 }
COL_INK   :: rl.Color { 214, 214, 214, 255 }
COL_MUTED :: rl.Color { 176, 176, 180, 255 }
COL_FAINT :: rl.Color { 92, 92, 97, 255 }
COL_DOT   :: rl.Color { 74, 74, 78, 255 }

Fonts :: enum {
	Date, MeridiemBig, MeridiemMini, Label
}

Clock :: struct {
	tz:        string,
	cardCount: int,
	digit:     [MAX_CARDS]int,
	prev:      [MAX_CARDS]int,
	flipAt:    [MAX_CARDS]f64,
	phase:     [MAX_CARDS]f32,
	meridiem:  cstring,
	date:      [64]byte,
	zone:      [64]byte,
}

Deck :: struct {
	face:  [10]rl.RenderTexture2D,
	built: bool,
}

Metrics :: struct {
	size, cardW, cardH, tight, gap: f32
}

MetricsFor :: proc(size: f32) -> Metrics {
	return {size, size, size*CARD_ASPECT, size*0.1, size*0.4}
}

TimeIn :: proc(stamp: time.Time, tz: string) -> (res: time.Time, ok: bool) {
	region := timezone.region_load(tz, context.allocator) or_return
	defer timezone.region_destroy(region, context.allocator)
	stampDt    := time.time_to_datetime(stamp) or_return
	stampDtNew := timezone.datetime_to_tz(stampDt, region) or_return
	stampNew   := time.datetime_to_time(stampDtNew) or_return
	return stampNew, true
}

ClockSample :: proc(c: ^Clock, stamp: time.Time, startedAt: f64) {
	weekday := time.weekday(stamp)
	year, month, day := time.date(stamp)
	hour, min, sec := time.clock(stamp)

	hour12 := hour % 12
	if hour12 == 0 {
		hour12 = 12
	}

	want := [MAX_CARDS]int {
		hour12 / 10, hour12 % 10,
		min    / 10, min    % 10,
		sec    / 10, sec    % 10,
	}

	for i in 0..<c.cardCount {
		(want[i] != c.digit[i]) or_continue
		c.prev[i] = c.digit[i]
		c.digit[i] = want[i]
		c.flipAt[i] = startedAt
	}

	c.meridiem = hour < 12 ? "AM" : "PM"
	fmt.bprintf(c.date[:], "%v, %v %-2d %d", weekday, month, day, year)
	fmt.bprintf(c.zone[:], "%s", c.tz) // TODO
}

ClockAdvance :: proc(c: ^Clock, now: f64) {
	for i in 0..<c.cardCount {
		if c.flipAt[i] < 0 {
			c.phase[i] = PHASE_SETTLED
			continue
		}
		progress := (now - c.flipAt[i])/FLIP_SECONDS
		if progress >= 1 {
			c.flipAt[i] = FLIP_IDLE
			c.phase[i] = PHASE_SETTLED
		} else {
			c.phase[i] = f32(progress < 0 ? 0 : progress)
		}
	}
}

FontPath :: proc(bold: bool) -> cstring { // TODO
	SEGOE_UI      :: "C:/Windows/Fonts/segoeui.ttf"
	SEGOE_UI_BOLD :: "C:/Windows/Fonts/segoeuib.ttf"
	return bold ? SEGOE_UI_BOLD : SEGOE_UI
}

BakeFont :: proc(px: f32, bold: bool) -> rl.Font {
	path := FontPath(bold)
	f := rl.LoadFontEx(path, i32(px + 0.5), nil, 0)
	if f.texture.id == 0 {
		return rl.GetFontDefault()
	}
	rl.SetTextureFilter(f.texture, .BILINEAR)
	return f
}

UnloadBaked :: proc(f: rl.Font) {
	if f.texture.id != 0 && f.texture.id != rl.GetFontDefault().texture.id {
		rl.UnloadFont(f)
	}
}

UnloadFonts :: proc(fonts: [Fonts]rl.Font) {
	for f in fonts {
		UnloadBaked(f)
	}
}

DrawTextAt :: proc(f: rl.Font, s: cstring, x, y, sp: f32, col: rl.Color) {
	rl.DrawTextEx(f, s, {math.floor(x + 0.5), math.floor(y + 0.5)}, f32(f.baseSize), sp, col)
}

DrawCentered :: proc(f: rl.Font, s: cstring, cx, y, sp: f32, col: rl.Color) {
	m := rl.MeasureTextEx(f, s, f32(f.baseSize), sp)
	DrawTextAt(f, s, cx - m.x/2, y, sp, col)
}

DeckFree :: proc(d: ^Deck) {
	if !d.built {
		return
	}
	for i in 0..<10 {
		rl.UnloadRenderTexture(d.face[i])
	}
	d.built = false
}

DeckBuild :: proc(d: ^Deck, m: Metrics) {
	DeckFree(d)
	defer d.built = true

	texW := i32(m.cardW*SUPERSAMPLE + 0.5)
	texH := i32(m.cardH*SUPERSAMPLE + 0.5)

	digitFont := BakeFont(f32(texH)/CARD_ASPECT, true)
	defer UnloadBaked(digitFont)

	zeroGlyph := rl.GetGlyphInfo(digitFont, '0')
	glyphH := f32(zeroGlyph.image.height)

	for n in 0..<10 {
		d.face[n] = rl.LoadRenderTexture(texW, texH)

		rl.BeginTextureMode(d.face[n])
		rl.ClearBackground(rl.BLANK)
		rl.DrawRectangleRounded({0, 0, f32(texW), f32(texH)}, CARD_ROUNDNESS, 20, COL_CARD)
		rl.DrawRectangle(0, texH/2 - SUPERSAMPLE/2, texW, SUPERSAMPLE, COL_SEAM)
		s := rl.TextFormat("%d", n)
		digitSize := rl.MeasureTextEx(digitFont, s, f32(digitFont.baseSize), 0)
		y := (f32(texH) - glyphH)/2 - f32(zeroGlyph.offsetY)
		rl.DrawTextEx(digitFont, s, {(f32(texW) - digitSize.x)/2, y}, f32(digitFont.baseSize), 0, COL_INK)
		rl.EndTextureMode()

		rl.SetTextureFilter(d.face[n].texture, .BILINEAR)
	}
}

FlippedSource :: proc(texH, x, y, w, h: f32) -> rl.Rectangle {
	return {x, texH - y - h, w, -h}
}

DrawLeaf :: proc(tex: rl.Texture2D, texH, srcY, srcH, cx, seamY, w, leafH, grow: f32, upper: bool, tint: rl.Color) {
	halfSeam := w/2
	halfFree := halfSeam * (1 + grow)
	freeY    := upper ? seamY - leafH : seamY + leafH
	vTop     := (texH - srcY) / texH
	vBot     := (texH - (srcY + srcH)) / texH

	tl, tr, bl, br: rl.Vector2
	if upper {
		tl = {cx - halfFree, freeY}
		tr = {cx + halfFree, freeY}
		bl = {cx - halfSeam, seamY}
		br = {cx + halfSeam, seamY}
	} else {
		tl = {cx - halfSeam, seamY}
		tr = {cx + halfSeam, seamY}
		bl = {cx - halfFree, freeY}
		br = {cx + halfFree, freeY}
	}

	gl.SetTexture(tex.id)
	gl.Begin(gl.QUADS)
	gl.Color4ub(tint.r, tint.g, tint.b, tint.a)
	gl.Normal3f(0, 0, 1)

	gl.TexCoord2f(0, vTop); gl.Vertex2f(tl.x, tl.y)
	gl.TexCoord2f(0, vBot); gl.Vertex2f(bl.x, bl.y)
	gl.TexCoord2f(1, vBot); gl.Vertex2f(br.x, br.y)
	gl.TexCoord2f(1, vTop); gl.Vertex2f(tr.x, tr.y)

	gl.End()
	gl.SetTexture(0)
}

DrawCard :: proc(d: Deck, r: rl.Rectangle, digit, prev: int, phase: f32) {
	texNew    := d.face[digit].texture
	texW      := f32(texNew.width)
	texH      := f32(texNew.height)
	wholeFace := FlippedSource(texH, 0, 0, texW, texH)
	origin    := rl.Vector2 {0, 0}

	if phase < 0 {
		rl.DrawTexturePro(texNew, wholeFace, r, {0, 0}, 0, rl.WHITE)
		return
	}

	texOld   := d.face[prev].texture
	halfFace := texH/2
	halfCard := r.height/2
	seamY    := r.y + halfCard
	cx       := r.y + r.width/2

	rl.DrawTexturePro(texNew, wholeFace, r, {0, 0}, 0, rl.WHITE)
	rl.BeginScissorMode(i32(r.x), i32(seamY + 0.5), i32(r.width + 0.5), i32(halfCard + 1))
	rl.DrawTexturePro(texNew, wholeFace, r, {0, 0}, 0, rl.WHITE)
	rl.EndScissorMode()

	foldAngle   := math.PI * phase
	foreshorten := abs(math.cos(foldAngle))
	lift        := math.sin(foldAngle)
	leafH       := halfCard * foreshorten
	grow        := PERSPECTIVE * lift

	shade := u8(255 - FOLD_SHADE * lift)
	tint  := rl.Color{shade, shade, shade, 255}

	if foldAngle < math.PI*0.5 {
		DrawLeaf(texOld, texH, 0, halfFace, cx, seamY, r.width, leafH, grow, true, tint)
	} else {
		DrawLeaf(texNew, texH, halfFace, halfFace, cx, seamY, r.width, leafH, grow, false, tint)
	}

	rl.DrawRectangle(i32(r.x), i32(seamY - 1), i32(r.width), 2, COL_SEAM)
}

DrawColon :: proc(cx, cy, size: f32) {
	rad := size * 0.05
	rl.DrawCircleV({cx, cy - size*0.19}, rad, COL_DOT)
	rl.DrawCircleV({cx, cy + size*0.19}, rad, COL_DOT)
}

RowWidth :: proc(c: Clock, m: Metrics) -> f32 {
	groups := f32(c.cardCount/2)
	return groups*(2*m.cardW + m.tight) + (groups - 1)*m.gap
}

DrawClock :: proc(c: Clock, d: Deck, centerX, topY: f32, m: Metrics, meridiemFont: rl.Font) {
	x := centerX - RowWidth(c, m)/2
	for i in 0..<c.cardCount {
		DrawCard(d, {math.floor(x), math.floor(topY), m.cardW, m.cardH}, c.digit[i], c.prev[i], c.phase[i])
		x += m.cardW
		if i % 2 == 0 {
			x += m.tight
		} else if (i + 1 < c.cardCount) {
			DrawColon(x + m.gap/2, topY + m.cardH/2, m.size)
			x += m.gap
		}
	}
	t := rl.MeasureTextEx(meridiemFont, c.meridiem, f32(meridiemFont.baseSize), 0)
	DrawTextAt(meridiemFont, c.meridiem, x + m.gap, topY + (m.cardH - t.y)/2, 0, COL_INK)
}

main :: proc() {
	rl.SetConfigFlags({.WINDOW_RESIZABLE, .MSAA_4X_HINT})
	rl.InitWindow(1280, 720, "Flip Clock")
	defer rl.CloseWindow()
	rl.SetExitKey(.KEY_NULL)
	rl.SetTargetFPS(10)

	ist := Clock{ tz = "Asia/Kolkata",        cardCount = 6 }
	pst := Clock{ tz = "America/Los_Angeles", cardCount = 4 }
	clocks := []^Clock{&ist, &pst}

	for i in 0..<len(clocks) {
		for j in 0..<MAX_CARDS {
			clocks[i].flipAt[j] = FLIP_IDLE
		}
		ClockSample(clocks[i], time.now(), FLIP_IDLE)
	}

	deckBig, deckMini: Deck
	defer DeckFree(&deckBig)
	defer DeckFree(&deckMini)

	fonts: [Fonts]rl.Font
	defer UnloadFonts(fonts)

	bakedForSize: f32
	settleSeconds: f32

	for !rl.WindowShouldClose() {
		if rl.IsKeyPressed(.F11) || rl.IsKeyPressed(.F) {
			rl.ToggleFullscreen()
		}

		w, h := f32(rl.GetScreenWidth()), f32(rl.GetScreenHeight())
		size := min(h*WINDOW_HEIGHT_FRACTION, (w/2)/ROW_HALF_WIDTH_IN_EMS)
		big := MetricsFor(size)
		mini := MetricsFor(size*MINI_SCALE)

		if abs(size - bakedForSize) > 0.5 {
			settleSeconds += rl.GetFrameTime()
		} else {
			settleSeconds = 0
		}

		if bakedForSize == 0 || settleSeconds > 0.2 {
			DeckBuild(&deckBig, big)
			DeckBuild(&deckMini, mini)
			UnloadFonts(fonts)
			fonts[.Date] = BakeFont(size*0.175, false)
			fonts[.MeridiemBig] = BakeFont(size*0.22, true)
			fonts[.MeridiemMini] = BakeFont(mini.size*0.32, true)
			fonts[.Label] = BakeFont(size*0.13, false)
			bakedForSize = size
			settleSeconds = 0
		}

		now := time.now()
		monotonicNow := rl.GetTime()
		secondTurnedOver := monotonicNow - f64(now._nsec)/1e9 // TODO

		for i in 0..<len(clocks) {
			ClockSample(clocks[i], now, secondTurnedOver)
			ClockAdvance(clocks[i], monotonicNow)
		}

		blockH := big.cardH + size*0.9 + mini.cardH + size*0.55
		mainTop := (h - blockH)*0.5 + size*0.55
		miniTop := mainTop + big.cardH + size*0.9
		labelTracking := size*0.13*0.16

		rl.BeginDrawing()
		rl.ClearBackground(COL_BG)

		DrawCentered(fonts[.Date], cstring(raw_data(ist.date[:])), w/2, mainTop - size*0.52, 0, COL_MUTED)
		DrawClock(ist, deckBig, w/2, mainTop, big, fonts[.MeridiemBig])
		DrawCentered(fonts[.Label], rl.TextFormat("%s  -  INDIA STANDARD TIME", "IST"),
			w/2, mainTop + big.cardH + size*0.22, labelTracking, COL_FAINT)

		DrawClock(pst, deckMini, w/2, miniTop, mini, fonts[.MeridiemMini])
		DrawCentered(fonts[.Label], rl.TextFormat("%s  -  PACIFIC TIME", "PDT"),
			w/2, miniTop + mini.cardH + mini.size*0.35, labelTracking, COL_FAINT)
		rl.EndDrawing()
	}
}