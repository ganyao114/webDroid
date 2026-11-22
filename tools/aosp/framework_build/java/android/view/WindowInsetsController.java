package android.view;

public interface WindowInsetsController {
    int APPEARANCE_LIGHT_STATUS_BARS = 8;
    int APPEARANCE_LIGHT_NAVIGATION_BARS = 16;
    void hide(int types);
    void setSystemBarsAppearance(int appearance, int mask);
    int getSystemBarsAppearance();
}
