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

public class LayerDrawable extends Drawable implements Drawable.Callback {
    private static final class Layer {
        Drawable drawable;
        int id = -1, left, top, right, bottom;
        Layer(Drawable drawable) { this.drawable = drawable; }
    }
    private final ArrayList<Layer> layers = new ArrayList<>();

    public LayerDrawable(Drawable[] drawables) {
        for (Drawable drawable : drawables) addLayer(drawable);
    }
    public int addLayer(Drawable drawable) {
        Layer layer = new Layer(drawable);
        layers.add(layer);
        if (drawable != null) drawable.setCallback(this);
        onBoundsChange(getBounds());
        return layers.size() - 1;
    }
    public int getNumberOfLayers() { return layers.size(); }
    public Drawable getDrawable(int index) { return layers.get(index).drawable; }
    public int getId(int index) { return layers.get(index).id; }
    public void setId(int index, int id) { layers.get(index).id = id; }
    public Drawable findDrawableByLayerId(int id) {
        for (int i = layers.size() - 1; i >= 0; --i) if (layers.get(i).id == id) return getDrawable(i);
        return null;
    }
    public void setLayerInset(int index, int l, int t, int r, int b) {
        Layer layer = layers.get(index);
        layer.left = l; layer.top = t; layer.right = r; layer.bottom = b;
        onBoundsChange(getBounds());
        invalidateSelf();
    }
    @Override protected void onBoundsChange(Rect b) {
        for (Layer layer : layers) {
            if (layer.drawable != null) layer.drawable.setBounds(b.left + layer.left, b.top + layer.top,
                    b.right - layer.right, b.bottom - layer.bottom);
        }
    }
    @Override public void draw(Canvas canvas) {
        for (Layer layer : layers) if (layer.drawable != null) layer.drawable.draw(canvas);
    }
    @Override public void setAlpha(int alpha) {
        super.setAlpha(alpha);
        for (Layer layer : layers) if (layer.drawable != null) layer.drawable.setAlpha(alpha);
    }
    @Override public boolean isStateful() {
        for (Layer layer : layers) if (layer.drawable != null && layer.drawable.isStateful()) return true;
        return false;
    }
    @Override protected boolean onStateChange(int[] states) {
        boolean changed = false;
        for (Layer layer : layers) if (layer.drawable != null) changed |= layer.drawable.setState(states);
        return changed;
    }
    @Override public int getIntrinsicWidth() { return intrinsic(true); }
    @Override public int getIntrinsicHeight() { return intrinsic(false); }
    private int intrinsic(boolean horizontal) {
        int result = -1;
        for (Layer layer : layers) {
            if (layer.drawable == null) continue;
            int size = horizontal ? layer.drawable.getIntrinsicWidth() : layer.drawable.getIntrinsicHeight();
            if (size >= 0) result = Math.max(result, size + (horizontal ? layer.left + layer.right : layer.top + layer.bottom));
        }
        return result;
    }
    @Override public void invalidateDrawable(Drawable drawable) { invalidateSelf(); }
    @Override public void scheduleDrawable(Drawable drawable, Runnable task, long when) { scheduleSelf(task, when); }
    @Override public void unscheduleDrawable(Drawable drawable, Runnable task) { unscheduleSelf(task); }

    static LayerDrawable inflateLayers(Resources res, XmlResourceParser xml, Resources.Theme theme)
            throws IOException, XmlPullParserException {
        LayerDrawable result = new LayerDrawable(new Drawable[0]);
        int depth = xml.getDepth();
        for (int event = xml.next(); event != XmlPullParser.END_DOCUMENT; event = xml.next()) {
            if (event == XmlPullParser.END_TAG && xml.getDepth() == depth) break;
            if (event != XmlPullParser.START_TAG || xml.getDepth() != depth + 1 || !"item".equals(xml.getName())) continue;
            XmlValues v = new XmlValues(res, theme, xml);
            int id = v.id("id", -1);
            int l = v.pixels("left", 0), t = v.pixels("top", 0), r = v.pixels("right", 0), b = v.pixels("bottom", 0);
            int layer = result.addLayer(readChild(res, xml, theme));
            result.setId(layer, id);
            result.setLayerInset(layer, l, t, r, b);
        }
        return result;
    }
}
