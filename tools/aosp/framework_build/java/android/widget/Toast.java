package android.widget;

import android.app.Activity;
import android.content.Context;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.view.Gravity;
import android.view.ViewGroup;

public final class Toast {
    public static final int LENGTH_SHORT = 0, LENGTH_LONG = 1;
    private final Context context;
    private final CharSequence text;
    private final int duration;
    private TextView view;

    private Toast(Context context, CharSequence text, int duration) {
        this.context = context;
        this.text = text;
        this.duration = duration;
    }

    public static Toast makeText(Context context, CharSequence text, int duration) {
        return new Toast(context, text, duration);
    }
    public static Toast makeText(Context context, int resource, int duration) {
        return makeText(context, context.getText(resource), duration);
    }
    public void show() {
        if (!(context instanceof Activity)) return;
        cancel();
        Activity activity = (Activity) context;
        float density = activity.getResources().getDisplayMetrics().density;
        TextView message = new TextView(activity);
        message.setText(text);
        message.setTextColor(0xfff4f1fb);
        message.setPadding((int) (18 * density), (int) (10 * density),
                (int) (18 * density), (int) (10 * density));
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xf02d2b36);
        background.setCornerRadius(20 * density);
        message.setBackground(background);
        FrameLayout.LayoutParams layout = new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        layout.bottomMargin = (int) (40 * density);
        activity.getWindow().getWindowManager().addView(message, layout);
        view = message;
        new Handler(activity.getMainLooper()).postDelayed(() -> {
            if (view == message) cancel();
        }, duration == LENGTH_LONG ? 3500 : 2000);
    }
    public void cancel() {
        if (view != null) {
            if (view.getParent() instanceof ViewGroup) ((ViewGroup) view.getParent()).removeView(view);
            view = null;
        }
    }
}
