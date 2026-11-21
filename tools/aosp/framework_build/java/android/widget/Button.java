package android.widget;

import android.content.Context;
import android.util.AttributeSet;

public class Button extends TextView {
    public Button(Context context) { this(context, null); }
    public Button(Context context, AttributeSet attrs) { this(context, attrs, android.R.attr.buttonStyle); }
    public Button(Context context, AttributeSet attrs, int defStyle) { this(context, attrs, defStyle, 0); }
    public Button(Context context, AttributeSet attrs, int defStyle, int defRes) {
        super(context, attrs, defStyle, defRes);
        setClickable(true);
        setFocusable(true);
    }
}
