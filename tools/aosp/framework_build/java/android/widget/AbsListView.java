package android.widget;

import android.content.Context;
import android.util.AttributeSet;
import android.view.ViewGroup;

public abstract class AbsListView extends ViewGroup {
    public static class LayoutParams extends ViewGroup.LayoutParams {
        public LayoutParams(int width, int height) { super(width, height); }
    }

    protected AbsListView(Context context, AttributeSet attrs) { super(context, attrs); }
}
