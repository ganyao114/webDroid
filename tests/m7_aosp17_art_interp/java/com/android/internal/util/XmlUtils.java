package com.android.internal.util;

public final class XmlUtils {
    public static int convertValueToList(CharSequence value, String[] options, int fallback) {
        if (value != null) {
            for (int i = 0; i < options.length; i++) {
                if (options[i].equals(value.toString())) return i;
            }
        }
        return fallback;
    }

    public static int convertValueToInt(CharSequence value, int fallback) {
        if (value == null) return fallback;
        String number = value.toString();
        int start = number.startsWith("-") || number.startsWith("+") ? 1 : 0;
        int radix = 10;
        if (number.startsWith("0x", start) || number.startsWith("0X", start)) {
            number = number.substring(0, start) + number.substring(start + 2);
            radix = 16;
        } else if (number.startsWith("#", start)) {
            number = number.substring(0, start) + number.substring(start + 1);
            radix = 16;
        } else if (number.startsWith("0", start) && number.length() - start > 1) {
            radix = 8;
        }
        return Integer.parseInt(number, radix);
    }
}
