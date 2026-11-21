// Compile-time declaration for a hidden AOSP API.
package com.android.internal.util;

import java.util.Collection;
import java.util.Map;

public final class CollectionUtils {
    public static native boolean isEmpty(Collection<?> collection);
    public static native int size(Map<?, ?> map);
}
