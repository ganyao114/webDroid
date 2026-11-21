package com.android.launcher3.allapps;

import android.content.Context;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.text.Editable;
import android.text.TextWatcher;
import android.util.AttributeSet;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.InputMethodManager;
import android.widget.AbsListView;
import android.widget.BaseAdapter;
import android.widget.FrameLayout;
import android.widget.GridView;
import android.widget.LinearLayout;
import android.widget.TextView;
import com.android.launcher3.AbstractFloatingView;
import com.android.launcher3.BubbleTextView;
import com.android.launcher3.DeviceProfile;
import com.android.launcher3.Flags;
import com.android.launcher3.R;
import com.android.launcher3.allapps.search.AppsSearchContainerLayout;
import com.android.launcher3.model.data.AppInfo;
import com.android.launcher3.search.StringMatcherUtility;
import com.android.launcher3.util.IntArray;
import java.text.Normalizer;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.regex.Pattern;

public class ActivityAllAppsContainerView extends AbstractFloatingView {
    public record Entry(AppInfo item, Function<Context, Drawable> icon) {}

    private static final Pattern MARKS = Pattern.compile("\\p{M}+");
    private static final Pattern SPACES = Pattern.compile("\\s+", Pattern.UNICODE_CHARACTER_CLASS);

    private final AppsSearchContainerLayout search;
    private final TextView empty;
    private final GridView grid;
    private final Adapter adapter = new Adapter();
    private final IntArray matches = new IntArray();
    private final StringMatcherUtility.StringMatcher matcher =
            StringMatcherUtility.StringMatcher.getInstance();
    private List<Entry> entries = List.of();
    private final List<String> searchKeys = new ArrayList<>();
    private DeviceProfile profile;
    private Consumer<AppInfo> launch;
    private Runnable close;
    private float downY;
    private boolean dragFromHeader;

    public ActivityAllAppsContainerView(Context context, AttributeSet attrs) {
        super(context, attrs);
        setVisibility(GONE);
        setFocusableInTouchMode(true);
        var column = new LinearLayout(context);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(px(12), px(16), px(12), 0);

        GradientDrawable background = new GradientDrawable();
        int radius = px(28);
        background.setCornerRadii(new float[]{radius, radius, radius, radius, 0, 0, 0, 0});
        background.setColor(context.getColor(R.color.materialColorSurface));
        column.setBackground(background);
        var panel = new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT);
        panel.topMargin = px(42);
        addView(column, panel);

        search = (AppsSearchContainerLayout) LayoutInflater.from(context)
                .inflate(R.layout.search_container_all_apps, column, false);
        search.setGravity(Gravity.CENTER_VERTICAL | Gravity.START);
        search.setBackgroundResource(R.drawable.bg_all_apps_searchbox);
        search.setHint(R.string.all_apps_search_bar_hint);
        Drawable icon = context.getDrawable(R.drawable.ic_allapps_search).mutate();
        icon.setTint(context.getColor(R.color.materialColorOnSurfaceVariant));
        icon.setBounds(0, 0, px(24), px(24));
        search.setCompoundDrawablesRelative(icon, null, null, null);
        search.setCompoundDrawablePadding(px(10));
        column.addView(search);

        var resultPanel = new FrameLayout(context);
        column.addView(resultPanel, new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, 0, 1f));
        grid = new GridView(context);
        grid.setClipToPadding(false);
        grid.setPadding(0, px(12), 0, px(16));
        grid.setStretchMode(GridView.STRETCH_COLUMN_WIDTH);
        grid.setAdapter(adapter);
        resultPanel.addView(grid, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        empty = new TextView(context);
        empty.setTextColor(context.getColor(R.color.materialColorOnSurfaceVariant));
        empty.setGravity(Gravity.CENTER);
        resultPanel.addView(empty, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        grid.setEmptyView(empty);

        search.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence value, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence value, int start, int before, int count) {
                filter(value.toString());
            }
            @Override public void afterTextChanged(Editable value) {}
        });
        search.setOnEditorActionListener((view, action, event) -> {
            if (matches.isEmpty() || launch == null) return false;
            launch.accept(entries.get(matches.get(0)).item);
            return true;
        });
        setOnClickListener(view -> {
            if (close != null) close.run();
        });
    }

    private int px(float dp) { return DeviceProfile.pixels(getResources(), dp); }

    public void configure(DeviceProfile profile, Consumer<AppInfo> launch, Runnable close) {
        this.profile = profile;
        this.launch = launch;
        this.close = close;
        grid.setNumColumns(profile.columns);
        adapter.notifyDataSetChanged();
    }

    public void setApps(List<Entry> apps) {
        entries = List.copyOf(apps);
        searchKeys.clear();
        for (Entry entry : entries) {
            searchKeys.add(normalize(entry.item.title + " " + entry.item.componentName.getPackageName()));
        }
        filter(search.getText().toString());
    }

    private static String normalize(String value) {
        String normalized = Normalizer.normalize(value, Normalizer.Form.NFKD).toLowerCase(Locale.ROOT);
        return MARKS.matcher(normalized).replaceAll("").strip();
    }

    private void filter(String query) {
        matches.clear();
        String[] terms = SPACES.split(normalize(query));
        for (int index = 0; index < entries.size(); index++) {
            String key = searchKeys.get(index);
            boolean match = true;
            for (String term : terms) {
                if (!key.contains(term) && !StringMatcherUtility.matches(term,
                        entries.get(index).item.title.toString(), matcher)) {
                    match = false;
                    break;
                }
            }
            if (match) matches.add(index);
        }
        empty.setText(query.isBlank() ? "" : getResources().getString(R.string.all_apps_no_search_results, query));
        adapter.notifyDataSetChanged();
        grid.setSelection(0);
    }

    public void takeDrawerFocus() { if (!search.requestFocus()) requestFocus(); }

    public void finishSearch() {
        InputMethodManager keyboard = getContext().getSystemService(InputMethodManager.class);
        keyboard.hideSoftInputFromWindow(search.getWindowToken(), 0);
        search.clearFocus();
        search.setText("");
    }

    @Override
    public boolean onInterceptTouchEvent(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                downY = event.getY();
                dragFromHeader = downY < px(42) + search.getBottom();
                break;
            case MotionEvent.ACTION_MOVE:
                if (dragFromHeader && event.getY() - downY > px(12)) return true;
                break;
            case MotionEvent.ACTION_POINTER_DOWN:
            case MotionEvent.ACTION_CANCEL:
            case MotionEvent.ACTION_UP:
                dragFromHeader = false;
                break;
            default:
                break;
        }
        return super.onInterceptTouchEvent(event);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (dragFromHeader && event.getActionMasked() == MotionEvent.ACTION_UP
                && event.getY() - downY > px(42)) {
            dragFromHeader = false;
            if (close != null) close.run();
            return true;
        }
        return super.onTouchEvent(event);
    }

    private final class Adapter extends BaseAdapter {
        @Override public int getCount() { return matches.size(); }
        @Override public Entry getItem(int position) { return entries.get(matches.get(position)); }
        @Override public long getItemId(int position) { return matches.get(position); }
        @Override public boolean hasStableIds() { return true; }

        @Override
        public View getView(int position, View recycled, ViewGroup parent) {
            Entry entry = getItem(position);
            BubbleTextView icon = recycled instanceof BubbleTextView existing ? existing
                    : (BubbleTextView) LayoutInflater.from(getContext()).inflate(R.layout.all_apps_icon, parent, false);
            if (Flags.enableTwolineAllapps()) {
                icon.setSingleLine(false);
                icon.setMaxLines(2);
            }
            icon.setLayoutParams(new AbsListView.LayoutParams(LayoutParams.MATCH_PARENT, profile.drawerCellPx));
            icon.bind(entry.item, entry.icon.apply(getContext()), profile, true, view -> launch.accept(entry.item));
            return icon;
        }
    }
}
