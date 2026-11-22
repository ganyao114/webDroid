package android.app;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;

/** Search client backed by the embedded package catalog. */
public class SearchManager {
    public static final String INTENT_ACTION_GLOBAL_SEARCH = "android.search.action.GLOBAL_SEARCH";
    private final Context context;

    SearchManager(Context context) { this.context = context; }

    public ComponentName getGlobalSearchActivity() {
        var activity = context.getPackageManager().resolveActivity(new Intent(INTENT_ACTION_GLOBAL_SEARCH),
                PackageManager.MATCH_DEFAULT_ONLY);
        return activity == null ? null : new ComponentName(activity.activityInfo.packageName, activity.activityInfo.name);
    }
}
