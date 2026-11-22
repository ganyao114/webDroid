package android.graphics;

import java.util.Arrays;

public class Path {
    public enum Direction { CW, CCW }
    public enum FillType { WINDING, EVEN_ODD, INVERSE_WINDING, INVERSE_EVEN_ODD }
    private static final int STRIDE = 7;
    private static final int MOVE = 1, LINE = 2, QUAD = 3, CUBIC = 4, CLOSE = 5;
    private static final float KAPPA = 0.55228475f;
    float[] verbs = new float[STRIDE * 8];
    int size;
    private FillType fillType = FillType.WINDING;
    private float x, y, contourX, contourY;
    private boolean needsMove = true;
    private RectF roundClip;
    private float clipRx, clipRy;

    public Path() {}
    public Path(Path source) { if (source != null) set(source); }
    public void set(Path source) {
        if (source == this) return;
        reserve(source.size);
        System.arraycopy(source.verbs, 0, verbs, 0, source.size * STRIDE);
        size = source.size;
        x = source.x; y = source.y; contourX = source.contourX; contourY = source.contourY;
        needsMove = source.needsMove; fillType = source.fillType;
        roundClip = source.roundClip == null ? null : new RectF(source.roundClip);
        clipRx = source.clipRx; clipRy = source.clipRy;
    }
    public void reset() { rewind(); }
    public void rewind() { size = 0; x = y = contourX = contourY = 0; needsMove = true; roundClip = null; }
    public boolean isEmpty() {
        for (int i = 0; i < size; ++i) {
            float verb = verbs[i * STRIDE];
            if (verb != MOVE && verb != CLOSE) return false;
        }
        return true;
    }
    public FillType getFillType() { return fillType; }
    public void setFillType(FillType fill) { fillType = java.util.Objects.requireNonNull(fill); }
    public boolean isInverseFillType() { return fillType.ordinal() >= 2; }
    public void toggleInverseFillType() { fillType = FillType.values()[fillType.ordinal() ^ 2]; }
    public void incReserve(int extra) { if (extra > 0) reserve(size + extra); }
    private void reserve(int capacity) {
        if (capacity <= verbs.length / STRIDE) return;
        float[] expanded = new float[Math.max(capacity * STRIDE, verbs.length * 2)];
        System.arraycopy(verbs, 0, expanded, 0, size * STRIDE);
        verbs = expanded;
    }
    private void add(int verb, float a, float b, float c, float d, float e, float f) {
        roundClip = null;
        reserve(size + 1);
        int at = size++ * STRIDE;
        verbs[at] = verb;
        verbs[at + 1] = a; verbs[at + 2] = b;
        verbs[at + 3] = c; verbs[at + 4] = d;
        verbs[at + 5] = e; verbs[at + 6] = f;
    }
    private void beginIfNeeded() { if (needsMove) moveTo(x, y); }
    public void moveTo(float x, float y) {
        add(MOVE, x, y, 0, 0, 0, 0);
        this.x = contourX = x;
        this.y = contourY = y;
        needsMove = false;
    }
    public void rMoveTo(float dx, float dy) { moveTo(x + dx, y + dy); }
    public void lineTo(float x, float y) {
        beginIfNeeded();
        add(LINE, x, y, 0, 0, 0, 0);
        this.x = x; this.y = y;
    }
    public void rLineTo(float dx, float dy) { lineTo(x + dx, y + dy); }
    public void quadTo(float x1, float y1, float x2, float y2) {
        beginIfNeeded();
        add(QUAD, x1, y1, x2, y2, 0, 0);
        x = x2; y = y2;
    }
    public void rQuadTo(float dx1, float dy1, float dx2, float dy2) { quadTo(x + dx1, y + dy1, x + dx2, y + dy2); }
    public void cubicTo(float x1, float y1, float x2, float y2, float x3, float y3) {
        beginIfNeeded();
        add(CUBIC, x1, y1, x2, y2, x3, y3);
        x = x3; y = y3;
    }
    public void rCubicTo(float dx1, float dy1, float dx2, float dy2, float dx3, float dy3) {
        cubicTo(x + dx1, y + dy1, x + dx2, y + dy2, x + dx3, y + dy3);
    }
    public void close() {
        if (needsMove) return;
        add(CLOSE, 0, 0, 0, 0, 0, 0);
        x = contourX; y = contourY;
        needsMove = true;
    }
    public void addRect(RectF rect, Direction direction) { addRect(rect.left, rect.top, rect.right, rect.bottom, direction); }
    public void addRect(float l, float t, float r, float b, Direction direction) {
        moveTo(l, t);
        if (direction == Direction.CW) { lineTo(r, t); lineTo(r, b); lineTo(l, b); }
        else { lineTo(l, b); lineTo(r, b); lineTo(r, t); }
        close();
    }
    public void addOval(RectF oval, Direction direction) { addOval(oval.left, oval.top, oval.right, oval.bottom, direction); }
    public void addOval(float l, float t, float r, float b, Direction direction) {
        addRoundRect(l, t, r, b, (r - l) * 0.5f, (b - t) * 0.5f, direction);
    }
    public void addCircle(float x, float y, float radius, Direction direction) {
        if (radius > 0) addOval(x - radius, y - radius, x + radius, y + radius, direction);
    }
    public void addRoundRect(RectF rect, float rx, float ry, Direction direction) {
        addRoundRect(rect.left, rect.top, rect.right, rect.bottom, rx, ry, direction);
    }
    public void addRoundRect(float l, float t, float r, float b, float rx, float ry, Direction direction) {
        if (l >= r || t >= b) return;
        rx = Math.max(0, Math.min(rx, (r - l) * 0.5f));
        ry = Math.max(0, Math.min(ry, (b - t) * 0.5f));
        appendRoundRect(l, t, r, b, new float[] {rx, ry, rx, ry, rx, ry, rx, ry}, direction);
    }
    public void addRoundRect(RectF rect, float[] radii, Direction direction) {
        addRoundRect(rect.left, rect.top, rect.right, rect.bottom, radii, direction);
    }
    public void addRoundRect(float l, float t, float r, float b, float[] radii, Direction direction) {
        if (radii.length < 8) throw new ArrayIndexOutOfBoundsException("Eight corner radii are required");
        if (l >= r || t >= b) return;
        float[] corners = Arrays.copyOf(radii, 8);
        for (int i = 0; i < corners.length; i += 2) {
            if (!(corners[i] > 0 && corners[i + 1] > 0
                    && Float.isFinite(corners[i]) && Float.isFinite(corners[i + 1]))) {
                corners[i] = corners[i + 1] = 0;
            }
        }
        float scale = 1;
        scale = Math.min(scale, (r - l) / (corners[0] + corners[2]));
        scale = Math.min(scale, (r - l) / (corners[4] + corners[6]));
        scale = Math.min(scale, (b - t) / (corners[1] + corners[7]));
        scale = Math.min(scale, (b - t) / (corners[3] + corners[5]));
        if (scale < 1) for (int i = 0; i < corners.length; i++) corners[i] *= scale;
        appendRoundRect(l, t, r, b, corners, direction);
    }
    private void appendRoundRect(float l, float t, float r, float b, float[] corners, Direction direction) {
        boolean solitary = size == 0;
        float tlX = corners[0], tlY = corners[1], trX = corners[2], trY = corners[3];
        float brX = corners[4], brY = corners[5], blX = corners[6], blY = corners[7];
        float tangent = 1 - KAPPA;
        moveTo(l + tlX, t);
        if (direction == Direction.CW) {
            lineTo(r - trX, t); cubicTo(r - trX * tangent, t, r, t + trY * tangent, r, t + trY);
            lineTo(r, b - brY); cubicTo(r, b - brY * tangent, r - brX * tangent, b, r - brX, b);
            lineTo(l + blX, b); cubicTo(l + blX * tangent, b, l, b - blY * tangent, l, b - blY);
            lineTo(l, t + tlY); cubicTo(l, t + tlY * tangent, l + tlX * tangent, t, l + tlX, t);
        } else {
            cubicTo(l + tlX * tangent, t, l, t + tlY * tangent, l, t + tlY); lineTo(l, b - blY);
            cubicTo(l, b - blY * tangent, l + blX * tangent, b, l + blX, b); lineTo(r - brX, b);
            cubicTo(r - brX * tangent, b, r, b - brY * tangent, r, b - brY); lineTo(r, t + trY);
            cubicTo(r, t + trY * tangent, r - trX * tangent, t, r - trX, t); lineTo(l + tlX, t);
        }
        close();
        if (solitary && tlX == trX && tlX == brX && tlX == blX && tlY == trY && tlY == brY && tlY == blY) {
            roundClip = new RectF(l, t, r, b);
            clipRx = tlX;
            clipRy = tlY;
        }
    }
    boolean clip(Canvas canvas) {
        if (roundClip == null || isInverseFillType())
            throw new UnsupportedOperationException("Canvas requires a rectangular or rounded path clip");
        return canvas.clipRoundRect(roundClip.left, roundClip.top, roundClip.right, roundClip.bottom, clipRx, clipRy);
    }
    public void addPath(Path path) { addPath(path, 0, 0); }
    public void addPath(Path path, Matrix matrix) {
        float[] points = new float[6];
        int end = path.size * STRIDE;
        for (int at = 0; at < end; at += STRIDE) {
            int verb = (int) path.verbs[at];
            int count = verb == CUBIC ? 3 : verb == QUAD ? 2 : verb == CLOSE ? 0 : 1;
            if (count != 0) {
                System.arraycopy(path.verbs, at + 1, points, 0, 2 * count);
                matrix.mapPoints(points, 0, points, 0, count);
            }
            switch (verb) {
                case MOVE: moveTo(points[0], points[1]); break;
                case LINE: lineTo(points[0], points[1]); break;
                case QUAD: quadTo(points[0], points[1], points[2], points[3]); break;
                case CUBIC: cubicTo(points[0], points[1], points[2], points[3], points[4], points[5]); break;
                case CLOSE: close(); break;
            }
        }
    }
    public void transform(Matrix matrix) { transform(matrix, this); }
    public void transform(Matrix matrix, Path dst) {
        if (dst == null) dst = this;
        Path source = dst == this ? new Path(this) : this;
        dst.rewind();
        dst.fillType = fillType;
        dst.addPath(source, matrix);
    }
    public void addPath(Path path, float dx, float dy) {
        int count = path.size;
        for (int index = 0; index < count; index++) {
            int i = index * STRIDE;
            float[] source = path.verbs;
            switch ((int) source[i]) {
                case MOVE: moveTo(source[i + 1] + dx, source[i + 2] + dy); break;
                case LINE: lineTo(source[i + 1] + dx, source[i + 2] + dy); break;
                case QUAD:
                    quadTo(source[i + 1] + dx, source[i + 2] + dy, source[i + 3] + dx, source[i + 4] + dy);
                    break;
                case CUBIC:
                    cubicTo(source[i + 1] + dx, source[i + 2] + dy, source[i + 3] + dx, source[i + 4] + dy,
                            source[i + 5] + dx, source[i + 6] + dy);
                    break;
                case CLOSE: close(); break;
            }
        }
    }
    public void offset(float dx, float dy) { offset(dx, dy, this); }
    public void offset(float dx, float dy, Path dst) {
        if (dst == null) dst = this;
        Path source = dst == this ? new Path(this) : this;
        dst.rewind();
        dst.fillType = fillType;
        dst.addPath(source, dx, dy);
    }
    public void computeBounds(RectF out, boolean exact) {
        if (isEmpty()) { out.setEmpty(); return; }
        float l = Float.POSITIVE_INFINITY, t = l, r = Float.NEGATIVE_INFINITY, b = r;
        for (int i = 0; i < size * STRIDE; i += STRIDE) {
            int verb = (int) verbs[i];
            int points = verb == CLOSE ? 0 : verb == CUBIC ? 3 : verb == QUAD ? 2 : 1;
            for (int j = 0; j < points; ++j) {
                float px = verbs[i + 2 * j + 1], py = verbs[i + 2 * j + 2];
                l = Math.min(l, px); t = Math.min(t, py); r = Math.max(r, px); b = Math.max(b, py);
            }
        }
        out.set(l, t, r, b);
    }

    public float[] approximate(float acceptableError) {
        if (!(acceptableError >= 0)) throw new IllegalArgumentException("Negative path error");
        float[] samples = new float[Math.max(6, size * 3)];
        int count = 0;
        float px = 0, py = 0, startX = 0, startY = 0, length = 0;
        for (int at = 0; at < size * STRIDE; at += STRIDE) {
            int verb = (int) verbs[at];
            int end = verb == CUBIC ? at + 5 : verb == QUAD ? at + 3 : at + 1;
            float toX = verb == CLOSE ? startX : verbs[end], toY = verb == CLOSE ? startY : verbs[end + 1];
            float fromX = px, fromY = py;
            if (verb == MOVE) { startX = toX; startY = toY; }
            int steps = verb == QUAD || verb == CUBIC
                    ? Math.min(256, Math.max(8, (int) Math.ceil(1 / Math.sqrt(acceptableError)))) : 1;
            if (count + steps * 3 > samples.length)
                samples = java.util.Arrays.copyOf(samples, Math.max(count + steps * 3, samples.length * 2));
            for (int step = 1; step <= steps; step++) {
                float t = (float) step / steps, u = 1 - t;
                float sx = toX, sy = toY;
                if (step < steps) {
                    if (verb == QUAD) {
                        sx = u*u*fromX + 2*t*u*verbs[at + 1] + t*t*toX;
                        sy = u*u*fromY + 2*t*u*verbs[at + 2] + t*t*toY;
                    } else {
                        sx = u*u*u*fromX + 3*t*u*u*verbs[at + 1] + 3*t*t*u*verbs[at + 3] + t*t*t*toX;
                        sy = u*u*u*fromY + 3*t*u*u*verbs[at + 2] + 3*t*t*u*verbs[at + 4] + t*t*t*toY;
                    }
                }
                if (verb != MOVE) {
                    float dx = sx - px, dy = sy - py;
                    length += (float) Math.sqrt(dx * dx + dy * dy);
                }
                samples[count++] = length;
                samples[count++] = px = sx;
                samples[count++] = py = sy;
            }
        }
        if (count == 0) return new float[] {0, 0, 0, 1, 0, 0};
        if (length == 0) return new float[] {0, samples[1], samples[2], 1, px, py};
        for (int i = 0; i < count; i += 3) samples[i] /= length;
        return java.util.Arrays.copyOf(samples, count);
    }

    public static Path fromPathData(String data) {
        float[] commands = nativeParsePathData(data);
        Path source = new Path();
        source.verbs = commands;
        source.size = commands.length / STRIDE;
        Path result = new Path();
        result.addPath(source);
        return result;
    }
    private static native float[] nativeParsePathData(String data);
}
