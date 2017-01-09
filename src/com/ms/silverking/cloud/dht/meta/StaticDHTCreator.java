package com.ms.silverking.cloud.dht.meta;

import java.io.File;
import java.io.IOException;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;

import org.apache.zookeeper.KeeperException;
import org.kohsuke.args4j.CmdLineException;
import org.kohsuke.args4j.CmdLineParser;

import com.google.common.collect.ImmutableMap;
import com.google.common.collect.ImmutableSet;
import com.ms.silverking.cloud.dht.NamespaceCreationOptions;
import com.ms.silverking.cloud.dht.client.ClientDHTConfiguration;
import com.ms.silverking.cloud.dht.gridconfig.SKGridConfiguration;
import com.ms.silverking.cloud.gridconfig.GridConfiguration;
import com.ms.silverking.cloud.management.MetaToolOptions;
import com.ms.silverking.cloud.meta.VersionedDefinition;
import com.ms.silverking.cloud.toporing.StaticRingCreator;
import com.ms.silverking.cloud.toporing.StaticRingCreator.RingCreationResults;
import com.ms.silverking.cloud.zookeeper.ZooKeeperConfig;
import com.ms.silverking.collection.CollectionUtil;
import com.ms.silverking.id.UUIDBase;
import com.ms.silverking.io.StreamParser;
import com.ms.silverking.io.StreamUtil;

/**
 * Simplifies creation of a "static" DHT (a DHT which will not change in ring topology, 
 * DHT configuration, etc. during its existence.) DHTs which need to accommodate change 
 * should not use this method of creation.
 */
public class StaticDHTCreator {
	private final ZooKeeperConfig	zkConfig;
	private final Set<String>		servers;
	private final int				replication;
	private final String			dhtName;
	private final String			gcName;
	private final int				port;
	private final String			passiveNodes;
	private final NamespaceCreationOptions	nsCreationOptions;
	private final File				gridConfigDir;
	
	private static final String	dhtNameBase = "SK.";
	private static final String	ringNameBase = "ring.";
	private static final String	gcNameBase = "GC_";
	private static final String	serverDelimiter = ",";
	
	StaticDHTCreator(ZooKeeperConfig zkConfig, Set<String> servers, int replication, String dhtName, String gcName, int port,
					NamespaceCreationOptions nsCreationOptions, String gridConfigDir) {
		this.zkConfig = zkConfig;
		this.servers = servers;
		this.replication = replication;
		this.dhtName = dhtName;
		this.gcName = gcName;
		passiveNodes = "";
		this.port = port;
		this.nsCreationOptions = nsCreationOptions;
		this.gridConfigDir = new File(gridConfigDir);
		if (!this.gridConfigDir.exists()) {
			throw new RuntimeException(gridConfigDir +" does not exist");
		}
	}
	
	public void createStaticDHT(UUIDBase uuid, int initialHeapSize, int maxHeapSize) throws IOException, KeeperException {
		String	ringName;
		String	classVarsName;
		RingCreationResults	rcResults;
		MetaClient			mc;
		
		mc = new MetaClient(dhtName, zkConfig);
		
		// Create Ring
		ringName = ringNameBase + uuid.toString();
		rcResults = StaticRingCreator.createStaticRing(ringName, zkConfig, servers, replication, uuid);		
		
		// Create class vars
		ClassVars	classVars;
		Map<String,String>	varsMap;
		MetaToolOptions	mto;
		
		classVarsName = "classVars." + uuid.toString();
		varsMap = new HashMap<>();
		varsMap.put("initialHeapSize", Integer.toString(initialHeapSize));
		varsMap.put("maxHeapSize", Integer.toString(maxHeapSize));
		classVars = new ClassVars(varsMap, VersionedDefinition.NO_VERSION);
		new ClassVarsZK(mc).writeToZK(classVars, classVarsName);		
		
		// Create DHTConfig
		DHTConfiguration	dhtConfig;
		
		dhtConfig = new DHTConfiguration(ringName, port, passiveNodes, nsCreationOptions, ImmutableMap.of(rcResults.hostGroupName, classVarsName), 0, Long.MIN_VALUE, null);
		new DHTConfigurationZK(mc).writeToZK(dhtConfig, null);
		
		// Write out curRingAndVersion
		new DHTRingCurTargetZK(mc, dhtConfig).setCurRingAndVersionPair(ringName, 0, 0);
		new DHTRingCurTargetZK(mc, dhtConfig).setTargetRingAndVersionPair(ringName, 0, 0);
		
		// Write GridConfig
		Map<String,String>	envMap;
		
		envMap = new HashMap<>();
		envMap.put(ClientDHTConfiguration.nameVar, dhtName);
		envMap.put(ClientDHTConfiguration.portVar, Integer.toString(port));
		envMap.put(ClientDHTConfiguration.zkLocVar, zkConfig.getEnsembleString());
		writeGridConfig(new SKGridConfiguration(gcName, envMap));
		System.out.println(gcName);
	}
	
	private void writeGridConfig(SKGridConfiguration skGridConfig) throws IOException {
		File	outputFile;
		
		System.out.println(skGridConfig.toEnvString());
		outputFile = new File(gridConfigDir, gcName + GridConfiguration.envSuffix);
		StreamUtil.streamToFile(skGridConfig.toEnvString(), outputFile);
	}
	
	public static void main(String[] args) {
		try {
			StaticDHTCreator		sdc;
			StaticDHTCreatorOptions	options;
			CmdLineParser	        parser;
			Set<String>				servers;
			String					dhtName;
			String					gcName;
			int						port;
			NamespaceCreationOptions	nsCreationOptions;
			UUIDBase				uuid;
			
			options = new StaticDHTCreatorOptions();
			parser = new CmdLineParser(options);
			try {
				parser.parseArgument(args);
			} catch (CmdLineException cle) {
				System.err.println(cle.getMessage());
				parser.printUsage(System.err);
				return;
			}
			if ((options.serverFile == null && options.servers == null) 
					|| (options.serverFile != null && options.servers != null)) {
				System.err.println("Exactly one of serverFile and servers should be provided");
				parser.printUsage(System.err);
				return;
			}
			if (options.serverFile != null) {
				servers = ImmutableSet.copyOf(StreamParser.parseFileLines(options.serverFile));
			} else {
				servers = CollectionUtil.parseSet(options.servers, serverDelimiter);
			}
			uuid = new UUIDBase(true);
			if (options.gridConfig != null) {
				gcName = options.gridConfig;
			} else {
				gcName = gcNameBase + uuid.toString();
			}
			if (options.dhtName != null) {
				dhtName = options.dhtName;
			} else {
				dhtName = dhtNameBase + uuid.toString();
			}
			port = options.port;
			if (options.nsCreationOptions != null) {
				nsCreationOptions = NamespaceCreationOptions.parse(options.nsCreationOptions);
			} else {
				nsCreationOptions = NamespaceCreationOptions.defaultOptions;
			}
			
			sdc = new StaticDHTCreator(new ZooKeeperConfig(options.zkEnsemble), servers, options.replication, dhtName, gcName, port, nsCreationOptions,
					options.gridConfigDir);
			sdc.createStaticDHT(uuid, options.initialHeapSize, options.maxHeapSize);
		} catch (Exception e) {
			e.printStackTrace();
		}
	}
}