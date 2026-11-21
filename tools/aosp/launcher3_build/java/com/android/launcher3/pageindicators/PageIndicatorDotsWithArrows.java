package com.android.launcher3.pageindicators;

import android.content.Context;
import android.util.AttributeSet;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.ImageButton;
import com.android.launcher3.DeviceProfile;
import com.android.launcher3.R;
import java.util.function.IntConsumer;

public final class PageIndicatorDotsWithArrows extends FrameLayout implements PageIndicator {
    private final View previous;
    private final View next;
    private final PageIndicatorDots dots;
    private int count;
    private int activePage;

    public PageIndicatorDotsWithArrows(Context context, AttributeSet attrs) {
        super(context, attrs);
        LayoutInflater.from(context).inflate(R.layout.page_indicator_dots_with_arrows, this, true);
        previous = findViewById(R.id.left_indicator_arrow);
        next = findViewById(R.id.right_indicator_arrow);
        dots = findViewById(R.id.page_indicator_dots);
        dots.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
    }

    public void setActions(IntConsumer move, Runnable openDrawer) {
        previous.setOnClickListener(view -> move.accept(-1));
        next.setOnClickListener(view -> move.accept(1));

        ImageButton drawer = new ImageButton(getContext());
        drawer.setBackgroundResource(R.drawable.bg_all_apps_button);
        drawer.setImageResource(R.drawable.ic_apps);
        drawer.setContentDescription(getResources().getString(R.string.all_apps_button_label));
        drawer.setOnClickListener(view -> openDrawer.run());
        int size = DeviceProfile.pixels(getResources(), 40);
        addView(drawer, new LayoutParams(size, size, Gravity.END | Gravity.CENTER_VERTICAL));
    }

    @Override public void setScroll(int current, int total) { dots.setScroll(current, total); }

    @Override
    public void setActiveMarker(int page) {
        activePage = page;
        dots.setActiveMarker(page);
        updateArrows();
    }

    @Override
    public void setMarkersCount(int count) {
        this.count = count;
        dots.setMarkersCount(count);
        updateArrows();
    }

    @Override
    public void setPauseScroll(boolean pause, boolean twoPanels) {
        dots.setPauseScroll(pause, twoPanels);
    }

    private void updateArrows() {
        previous.setVisibility(count > 1 ? VISIBLE : GONE);
        next.setVisibility(previous.getVisibility());
        previous.setEnabled(activePage > 0);
        next.setEnabled(activePage < count - 1);
        previous.setAlpha(previous.isEnabled() ? 1f : 0.35f);
        next.setAlpha(next.isEnabled() ? 1f : 0.35f);
    }
}
