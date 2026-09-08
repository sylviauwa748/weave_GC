import javax.management.MBeanServerConnection;
import javax.management.remote.JMXConnector;
import javax.management.remote.JMXConnectorFactory;
import javax.management.remote.JMXServiceURL;
import java.lang.management.GarbageCollectorMXBean;
import java.lang.management.ManagementFactory;
import java.util.List;

public class Observer {

    public static void main(String[] args) throws Exception {

        // 1. Address of the Target JVM's JMX endpoint
        JMXServiceURL url =
                new JMXServiceURL(
                        "service:jmx:rmi:///jndi/rmi://localhost:9999/jmxrmi"
                );

        // 2. Connect to the Target JVM
        JMXConnector connector = JMXConnectorFactory.connect(url);

        // 3. Get a connection to the Target's MBeanServer.
        MBeanServerConnection connection =
                connector.getMBeanServerConnection();

        // 4. Ask the Target JVM for its garbage collector MXBeans.
        List<GarbageCollectorMXBean> collectors =
                ManagementFactory.getPlatformMXBeans(
                        connection,
                        GarbageCollectorMXBean.class
                );

        // 5. Print what we discovered.
        System.out.println("Connected to Target JVM");

        // 5. Keep observing
        while (true) {
            System.out.println("----- Snapshot -----");
            for (GarbageCollectorMXBean collector : collectors) {
                System.out.println( "Collector: " + collector.getName()
                );
                System.out.println( "Collection count: " + collector.getCollectionCount()
                );
                System.out.println( "Collection time: " + collector.getCollectionTime() + " ms"
                );
            }
            System.out.println();
            // Wait one second before taking the next snapshot
            Thread.sleep(1000);
        }
    }
}
