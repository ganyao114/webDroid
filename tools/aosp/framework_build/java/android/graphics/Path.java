package android.graphics;

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

    public Path() {}
    public Path(Path source) { if (source != null) set(source); }
    public void set(Path source) {
        if (source == this) return;
        reserve(source.size);
        System.arraycopy(source.verbs, 0, verbs, 0, source.size * STRIDE);
        size = source.size;
        x = source.x; y = source.y; contourX = source.contourX; contourY = source.contourY;
        needsMove = source.needsMove; fillType = source.fillType;
    }
    public void reset() { rewind(); }
    public void rewind() { size = 0; x = y = contourX = contourY = 0; needsMove = true; }
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
        float kx = rx * (1 - KAPPA), ky = ry * (1 - KAPPA);
        moveTo(l + rx, t);
        if (direction == Direction.CW) {
            lineTo(r - rx, t); cubicTo(r - kx, t, r, t + ky, r, t + ry);
            lineTo(r, b - ry); cubicTo(r, b - ky, r - kx, b, r - rx, b);
            lineTo(l + rx, b); cubicTo(l + kx, b, l, b - ky, l, b - ry);
            lineTo(l, t + ry); cubicTo(l, t + ky, l + kx, t, l + rx, t);
        } else {
            cubicTo(l + kx, t, l, t + ky, l, t + ry); lineTo(l, b - ry);
            cubicTo(l, b - ky, l + kx, b, l + rx, b); lineTo(r - rx, b);
            cubicTo(r - kx, b, r, b - ky, r, b - ry); lineTo(r, t + ry);
            cubicTo(r, t + ky, r - kx, t, r - rx, t); lineTo(l + rx, t);
        }
        close();
    }
    public void addPath(Path path) { addPath(path, 0, 0); }
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
