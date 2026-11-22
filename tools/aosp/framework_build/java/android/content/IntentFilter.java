/*
 * Copyright (C) 2006 The Android Open Source Project
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

package android.content;

import android.net.Uri;
import android.util.ArraySet;
import java.util.Set;

/** The action and category subset of AOSP IntentFilter for local broadcasts without data. */
public class IntentFilter {
    public static final int MATCH_ADJUSTMENT_NORMAL = 0x8000;
    public static final int MATCH_CATEGORY_EMPTY = 0x0100000;
    public static final int NO_MATCH_DATA = -2;
    public static final int NO_MATCH_ACTION = -3;
    public static final int NO_MATCH_CATEGORY = -4;

    private final ArraySet<String> mActions = new ArraySet<>();
    private final ArraySet<String> mCategories = new ArraySet<>();

    public IntentFilter() {}

    public IntentFilter(String action) {
        addAction(action);
    }

    public final void addAction(String action) {
        mActions.add(action.intern());
    }

    public final int countActions() {
        return mActions.size();
    }

    public final String getAction(int index) {
        return mActions.valueAt(index);
    }

    public final boolean hasAction(String action) {
        return action != null && mActions.contains(action);
    }

    public final void addCategory(String category) {
        mCategories.add(category.intern());
    }

    public final int match(String action, String type, String scheme, Uri data,
            Set<String> categories, String logTag) {
        if (action != null && !hasAction(action)) return NO_MATCH_ACTION;
        if (type != null || data != null) return NO_MATCH_DATA;
        if (categories != null) {
            for (String category : categories) {
                if (!mCategories.contains(category)) return NO_MATCH_CATEGORY;
            }
        }
        return MATCH_CATEGORY_EMPTY + MATCH_ADJUSTMENT_NORMAL;
    }
}
