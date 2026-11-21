package com.android.launcher3.widgetpicker;

import android.app.Activity;
import android.os.Bundle;

/** Preserves the manifest entry point on SDK builds without a widget host. */
public final class WidgetPickerActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedState) {
        super.onCreate(savedState);
        setResult(RESULT_CANCELED);
        finish();
    }
}
