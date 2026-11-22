/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.app;

import android.content.Context;
import android.content.res.Resources;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;

/**
 * The locally hosted lifecycle of AOSP's android.app.Fragment. The embedded Activity calls the
 * lifecycle callbacks directly because its window has no platform FragmentManager.
 */
public class Fragment {
    private Context mContext;
    private Bundle mArguments;

    public Fragment() {}

    public void setArguments(Bundle args) {
        mArguments = args;
    }

    public final Bundle getArguments() {
        return mArguments;
    }

    public Context getContext() {
        return mContext;
    }

    public final Activity getActivity() {
        return mContext instanceof Activity ? (Activity) mContext : null;
    }

    public final Resources getResources() {
        if (mContext == null) {
            throw new IllegalStateException("Fragment " + this + " not attached to Activity");
        }
        return mContext.getResources();
    }

    public final CharSequence getText(int resId) {
        return getResources().getText(resId);
    }

    public final String getString(int resId) {
        return getResources().getString(resId);
    }

    public void onAttach(Context context) {
        mContext = context;
        final Activity activity = getActivity();
        if (activity != null) {
            onAttach(activity);
        }
    }

    public void onAttach(Activity activity) {}
    public void onCreate(Bundle savedInstanceState) {}

    public View onCreateView(LayoutInflater inflater, ViewGroup container, Bundle savedInstanceState) {
        return null;
    }

    public void onResume() {}
    public void onDestroyView() {}
    public void onDestroy() {}

    public void onDetach() {
        mContext = null;
    }
}
