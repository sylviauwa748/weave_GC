public class Target {

    private static volatile byte[] sink;

    public static void main(String[] args) throws InterruptedException {

        System.out.println("Target JVM Started");

        while (true) {

            // Low allocation
            for (int i = 0; i < 100_000; i++) {
                sink = new byte[1024];
            }

            Thread.sleep(1000);

            // High allocation
            for (int i = 0; i < 1_000_000; i++) {
                sink = new byte[1024];
            }

            Thread.sleep(1000);
        }
    }
}