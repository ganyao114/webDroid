package org.wasmandroid.ui;

import android.content.res.Resources;
import android.content.res.TypedArray;
import android.content.res.XmlResourceParser;
import android.graphics.drawable.Drawable;
import android.util.TypedValue;
import java.io.IOException;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

/** A cursor over a binary XML element. Value resolution stays in AssetManager2. */
public final class XmlValues {
    public static final String ANDROID = "http://schemas.android.com/apk/res/android";
    private final Resources resources;
    private final Resources.Theme theme;
    private final XmlResourceParser xml;

    public XmlValues(Resources resources, Resources.Theme theme, XmlResourceParser xml) {
        this.resources = resources;
        this.theme = theme;
        this.xml = xml;
    }

    public static void toStartTag(XmlPullParser xml) throws IOException, XmlPullParserException {
        int event = xml.getEventType();
        while (event != XmlPullParser.START_TAG && event != XmlPullParser.END_DOCUMENT) event = xml.next();
        if (event != XmlPullParser.START_TAG) throw new XmlPullParserException("No XML start tag");
    }

    public static void skip(XmlPullParser xml) throws IOException, XmlPullParserException {
        int depth = xml.getDepth();
        for (int event = xml.next(); event != XmlPullParser.END_DOCUMENT; event = xml.next()) {
            if (event == XmlPullParser.END_TAG && xml.getDepth() == depth) return;
        }
    }

    private int attribute(String name) {
        int count = xml.getAttributeCount();
        for (int i = 0; i < count; i++) {
            if (name.equals(xml.getAttributeName(i)) && ANDROID.equals(xml.getAttributeNamespace(i))) {
                return xml.getAttributeNameResource(i);
            }
        }
        return 0;
    }

    private TypedArray obtain(int id) {
        return theme == null ? resources.obtainAttributes(xml, new int[] {id})
                : theme.obtainStyledAttributes(xml, new int[] {id}, 0, 0);
    }

    public boolean has(String name) { return attribute(name) != 0; }
    public int id(String name, int fallback) {
        int id = attribute(name);
        if (id == 0) return fallback;
        try (TypedArray a = obtain(id)) { return a.getResourceId(0, fallback); }
    }
    public String text(String name, String fallback) {
        int id = attribute(name);
        if (id == 0) return fallback;
        try (TypedArray a = obtain(id)) {
            String text = a.getString(0);
            return text == null ? fallback : text;
        }
    }
    public int color(String name, int fallback) {
        int id = attribute(name);
        if (id == 0) return fallback;
        try (TypedArray a = obtain(id)) { return a.getColor(0, fallback); }
    }
    public int integer(String name, int fallback) {
        int id = attribute(name);
        if (id == 0) return fallback;
        try (TypedArray a = obtain(id)) { return a.getInt(0, fallback); }
    }
    public float number(String name, float fallback) {
        int id = attribute(name);
        if (id == 0) return fallback;
        try (TypedArray a = obtain(id)) {
            if (a.getType(0) == TypedValue.TYPE_FRACTION) return a.getFraction(0, 1, 1, fallback);
            return a.getFloat(0, fallback);
        }
    }
    public float dimension(String name, float fallback) {
        int id = attribute(name);
        if (id == 0) return fallback;
        try (TypedArray a = obtain(id)) { return a.getDimension(0, fallback); }
    }
    public int pixels(String name, int fallback) {
        int id = attribute(name);
        if (id == 0) return fallback;
        try (TypedArray a = obtain(id)) { return a.getDimensionPixelSize(0, fallback); }
    }
    public Drawable drawable(String name) {
        int id = attribute(name);
        if (id == 0) return null;
        try (TypedArray a = obtain(id)) { return a.getDrawable(0); }
    }
}
