package androidx.appfunctions.service;

import android.app.appfunctions.AppFunctionException;
import android.app.appfunctions.AppFunctionService;
import android.app.appfunctions.ExecuteAppFunctionRequest;
import android.app.appfunctions.ExecuteAppFunctionResponse;
import android.content.pm.SigningInfo;
import android.os.CancellationSignal;
import android.os.OutcomeReceiver;

/** SDK service adapter for builds without the optional workspace-function library. */
public final class ExtensionAppFunctionService extends AppFunctionService {
    @Override
    public void onExecuteFunction(ExecuteAppFunctionRequest request, String caller, SigningInfo signing,
                                  CancellationSignal cancellation,
                                  OutcomeReceiver<ExecuteAppFunctionResponse, AppFunctionException> callback) {
        callback.onError(new AppFunctionException(AppFunctionException.ERROR_FUNCTION_NOT_FOUND,
                request.getFunctionIdentifier()));
    }
}
