package android.view;

import android.content.Context;
import android.content.res.TypedArray;
import android.util.AttributeSet;

public final class ViewStub extends View {
    public interface OnInflateListener { void onInflate(ViewStub stub, View inflated); }
    private int resource, inflatedId = NO_ID;
    private View inflated;
    private LayoutInflater inflater;
    private OnInflateListener listener;

    public ViewStub(Context context) { this(context, null); }
    public ViewStub(Context context, int resource) { this(context); this.resource = resource; }
    public ViewStub(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public ViewStub(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        if (attrs != null) {
            try (TypedArray a = context.obtainStyledAttributes(attrs,
                    new int[] {android.R.attr.layout, android.R.attr.inflatedId})) {
                resource = a.getResourceId(0, 0);
                inflatedId = a.getResourceId(1, NO_ID);
            }
        }
        super.setVisibility(GONE);
    }
    public int getLayoutResource() { return resource; }
    public void setLayoutResource(int resource) { this.resource = resource; }
    public int getInflatedId() { return inflatedId; }
    public void setInflatedId(int id) { inflatedId = id; }
    public LayoutInflater getLayoutInflater() { return inflater; }
    public void setLayoutInflater(LayoutInflater inflater) { this.inflater = inflater; }
    public void setOnInflateListener(OnInflateListener listener) { this.listener = listener; }
    @Override protected void onMeasure(int w, int h) { setMeasuredDimension(0, 0); }
    @Override public void setVisibility(int visibility) {
        if (inflated != null) { inflated.setVisibility(visibility); return; }
        super.setVisibility(visibility);
        if (visibility == VISIBLE || visibility == INVISIBLE) inflate().setVisibility(visibility);
    }
    public View inflate() {
        if (!(getParent() instanceof ViewGroup)) throw new IllegalStateException("ViewStub has no parent");
        if (resource == 0) throw new IllegalArgumentException("ViewStub has no layout resource");
        ViewGroup parent = (ViewGroup) getParent();
        LayoutInflater use = inflater != null ? inflater : LayoutInflater.from(getContext());
        View view = use.inflate(resource, parent, false);
        if (inflatedId != NO_ID) view.setId(inflatedId);
        int index = parent.indexOfChild(this);
        parent.removeViewAt(index);
        parent.addView(view, index, getLayoutParams());
        inflated = view;
        if (listener != null) listener.onInflate(this, view);
        return view;
    }
}
