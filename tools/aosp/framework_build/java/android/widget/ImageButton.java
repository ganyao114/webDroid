package android.widget;

import android.content.Context;
import android.util.AttributeSet;

public class ImageButton extends ImageView {
    public ImageButton(Context context) { this(context, null); }
    public ImageButton(Context context, AttributeSet attrs) { this(context, attrs, android.R.attr.imageButtonStyle); }
    public ImageButton(Context context, AttributeSet attrs, int style) {
        super(context, attrs, style);
        setClickable(true);
        setFocusable(true);
    }
}
