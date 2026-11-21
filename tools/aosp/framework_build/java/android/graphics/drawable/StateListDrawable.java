package android.graphics.drawable;

import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.graphics.Canvas;
import android.graphics.Rect;
import java.io.IOException;
import java.util.ArrayList;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;
import org.wasmandroid.ui.XmlValues;

public class StateListDrawable extends Drawable implements Drawable.Callback {
    private final ArrayList<int[]> states = new ArrayList<>();
    private final ArrayList<Drawable> drawables = new ArrayList<>();
    private Drawable current;

    public void addState(int[] stateSet, Drawable drawable) {
        if (drawable == null) return;
        states.add(stateSet);
        drawables.add(drawable);
        drawable.setCallback(this);
        onStateChange(getState());
    }
    @Override public boolean isStateful() { return true; }
    @Override public Drawable getCurrent() { return current; }
    @Override public void draw(Canvas canvas) { if (current != null) current.draw(canvas); }
    @Override public int getIntrinsicWidth() { return current == null ? -1 : current.getIntrinsicWidth(); }
    @Override public int getIntrinsicHeight() { return current == null ? -1 : current.getIntrinsicHeight(); }
    @Override protected void onBoundsChange(Rect bounds) { if (current != null) current.setBounds(bounds); }
    @Override public void setAlpha(int alpha) { super.setAlpha(alpha); if (current != null) current.setAlpha(alpha); }
    @Override protected boolean onStateChange(int[] state) {
        Drawable next = null;
        for (int i = 0; i < states.size(); ++i) {
            if (matches(states.get(i), state)) { next = drawables.get(i); break; }
        }
        if (next == current) return current != null && current.setState(state);
        current = next;
        if (current != null) {
            current.setBounds(getBounds());
            current.setAlpha(getAlpha());
            current.setState(state);
        }
        invalidateSelf();
        return true;
    }
    private static boolean matches(int[] spec, int[] state) {
        for (int wanted : spec) {
            if (wanted == 0) break;
            boolean found = false;
            if (state != null) for (int have : state) if (have == Math.abs(wanted)) found = true;
            if (found != (wanted > 0)) return false;
        }
        return true;
    }
    @Override public void invalidateDrawable(Drawable drawable) { if (drawable == current) invalidateSelf(); }
    @Override public void scheduleDrawable(Drawable drawable, Runnable task, long when) { scheduleSelf(task, when); }
    @Override public void unscheduleDrawable(Drawable drawable, Runnable task) { unscheduleSelf(task); }

    static StateListDrawable inflateSelector(Resources res, XmlResourceParser xml, Resources.Theme theme)
            throws IOException, XmlPullParserException {
        StateListDrawable result = new StateListDrawable();
        int depth = xml.getDepth();
        for (int event = xml.next(); event != XmlPullParser.END_DOCUMENT; event = xml.next()) {
            if (event == XmlPullParser.END_TAG && xml.getDepth() == depth) break;
            if (event != XmlPullParser.START_TAG || xml.getDepth() != depth + 1 || !"item".equals(xml.getName())) continue;
            int[] spec = new int[xml.getAttributeCount()];
            int count = 0;
            for (int i = 0; i < spec.length; ++i) {
                int id = xml.getAttributeNameResource(i);
                if (id != 0 && xml.getAttributeName(i).startsWith("state_")) {
                    spec[count++] = xml.getAttributeBooleanValue(i, false) ? id : -id;
                }
            }
            result.addState(spec, readChild(res, xml, theme));
        }
        return result;
    }
}
