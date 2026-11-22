package android.database;

import android.database.sqlite.SQLiteStatement;

public final class DatabaseUtils {
    private DatabaseUtils() {}
    public static long longForQuery(SQLiteStatement statement, String[] selectionArgs) {
        if (selectionArgs != null) statement.bindAllArgsAsStrings(selectionArgs);
        return statement.simpleQueryForLong();
    }

    public static int getTypeOfObject(Object value) {
        if (value == null) return Cursor.FIELD_TYPE_NULL;
        if (value instanceof byte[]) return Cursor.FIELD_TYPE_BLOB;
        if (value instanceof Double || value instanceof Float) return Cursor.FIELD_TYPE_FLOAT;
        if (value instanceof Number) return Cursor.FIELD_TYPE_INTEGER;
        return Cursor.FIELD_TYPE_STRING;
    }
}
