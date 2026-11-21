package android.graphics.drawable;

import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.util.PathParser;
import java.io.IOException;
import java.util.ArrayList;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;
import org.wasmandroid.ui.NativeMath;
import org.wasmandroid.ui.XmlValues;

public class VectorDrawable extends Drawable {
    private final Group root;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private float viewportWidth = 1, viewportHeight = 1;
    private int width = -1, height = -1, xmlAlpha = 255;

    private interface Node {
        void draw(Canvas canvas, VectorDrawable owner, int alpha);
    }
    private static final class Group implements Node {
        final ArrayList<Node> children = new ArrayList<>();
        float pivotX, pivotY, translateX, translateY, rotation, scaleX = 1, scaleY = 1;
        @Override public void draw(Canvas canvas, VectorDrawable owner, int alpha) {
            int save = canvas.save();
            try {
                canvas.translate(translateX + pivotX, translateY + pivotY);
                canvas.rotate(rotation);
                canvas.scale(scaleX, scaleY);
                canvas.translate(-pivotX, -pivotY);
                for (int i = 0; i < children.size(); i++) children.get(i).draw(canvas, owner, alpha);
            } finally {
                canvas.restoreToCount(save);
            }
        }
    }
    private static final class Figure implements Node {
        Path path;
        int fill, stroke;
        float strokeWidth;
        @Override public void draw(Canvas canvas, VectorDrawable owner, int alpha) {
            Paint paint = owner.paint;
            if (fill >>> 24 != 0) {
                paint.setColor(owner.tinted(withAlpha(fill, alpha / 255f)));
                paint.setStyle(Paint.Style.FILL);
                canvas.drawPath(path, paint);
            }
            if (strokeWidth > 0 && stroke >>> 24 != 0) {
                paint.setColor(owner.tinted(withAlpha(stroke, alpha / 255f)));
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(strokeWidth);
                canvas.drawPath(path, paint);
            }
        }
    }

    public VectorDrawable() { root = new Group(); }
    private VectorDrawable(VectorDrawable source) {
        root = source.root;
        viewportWidth = source.viewportWidth; viewportHeight = source.viewportHeight;
        width = source.width; height = source.height; xmlAlpha = source.xmlAlpha;
    }
    @Override public ConstantState getConstantState() {
        return new ConstantState() {
            @Override public Drawable newDrawable() { return new VectorDrawable(VectorDrawable.this); }
            @Override public int getChangingConfigurations() { return 0; }
        };
    }
    @Override public int getIntrinsicWidth() { return width; }
    @Override public int getIntrinsicHeight() { return height; }
    @Override public void draw(Canvas canvas) {
        Rect b = getBounds();
        if (b.isEmpty() || getAlpha() == 0) return;
        int save = canvas.save();
        try {
            canvas.clipRect(b);
            canvas.translate(b.left, b.top);
            canvas.scale(b.width() / viewportWidth, b.height() / viewportHeight);
            root.draw(canvas, this, (getAlpha() * xmlAlpha + 127) / 255);
        } finally {
            canvas.restoreToCount(save);
        }
    }
    private static int withAlpha(int color, float alpha) {
        return color & 0xffffff | (NativeMath.round((color >>> 24) * Math.max(0, Math.min(1, alpha))) << 24);
    }

    static VectorDrawable inflateVector(Resources res, XmlResourceParser xml, Resources.Theme theme)
            throws IOException, XmlPullParserException {
        VectorDrawable result = new VectorDrawable();
        XmlValues values = new XmlValues(res, theme, xml);
        result.width = values.pixels("width", -1);
        result.height = values.pixels("height", -1);
        result.viewportWidth = values.number("viewportWidth", 0);
        result.viewportHeight = values.number("viewportHeight", 0);
        result.xmlAlpha = Math.max(0, Math.min(255, NativeMath.round(values.number("alpha", 1) * 255)));
        if (result.viewportWidth <= 0 || result.viewportHeight <= 0 || result.width <= 0 || result.height <= 0) {
            throw new XmlPullParserException("Vector dimensions and viewport must be positive");
        }
        inflateGroup(result.root, res, xml, theme);
        return result;
    }

    private static void inflateGroup(Group group, Resources res, XmlResourceParser xml, Resources.Theme theme)
            throws IOException, XmlPullParserException {
        int depth = xml.getDepth();
        for (int event = xml.next(); event != XmlPullParser.END_DOCUMENT; event = xml.next()) {
            if (event == XmlPullParser.END_TAG && xml.getDepth() == depth) break;
            if (event != XmlPullParser.START_TAG || xml.getDepth() != depth + 1) continue;
            XmlValues attrs = new XmlValues(res, theme, xml);
            switch (xml.getName()) {
                case "group": {
                    Group child = new Group();
                    child.pivotX = attrs.number("pivotX", 0);
                    child.pivotY = attrs.number("pivotY", 0);
                    child.translateX = attrs.number("translateX", 0);
                    child.translateY = attrs.number("translateY", 0);
                    child.rotation = attrs.number("rotation", 0);
                    child.scaleX = attrs.number("scaleX", 1);
                    child.scaleY = attrs.number("scaleY", 1);
                    group.children.add(child);
                    inflateGroup(child, res, xml, theme);
                    break;
                }
                case "path": {
                    Figure figure = new Figure();
                    String data = attrs.text("pathData", null);
                    if (data == null) throw new XmlPullParserException("Vector path has no pathData");
                    figure.path = PathParser.createPathFromPathData(data);
                    figure.path.setFillType(attrs.integer("fillType", 0) == 1 ? Path.FillType.EVEN_ODD : Path.FillType.WINDING);
                    figure.fill = withAlpha(attrs.color("fillColor", 0), attrs.number("fillAlpha", 1));
                    figure.stroke = withAlpha(attrs.color("strokeColor", 0), attrs.number("strokeAlpha", 1));
                    figure.strokeWidth = attrs.number("strokeWidth", 0);
                    group.children.add(figure);
                    XmlValues.skip(xml);
                    break;
                }
                default: XmlValues.skip(xml); break;
            }
        }
    }
}
