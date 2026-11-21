package android.content.res;

import android.util.TypedValue;
import java.io.IOException;
import java.util.ArrayList;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;
import org.wasmandroid.ui.XmlValues;

public class ColorStateList {
    private final int[][] states;
    private final int[] colors;
    private final int defaultColor;

    public ColorStateList(int[][] states, int[] colors) {
        if (states.length != colors.length) throw new IllegalArgumentException("Colors and states differ in size");
        this.states = states;
        this.colors = colors;
        int color = colors.length == 0 ? 0xffff0000 : colors[0];
        for (int i = 0; i < states.length; i++) {
            if (states[i].length == 0) {
                color = colors[i];
                break;
            }
        }
        defaultColor = color;
    }

    public static ColorStateList valueOf(int color) {
        return new ColorStateList(new int[][] {new int[0]}, new int[] {color});
    }

    public int getDefaultColor() { return defaultColor; }
    public boolean isStateful() { return states.length > 0 && states[0].length != 0; }
    public int getColorForState(int[] state, int fallback) {
        for (int i = 0; i < states.length; i++) {
            boolean matches = true;
            for (int spec : states[i]) {
                if (spec == 0) break;
                boolean found = false;
                if (state != null) {
                    for (int item : state) if (item == Math.abs(spec)) found = true;
                }
                if (found != (spec > 0)) {
                    matches = false;
                    break;
                }
            }
            if (matches) return colors[i];
        }
        return fallback;
    }

    public static ColorStateList createFromXml(Resources res, XmlPullParser parser, Resources.Theme theme)
            throws IOException, XmlPullParserException {
        XmlValues.toStartTag(parser);
        if (!(parser instanceof XmlResourceParser) || !"selector".equals(parser.getName())) {
            throw new XmlPullParserException("Expected a compiled color selector");
        }
        XmlResourceParser xml = (XmlResourceParser) parser;
        ArrayList<int[]> states = new ArrayList<>();
        ArrayList<Integer> colors = new ArrayList<>();
        int depth = xml.getDepth();
        for (int event = xml.next(); event != XmlPullParser.END_DOCUMENT; event = xml.next()) {
            if (event == XmlPullParser.END_TAG && xml.getDepth() == depth) break;
            if (event != XmlPullParser.START_TAG || xml.getDepth() != depth + 1
                    || !"item".equals(xml.getName())) continue;
            XmlValues attrs = new XmlValues(res, theme, xml);
            int color = attrs.color("color", 0xffff00ff);
            float alpha = attrs.number("alpha", 1f);
            color = (color & 0xffffff) | ((int) Math.max(0, Math.min(255,
                    (color >>> 24) * alpha + 0.5f)) << 24);
            int[] spec = new int[xml.getAttributeCount()];
            int count = 0;
            for (int i = 0; i < spec.length; ++i) {
                int id = xml.getAttributeNameResource(i);
                String name = xml.getAttributeName(i);
                if (id != 0 && name.startsWith("state_")) {
                    spec[count++] = xml.getAttributeBooleanValue(i, false) ? id : -id;
                }
            }
            int[] exact = new int[count];
            System.arraycopy(spec, 0, exact, 0, count);
            states.add(exact);
            colors.add(color);
        }
        int[][] stateArray = new int[states.size()][];
        int[] colorArray = new int[states.size()];
        for (int i = 0; i < colors.size(); i++) {
            stateArray[i] = states.get(i);
            colorArray[i] = colors.get(i);
        }
        return new ColorStateList(stateArray, colorArray);
    }
}
