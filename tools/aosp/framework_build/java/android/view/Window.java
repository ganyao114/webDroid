package android.view;

import android.content.Context;
import android.graphics.drawable.Drawable;
import android.widget.FrameLayout;

public class Window {
    public static final int FEATURE_NO_TITLE = 1, FEATURE_ACTION_BAR = 8;
    private final Context context;
    private final FrameLayout decor, content;
    private int flags;
    public Window(Context context) {
        this.context = context;
        decor = new FrameLayout(context);
        content = new FrameLayout(context);
        content.setId(android.R.id.content);
        decor.addView(content, new FrameLayout.LayoutParams(-1, -1));
    }
    public Context getContext() { return context; }
    public View getDecorView() { return decor; }
    public View peekDecorView() { return decor; }
    public FrameLayout getContentParent() { return content; }
    public void setBackgroundDrawable(Drawable drawable) { decor.setBackground(drawable); }
    public void setBackgroundDrawableResource(int resId) { decor.setBackgroundResource(resId); }
    public boolean requestFeature(int feature) { return feature == FEATURE_NO_TITLE; }
    public void addFlags(int flags) { this.flags |= flags; }
    public void clearFlags(int flags) { this.flags &= ~flags; }
    public void setFlags(int flags, int mask) { this.flags = (this.flags & ~mask) | (flags & mask); }
    public void setContentView(int resource) {
        content.removeAllViews();
        LayoutInflater.from(context).inflate(resource, content, true);
    }
    public void setContentView(View view) { setContentView(view, new FrameLayout.LayoutParams(-1, -1)); }
    public void setContentView(View view, ViewGroup.LayoutParams params) {
        content.removeAllViews();
        content.addView(view, params);
    }
    public void addContentView(View view, ViewGroup.LayoutParams params) { content.addView(view, params); }
    public <T extends View> T findViewById(int id) { return decor.findViewById(id); }
    public View getCurrentFocus() { return decor.findFocus(); }
}
