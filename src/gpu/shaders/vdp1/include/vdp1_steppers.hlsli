// Brimir: 32-bit ports of the VDP1 steppers (src/core/include/ymir/hw/vdp/renderer/common/vdp1_steppers.hpp:
// LineStepper<13>, TextureStepper, GouraudChannelStepper), written from the CPU reference's ports
// (tests/unit/vdp1_raster_reference.hpp, namespace port), which tests/unit/test_vdp1_raster_reference.cpp checks against
// the steppers. Brimir only (not derived from Ymir's shaders, which jump with SetStep).
//
// Every operation is the reference's: int arithmetic wraps modulo 2^32 (SPIR-V integer add, sub, mul and shift left are
// modular, as the reference's Add/Sub/Mul/Shl), comparisons are signed where the reference's are, and the line is
// stepped one pixel at a time (no SetStep jumps). LineSystemClip includes master's stepping fallback for the lines whose
// minor-step loop never ends (vdp1_steppers.hpp, 1a769b1).
#ifndef BRIMIR_VDP1_STEPPERS_HLSLI
#define BRIMIR_VDP1_STEPPERS_HLSLI

static const uint kLineShift = 32 - 13;
static const int kIntMin = int(0x80000000u);

// ---------------------------------------------------------------------------------------------------------------------
// LineStepper<13>

struct LineStepper {
    int num, den, accum, accumTarget;
    int xMajInc, yMajInc, xMinInc, yMinInc;
    int x, y, xEnd, yEnd;
    uint dmaj, step, length;
    int aaXInc, aaYInc;
    bool antiAlias;
};

LineStepper MakeLineStepper(int x1, int y1, int x2, int y2, bool aa) {
    LineStepper l;
    l.x = x1;
    l.y = y1;
    l.xEnd = x2;
    l.yEnd = y2;
    l.antiAlias = aa;
    l.aaXInc = 0;
    l.aaYInc = 0;

    int dx = x2 - x1;
    int dy = y2 - y1;
    int adx = abs(dx);
    int ady = abs(dy);
    l.dmaj = uint(max(adx, ady));
    l.length = l.dmaj + 1;
    l.step = 0;

    const bool xMajor = adx >= ady;
    if (xMajor) {
        l.xMajInc = dx >= 0 ? 1 : -1;
        l.yMajInc = 0;
        l.xMinInc = 0;
        l.yMinInc = dy >= 0 ? 1 : -1;
    } else {
        l.xMajInc = 0;
        l.yMajInc = dy >= 0 ? 1 : -1;
        l.xMinInc = dx >= 0 ? 1 : -1;
        l.yMinInc = 0;
        const int t = dx;
        dx = dy;
        dy = t;
        const int at = adx;
        adx = ady;
        ady = at;
    }
    l.num = ady << 1;
    l.den = adx << 1;
    l.accum = adx + 1;
    l.accumTarget = 0;
    if (!aa && dx < 0) {
        l.accumTarget += 1;
    }
    l.accum += l.num;

    l.x -= l.xMajInc;
    l.y -= l.yMajInc;

    if (aa) {
        l.accum -= 1;
        l.accumTarget -= 1;
        const bool samesign = (x1 > x2) == (y1 > y2);
        if (xMajor) {
            l.aaXInc = samesign ? 0 : -l.xMajInc;
            l.aaYInc = samesign ? -l.yMinInc : 0;
        } else {
            l.aaXInc = samesign ? 0 : -l.xMinInc;
            l.aaYInc = samesign ? -l.yMajInc : 0;
        }
    }

    l.num = l.num << kLineShift;
    l.den = l.den << kLineShift;
    l.accum = l.accum << kLineShift;
    l.accumTarget = l.accumTarget << kLineShift;
    return l;
}

// Returns true if an antialias pixel is drawn at this step
bool LineStep(inout LineStepper l) {
    l.step += 1;
    l.x += l.xMajInc;
    l.y += l.yMajInc;
    l.accum -= l.num;
    if (l.accum <= l.accumTarget) {
        l.accum += l.den;
        l.x += l.xMinInc;
        l.y += l.yMinInc;
        return l.antiAlias;
    }
    return false;
}

bool LineCanStep(LineStepper l) {
    return l.step <= l.length;
}

// The minor-step loop never ends: den is 2^31 and both accumulator values are <= accumTarget
bool LineMinorLoopNeverEnds(LineStepper l, int acc) {
    return l.den == kIntMin && (acc + l.den) <= l.accumTarget;
}

bool LineInsideBox(int x, int y, int lo, int maxX, int maxY) {
    return x >= lo && x <= maxX && y >= lo && y <= maxY;
}

// Master's rule for those lines: step a copy over all dmaj + 1 pixels; keep the steps from the first to the last whose
// pixel (main, or antialias when drawn) lies in [lo, maxX] x [lo, maxY]; none: fully clipped. Returns the steps skipped.
uint LineClipByStepping(inout LineStepper l, int lo, int maxX, int maxY) {
    LineStepper probe = l;
    bool found = false;
    uint first = 0;
    uint last = 0;
    for (uint i = 0; i <= l.dmaj; ++i) {
        const bool aa = LineStep(probe);
        if (LineInsideBox(probe.x, probe.y, lo, maxX, maxY) ||
            (aa && LineInsideBox(probe.x + probe.aaXInc, probe.y + probe.aaYInc, lo, maxX, maxY))) {
            first = found ? first : i;
            found = true;
            last = i;
        }
        if (i == 0xFFFFFFFFu) {
            break;
        }
    }
    if (!found) {
        l.x = l.xEnd;
        l.y = l.yEnd;
        l.length = 0;
        return 0;
    }
    for (uint j = 0; j < first; ++j) {
        LineStep(l);
    }
    l.step = 0;
    l.length = last - first + 1;
    return first;
}

// LineStepper::SystemClip: clips to 0..width x 0..height; returns the steps skipped at the start
uint LineSystemClip(inout LineStepper l, int width, int height) {
    const int kPadding = 1;
    width += kPadding + 1;
    height += kPadding + 1;

    uint len = l.dmaj;
    if (len == 0) {
        return 0;
    }

    const int xs = l.x;
    const int ys = l.y;
    const int xe = l.xEnd;
    const int ye = l.yEnd;
    if (xs >= -kPadding && xs <= width && ys >= -kPadding && ys <= height && xe >= -kPadding && xe <= width &&
        ye >= -kPadding && ye <= height) {
        return 0;
    }
    if ((xs < -kPadding && xe < -kPadding) || (xs > width && xe > width) || (ys < -kPadding && ye < -kPadding) ||
        (ys > height && ye > height)) {
        l.x = xe;
        l.y = ye;
        l.length = 0;
        return 0;
    }

    const bool xmajor = l.xMajInc != 0;
    const int inc = l.xMajInc + l.yMajInc;
    const int start = xmajor ? xs : ys;
    const int end = xmajor ? xe : ye;
    const int maxLen = xmajor ? width : height;
    const int startAccum = l.accum;
    bool fallback = false;

    uint startClip = 0;
    if (inc > 0 && start < -kPadding) {
        startClip = uint(-start - 1 - kPadding);
    } else if (inc < 0 && start > maxLen) {
        startClip = uint(start - maxLen);
    }
    if (startClip > 0) {
        startClip = min(startClip, len - 1);
        l.x += int(uint(l.xMajInc) * startClip);
        l.y += int(uint(l.yMajInc) * startClip);
        for (uint i = 0; i < startClip && !fallback; ++i) {
            l.accum -= l.num;
            if (l.den != 0) {
                while (l.accum <= l.accumTarget) {
                    if (LineMinorLoopNeverEnds(l, l.accum)) {
                        fallback = true;
                        break;
                    }
                    l.accum += l.den;
                    l.x += l.xMinInc;
                    l.y += l.yMinInc;
                }
            }
        }
        if (!fallback) {
            len -= startClip;
        }
    }

    if (!fallback) {
        uint endClip = 0;
        if (inc < 0 && end < -kPadding) {
            endClip = uint(-end - 1 - kPadding);
        } else if (inc > 0 && end > maxLen) {
            endClip = uint(end - maxLen);
        }
        endClip = min(endClip, len);
        if (endClip > 0) {
            len -= endClip;
            // The end loop only moves the (unused) end point; it matters only when it never ends (den = 2^31)
            if (l.den == kIntMin) {
                int tempAccum = l.accum;
                for (uint i = 0; i < len && !fallback; ++i) {
                    tempAccum -= l.num;
                    while (tempAccum <= l.accumTarget) {
                        if (LineMinorLoopNeverEnds(l, tempAccum)) {
                            fallback = true;
                            break;
                        }
                        tempAccum += l.den;
                    }
                }
            }
        }
        if (!fallback) {
            l.length = len + 1;
            return startClip;
        }
    }

    l.x = xs;
    l.y = ys;
    l.accum = startAccum;
    return LineClipByStepping(l, -kPadding, width, height);
}

// ---------------------------------------------------------------------------------------------------------------------
// TextureStepper

struct TextureStepper {
    int num, den, accum, value, inc;
};

TextureStepper MakeTextureStepper(uint len, int start, int end, bool hss, int hssSelect) {
    TextureStepper t;
    if (hss) {
        start >>= 1;
        end >>= 1;
    }
    const int delta = end - start;
    const uint absDelta = uint(abs(delta));

    t.value = start;
    t.inc = delta >= 0 ? 1 : -1;
    if (hss) {
        t.value = (t.value << 1) | hssSelect;
        t.inc = t.inc << 1;
    }

    t.num = int(absDelta);
    t.den = int(len);
    if (len <= absDelta) {
        t.num += 1;
        t.accum = int(absDelta - (len << 1));
        if (delta >= 0) {
            t.accum += 1;
        }
    } else {
        t.den -= 1;
        t.accum = int(len - (len << 1));
        if (delta < 0) {
            t.accum += 1;
        }
    }
    t.num = t.num << 1;
    t.den = t.den << 1;
    return t;
}

bool TextureShouldStepTexel(TextureStepper t) {
    return t.accum >= 0;
}
void TextureStepTexel(inout TextureStepper t) {
    t.value += t.inc;
    t.accum -= t.den;
}
void TextureStepPixel(inout TextureStepper t) {
    t.accum += t.num;
}
void TextureSkipPixels(inout TextureStepper t, uint count) {
    t.accum += int(uint(t.num) * count);
}

// ---------------------------------------------------------------------------------------------------------------------
// GouraudChannelStepper (one channel; GouraudStepper is three of them)

struct GouraudChannel {
    int num, den, accum, value, intInc, fracInc;
};

GouraudChannel MakeGouraudChannel(uint len, uint start, uint end) {
    GouraudChannel g;
    const int delta = int(end) - int(start);
    const uint absDelta = uint(abs(delta));

    g.value = int(start);
    g.intInc = 0;
    g.fracInc = delta >= 0 ? 1 : -1;

    g.num = int(absDelta);
    g.den = int(len);
    if (len <= absDelta) {
        g.num += 1;
        g.accum = int(absDelta - (len << 1));
        if (delta >= 0) {
            g.accum += 1;
        }
    } else {
        g.den -= 1;
        g.accum = int(0u - len);
        if (delta < 0) {
            g.accum += 1;
        }
    }
    g.num = g.num << 1;
    g.den = g.den << 1;

    if (g.den != 0) {
        while (g.accum >= 0) {
            g.value += g.fracInc;
            g.accum -= g.den;
        }
        while (g.num >= g.den) {
            g.intInc += g.fracInc;
            g.num -= g.den;
        }
    }
    g.accum = ~g.accum;
    return g;
}

void GouraudChannelStep(inout GouraudChannel g) {
    g.value += g.intInc;
    g.accum -= g.num;
    if (g.accum < 0) {
        g.value += g.fracInc;
        g.accum += g.den;
    }
}

void GouraudChannelSkip(inout GouraudChannel g, uint steps) {
    g.value += int(uint(g.intInc) * steps);
    g.accum -= int(uint(g.num) * steps);
    if (g.den != 0) {
        while (g.accum < 0) {
            g.value += g.fracInc;
            g.accum += g.den;
        }
    }
}

// clamp(value + color - 16, 0, 31)
uint GouraudChannelBlend(GouraudChannel g, uint color) {
    return uint(clamp(g.value + int(color) - 16, 0, 31));
}

struct GouraudStepper {
    GouraudChannel r, g, b;
};

// start and end are Color555 words
GouraudStepper MakeGouraudStepper(uint len, uint start, uint end) {
    GouraudStepper s;
    s.r = MakeGouraudChannel(len, start & 0x1F, end & 0x1F);
    s.g = MakeGouraudChannel(len, (start >> 5) & 0x1F, (end >> 5) & 0x1F);
    s.b = MakeGouraudChannel(len, (start >> 10) & 0x1F, (end >> 10) & 0x1F);
    return s;
}

void GouraudStep(inout GouraudStepper s) {
    GouraudChannelStep(s.r);
    GouraudChannelStep(s.g);
    GouraudChannelStep(s.b);
}

void GouraudSkip(inout GouraudStepper s, uint steps) {
    if (steps > 0) {
        GouraudChannelSkip(s.r, steps);
        GouraudChannelSkip(s.g, steps);
        GouraudChannelSkip(s.b, steps);
    }
}

// GouraudStepper::Blend: each channel blended, the MSB of the base kept
uint GouraudBlend(GouraudStepper s, uint base) {
    return GouraudChannelBlend(s.r, base & 0x1F) | (GouraudChannelBlend(s.g, (base >> 5) & 0x1F) << 5) |
           (GouraudChannelBlend(s.b, (base >> 10) & 0x1F) << 10) | (base & 0x8000);
}

#endif
