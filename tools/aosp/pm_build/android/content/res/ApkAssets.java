// Compile-time declaration for a hidden AOSP API.
package android.content.res;

import android.content.res.loader.AssetsProvider;

import java.io.FileDescriptor;
import java.io.IOException;

public final class ApkAssets {
    public static native ApkAssets loadFromPath(String path) throws IOException;

    public static native ApkAssets loadFromFd(FileDescriptor fd, String name, int flags,
            AssetsProvider assets) throws IOException;

    public native XmlResourceParser openXml(String name) throws IOException;

    public native void close();
}
