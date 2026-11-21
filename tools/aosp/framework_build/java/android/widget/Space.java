package android.widget;

import android.content.Context;
import android.util.AttributeSet;
import android.view.View;

public final class Space extends View {
    public Space(Context context) { this(context, null); }
    public Space(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public Space(Context context, AttributeSet attrs, int defStyle) {
        super(context, attrs, defStyle);
        if (getVisibility() == VISIBLE) setVisibility(INVISIBLE);
    }
    @Override protected void onMeasure(int w, int h) {
        setMeasuredDimension(resolveSize(getSuggestedMinimumWidth(), w), resolveSize(getSuggestedMinimumHeight(), h));
    }
}
