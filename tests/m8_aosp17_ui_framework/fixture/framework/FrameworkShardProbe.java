package org.wasmandroid.frameworktest;

import android.graphics.Point;

public final class FrameworkShardProbe extends Point {
    public static int run() {
        Point point = new FrameworkShardProbe();
        point.set(2, 7);
        return point.hashCode();
    }
}
