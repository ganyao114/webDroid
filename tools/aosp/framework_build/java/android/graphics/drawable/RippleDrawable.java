package android.graphics.drawable;

import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import java.io.IOException;
import java.util.ArrayList;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;
import org.wasmandroid.ui.XmlValues;

final class RippleDrawable extends Drawable implements Drawable.Callback {
    private final int color;
    private final ArrayList<Drawable> layers;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private boolean active;

    private RippleDrawable(int color, ArrayList<Drawable> layers) {
        this.color = color;
        this.layers = layers;
        for (Drawable layer : layers) layer.setCallback(this);
    }
    @Override public boolean isStateful() { return true; }
    @Override protected boolean onStateChange(int[] state) {
        boolean pressed = false;
        for (int value : state) pressed |= value == android.R.attr.state_pressed;
        for (Drawable layer : layers) layer.setState(state);
        if (active != pressed) { active = pressed; invalidateSelf(); }
        return true;
    }
    @Override protected void onBoundsChange(Rect bounds) {
        for (Drawable layer : layers) layer.setBounds(bounds);
    }
    @Override public void draw(Canvas canvas) {
        for (int i = 0; i < layers.size(); i++) layers.get(i).draw(canvas);
        if (active) {
            Rect b = getBounds();
            paint.setColor(modulateAlpha(color));
            canvas.drawRoundRect(b.left, b.top, b.right, b.bottom, b.height() * .2f, b.height() * .2f, paint);
        }
    }
    @Override public void invalidateDrawable(Drawable drawable) { invalidateSelf(); }
    @Override public void scheduleDrawable(Drawable drawable, Runnable task, long when) { scheduleSelf(task, when); }
    @Override public void unscheduleDrawable(Drawable drawable, Runnable task) { unscheduleSelf(task); }

    static RippleDrawable inflateRipple(Resources res, XmlResourceParser xml, Resources.Theme theme)
            throws IOException, XmlPullParserException {
        int color = new XmlValues(res, theme, xml).color("color", 0x33000000);
        var layers = new ArrayList<Drawable>();
        int depth = xml.getDepth();
        for (int event = xml.next(); event != XmlPullParser.END_DOCUMENT; event = xml.next()) {
            if (event == XmlPullParser.END_TAG && xml.getDepth() == depth) break;
            if (event != XmlPullParser.START_TAG || xml.getDepth() != depth + 1) continue;
            if (!"item".equals(xml.getName())
                    || xml.getAttributeResourceValue(XmlValues.ANDROID, "id", 0) == android.R.id.mask) {
                XmlValues.skip(xml);
            } else {
                layers.add(readChild(res, xml, theme));
            }
        }
        return new RippleDrawable(color, layers);
    }
}
