package com.android.launcher3;

public final class LauncherUiState {
    private final DeviceProfileRef profile;

    LauncherUiState(DeviceProfile deviceProfile) { profile = new DeviceProfileRef(deviceProfile); }
    public DeviceProfileRef getDeviceProfileRef() { return profile; }

    public record DeviceProfileRef(DeviceProfile value) {
        public DeviceProfile getValue() { return value; }
    }
}
