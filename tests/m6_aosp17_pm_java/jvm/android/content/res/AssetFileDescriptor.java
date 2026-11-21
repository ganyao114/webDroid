package android.content.res;

import android.os.ParcelFileDescriptor;

// Fields resolved by the upstream JNI registry for the custom asset-provider path.
public final class AssetFileDescriptor {
    private ParcelFileDescriptor mFd;
    private long mStartOffset;
    private long mLength;
}
