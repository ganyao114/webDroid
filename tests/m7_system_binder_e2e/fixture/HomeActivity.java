package org.wasmandroid.home;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

public final class HomeActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        TextView view = new TextView(this);
        view.setText(R.string.app_name);
        setContentView(view);
    }
}
