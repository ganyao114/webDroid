package android.util;

import org.xmlpull.v1.XmlPullParser;

public final class Xml {
    private Xml() {}

    /** The resource parser already implements the attribute-set API. */
    public static AttributeSet asAttributeSet(XmlPullParser parser) {
        if (parser instanceof AttributeSet attributes) return attributes;
        if (parser == null) return null;
        throw new IllegalArgumentException("Expected an Android resource parser");
    }
}
