package com.ms.silverking.cloud.dht.daemon;

import org.kohsuke.args4j.Option;

public class DHTNodeOptions {
    public static final int	defaultInactiveNodeTimeoutSeconds = Integer.MAX_VALUE;
    
    @Option(name="-n", usage="dhtName", required=true)
    public String dhtName;
    @Option(name="-z", usage="zkConfig", required=true)
    public String zkConfig;
    @Option(name="-into", usage="inactiveNodeTimeoutSeconds", required=false)
    public int inactiveNodeTimeoutSeconds = defaultInactiveNodeTimeoutSeconds;
}