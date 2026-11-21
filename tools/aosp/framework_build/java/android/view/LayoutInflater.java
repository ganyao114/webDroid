package android.view;

import android.content.Context;
import android.content.res.TypedArray;
import android.content.res.XmlResourceParser;
import android.util.AttributeSet;
import java.io.IOException;
import java.util.ArrayList;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;
import org.wasmandroid.ui.XmlValues;

public class LayoutInflater {
    public interface Factory { View onCreateView(String name, Context context, AttributeSet attrs); }
    public interface Factory2 extends Factory {
        View onCreateView(View parent, String name, Context context, AttributeSet attrs);
    }
    private final Context context;
    private Factory factory;
    private Factory2 factory2;
    private final ArrayList<Integer> layoutStack = new ArrayList<>();
    private static final int[] INCLUDE_ATTRS = {android.R.attr.id, android.R.attr.visibility, android.R.attr.theme};

    public LayoutInflater(Context context) { this.context = context; }
    protected LayoutInflater(LayoutInflater original, Context context) {
        this.context = context;
        factory = original.factory; factory2 = original.factory2;
    }
    public LayoutInflater cloneInContext(Context context) { return new LayoutInflater(this, context); }
    public Context getContext() { return context; }
    public static LayoutInflater from(Context context) {
        return (LayoutInflater) context.getSystemService(Context.LAYOUT_INFLATER_SERVICE);
    }
    public Factory getFactory() { return factory; }
    public Factory2 getFactory2() { return factory2; }
    public void setFactory(Factory factory) {
        if (this.factory != null) throw new IllegalStateException("LayoutInflater already has a factory");
        if (factory == null) throw new NullPointerException("factory");
        this.factory = factory;
    }
    public void setFactory2(Factory2 factory) { setFactory(factory); factory2 = factory; }
    public View inflate(int resource, ViewGroup root) { return inflate(resource, root, root != null); }
    public View inflate(XmlPullParser parser, ViewGroup root) { return inflate(parser, root, root != null); }

    public View inflate(int resource, ViewGroup root, boolean attachToRoot) {
        push(resource);
        try (XmlResourceParser parser = context.getResources().getLayout(resource)) {
            return inflate(parser, root, attachToRoot);
        } finally {
            layoutStack.remove(layoutStack.size() - 1);
        }
    }
    public View inflate(XmlPullParser parser, ViewGroup root, boolean attachToRoot) {
        if (!(parser instanceof XmlResourceParser)) throw new InflateException("Expected compiled layout XML");
        XmlResourceParser xml = (XmlResourceParser) parser;
        try {
            XmlValues.toStartTag(xml);
            String name = xml.getName();
            if ("merge".equals(name)) {
                if (root == null || !attachToRoot) throw error(xml, "<merge> needs an attached ViewGroup");
                children(xml, root, context);
                return root;
            }
            View view = createFromTag(root, name, context, xml, false);
            ViewGroup.LayoutParams params = root == null ? null : root.generateLayoutParams(xml);
            if (!attachToRoot && params != null) view.setLayoutParams(params);
            children(xml, view, view.getContext());
            view.onFinishInflate();
            if (root != null && attachToRoot) { root.addView(view, params); return root; }
            return view;
        } catch (IOException | XmlPullParserException exception) {
            throw new InflateException(xml.getPositionDescription(), exception);
        }
    }

    private void children(XmlResourceParser xml, View parent, Context context)
            throws XmlPullParserException, IOException {
        int depth = xml.getDepth();
        for (int event = xml.next(); event != XmlPullParser.END_DOCUMENT; event = xml.next()) {
            if (event == XmlPullParser.END_TAG && xml.getDepth() == depth) return;
            if (event != XmlPullParser.START_TAG || xml.getDepth() != depth + 1) continue;
            String name = xml.getName();
            if ("requestFocus".equals(name)) { parent.requestFocus(); XmlValues.skip(xml); continue; }
            if (!(parent instanceof ViewGroup)) throw error(xml, "Parent is not a ViewGroup");
            ViewGroup group = (ViewGroup) parent;
            if ("include".equals(name)) {
                include(xml, group, context);
            } else {
                if ("merge".equals(name)) throw error(xml, "<merge> must be the root element");
                View view = createFromTag(parent, name, context, xml, false);
                ViewGroup.LayoutParams params = group.generateLayoutParams(xml);
                children(xml, view, view.getContext());
                view.onFinishInflate();
                group.addView(view, params);
            }
        }
    }

    private void include(XmlResourceParser include, ViewGroup parent, Context base)
            throws XmlPullParserException, IOException {
        int resource = include.getAttributeResourceValue(null, "layout", 0);
        if (resource == 0) resource = include.getAttributeResourceValue(XmlValues.ANDROID, "layout", 0);
        if (resource == 0) throw error(include, "<include> requires a layout resource");
        int id, visibility, theme;
        try (TypedArray a = base.obtainStyledAttributes(include, INCLUDE_ATTRS)) {
            id = a.getResourceId(0, View.NO_ID);
            visibility = a.getInt(1, -1);
            theme = a.getResourceId(2, 0);
        }
        Context childContext = theme == 0 ? base : new ContextThemeWrapper(base, theme);
        push(resource);
        try (XmlResourceParser xml = childContext.getResources().getLayout(resource)) {
            XmlValues.toStartTag(xml);
            if ("merge".equals(xml.getName())) children(xml, parent, childContext);
            else {
                View view = createFromTag(parent, xml.getName(), childContext, xml, theme != 0);
                ViewGroup.LayoutParams lp;
                try { lp = parent.generateLayoutParams(include); }
                catch (RuntimeException missing) { lp = parent.generateLayoutParams(xml); }
                view.setLayoutParams(lp);
                children(xml, view, view.getContext());
                view.onFinishInflate();
                if (id != View.NO_ID) view.setId(id);
                if (visibility >= 0) view.setVisibility(visibility * 4);
                parent.addView(view, lp);
            }
        } finally {
            layoutStack.remove(layoutStack.size() - 1);
        }
        XmlValues.skip(include);
    }

    private void push(int id) {
        if (layoutStack.contains(id)) throw new InflateException("Recursive layout include " + id);
        layoutStack.add(id);
    }

    private View createFromTag(View parent, String name, Context base, XmlResourceParser xml, boolean ignoreTheme) {
        if ("view".equals(name)) name = xml.getAttributeValue(null, "class");
        if (name == null) throw error(xml, "Missing view class name");
        Context themed = base;
        if (!ignoreTheme) {
            try (TypedArray a = base.obtainStyledAttributes(xml, new int[] {android.R.attr.theme})) {
                int theme = a.getResourceId(0, 0);
                if (theme != 0) themed = new ContextThemeWrapper(base, theme);
            }
        }
        View result = factory2 != null ? factory2.onCreateView(parent, name, themed, xml)
                : factory != null ? factory.onCreateView(name, themed, xml) : null;
        if (result == null) {
            try {
                result = name.indexOf('.') >= 0 ? createView(themed, name, null, xml)
                        : onCreateView(themed, parent, name, xml);
            } catch (ClassNotFoundException missing) {
                throw new InflateException(xml.getPositionDescription() + " cannot find " + name, missing);
            }
        }
        return result;
    }
    protected View onCreateView(String name, AttributeSet attrs) throws ClassNotFoundException {
        return onCreateView(context, null, name, attrs);
    }
    public View onCreateView(Context context, View parent, String name, AttributeSet attrs) throws ClassNotFoundException {
        try { return createView(context, name, "android.widget.", attrs); }
        catch (ClassNotFoundException missing) { return createView(context, name, "android.view.", attrs); }
    }
    public final View createView(String name, String prefix, AttributeSet attrs) throws ClassNotFoundException {
        return createView(context, name, prefix, attrs);
    }
    public final View createView(Context context, String name, String prefix, AttributeSet attrs)
            throws ClassNotFoundException {
        return nativeCreateView(prefix == null ? name : prefix + name, context, attrs);
    }
    private static InflateException error(XmlPullParser xml, String message) {
        return new InflateException(xml.getPositionDescription() + " " + message);
    }
    private static native View nativeCreateView(String name, Context context, AttributeSet attrs)
            throws ClassNotFoundException;
}
