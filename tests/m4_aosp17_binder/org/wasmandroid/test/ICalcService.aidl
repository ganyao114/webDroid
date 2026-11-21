package org.wasmandroid.test;

interface ICalcService {
    int add(int lhs, int rhs);
    long scale(long value, long factor);
    String decorate(String value);
    byte[] reverseBytes(in byte[] value);
    IBinder echoBinder(IBinder value);
}
