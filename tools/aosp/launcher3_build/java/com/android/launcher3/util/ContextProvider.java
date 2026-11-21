package com.android.launcher3.util;

import android.content.Context;

@FunctionalInterface
public interface ContextProvider<T> {
    T get(Context context);
}
