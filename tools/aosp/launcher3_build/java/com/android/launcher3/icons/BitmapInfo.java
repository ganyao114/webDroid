package com.android.launcher3.icons;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import com.android.launcher3.R;
import com.android.launcher3.icons.cache.CacheLookupFlag;

public final class BitmapInfo {
    public @interface DrawableCreationFlags {}

    public static final int FLAG_THEMED = 1;
    public static final BitmapInfo LOW_RES_INFO = new BitmapInfo(null, false);
    public final Bitmap icon;
    private final boolean fullBleed;

    public BitmapInfo(Bitmap icon, boolean fullBleed) {
        this.icon = icon;
        this.fullBleed = fullBleed;
    }

    public boolean isFullBleed() {
        return fullBleed;
    }

    public CacheLookupFlag getMatchingLookupFlag() {
        CacheLookupFlag flag = CacheLookupFlag.DEFAULT_LOOKUP_FLAG;
        return icon == null ? flag.withLowRes() : flag;
    }

    public FastBitmapDrawable newIcon(Context context, int creationFlags, IconShape shape) {
        Bitmap bitmap = icon;
        if (bitmap == null) {
            Drawable fallback = context.getDrawable(R.drawable.ic_launcher_home);
            bitmap = Bitmap.createBitmap(96, 96, Bitmap.Config.ARGB_8888);
            fallback.setBounds(0, 0, bitmap.getWidth(), bitmap.getHeight());
            fallback.draw(new Canvas(bitmap));
        }
        return new FastBitmapDrawable(context.getResources(), bitmap, shape);
    }
}
