/*
 *  Copyright (C) 2012-2025  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "boxedwine.h"
#include "x11.h"
#ifdef __wasm_simd128__
#include <wasm_simd128.h>
#endif

XDrawable::XDrawable(U32 width, U32 height, U32 depth, const VisualPtr& visual, bool isWindow, bool isPBuffer) : id(XServer::getNextId()), isWindow(isWindow), isPBuffer(isPBuffer), depth(depth), visual(visual), w(width), h(height) {
	data = nullptr;
	setSize(width, height);
}

XPBuffer::XPBuffer(U32 width, U32 height, U32 depth, const VisualPtr& visual, U32 fbConfigId, bool preservedContents, U32 eventMask)
	: XDrawable(width, height, depth, visual, false, true), fbConfigId(fbConfigId), preservedContents(preservedContents), eventMask(eventMask) {
	isOpenGL = true;
}

XDrawable::~XDrawable() {
	delete[] data;
}

U32 XDrawable::getImage(KThread* thread, S32 x, S32 y, U32 width, U32 height, U32 planeMask, U32 format, U32 redMask, U32 greenMask, U32 blueMask) {
	if (!this->data || !width || !height) {
		return 0;
	}
	if (planeMask != AllPlanes) {
		kpanic_fmt("XDrawable::createXImage wasn't expecting planeMask = %x", planeMask);
	}
	if (format != ZPixmap) {
		kpanic_fmt("XDrawable::createXImage wasn't expecting format = %x", format);
	}
	U64 stride = (((U64)visual->bits_per_rgb * width + 31) / 32) * 4;
	if (stride > 0xffffffff || stride * height > 0xffffffff) return 0;
	U32 bytesPerLine = (U32)stride;
	U32 len = bytesPerLine * height;
	U32 image = thread->process->alloc(thread, sizeof(XImage));
	U32 data = thread->process->alloc(thread, len);

	// Wine's software-cursor save rectangle can cross any drawable edge.
	// Preserve its requested size and clear unavailable pixels, as copying
	// through a temporary pixmap would do. The filesystem's legacy X11 shim
	// dereferences the returned image, so rejecting an edge rectangle with
	// NULL faults while Wine holds its graphics error-handler mutex.
	std::vector<U8> pixels(len, 0);
	readImage(pixels.data(), bytesPerLine, x, y, width, height, 0, 0);
	thread->memory->memcpy(data, pixels.data(), len);

	XImage::set(thread->memory, image, width, height, 0, format, data, 32, depth, bytesPerLine, visual->bits_per_rgb, redMask, greenMask, blueMask);
	return image;
}

void XDrawable::readImage(U8* dst, U32 dstPitch, S32 x, S32 y, U32 width, U32 height, U32 dstX, U32 dstY) {
	BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mutex);
	S64 left = std::max<S64>(0, x), top = std::max<S64>(0, y);
	S64 right = std::min<S64>(w, (S64)x + width), bottom = std::min<S64>(h, (S64)y + height);
	if (!data || left >= right || top >= bottom) return;
	for (S64 sourceY = top; sourceY < bottom; ++sourceY) {
		const U8* src = data + bytes_per_line * sourceY;
		U8* row = dst + dstPitch * ((U64)dstY + sourceY - y);
		if (visual->bits_per_rgb == 1) {
			for (S64 pixel = left; pixel < right; ++pixel) {
				U32 target = dstX + (U32)(pixel - x);
				U8 mask = (U8)(1 << (target & 7));
				row[target / 8] = (row[target / 8] & ~mask) |
					(((src[pixel / 8] >> (pixel & 7)) & 1) << (target & 7));
			}
		} else {
			U32 pixelBytes = visual->bits_per_rgb / 8;
			memcpy(row + (dstX + left - x) * pixelBytes, src + left * pixelBytes, (right - left) * pixelBytes);
		}
	}
}

U64 XDrawable::calculateBytesPerLine(U32 bitsPerPixel, U32 width) {
	U64 result = ((U64)width * bitsPerPixel + 7) / 8;
	result = (result + 3) / 4 * 4;
	return result;
}

// The largest drawable that gets pixels (8192x8192 at 32 bpp). An X server keeps none for an unmapped window, here every
// window has them: a game's huge hidden popup must not end the emulator. Larger ones keep their size for the guest and
// have no pixels, drawing on them does nothing and the composite skips them (bytes_per_line 0, as putBitsOnWnd checks).
static const U64 MAX_DRAWABLE_BYTES = 256 * 1024 * 1024;

void XDrawable::lockData() {
	BOXEDWINE_MUTEX_LOCK(mutex);
}

void XDrawable::unlockData() {
	BOXEDWINE_MUTEX_UNLOCK(mutex);
}

void XDrawable::setSize(U32 width, U32 height) {
	BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mutex);
	w = width;
	h = height;
	U64 pitch = calculateBytesPerLine(visual?visual->bits_per_rgb:32, width);
	if (data) {
		delete[] data;
		data = nullptr;
	}
	size = 0;
	bytes_per_line = (U32)pitch;
	if (isPBuffer || !pitch || !height) {
		return;
	}
	if (pitch > MAX_DRAWABLE_BYTES / height || !(data = new (std::nothrow) U8[pitch * height])) {
		bytes_per_line = 0;
		return;
	}
	size = (U32)(pitch * height);
	memset(data, 0, size);
}

int XDrawable::putImage(KThread* thread, const std::shared_ptr<XGC>& gc, XImage* image, S32 src_x, S32 src_y, S32 dest_x, S32 dest_y, U32 width, U32 height) {
	if (gc->values.function != GXcopy) {
		kwarn_fmt("XPixmap::putImage function not supported %d", gc->values.function);
	}
	return copyImageData(thread, gc, image->data, image->bytes_per_line, image->bits_per_pixel, src_x, src_y, dest_x, dest_y, width, height);
}

// n bytes equal? 64 at a time with wasm SIMD (emscripten's memcmp is a byte loop, and a U64 loop
// made the compare of an 800x600 frame cost more than the copy it saves)
bool sameBytes(const U8* a, const U8* b, U32 n) {
	U32 i = 0;
#ifdef __wasm_simd128__
	for (; i + 64 <= n; i += 64) {
		v128_t d = wasm_v128_or(wasm_v128_xor(wasm_v128_load(a + i), wasm_v128_load(b + i)),
			wasm_v128_xor(wasm_v128_load(a + i + 16), wasm_v128_load(b + i + 16)));
		d = wasm_v128_or(d, wasm_v128_or(wasm_v128_xor(wasm_v128_load(a + i + 32), wasm_v128_load(b + i + 32)),
			wasm_v128_xor(wasm_v128_load(a + i + 48), wasm_v128_load(b + i + 48))));
		if (wasm_v128_any_true(d)) return false;
	}
#endif
	for (; i + 8 <= n; i += 8) {
		U64 x, y;
		memcpy(&x, a + i, 8);
		memcpy(&y, b + i, 8);
		if (x != y) return false;
	}
	for (; i < n; i++) {
		if (a[i] != b[i]) return false;
	}
	return true;
}

// guest bytes at src equal to the host bytes at dst
static bool sameAsGuest(KMemory* memory, U32 src, const U8* dst, U32 len) {
	bool same = true;
	memory->performOnMemory(src, len, true, [&same, &dst](U8* ram, U32 n) {
		same = sameBytes(ram, dst, n);
		dst += n;
		return same;
	});
	return same;
}

// Clips a copy to [0, limitW) x [0, limitH) at (x, y), moving the other side's corner with it. false = nothing left.
static bool clipCopy(S64& x, S64& y, S64& otherX, S64& otherY, S64& width, S64& height, U32 limitW, U32 limitH) {
	if (x < 0) { otherX -= x; width += x; x = 0; }
	if (y < 0) { otherY -= y; height += y; y = 0; }
	width = std::min<S64>(width, (S64)limitW - x);
	height = std::min<S64>(height, (S64)limitH - y);
	return width > 0 && height > 0;
}

int XDrawable::copyImageData(KThread* thread, const std::shared_ptr<XGC>& gc, U32 data, U32 bytes_per_line, S32 bits_per_pixel, S32 src_x, S32 src_y, S32 dst_x, S32 dst_y, U32 width, U32 height) {
	if (!this->data) { // no pixels: a PBuffer, or too large to have any (setSize)
		return Success;
	}
	if (bits_per_pixel != this->visual->bits_per_rgb) {
		return BadMatch;
	}
	if (gc && (gc->clip_rects.size() || gc->values.clip_mask || gc->values.clip_x_origin || gc->values.clip_y_origin)) {
		//klog("XDrawable::copyImageData clipping not implemented");
	}
	BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mutex);
	S64 srcX = src_x, srcY = src_y, dstX = dst_x, dstY = dst_y, copyW = width, copyH = height;
	if (!this->data || !clipCopy(dstX, dstY, srcX, srcY, copyW, copyH, w, h)) {
		return Success;
	}
	width = (U32)copyW;
	height = (U32)copyH;
	U32 src = data + bytes_per_line * (U32)srcY + (U32)((bits_per_pixel * srcX + 7) / 8);
	U8* dst = this->data + this->bytes_per_line * dstY + (bits_per_pixel * dstX + 7) / 8;
	KMemory* memory = thread->memory;
	U32 copyPerLine = (bits_per_pixel * width + 7) / 8;

	if (!gc || gc->values.function == GXcopy) {
		// Games that redraw every frame put the same picture again and again: rows equal to what the
		// drawable holds are not copied, and an image that changed nothing does not mark it dirty (no
		// composite, no present compare).
		bool changed = false;
		for (U32 y = 0; y < height; y++) {
			if (!memory->canRead(src, copyPerLine)) {
				if (changed) setDirty();
				return BadValue;
			}
			if (!sameAsGuest(memory, src, dst, copyPerLine)) {
				memory->memcpy(dst, src, copyPerLine);
				changed = true;
			}
			src += bytes_per_line;
			dst += this->bytes_per_line;
		}
		if (changed) setDirty();
		return Success;
	} else if (gc->values.function == GXxor && bits_per_pixel == 32) {
		for (U32 y = 0; y < height; y++) {
			if (!memory->canRead(src, copyPerLine)) {
				return BadValue;
			}
			U32* dstPixel = (U32*)dst;
			for (U32 x = 0; x < width; x++) {
				dstPixel[x] ^= memory->readd(src + x * 4);
			}			
			src += bytes_per_line;
			dst += this->bytes_per_line;
		}
	}
	setDirty();
	return Success;
}

static U32 readPixel(const U8* row, U32 x, U32 bits);

int XDrawable::copy(KThread* thread,const std::shared_ptr<XGC>& gc, const std::shared_ptr<XDrawable>& srcDrawable, S32 srcX, S32 srcY, U32 width, U32 height, S32 dstX, S32 dstY) {
	if (!data || !srcDrawable->data) { // no pixels: a PBuffer, or too large to have any (setSize)
		return Success;
	}
	if (srcDrawable->visual->bits_per_rgb != this->visual->bits_per_rgb) {
		return BadMatch;
	}
	if (gc && (gc->clip_rects.size() || gc->values.clip_mask || gc->values.clip_x_origin || gc->values.clip_y_origin)) {
		//klog("XDrawable::copyImageData clipping not implemented");
	}
	// Lower id first so two copies between the same pair can't deadlock. The mutex is recursive: a copy within one drawable is fine.
	XDrawable* first = id < srcDrawable->id ? this : srcDrawable.get();
	XDrawable* second = first == this ? srcDrawable.get() : this;
	BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(first->mutex);
	{
		BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(second->mutex);
		// The rectangle must lie in both drawables; a PBuffer or an empty drawable has no pixels.
		S64 sx = srcX, sy = srcY, dx = dstX, dy = dstY, copyW = width, copyH = height;
		if (!data || !srcDrawable->data || !clipCopy(dx, dy, sx, sy, copyW, copyH, w, h) || !clipCopy(sx, sy, dx, dy, copyW, copyH, srcDrawable->w, srcDrawable->h)) {
			return Success;
		}
		width = (U32)copyW;
		height = (U32)copyH;
		U32 bits = this->visual->bits_per_rgb;

		if (bits == 1) {
			// Bitmaps (cursor masks: winex11 builds them with GXandReverse/GXorReverse/GXand/GXor copies at 1-pixel
			// offsets): byte copies would ignore the GC function and shift by whole bytes. Pixel by pixel through the
			// GC, from a copy of the source rows so a copy within one bitmap reads the original bits.
			std::vector<U8> rows(srcDrawable->data + srcDrawable->bytes_per_line * sy, srcDrawable->data + srcDrawable->bytes_per_line * (sy + height));
			for (U32 y = 0; y < height; y++) {
				for (U32 x = 0; x < width; x++) {
					drawPixel(*gc, (S32)dx + x, (S32)dy + y, readPixel(rows.data() + y * srcDrawable->bytes_per_line, (U32)sx + x, 1));
				}
			}
		} else {
			U8* src = srcDrawable->data + srcDrawable->bytes_per_line * sy + (bits * sx + 7) / 8;
			U8* dst = data + bytes_per_line * dy + (bits * dx + 7) / 8;
			S64 srcStep = srcDrawable->bytes_per_line, dstStep = bytes_per_line;
			if (srcDrawable.get() == this && dy > sy) {
				// Moving down within one drawable: from the bottom up, or a row overwrites one not yet read.
				src += srcStep * (height - 1);
				dst += dstStep * (height - 1);
				srcStep = -srcStep;
				dstStep = -dstStep;
			}
			U32 copyPerLine = (bits * width + 7) / 8;
			for (U32 y = 0; y < height; y++) {
				memmove(dst, src, copyPerLine);
				src += srcStep;
				dst += dstStep;
			}
		}
	}
	setDirty();
	return Success;
}

static U32 readPixel(const U8* row, U32 x, U32 bits) {
	if (bits == 1) return (row[x / 8] >> (x & 7)) & 1;
	U32 result = 0;
	memcpy(&result, row + x * (bits / 8), bits / 8);
	return result;
}

static void writePixel(U8* row, U32 x, U32 bits, U32 value) {
	if (bits == 1) {
		U8 mask = (U8)(1 << (x & 7));
		row[x / 8] = (row[x / 8] & ~mask) | ((value & 1) << (x & 7));
	} else {
		memcpy(row + x * (bits / 8), &value, bits / 8);
	}
}

void XDrawable::drawPixel(const XGC& gc, S32 x, S32 y, U32 src) {
	XPixmapPtr mask;
	if (gc.values.clip_mask) mask = XServer::getServer()->getPixmap(gc.values.clip_mask);
	drawPixel(gc, x, y, src, mask.get());
}

void XDrawable::drawPixel(const XGC& gc, S32 x, S32 y, U32 src, const XDrawable* mask) {
	if (!data || x < 0 || y < 0 || (U32)x >= w || (U32)y >= h) return;
	S64 cx = (S64)x - gc.values.clip_x_origin, cy = (S64)y - gc.values.clip_y_origin;
	if (gc.hasClipRects) {
		bool visible = false;
		for (const auto& rect : gc.clip_rects) {
			if (cx >= rect.x && cy >= rect.y && cx < rect.x + rect.width && cy < rect.y + rect.height) {
				visible = true;
				break;
			}
		}
		if (!visible) return;
	}
	if (gc.values.clip_mask) {
		if (!mask || !mask->getData() || cx < 0 || cy < 0 || cx >= mask->width() || cy >= mask->height() ||
			!(mask->getData()[cy * mask->getBytesPerLine() + cx / 8] & (1 << (cx & 7)))) return;
	}
	U8* row = data + y * bytes_per_line;
	U32 dst = readPixel(row, x, visual->bits_per_rgb), result = src;
	switch (gc.values.function) {
	case GXclear: result = 0; break;
	case GXand: result = src & dst; break;
	case GXandReverse: result = src & ~dst; break;
	case GXcopy: break;
	case GXandInverted: result = ~src & dst; break;
	case GXnoop: return;
	case GXxor: result = src ^ dst; break;
	case GXor: result = src | dst; break;
	case GXnor: result = ~(src | dst); break;
	case GXequiv: result = ~(src ^ dst); break;
	case GXinvert: result = ~dst; break;
	case GXorReverse: result = src | ~dst; break;
	case GXcopyInverted: result = ~src; break;
	case GXorInverted: result = ~src | dst; break;
	case GXnand: result = ~(src & dst); break;
	case GXset: result = 0xffffffff; break;
	}
	writePixel(row, x, visual->bits_per_rgb, (dst & ~gc.values.plane_mask) | (result & gc.values.plane_mask));
}

int XDrawable::drawLine(KThread* thread, const std::shared_ptr<XGC>& gc, S32 x1, S32 y1, S32 x2, S32 y2) {
	U32 bits = visual->bits_per_rgb;
	if (bits != 1 && bits != 8 && bits != 16 && bits != 32) return BadMatch;
	if (x1 == x2 || y1 == y2) {
		bool dashed = gc->values.line_style != LineSolid;
		U64 cycle = 0;
		if (dashed) {
			for (U8 length : gc->dashList) {
				if (!length) return BadValue;
				cycle += length;
			}
			if (!cycle) return BadValue;
		}
		BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mutex);
		S32 left = std::max<S32>(0, std::min(x1, x2));
		S32 top = std::max<S32>(0, std::min(y1, y2));
		S32 right = std::min<S32>(w - 1, std::max(x1, x2));
		S32 bottom = std::min<S32>(h - 1, std::max(y1, y2));
		for (S32 y = top; y <= bottom; y++) {
			for (S32 x = left; x <= right; x++) {
				if (gc->values.cap_style == CapNotLast && x == x2 && y == y2) continue;
				U32 source = gc->values.foreground;
				if (dashed) {
					U64 distance = x1 == x2 ? (U64)std::abs((S64)y - y1) : (U64)std::abs((S64)x - x1);
					U64 phase = (distance + (U32)gc->values.dash_offset) % cycle;
					size_t dash = 0;
					while (phase >= gc->dashList[dash]) phase -= gc->dashList[dash++];
					if (dash & 1) {
						if (gc->values.line_style == LineOnOffDash) continue;
						source = gc->values.background;
					}
				}
				drawPixel(*gc, x, y, source);
			}
		}
		setDirty();
		return Success;
	}
	klog("XDrawable::drawLine diag line not supported");
	return Success;
}

int XDrawable::fillRectangle(KThread* thread, const std::shared_ptr<XGC>& gc, S32 x, S32 y, U32 width, U32 height) {
	U32 bits = visual->bits_per_rgb;
	if (bits != 1 && bits != 8 && bits != 16 && bits != 32) return BadMatch;
	S32 style = gc->values.fill_style;
	if (style < FillSolid || style > FillOpaqueStippled) return BadValue;
	bool stippled = style == FillStippled || style == FillOpaqueStippled;
	XDrawablePtr pattern = style == FillTiled ? gc->tile : stippled ? gc->stipple : nullptr;
	// The default tile is black; the default stipple is all ones. A configured
	// tile must not affect FillSolid (Wine reuses GCs between brush styles).
	if (!pattern && ((style == FillTiled && gc->values.tile) || (stippled && gc->values.stipple))) return BadPixmap;
	U32 patternWidth = 0, patternHeight = 0, patternPitch = 0, patternBits = 0;
	std::vector<U8> pixels;
	if (pattern) {
		// Snapshot before locking the destination, including when it is also
		// the pattern. This avoids opposite-direction drawable lock ordering.
		BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pattern->mutex);
		if ((stippled && (pattern->depth != 1 || pattern->visual->bits_per_rgb != 1)) || (!stippled && (pattern->depth != depth || pattern->visual->bits_per_rgb != bits))) return BadMatch;
		if (!pattern->data || !pattern->w || !pattern->h) return BadPixmap;
		patternWidth = pattern->w; patternHeight = pattern->h;
		patternPitch = pattern->bytes_per_line; patternBits = pattern->visual->bits_per_rgb;
		pixels.assign(pattern->data, pattern->data + pattern->size);
	}
	BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mutex);
	S32 left = std::max<S32>(0, x), top = std::max<S32>(0, y);
	S32 right = (S32)std::min<S64>(w, (S64)x + width);
	S32 bottom = (S32)std::min<S64>(h, (S64)y + height);
	if (!data || left >= right || top >= bottom) return Success;
	bool simpleCopy = style == FillSolid && gc->values.function == GXcopy && gc->values.plane_mask == 0xffffffff && !gc->hasClipRects && !gc->values.clip_mask;
	XPixmapPtr clipMask; // looked up once, not for every pixel
	if (gc->values.clip_mask) clipMask = XServer::getServer()->getPixmap(gc->values.clip_mask);
	for (S32 dstY = top; dstY < bottom; dstY++) {
		if (simpleCopy && bits == 32) {
			U32* row = (U32*)(data + dstY * bytes_per_line);
			std::fill(row + left, row + right, gc->values.foreground);
			continue;
		}
		if (simpleCopy && bits == 8) {
			memset(data + dstY * bytes_per_line + left, (U8)gc->values.foreground, right - left);
			continue;
		}
		for (S32 dstX = left; dstX < right; dstX++) {
			U32 source = style == FillTiled ? 0 : gc->values.foreground;
			if (pattern) {
				// X11 origins are relative to the drawable, not to this rectangle.
				S64 px = ((S64)dstX - gc->values.ts_x_origin) % patternWidth;
				S64 py = ((S64)dstY - gc->values.ts_y_origin) % patternHeight;
				if (px < 0) px += patternWidth;
				if (py < 0) py += patternHeight;
				U32 pixel = readPixel(pixels.data() + py * patternPitch, (U32)px, patternBits);
				if (stippled) {
					if (!pixel && style == FillStippled) continue;
					source = pixel ? gc->values.foreground : gc->values.background;
				} else source = pixel;
			}
			drawPixel(*gc, dstX, dstY, source, clipMask.get());
		}
	}
	setDirty();
	return Success;
}

int XDrawable::fillPolygon(KThread* thread, const std::shared_ptr<XGC>& gc, const std::vector<XPoint>& points) {
	if (gc->values.fill_rule != EvenOddRule && gc->values.fill_rule != WindingRule) return BadValue;
	if (points.size() < 3) return Success;
	S32 top = points[0].y, bottom = top;
	for (const auto& point : points) {
		top = std::min<S32>(top, point.y);
		bottom = std::max<S32>(bottom, point.y);
	}
	top = std::max<S32>(0, top);
	bottom = std::min<S32>(h, bottom);
	struct Crossing { S32 x, direction; };
	std::vector<Crossing> crossings;
	crossings.reserve(points.size());
	for (S32 y = top; y < bottom; y++) {
		crossings.clear();
		for (size_t i = 0; i < points.size(); i++) {
			XPoint a = points[i], b = points[(i + 1) % points.size()];
			S32 direction = a.y < b.y ? 1 : -1;
			if (a.y > b.y) std::swap(a, b);
			// X11 includes the top/left boundary and excludes bottom/right.
			// Evaluate at integer pixel centers, with each bottom endpoint excluded.
			if (y < a.y || y >= b.y) continue;
			S64 dy = b.y - a.y;
			S64 numerator = (S64)a.x * dy + (S64)(y - a.y) * (b.x - a.x);
			S32 x = (S32)(numerator >= 0 ? (numerator + dy - 1) / dy : numerator / dy);
			crossings.push_back({x, direction});
		}
		std::sort(crossings.begin(), crossings.end(), [](const Crossing& a, const Crossing& b) { return a.x < b.x; });
		S32 winding = 0, left = 0;
		for (size_t i = 0; i < crossings.size();) {
			S32 x = crossings[i].x;
			bool wasInside = winding != 0;
			do {
				if (gc->values.fill_rule == EvenOddRule) winding ^= 1;
				else winding += crossings[i].direction;
				i++;
			} while (i < crossings.size() && crossings[i].x == x);
			if (!wasInside && winding) left = x;
			else if (wasInside && !winding) {
				int result = fillRectangle(thread, gc, left, y, x - left, 1);
				if (result != Success) return result;
			}
		}
	}
	return Success;
}

int XDrawable::drawRectangle(KThread* thread, const std::shared_ptr<XGC>& gc, S32 x, S32 y, U32 width, U32 height) {
	if (visual->bits_per_rgb != 8 && (gc->clip_rects.size() || gc->values.clip_mask || gc->values.clip_x_origin || gc->values.clip_y_origin)) {
		klog("XDrawable::drawRectangle clipping not implemented");
	}
	// draw rectangle with no overlapping pixels
	drawLine(thread, gc, x, y, x + width, y); // top (includes left and right)
	drawLine(thread, gc, x + width, y + 1, x + width, y + height); // right	( includes bottom but not top)
	drawLine(thread, gc, x, y + 1, x, y + height); // left (includes bottom but not top)
	drawLine(thread, gc, x + 1, y + height, x + width - 1, y + height); // bottom (does not include left and right)
	return Success;
}
