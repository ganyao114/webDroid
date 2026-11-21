package com.android.launcher3.icons;

import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.ColorMatrix;
import android.graphics.ColorMatrixColorFilter;
import android.graphics.Path;
import android.graphics.drawable.BitmapDrawable;

public final class FastBitmapDrawable extends BitmapDrawable {
    private final IconShape shape;
    private final Path mask = new Path();

    public FastBitmapDrawable(Resources resources, Bitmap bitmap, IconShape shape) {
        super(resources, bitmap);
        this.shape = shape;
    }

    @Override
    public void draw(Canvas canvas) {
        if (shape == null) {
            super.draw(canvas);
            return;
        }
        int save = canvas.save();
        mask.rewind();
        shape.addToPath(mask, getBounds());
        canvas.clipPath(mask);
        super.draw(canvas);
        canvas.restoreToCount(save);
    }

    public void setDisabled(boolean disabled) {
        ColorMatrix matrix = new ColorMatrix();
        matrix.setSaturation(disabled ? 0 : 1);
        setColorFilter(disabled ? new ColorMatrixColorFilter(matrix) : null);
        setAlpha(disabled ? 128 : 255);
    }
}
