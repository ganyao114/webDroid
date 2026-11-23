package org.wasmandroid.uismoke;

import android.graphics.Bitmap;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.ColorSpace;
import android.graphics.ComposeShader;
import android.graphics.Interpolator;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffXfermode;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Region;
import android.graphics.RegionIterator;
import android.graphics.Shader;
import android.os.Parcel;
import android.text.AndroidCharacter;

import com.android.internal.util.ArrayUtils;

public final class NativeGraphicsProbe {
    private static final int BLUE = 0xff2163ce;
    private static final int ORANGE = 0xffdf9731;
    private static final int GRAY = 0xffa0a0a0;

    private static void check(boolean condition, String detail) {
        if (!condition) throw new AssertionError(detail);
    }

    private static void pixel(Bitmap bitmap, int x, int y, int expected) {
        int actual = bitmap.getPixel(x, y);
        if (actual != expected)
            throw new AssertionError("pixel " + x + "," + y + " expected " + expected + " got " + actual);
    }

    private static Bitmap create(int w, int h) {
        return Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
    }

    private static Bitmap texture() {
        Bitmap bitmap = create(2, 2);
        bitmap.setPixel(0, 0, BLUE);
        bitmap.setPixel(1, 0, ORANGE);
        bitmap.setPixel(0, 1, GRAY);
        bitmap.setPixel(1, 1, 0xff6e2146);
        return bitmap;
    }

    private static Paint texturePaint(Bitmap bitmap) {
        BitmapShader shader = new BitmapShader(bitmap, Shader.TileMode.REPEAT, Shader.TileMode.MIRROR);
        shader.setFilterMode(BitmapShader.FILTER_MODE_NEAREST);
        Matrix local = new Matrix();
        local.setScale(3, 3);
        local.postTranslate(2, 2);
        shader.setLocalMatrix(local);
        Paint paint = new Paint();
        paint.setShader(shader);
        paint.setAlpha(128);
        return paint;
    }

    public static void drawRecordedShaders(Canvas canvas) {
        Paint paint = texturePaint(texture());
        int save = canvas.save();
        canvas.translate(12, 110);
        canvas.clipRect(0, 0, 88, 21);
        canvas.drawRoundRect(0, 0, 20, 20, 7, 7, paint);
        canvas.drawCircle(32, 10, 10, paint);
        Path path = new Path();
        path.moveTo(46, 2);
        path.lineTo(68, 4);
        path.lineTo(65, 19);
        path.lineTo(49, 17);
        path.close();
        canvas.drawPath(path, paint);
        canvas.restoreToCount(save);
    }

    private static void verifyShaders() {
        Bitmap source = texture();
        Bitmap target = create(24, 24);
        Canvas canvas = new Canvas(target);
        Paint paint = texturePaint(source);
        canvas.save();
        canvas.clipRect(3, 0, 20, 20);
        Path excluded = new Path();
        excluded.addCircle(13, 8, 2, Path.Direction.CW);
        canvas.clipOutPath(excluded);
        canvas.drawRoundRect(0, 0, 21, 21, 9, 9, paint);
        pixel(target, 2, 8, 0);
        pixel(target, 3, 1, 0);
        pixel(target, 4, 4, 0x802163ce);
        pixel(target, 6, 4, 0x80df9731);
        pixel(target, 12, 8, 0);
        canvas.restore();
        target.eraseColor(0);
        canvas.drawCircle(10, 10, 8, paint);
        pixel(target, 4, 4, 0x802163ce);
        pixel(target, 2, 2, 0);
        target.eraseColor(0);
        Path triangle = new Path();
        triangle.moveTo(2, 2);
        triangle.lineTo(20, 2);
        triangle.lineTo(2, 20);
        triangle.close();
        canvas.drawPath(triangle, paint);
        pixel(target, 6, 4, 0x80df9731);
        pixel(target, 10, 4, 0x802163ce);
        pixel(target, 4, 9, 0x80a0a0a0);
        pixel(target, 19, 19, 0);
        source.setPixel(1, 0, 0xff00664a);
        target.eraseColor(0);
        canvas.drawPath(triangle, paint);
        pixel(target, 6, 4, 0x8000664a);

        Bitmap half = create(1, 1);
        half.eraseColor(0x800000ff);
        BitmapShader overlay = new BitmapShader(half, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP);
        source.setPixel(0, 0, 0);
        ComposeShader compose = new ComposeShader(paint.getShader(), overlay, PorterDuff.Mode.SRC_IN);
        Matrix composeLocal = new Matrix();
        composeLocal.setTranslate(3, 0);
        compose.setLocalMatrix(composeLocal);
        paint.setShader(compose);
        target.eraseColor(0);
        canvas.drawPath(triangle, paint);
        pixel(target, 6, 4, 0);
        pixel(target, 9, 4, 0x400000ff);

        Bitmap filter = create(2, 1);
        filter.setPixel(0, 0, 0xffff0000);
        filter.setPixel(1, 0, 0xff0000ff);
        BitmapShader linear = new BitmapShader(filter, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP);
        linear.setFilterMode(BitmapShader.FILTER_MODE_LINEAR);
        Matrix matrix = new Matrix();
        matrix.setTranslate(.5f, 0);
        linear.setLocalMatrix(matrix);
        paint.setAlpha(255);
        paint.setShader(linear);
        target.eraseColor(0);
        canvas.drawRect(0, 0, 4, 4, paint);
        pixel(target, 1, 0, 0xff800080);

        canvas.save();
        canvas.clipShader(new BitmapShader(half, Shader.TileMode.DECAL, Shader.TileMode.DECAL));
        target.eraseColor(0);
        canvas.drawColor(BLUE);
        pixel(target, 0, 0, 0x802163ce);
        pixel(target, 1, 0, 0);
        canvas.restore();

        BitmapShader self = new BitmapShader(filter, Shader.TileMode.REPEAT, Shader.TileMode.CLAMP);
        self.setFilterMode(BitmapShader.FILTER_MODE_NEAREST);
        matrix.setTranslate(1, 0);
        self.setLocalMatrix(matrix);
        paint.setShader(self);
        new Canvas(filter).drawRect(0, 0, 2, 1, paint);
        pixel(filter, 0, 0, 0xff0000ff);
        pixel(filter, 1, 0, 0xffff0000);
    }

    private static void verifyRegionAndDraws() {
        Region region = new Region(2, 2, 9, 7);
        check(region.op(new Rect(6, 4, 12, 10), Region.Op.UNION), "Region union");
        Rect bounds = region.getBounds();
        check(region.isComplex() && bounds.equals(new Rect(2, 2, 12, 10)) &&
                region.contains(10, 8) && !region.contains(3, 8), "Region shape and bounds");
        check(region.op(3, 3, 12, 8, Region.Op.INTERSECT) && region.quickReject(0, 0, 1, 1),
                "Region intersection and quick reject");
        Region shifted = new Region();
        region.translate(1, 2, shifted);
        check(shifted.contains(4, 5) && !shifted.contains(3, 3), "Region translation");
        Path boundary = region.getBoundaryPath();
        Region fromPath = new Region();
        check(fromPath.setPath(boundary, new Region(0, 0, 15, 15)) && fromPath.equals(region),
                "Region boundary path round trip");
        RegionIterator iterator = new RegionIterator(fromPath);
        fromPath.setEmpty();
        int area = 0;
        while (iterator.next(bounds)) area += bounds.width() * bounds.height();
        check(area == 39, "RegionIterator snapshots the source bands");
        Parcel regionParcel = Parcel.obtain();
        region.writeToParcel(regionParcel, 0);
        regionParcel.setDataPosition(0);
        check(Region.CREATOR.createFromParcel(regionParcel).equals(region), "Region parcel round trip");
        regionParcel.recycle();

        Bitmap target = create(48, 32);
        Canvas canvas = new Canvas(target);
        Paint paint = new Paint();
        paint.setColor(BLUE);
        canvas.drawRegion(region, paint);
        pixel(target, 4, 4, BLUE);
        pixel(target, 1, 1, 0);
        paint.setStrokeWidth(2);
        canvas.drawLines(new float[]{16, 4, 22, 4, 16, 9, 22, 9}, paint);
        canvas.drawPoints(new float[]{17, 15, 22, 17}, paint);
        pixel(target, 18, 4, BLUE);
        pixel(target, 17, 15, BLUE);
        canvas.drawDoubleRoundRect(new RectF(24, 1, 46, 20), 4, 4,
                new RectF(31, 5, 40, 16), 2, 2, paint);
        pixel(target, 26, 9, BLUE);
        pixel(target, 35, 9, 0);
        paint.setColor(0x8000ff00);
        paint.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.SRC));
        canvas.drawRect(3, 3, 8, 7, paint);
        pixel(target, 4, 4, 0x8000ff00);
        paint.setXfermode(null);
        Path textPath = new Path();
        paint.getTextPath("Hi", 0, 2, 0, 20, textPath);
        check(!textPath.isEmpty(), "Paint text path");

        Parcel parcel = Parcel.obtain();
        target.writeToParcel(parcel, 0);
        parcel.setDataPosition(0);
        Bitmap copy = Bitmap.CREATOR.createFromParcel(parcel);
        check(copy.sameAs(target) && ColorSpace.get(ColorSpace.Named.SRGB).equals(copy.getColorSpace()),
                "Bitmap pixels and color space round trip through Parcel");
        parcel.recycle();
    }

    public static void verify() {
        verifyShaders();
        verifyRegionAndDraws();
        Interpolator interp = new Interpolator(1);
        float[] values = new float[1];
        interp.setKeyFrame(0, 100, new float[]{10});
        interp.setKeyFrame(1, 200, new float[]{20});
        check(interp.timeToValues(150, values) == Interpolator.Result.NORMAL && values[0] == 15,
                "Interpolator samples between keyframes");
        check(interp.timeToValues(250, values) == Interpolator.Result.FREEZE_END && values[0] == 20,
                "Interpolator freezes at its last keyframe");
        interp.setRepeatMirror(2, true);
        check(interp.timeToValues(225, values) == Interpolator.Result.NORMAL && values[0] == 17.5f,
                "Interpolator mirrors the second cycle");
        interp.reset(2);

        char[] text = {'A', '\u05d0', '\u0627', '\ud83d', '\ude00', '\u6f22'};
        byte[] properties = new byte[6];
        AndroidCharacter.getDirectionalities(text, properties, text.length);
        check(properties[0] == 0 && properties[1] == 1 && properties[2] == 2 &&
                properties[3] == properties[4], "AndroidCharacter directionalities");
        AndroidCharacter.getEastAsianWidths(text, 3, 3, properties);
        check(properties[0] == 5 && properties[1] == 5 && properties[2] == 5 &&
                AndroidCharacter.getEastAsianWidth('A') == 4, "AndroidCharacter East Asian widths");
        char[] brackets = {'x', '(', ']', 'y'};
        check(AndroidCharacter.mirror(brackets, 1, 2) && brackets[0] == 'x' &&
                brackets[1] == ')' && brackets[2] == '[' && AndroidCharacter.getMirror('<') == '>',
                "AndroidCharacter mirroring");
        ArrayUtils.zeroize(brackets);
        ArrayUtils.zeroize(properties);
        for (char ch : brackets) check(ch == 0, "ArrayUtils.zeroize(char[])");
        for (byte b : properties) check(b == 0, "ArrayUtils.zeroize(byte[])");
    }
}
