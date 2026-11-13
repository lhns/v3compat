// Bench: a JIT-heavy HotSpot workload. Each kernel runs ROUNDS rounds and prints each round's time, so the
// first rounds (translation, tiered compilation, code patching) separate from steady state; the checksum
// must match native.
import java.math.BigInteger;
import java.util.*;
import java.util.function.*;
import java.util.stream.*;

public class Bench {
    static int check;
    static void mix(long x) { check = (check ^ (int) x ^ (int) (x >>> 32)) * 0x9E3779B1 + 0x7F4A7C15; }
    static int scale = 1;

    interface Shape { int area(); }
    record Sq(int s) implements Shape { public int area() { return s * s; } }
    record Ci(int r) implements Shape { public int area() { return 3 * r * r; } }
    record Tri(int b, int h) implements Shape { public int area() { return (b * h) >> 1; } }

    static long objects() {
        int s = 0;
        for (int i = 0; i < 2_000_000 * scale; i++) {
            Shape o = i % 3 == 0 ? new Sq(i & 63) : i % 3 == 1 ? new Ci(i & 31) : new Tri(i & 127, i & 15);
            s += o.area();
        }
        return s;
    }

    // Loops the C2 superword pass vectorizes (AVX2 integer forms), and Math.fma (an FMA intrinsic).
    static final int[] ia = new int[4096], ib = new int[4096];
    static final double[] da = new double[4096], db = new double[4096];
    static long arrays() {
        long s = 0;
        for (int i = 0; i < ia.length; i++) { ia[i] = i * 7; ib[i] = i ^ 0x55; da[i] = i * 0.25; db[i] = 1.0 / (i + 1); }
        for (int r = 0; r < 2000 * scale; r++) {
            for (int i = 0; i < ia.length; i++) ia[i] = ia[i] * 3 + (ib[i] >> 2);
            double d = 0;
            for (int i = 0; i < da.length; i++) d = Math.fma(da[i], db[i], d);
            s += ia[r & 4095] + Double.doubleToLongBits(d);
        }
        return s;
    }

    static long bits() {
        long s = 0, x = 0x123456789abcdefL;
        for (int i = 0; i < 20_000_000 * scale; i++) {
            x = x * 6364136223846793005L + 1442695040888963407L;
            s += Long.numberOfLeadingZeros(x >>> (i & 31)) + Long.numberOfTrailingZeros(x | 1L << 40)
                 + Long.bitCount(x) + (x >>> (i & 63)) + Long.rotateLeft(x, i);
        }
        return s;
    }

    static long strings() {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 200_000 * scale; i++) sb.append("item-").append(Integer.toString(i, 36)).append(',');
        String str = sb.toString();
        String[] parts = str.split(",");
        long s = parts.length + str.indexOf("item-zz") + str.hashCode();
        for (String p : parts) if (p.endsWith("7")) s += p.length();
        return s;
    }

    static long collections() {
        Map<String, Integer> m = new HashMap<>();
        for (int i = 0; i < 400_000 * scale; i++) m.merge("k" + ((i * 2654435761L) >>> 52), 1, Integer::sum);
        List<Map.Entry<String, Integer>> l = new ArrayList<>(m.entrySet());
        l.sort(Map.Entry.<String, Integer>comparingByValue().reversed().thenComparing(Map.Entry.comparingByKey()));
        return l.size() ^ l.get(0).getValue()
               ^ l.stream().limit(50).collect(Collectors.summingInt(e -> e.getKey().length()));
    }

    static long bigint() {
        BigInteger a = BigInteger.ONE;
        for (int i = 1; i < 1500 * scale; i++) a = a.multiply(BigInteger.valueOf(i));
        return a.bitLength() + a.mod(BigInteger.valueOf(1_000_000_007L)).longValue();
    }

    // Many distinct lambdas and classes through streams.
    static long streams() {
        long s = 0;
        for (int k = 0; k < 40 * scale; k++) {
            final int kk = k;
            s += IntStream.range(0, 20000).filter(i -> (i ^ kk) % 3 != 0).mapToLong(i -> (long) i * kk)
                 .map(v -> v ^ (v >>> 7)).boxed().collect(Collectors.groupingBy(v -> v & 7, Collectors.counting()))
                 .values().stream().mapToLong(Long::longValue).sum();
        }
        return s;
    }

    public static void main(String[] args) {
        int rounds = args.length > 0 ? Integer.parseInt(args[0]) : 8;
        if (args.length > 1) scale = Integer.parseInt(args[1]);
        String[] names = {"objects", "arrays", "bits", "strings", "collections", "bigint", "streams"};
        List<LongSupplier> ks = List.of(Bench::objects, Bench::arrays, Bench::bits, Bench::strings,
                                        Bench::collections, Bench::bigint, Bench::streams);
        long t00 = System.nanoTime();
        for (int k = 0; k < ks.size(); k++) {
            StringBuilder out = new StringBuilder("round " + names[k]), vals = new StringBuilder("value " + names[k]);
            for (int r = 0; r < rounds; r++) {
                long t0 = System.nanoTime();
                long v = ks.get(k).getAsLong();
                mix(v);
                out.append(String.format(" %.1f", (System.nanoTime() - t0) / 1e6));
                vals.append(String.format(" %x", v));
            }
            System.out.println(out + " ms");
            System.out.println(vals);
        }
        System.out.printf("total %d ms%n", (System.nanoTime() - t00) / 1_000_000);
        System.out.printf("checksum %08x%n", check);
    }
}
