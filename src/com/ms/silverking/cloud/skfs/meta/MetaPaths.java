package com.ms.silverking.cloud.skfs.meta;

import com.google.common.collect.ImmutableList;

public class MetaPaths extends MetaPathsBase {
    // configuration instance paths
    private final String  configPath;
    
    // dht global base directories
    public static final String  skfsGlobalBase = cloudGlobalBase +"/skfs";
    public static final String  configsBase = skfsGlobalBase +"/configs";
    
    public MetaPaths(String skfsConfigName) {
    
    public static String getConfigPath(String skfsConfigName) {
        return configsBase +"/"+ skfsConfigName;
    }
    
    public String getConfigPath() {
        return configPath;
    }
}