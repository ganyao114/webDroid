package android.graphics;

import java.util.ArrayList;
import java.util.Arrays;
import org.wasmandroid.ui.SoftwareRaster;

/** A recording canvas for a SurfaceFlinger layer or a software bitmap target. */
public class Canvas {
    public boolean clipPath(Path path) { return path.clip(this); }
    private static final int SAVE = 1, RESTORE = 2, TRANSLATE = 3, SCALE = 4, ROTATE = 5;
    private static final int CLIP_RECT = 6, CLIP_ROUND_RECT = 7, DRAW_COLOR = 8;
    private static final int RECT = 9, ROUND_RECT = 10, CIRCLE = 11, LINE = 12;
    private final int width, height;
    private final Bitmap bitmapTarget;
    private final ArrayList<float[]> bitmapStates;
    private float[] bitmapState;
    private int density;
    private int saveCount = 1;
    private float alpha = 1f;
    private float[] savedAlpha;

    public Canvas() { this(0, 0, 160); }
    public Canvas(Bitmap bitmap) {
        this(bitmap.getWidth(), bitmap.getHeight(), bitmap.getDensity(), bitmap);
        if (!bitmap.isMutable() || bitmap.isRecycled()) throw new IllegalStateException("Bitmap is not writable");
    }

    /** Create a canvas for a host-started frame. */
    public Canvas(int width, int height, int density) {
        this(width, height, density, null);
    }
    private Canvas(int width, int height, int density, Bitmap target) {
        this.width = width; this.height = height; this.density = density;
        bitmapTarget = target;
        bitmapStates = target == null ? null : new ArrayList<>();
        bitmapState = target == null ? null : new float[]{1, 0, 0, 0, 1, 0, 0, 0, width, height};
    }
    public int getWidth() { return width; }
    public int getHeight() { return height; }
    public int getDensity() { return density; }
    public void setDensity(int density) { this.density = density; }
    public boolean isHardwareAccelerated() { return bitmapTarget == null && width > 0 && height > 0; }
    public int getSaveCount() { return saveCount; }
    public int save() {
        if (savedAlpha == null || saveCount > savedAlpha.length) {
            float[] stack = new float[savedAlpha == null ? 8 : savedAlpha.length * 2];
            if (savedAlpha != null) System.arraycopy(savedAlpha, 0, stack, 0, savedAlpha.length);
            savedAlpha = stack;
        }
        operation(SAVE, 0, 0, 0, 0, 0, 0);
        savedAlpha[saveCount - 1] = alpha;
        return saveCount++;
    }
    public void restore() {
        if (saveCount == 1) throw new IllegalStateException("Canvas restore underflow");
        operation(RESTORE, 0, 0, 0, 0, 0, 0);
        alpha = savedAlpha[--saveCount - 1];
    }
    public void restoreToCount(int count) {
        if (count < 1 || count > saveCount) throw new IllegalArgumentException("Invalid canvas save count");
        while (saveCount > count) restore();
    }
    public void translate(float x, float y) {
        if (x != 0 || y != 0) operation(TRANSLATE, x, y, 0, 0, 0, 0);
    }
    public void scale(float x, float y) {
        if (x != 1 || y != 1) operation(SCALE, x, y, 0, 0, 0, 0);
    }
    public final void scale(float x, float y, float px, float py) {
        translate(px, py);
        scale(x, y);
        translate(-px, -py);
    }
    public void rotate(float degrees) { rotate(degrees, 0, 0); }
    public final void rotate(float degrees, float px, float py) {
        if (degrees != 0) operation(ROTATE, degrees, px, py, 0, 0, 0);
    }
    public boolean clipRect(Rect rect) { return clipRect(rect.left, rect.top, rect.right, rect.bottom); }
    public boolean clipRect(RectF rect) { return clipRect(rect.left, rect.top, rect.right, rect.bottom); }
    public boolean clipRect(int l, int t, int r, int b) { return clipRect((float) l, t, r, b); }
    public boolean clipRect(float l, float t, float r, float b) {
        operation(CLIP_RECT, l, t, Math.max(l, r), Math.max(t, b), 0, 0);
        return l < r && t < b;
    }
    public boolean clipRoundRect(float l, float t, float r, float b, float rx, float ry) {
        operation(CLIP_ROUND_RECT, l, t, Math.max(l, r), Math.max(t, b), rx, ry);
        return l < r && t < b;
    }
    public boolean clipRoundRect(RectF rect, float rx, float ry) {
        return clipRoundRect(rect.left, rect.top, rect.right, rect.bottom, rx, ry);
    }
    /** Modulates the opacity of draws in the current saved canvas state. */
    public void multiplyAlpha(float alpha) { this.alpha *= Math.max(0, Math.min(1, alpha)); }
    private int drawColorWithAlpha(int color) {
        if (alpha == 1) return color;
        return (color & 0xffffff) | ((int) ((color >>> 24) * alpha + 0.5f) << 24);
    }
    public void drawColor(int color) {
        if (bitmapTarget != null) {
            Path fill = new Path();
            fill.setFillType(Path.FillType.INVERSE_WINDING);
            rasterPath(fill, drawColorWithAlpha(color), null, Paint.Style.FILL);
            return;
        }
        nativeShape(DRAW_COLOR, 0, 0, 0, 0, 0, 0, drawColorWithAlpha(color), 0, 0);
    }
    public void drawARGB(int a, int r, int g, int b) { drawColor(Color.argb(a, r, g, b)); }
    public void drawRGB(int r, int g, int b) { drawColor(Color.rgb(r, g, b)); }
    public void drawPaint(Paint paint) {
        if (bitmapTarget == null || paint.getShader() == null) {
            drawColor(paint.getColor());
        } else {
            Path fill = new Path();
            fill.setFillType(Path.FillType.INVERSE_WINDING);
            rasterPath(fill, drawColorWithAlpha(paint.getColor()), paint, Paint.Style.FILL);
        }
    }
    public void drawRect(float l, float t, float r, float b, Paint paint) {
        if (l < r && t < b) shape(RECT, l, t, r, b, 0, 0, paint);
    }
    public void drawRect(Rect rect, Paint paint) { drawRect(rect.left, rect.top, rect.right, rect.bottom, paint); }
    public void drawRect(RectF rect, Paint paint) { drawRect(rect.left, rect.top, rect.right, rect.bottom, paint); }
    public void drawRoundRect(float l, float t, float r, float b, float rx, float ry, Paint paint) {
        if (l < r && t < b) shape(ROUND_RECT, l, t, r, b, Math.max(0, rx), Math.max(0, ry), paint);
    }
    public void drawRoundRect(RectF rect, float rx, float ry, Paint paint) {
        drawRoundRect(rect.left, rect.top, rect.right, rect.bottom, rx, ry, paint);
    }
    public void drawCircle(float x, float y, float radius, Paint paint) {
        if (radius > 0) shape(CIRCLE, x, y, radius, 0, 0, 0, paint);
    }
    public void drawOval(float l, float t, float r, float b, Paint paint) {
        Path path = new Path();
        path.addOval(l, t, r, b, Path.Direction.CW);
        drawPath(path, paint);
    }
    public void drawOval(RectF rect, Paint paint) { drawOval(rect.left, rect.top, rect.right, rect.bottom, paint); }
    public void drawLine(float x0, float y0, float x1, float y1, Paint paint) {
        nativeShape(LINE, x0, y0, x1, y1, 0, 0, drawColorWithAlpha(paint.getColor()), 1, paint.getStrokeWidth());
    }
    public void drawLines(float[] points, Paint paint) { drawLines(points, 0, points.length, paint); }
    public void drawLines(float[] points, int offset, int count, Paint paint) {
        if (offset < 0 || count < 0 || offset > points.length - count) throw new ArrayIndexOutOfBoundsException();
        for (int i = offset; i + 3 < offset + count; i += 4) {
            drawLine(points[i], points[i + 1], points[i + 2], points[i + 3], paint);
        }
    }
    public void drawPath(Path path, Paint paint) {
        if (path.isEmpty()) return;
        if (bitmapTarget != null) {
            rasterPath(path, drawColorWithAlpha(paint.getColor()), paint, paint.getStyle());
            return;
        }
        if (path.isInverseFillType()) throw new UnsupportedOperationException("Inverse path fills require a clip path");
        nativeDrawPath(path.verbs, path.size, path.getFillType().ordinal(), drawColorWithAlpha(paint.getColor()),
                paint.getStyle().ordinal(), paint.getStrokeWidth());
    }
    public void drawBitmap(Bitmap bitmap, float left, float top, Paint paint) {
        drawBitmap(bitmap, null, new RectF(left, top, left + bitmap.getScaledWidth(density),
                top + bitmap.getScaledHeight(density)), paint);
    }
    public void drawBitmap(Bitmap bitmap, Rect src, Rect dst, Paint paint) {
        drawBitmap(bitmap, src, new RectF(dst), paint);
    }
    public void drawBitmap(Bitmap bitmap, Rect src, RectF dst, Paint paint) {
        drawMaskedBitmap(bitmap, src, dst, paint, 0, 0);
    }
    public void drawMaskedBitmap(Bitmap bitmap, Rect src, RectF dst, Paint paint, int mask, float radius) {
        Rect region = src == null ? new Rect(0, 0, bitmap.width, bitmap.height) : src;
        if (region.isEmpty() || dst.isEmpty()) return;
        float opacity = alpha * (paint == null ? 1f : paint.getAlpha() / 255f);
        if (bitmapTarget != null) {
            float[] rectangles = {region.left, region.top, region.right, region.bottom,
                    dst.left, dst.top, dst.right, dst.bottom};
            SoftwareRaster.drawBitmap(bitmapTarget.requirePixels(), width, height, bitmap.requirePixels(),
                    bitmap.width, bitmap.height, rectangles, bitmapState, opacity,
                    paint == null ? Color.BLACK : paint.getColor(), bitmap.hasAlpha(),
                    bitmap.getConfig() == Bitmap.Config.ALPHA_8, bitmapTarget.getConfig() == Bitmap.Config.ALPHA_8,
                    paint == null || paint.isFilterBitmap(), mask, radius);
            bitmapTarget.generation++;
            return;
        }
        nativeDrawBitmap(bitmap.requirePixels(), bitmap.width, bitmap.height, bitmap.identity,
                bitmap.generation, bitmap.hasAlpha(), region.left, region.top, region.right, region.bottom,
                dst.left, dst.top, dst.right, dst.bottom,
                opacity, mask, radius);
    }
    public void drawText(String text, float x, float y, Paint paint) {
        if (text == null) throw new NullPointerException("text");
        if (text.length() == 0 || paint.getTextSize() == 0) return;
        int state = getSaveCount();
        try {
            float scaleX = paint.textScaleForDrawing(text);
            if (scaleX != 1f) {
                save();
                scale(scaleX, 1f, x, y);
            }
            int shadow = drawColorWithAlpha(paint.getShadowLayerColor());
            if (paint.hasShadowLayer() && Color.alpha(shadow) != 0) {
                nativeDrawText(text, x + paint.getShadowLayerDx(), y + paint.getShadowLayerDy(), paint.getTextSize(),
                        paint.weight(), paint.getTextAlign().ordinal(), shadow, paint.face());
            }
            nativeDrawText(text, x, y, paint.getTextSize(), paint.weight(), paint.getTextAlign().ordinal(),
                    drawColorWithAlpha(paint.getColor()), paint.face());
        } finally {
            restoreToCount(state);
        }
    }
    public void drawText(String text, int start, int end, float x, float y, Paint paint) {
        drawText(text.substring(start, end), x, y, paint);
    }
    public void drawText(CharSequence text, int start, int end, float x, float y, Paint paint) {
        drawText(text.subSequence(start, end).toString(), x, y, paint);
    }
    public void drawText(char[] text, int index, int count, float x, float y, Paint paint) {
        if (text == null || index < 0 || count < 0 || index > text.length - count) throw new IndexOutOfBoundsException();
        StringBuilder result = new StringBuilder();
        for (int i = 0; i < count; ++i) result.append(text[index + i]);
        drawText(result.toString(), x, y, paint);
    }
    public void drawLinearGradient(Rect bounds, float radius, int start, int end, float degrees) {
        drawLinearGradient(bounds.left, bounds.top, bounds.right, bounds.bottom, radius, start, end, degrees);
    }
    public void drawLinearGradient(float left, float top, float right, float bottom, float radius,
                                   int start, int end, float degrees) {
        nativeDrawGradient(left, top, right, bottom, radius,
                drawColorWithAlpha(start), drawColorWithAlpha(end), degrees);
    }

    private void operation(int operation, float a, float b, float c, float d, float e, float f) {
        if (bitmapTarget == null) {
            nativeShape(operation, a, b, c, d, e, f, 0, 0, 0);
            return;
        }
        switch (operation) {
            case SAVE:
                bitmapStates.add(Arrays.copyOf(bitmapState, bitmapState.length));
                break;
            case RESTORE:
                bitmapState = bitmapStates.remove(bitmapStates.size() - 1);
                break;
            case TRANSLATE:
                bitmapState[2] += bitmapState[0] * a + bitmapState[1] * b;
                bitmapState[5] += bitmapState[3] * a + bitmapState[4] * b;
                break;
            case SCALE:
                bitmapState[0] *= a;
                bitmapState[3] *= a;
                bitmapState[1] *= b;
                bitmapState[4] *= b;
                break;
            case ROTATE:
                translate(b, c);
                rotateBitmap(a);
                translate(-b, -c);
                break;
            case CLIP_RECT:
                clipBitmap(a, b, c, d, 0, 0);
                break;
            case CLIP_ROUND_RECT:
                clipBitmap(a, b, c, d, e, f);
                break;
            default:
                throw new UnsupportedOperationException("Bitmap canvas operation " + operation);
        }
    }
    private void rotateBitmap(float degrees) {
        float sin = (float) Math.sin(degrees * Math.PI / 180), cos = (float) Math.cos(degrees * Math.PI / 180);
        float a = bitmapState[0], b = bitmapState[1], c = bitmapState[3], d = bitmapState[4];
        bitmapState[0] = a * cos + b * sin;
        bitmapState[1] = b * cos - a * sin;
        bitmapState[3] = c * cos + d * sin;
        bitmapState[4] = d * cos - c * sin;
    }
    private void clipBitmap(float left, float top, float right, float bottom, float rx, float ry) {
        double a = bitmapState[0], b = bitmapState[1], tx = bitmapState[2];
        double c = bitmapState[3], d = bitmapState[4], ty = bitmapState[5];
        double determinant = a * d - b * c;
        if (!(left < right && top < bottom) || !Double.isFinite(determinant) || determinant == 0) {
            bitmapState[8] = bitmapState[6];
            return;
        }
        double x0 = a * left, x1 = a * right, x2 = b * top, x3 = b * bottom;
        double y0 = c * left, y1 = c * right, y2 = d * top, y3 = d * bottom;
        bitmapState[6] = (float) Math.max(bitmapState[6], Math.min(x0, x1) + Math.min(x2, x3) + tx);
        bitmapState[7] = (float) Math.max(bitmapState[7], Math.min(y0, y1) + Math.min(y2, y3) + ty);
        bitmapState[8] = (float) Math.min(bitmapState[8], Math.max(x0, x1) + Math.max(x2, x3) + tx);
        bitmapState[9] = (float) Math.min(bitmapState[9], Math.max(y0, y1) + Math.max(y2, y3) + ty);
        if (rx > 0 && ry > 0) {
            float radiusScale = Math.min(1, Math.min((right - left) / (2 * rx), (bottom - top) / (2 * ry)));
            rx *= radiusScale;
            ry *= radiusScale;
        } else {
            if (b == 0 && c == 0) return;
            rx = ry = 0;
        }
        // Keep the clip fixed in device space when later drawing changes the matrix.
        int offset = bitmapState.length;
        bitmapState = Arrays.copyOf(bitmapState, offset + 12);
        bitmapState[offset] = (float) (d / determinant);
        bitmapState[offset + 1] = (float) (-b / determinant);
        bitmapState[offset + 2] = (float) ((b * ty - d * tx) / determinant);
        bitmapState[offset + 3] = (float) (-c / determinant);
        bitmapState[offset + 4] = (float) (a / determinant);
        bitmapState[offset + 5] = (float) ((c * tx - a * ty) / determinant);
        bitmapState[offset + 6] = left;
        bitmapState[offset + 7] = top;
        bitmapState[offset + 8] = right;
        bitmapState[offset + 9] = bottom;
        bitmapState[offset + 10] = rx;
        bitmapState[offset + 11] = ry;
    }
    private void rasterPath(Path path, int color, Paint paint, Paint.Style style) {
        MaskFilter filter = paint == null ? null : paint.getMaskFilter();
        Shader shader = paint == null ? null : paint.getShader();
        BlurMaskFilter blur = filter instanceof BlurMaskFilter ? (BlurMaskFilter) filter : null;
        SoftwareRaster.drawPath(bitmapTarget.requirePixels(), width, height, path.verbs, path.size,
                path.getFillType().ordinal(), bitmapState, color, blur == null ? 0 : blur.radius,
                blur == null ? 0 : blur.style.ordinal(), bitmapTarget.getConfig() == Bitmap.Config.ALPHA_8,
                shader == null ? 0 : shader.type, shader == null ? 0 : shader.tileMode.ordinal(),
                shader == null ? null : shader.geometry, shader == null ? null : shader.positions,
                shader == null ? null : shader.colors, style.ordinal(),
                paint == null ? 0 : paint.getStrokeWidth(), paint == null ? 0 : paint.getStrokeCap().ordinal(),
                paint == null ? 0 : paint.getStrokeJoin().ordinal(), paint == null ? 4 : paint.getStrokeMiter());
        bitmapTarget.generation++;
    }
    private void shape(int operation, float a, float b, float c, float d, float e, float f, Paint paint) {
        if (bitmapTarget != null) {
            Path path = new Path();
            if (operation == RECT) path.addRect(a, b, c, d, Path.Direction.CW);
            else if (operation == ROUND_RECT) path.addRoundRect(a, b, c, d, e, f, Path.Direction.CW);
            else if (operation == CIRCLE) path.addCircle(a, b, c, Path.Direction.CW);
            drawPath(path, paint);
            return;
        }
        nativeShape(operation, a, b, c, d, e, f, drawColorWithAlpha(paint.getColor()),
                paint.getStyle().ordinal(), paint.getStrokeWidth());
    }
    private static native void nativeShape(int operation, float a, float b, float c, float d, float e, float f,
                                           int color, int style, float stroke);
    private static native void nativeDrawPath(float[] commands, int count, int fill, int color, int style, float stroke);
    private static native void nativeDrawText(String text, float x, float y, float size, int weight,
                                              int align, int color, int face);
    private static native void nativeDrawBitmap(int[] pixels, int width, int height, long identity,
                                                int generation, boolean hasAlpha, float sl, float st, float sr,
                                                float sb, float dl, float dt, float dr, float db, float alpha,
                                                int mask, float radius);
    private static native void nativeDrawGradient(float l, float t, float r, float b, float radius,
                                                  int start, int end, float degrees);
}
