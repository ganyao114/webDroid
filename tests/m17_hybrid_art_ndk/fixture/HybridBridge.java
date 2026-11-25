package com.wasmandroid.hybrid;

import android.graphics.Bitmap;
import android.content.res.AssetManager;
import android.view.Surface;
import java.nio.ByteBuffer;

final class HybridBridge {
    static long lastLong;
    static float lastFloat;
    static double lastDouble;
    static byte lastByte;

    static native String abi();
    static native int loadCount();
    static native long checksum(int seed);
    static native double score(double x, double y, double z);
    static native float scale(float input, int by);
    static native int run(HybridActivity activity, int seed, byte[] scratch, ByteBuffer buffer);
    static native ByteBuffer nativeBuffer();
    static native int readNativeBuffer();
    static native void requirePositive(int input);
    static native boolean verifyAbi();
    static native boolean pollTimer();
    static native boolean protectImageData();
    static native boolean editBitmap(Bitmap bitmap);
    static native boolean readAsset(AssetManager assets);
    static native boolean postSurface(Surface surface);
    static native String dynamicBanner(String label, int seed);
    static native int decorated_name(int[] values);
    static native int decorated_name(String value);
    static native int métier();
    static native double wide(int a, int b, int c, int d, int e, int f, int g, int h, int i,
            long wide, double x1, double x2, double x3, double x4, double x5, double x6,
            double x7, double x8, double x9, double x10, float tail);

    static int staticBump(int seed) {
        System.gc();
        return (int) (checksum(seed + 1) - checksum(seed));
    }

    static double sumMixed(int a, int b, int c, int d, int e, int f, int g, int h, int i,
            long wide, double x1, double x2, double x3, double x4, double x5, double x6,
            double x7, double x8, double x9, double x10, float tail) {
        long integers = a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i;
        return integers + (wide >>> 32) + x1 + 2 * x2 + 3 * x3 + 4 * x4 + 5 * x5
                + 6 * x6 + 7 * x7 + 8 * x8 + 9 * x9 + 10 * x10 + tail;
    }
}
