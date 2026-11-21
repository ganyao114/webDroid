// Compile-time declaration for a hidden AOSP API.
package com.android.internal.util;

import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

import java.io.IOException;

public final class XmlUtils {
    public static native int convertValueToInt(CharSequence value, int defaultValue);

    // Also referenced by the upstream XmlBlock used in the JVM test.
    public static native int convertValueToList(CharSequence value, String[] options, int defaultValue);

    public static native void skipCurrentTag(XmlPullParser parser)
            throws IOException, XmlPullParserException;
}
