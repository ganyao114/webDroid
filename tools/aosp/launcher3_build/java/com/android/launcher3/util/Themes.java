package com.android.launcher3.util;

import android.content.Context;
import android.content.res.TypedArray;

public final class Themes {
    private Themes() {}

    public static int getAttrColor(Context context, int attribute) {
        try (TypedArray value = context.obtainStyledAttributes(new int[]{attribute})) {
            return value.getColor(0, 0);
        }
    }
}
