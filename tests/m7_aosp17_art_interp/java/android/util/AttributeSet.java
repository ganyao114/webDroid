package android.util;

public interface AttributeSet {
    boolean getAttributeBooleanValue(int index, boolean fallback);
    boolean getAttributeBooleanValue(String namespace, String attribute, boolean fallback);
    int getAttributeCount();
    float getAttributeFloatValue(int index, float fallback);
    float getAttributeFloatValue(String namespace, String attribute, float fallback);
    int getAttributeIntValue(int index, int fallback);
    int getAttributeIntValue(String namespace, String attribute, int fallback);
    int getAttributeListValue(int index, String[] options, int fallback);
    int getAttributeListValue(String namespace, String attribute, String[] options, int fallback);
    String getAttributeName(int index);
    int getAttributeNameResource(int index);
    String getAttributeNamespace(int index);
    int getAttributeResourceValue(int index, int fallback);
    int getAttributeResourceValue(String namespace, String attribute, int fallback);
    int getAttributeUnsignedIntValue(int index, int fallback);
    int getAttributeUnsignedIntValue(String namespace, String attribute, int fallback);
    String getAttributeValue(int index);
    String getAttributeValue(String namespace, String attribute);
    String getClassAttribute();
    String getIdAttribute();
    int getIdAttributeResourceValue(int fallback);
    String getPositionDescription();
    int getStyleAttribute();
}
