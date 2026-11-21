// Compile-time declaration for a hidden AOSP API.
package com.android.internal.util;

import android.os.Parcel;

public interface Parcelling<T> {
    void parcel(T item, Parcel dest, int flags);
    T unparcel(Parcel source);
}
