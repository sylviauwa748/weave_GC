public class Target {

    private static volatile byte[] sink;

    public static void main(String[] args) throws InterruptedException {

        System.out.println("Target JVM Started");

        Runtime.getRuntime().addShutdownHook(
                new Thread(() -> {
                    System.out.println("Shutdown hook activated, performing cleanup tasks");
                })
        );

        while (true) {

            System.out.println("PHASE: NORMAL");
            for (int i = 0; i < 100_000; i++) {
                sink = new byte[1024];
            }
            Thread.sleep(3000);

            System.out.println("PHASE: HIGH ALLOCATION");
            for (int i = 0; i < 5_000_000; i++) {
                sink = new byte[1024];
            }
            Thread.sleep(3000);

            System.out.println("PHASE: EXTREME ALLOCATION");
            for (int i = 0; i < 20_000_000; i++) {
                sink = new byte[1024];
            }
            Thread.sleep(3000);
        }
    }
}