package com.ms.silverking.util;

import java.util.Random;

import com.ms.silverking.collection.CollectionUtil;

/**
 */
public class ArrayUtil<T> {
	private final Random	random;
	
	public enum MismatchedLengthMode {Exception, Ignore};
	
	public static final byte[] emptyByteArray = new byte[0];
	
	public ArrayUtil() {
		random = new Random();
	}
	
	public void shuffle(T[] a) {
		for (int i = 0; i < a.length; i++) {
			int	newPosition;
			T	temp;
			
			newPosition = random.nextInt(a.length);
			temp = a[newPosition];
			a[newPosition] = a[i];
			a[i] = temp;
		}
	}
	
    public boolean equals(T[] a, int startA, T[] b) {
        return equals(a, startA, b, 0, b.length);
    }
    
	public boolean equals(T[] a, int startA, T[] b, int startB, int length) {
	    int    ai;
	    int    bi;
	    
	    ai = startA;
	    bi = startB;
	    for (int i = 0; i < length; i++) {
	        if (a[ai] != b[bi]) {
	            return false;
	        }
	        ai++;
	        bi++;
	    }
	    return true;
	}
	
    public static boolean equals(byte[] a, int startA, byte[] b) {
        return equals(a, startA, b, 0, b.length);
    }
    
    public static boolean equals(byte[] a, int startA, byte[] b, int startB, int length) {
        int    ai;
        int    bi;
        
        ai = startA;
        bi = startB;
        for (int i = 0; i < length; i++) {
            if (a[ai] != b[bi]) {
                return false;
            }
            ai++;
            bi++;
        }
        return true;
    }
    
    public static int compare(byte[] a, byte[] b) {
    	return compare(a, b, MismatchedLengthMode.Exception);
    }
    
    public static int compare(byte[] a, byte[] b, MismatchedLengthMode mismatchedLengthMode) {
        if (a.length != b.length) {
        	if (mismatchedLengthMode == MismatchedLengthMode.Exception) {
        		throw new RuntimeException("Mismatched lengths "+ a.length +" "+ b.length);
        	}
        }
        return compare(a, 0, b, 0, a.length);
    }
    
    /**
     * Compare two byte arrays. Handle degenerate cases so that we impose a total
     * ordering on all byte arrays.
     */
    public static int compareForOrdering(byte[] a, byte[] b) {
        if (a == null) {
            if (b == null) {
                return 0;
            } else {
                return -1;
            }
        } else {
            if (b == null) {
                return 1;
            } else {
                if (a.length != b.length) {
                    int c;
                    
                    c = compare(a, 0, b, 0, Math.min(a.length, b.length));
                    if (c != 0) {
                        return c;
                    } else {
                        if (a.length < b.length) {
                            return -1;
                        } else {
                            return 1;
                        }
                    }
                } else {
                    return compare(a, 0, b, 0, a.length);
                }
            }
        }
    }
    
    private static int compare(byte[] a, int startA, byte[] b) {
        return compare(a, startA, b, 0, b.length);
    }
    
    private static int compare(byte[] a, int startA, byte[] b, int startB, int length) {
        int    ai;
        int    bi;
        
        ai = startA;
        bi = startB;
        for (int i = 0; i < length; i++) {
            if (a[ai] < b[bi]) {
                return -1;
            } else if (a[ai] != b[bi]) {
                return 1;
            }
            ai++;
            bi++;
        }
        return 0;
    }
    
    public static void display(byte[] array) {
        for (int i = 0; i < array.length; i++) {
            System.out.println(i +":\t"+ array[i]);
        }
    }
    
    public static void xor(byte[] a1, byte[] a2) {
        if (a1.length != a2.length) {
            System.err.printf("%d != %d\n", a1.length, a2.length);
            throw new RuntimeException("a1.length != a2.length");
        }
        for (int i = 0; i < a1.length; i++) {
            a1[i] ^= a2[i];
        }
    }
    
    public static Double[] doubleToDouble(double[] a1) {
        Double[]    a2;
        
        a2 = new Double[a1.length];
        for (int i = 0; i < a1.length; i++) {
            a2[i] = a1[i];
        }
        return a2;
    }
    
    public static int hashCode(byte[] a) {
        return hashCode(a, 0, a.length);
    }
    
    public static int hashCode(byte[] a, int offset, int length) {
        if (length > 0) {
            int h;
            int _offset;

            _offset = offset;
            h = 0;
            for (int i = 0; i < length; i++) {
                h = 31 * h + a[_offset++];
            }
            return h;
        } else {
            return 0;
        }
    }
    
    public static <K> String toString(K[] c) {
        return toString(c, CollectionUtil.defaultSeparator);
    }
    
    public static <K> String toString(K[] c, char separator) {
        return toString(c, CollectionUtil.defaultStartBrace, CollectionUtil.defaultEndBrace, separator, CollectionUtil.defaultEmptyDef);
    }
    
    public static <K> String toString(K[] c, String startBrace, String endBrace, 
                                      char separator, String emptyDef) {
    	return CollectionUtil.toString(java.util.Arrays.asList(c), startBrace, endBrace, separator, emptyDef);
    }
    
    public static <K> boolean containsNull(K[] c) {
        for (K item : c) {
            if (item == null) {
                return true;
            }
        }
        return false;
    }
    
    /**
     * Compute xor a1 ^ a2, updating a1
     * @param a1
     * @param a2
     */
    public static void xorInPlace(byte[] a1, byte[] a2) {
    	if (a1.length != a2.length) {
    		throw new RuntimeException("a1.length != a2.length"); 
    	}
    	for (int i = 0; i < a1.length; i++) {
    		a1[i] ^= a2[i];
    	}
    }
}
