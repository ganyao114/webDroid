package android.view;

import android.graphics.Outline;

public abstract class ViewOutlineProvider {
    public abstract void getOutline(View view, Outline outline);
}
