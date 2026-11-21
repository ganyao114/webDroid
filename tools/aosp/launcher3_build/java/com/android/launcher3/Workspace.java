package com.android.launcher3;

import android.content.Context;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;
import android.widget.OverScroller;
import com.android.launcher3.pageindicators.PageIndicator;
import com.android.launcher3.util.IntSet;

/** Connects the upstream pager to cells inflated from Launcher3's workspace_screen.xml. */
public final class Workspace<T extends View & PageIndicator> extends PagedView<T> {
    public static final IntSet EXTRA_EMPTY_SCREEN_IDS = IntSet.wrap(-201, -200);

    public Workspace(Context context, AttributeSet attrs) {
        super(context, attrs);
        LogDecelerateInterpolator easing = new LogDecelerateInterpolator(100, 0);
        mScroller = new OverScroller(context, easing::getInterpolation);
    }

    public void resetPages() {
        mScroller.forceFinished(true);
        mCurrentPage = 0;
        mCurrentScrollOverPage = 0;
        mCurrentPageScrollDiff = 0;
        mNextPage = INVALID_PAGE;
        mPageScrolls = null;
        mFirstLayout = true;
        removeAllViews();
    }

    public void addPage(CellLayout cells) {
        addView(cells, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
    }

    public void selectPage(int page, boolean animate) {
        if (getChildCount() == 0) return;
        runOnPageScrollsInitialized(() -> {
            if (animate) snapToPage(page);
            else setCurrentPage(page);
        });
    }

    @Override
    protected String getCurrentPageDescription() {
        return getContext().getString(R.string.workspace_scroll_format, getNextPage() + 1, getChildCount());
    }

    @Override
    protected void determineScrollingStart(MotionEvent event) {
        int index = event.findPointerIndex(mActivePointerId);
        if (index >= 0 && Math.abs(event.getX(index) - getDownMotionX())
                > Math.abs(event.getY(index) - getDownMotionY())) {
            super.determineScrollingStart(event);
        }
    }
}
