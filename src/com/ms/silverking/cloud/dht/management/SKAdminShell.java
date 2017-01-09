package com.ms.silverking.cloud.dht.management;

import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.PrintStream;
import java.rmi.Naming;
import java.rmi.NotBoundException;
import java.rmi.RemoteException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.Date;
import java.util.List;

import jline.console.ConsoleReader;

import org.apache.zookeeper.KeeperException;
import org.apache.zookeeper.WatchedEvent;
import org.apache.zookeeper.Watcher;
import org.kohsuke.args4j.CmdLineException;
import org.kohsuke.args4j.CmdLineParser;

import com.google.common.collect.ImmutableList;
import com.ms.silverking.cloud.dht.client.OperationException;
import com.ms.silverking.cloud.dht.daemon.storage.convergence.management.Mode;
import com.ms.silverking.cloud.dht.daemon.storage.convergence.management.RequestStatus;
import com.ms.silverking.cloud.dht.daemon.storage.convergence.management.RingMasterControl;
import com.ms.silverking.cloud.dht.daemon.storage.convergence.management.RingMasterControlImpl;
import com.ms.silverking.cloud.dht.gridconfig.SKGridConfiguration;
import com.ms.silverking.cloud.dht.meta.DHTConfiguration;
import com.ms.silverking.cloud.dht.meta.DHTConfigurationZK;
import com.ms.silverking.cloud.dht.meta.DHTRingCurTargetZK;
import com.ms.silverking.cloud.toporing.meta.MetaClient;
import com.ms.silverking.cloud.toporing.meta.MetaPaths;
import com.ms.silverking.cloud.zookeeper.ZooKeeperConfig;
import com.ms.silverking.cloud.zookeeper.ZooKeeperExtended;
import com.ms.silverking.collection.Triple;
import com.ms.silverking.id.UUIDBase;
import com.ms.silverking.io.FileUtil;
import com.ms.silverking.log.Log;
import com.ms.silverking.os.OSUtil;
import com.ms.silverking.text.StringUtil;
import com.ms.silverking.thread.ThreadUtil;
import com.ms.silverking.time.SimpleStopwatch;
import com.ms.silverking.time.Stopwatch;
import com.ms.silverking.time.Stopwatch.State;

/**
 * SilverKing administrative shell. This implementation is a sketch for a future, more powerful shell.
 */
public class SKAdminShell implements Watcher {
	private final SKGridConfiguration	gc;
	private final ZooKeeperExtended	zk;
	private final ZooKeeperConfig	zkConfig;
    private final BufferedReader    in;
    private final PrintStream       out;
    private final PrintStream       err;
    private final RingMasterControl	rmc;
    private final DHTRingCurTargetZK	curTargetZK;
    private List<Triple<Long,String,String>>	rings;
    private Stopwatch   sw;
    private boolean verbose;
    private int	defaultDisplayLimit = defaultDefaultDisplayLimit;
    private UUIDBase	mostRecentRequest;
    
    private static final int	zkTimeout = 2 * 60 * 1000;
    private static final int	defaultDefaultDisplayLimit = 10;
    private static final int	statusPollIntervalSeconds = 1;
	
    private static final String    multiLinePrompt = "> ";
    private static final String    prompt = "ska> ";
    private static final String    terminator = ";";
    
	public SKAdminShell(SKGridConfiguration gc, InputStream in, PrintStream out, PrintStream err, String server, int port) throws NotBoundException, KeeperException, IOException {
		this.gc = gc;
		zkConfig = new ZooKeeperConfig(gc.getClientDHTConfiguration().getZkLocs());
		zk = new ZooKeeperExtended(zkConfig, zkTimeout, this);
        this.in = new BufferedReader(new InputStreamReader(in));
        this.out = out;
        this.err = err;
        rmc = (RingMasterControl)Naming.lookup("rmi://"+ server +":"+ port +"/"+ RingMasterControl.registryName);
        sw = new SimpleStopwatch();
        rings = ImmutableList.of();
        
		com.ms.silverking.cloud.dht.meta.MetaClient	dhtMC;
		com.ms.silverking.cloud.dht.meta.MetaPaths  dhtMP;
	    long            	latestConfigVersion;
	    DHTConfiguration	dhtConfig;

	    dhtMC = new com.ms.silverking.cloud.dht.meta.MetaClient(gc);
	    dhtMP = dhtMC.getMetaPaths();
	    latestConfigVersion = dhtMC.getZooKeeper().getLatestVersion(dhtMP.getInstanceConfigPath()); 
	    dhtConfig = new DHTConfigurationZK(dhtMC).readFromZK(latestConfigVersion, null);
	    curTargetZK = new DHTRingCurTargetZK(dhtMC, dhtConfig);
	}
	
	@Override
	public void process(WatchedEvent event) {
	}
	
	/////////////////////////////////////////////////////////////////////////////
    
    private void shellLoop() throws Exception {
        ConsoleReader   reader;
        boolean running;
        String  cmd;
     
    	reader = new ConsoleReader();
        running = true;
        cmd = "";
        while (running) {
            String  s;
            
            if (OSUtil.isWindows()) {
                out.print(prompt);
                s = in.readLine();
            } else {
                s = reader.readLine(cmd.length() == 0 ? prompt : multiLinePrompt);
            }
            cmd += " "+ s;
            if (s.endsWith(terminator)) {
                cmd = cmd.trim();
                cmd = cmd.substring(0, cmd.length() - 1);
                //System.out.println("command: "+ cmd);
                try {
                    doCommand(cmd);
                } catch (Exception e) {
                    e.printStackTrace();
                }
                cmd = "";
            }
        }
    }
    
    private void runCommands(String commandsDef) throws Exception {
        List<String>    commands;
        
        commands = ImmutableList.copyOf(StringUtil.stripQuotes(commandsDef).split(terminator));
        runCommands(commands);
    }
    
    private void runCommands(File commandFile) throws Exception {
        runCommands(FileUtil.readFileAsString(commandFile));
    }
    
    private void runCommands(Iterable<String> commands) throws Exception {
        for (String command : commands) {
            command = command.trim();
            if (command.length() > 0) {
                out.println(prompt+ command);
                doCommand(command);
            }
        }
    }
    
	private void doCommand(String cmd) throws OperationException, IOException, KeeperException {
	    String[]   tokens;
	    
	    tokens = cmd.split("\\s+");
	    doCommand(tokens);
    }

    private void doCommand(String[] tokens) throws OperationException, IOException, KeeperException {
    	SKAdminShellAction      action;
        String[]    args;
        
        try {
            action = SKAdminShellAction.parse(tokens[0]);
        } catch (IllegalArgumentException iae) {
            out.println("Unknown action");
            return;
        }
        args = cdr(tokens);
        doCommand(action, args);
    }
    
    private String[] cdr(String[] a) {
        String[]    b;
        
        b = new String[a.length - 1];
        for (int i = 1; i < a.length; i++) {
            b[i - 1] = a[i];
        }
        return b;
    }
    
	/////////////////////////////////////////////////////////////////////////////
	
    private enum DisplayTarget	{DHTConfig,Rings,Mode,CurrentRing,TargetRing};
    
    private static class RingComparator implements Comparator<Triple<Long,String,String>> {
		@Override
		public int compare(Triple<Long, String, String> o1, Triple<Long, String, String> o2) {
			if (o1.getHead() < o2.getHead()) {
				return -1;
			} else if (o1.getHead() > o2.getHead()) {
				return 1;
			} else {
				return 0;
			}
		}
    }
    
    private void getRings(String ringName) throws IOException, KeeperException {
    	MetaClient	ringMC;
    	MetaPaths	ringMP;
    	String		ringConfigPath;
    	List<String>	configs;
    	Triple<String,Long,Long>	targetRing;
    	Triple<String,Long,Long>	currentRing;
        List<Triple<Long,String,String>>	_rings;
    	
		currentRing = curTargetZK.getCurRingAndVersionPair();
		targetRing = curTargetZK.getTargetRingAndVersionPair();
    	
		_rings = new ArrayList<>();
    	//ringMC = new MetaClient(new NamedRingConfiguration(ringName, null), zkConfig);
    	//ringMP = new MetaPaths(new NamedRingConfiguration(ringName, null));
    	ringConfigPath = MetaPaths.getRingConfigPath(ringName);
    	configs = zk.getChildren(ringConfigPath);
    	for (String config : configs) {
    		List<String>	versions;
    		
    		versions = zk.getChildren(ringConfigPath +"/"+ config +"/instance");
        	for (String version : versions) {
        		String	label;
        		Triple<String,Long,Long>	ring;
        		
        		ring = new Triple<>(ringName, Long.parseLong(config), Long.parseLong(version));
        		label = "";
        		if (currentRing.equals(ring)) {
        			label += "Current ";
        		}
        		if (targetRing.equals(ring)) {
        			label += "Target";
        		}
        		
        		long	creationTime;
        		
        		creationTime = zk.getCreationTime(ringConfigPath +"/"+ config +"/instance/"+ ZooKeeperExtended.padVersion(Long.parseLong(version)));
        		_rings.add(new Triple<>(creationTime, String.format("%s,%d,%d", ringName, Long.parseLong(config), Long.parseLong(version)), label));
        	}
    	}
    	_rings.sort(new RingComparator());
    	rings = _rings;
    }
    
    private void displayRings(String ringName, int displayLimit) throws IOException, KeeperException {
    	getRings(ringName);
    	for (int i = 0; i < rings.size(); i++) {
    		Triple<Long,String,String>	ring;
    		
    		ring = rings.get(i);
    		if ((rings.size() - i <= displayLimit) || (ring.getV3() != null && ring.getV3().length() > 0)) {
        		out.printf("%2d)\t%s\t%s\t%s\n", i, ring.getV2(), new Date(ring.getV1()), ring.getV3()); 
    		} 
    	}
    }
    
    private void displayRings(int displayLimit) throws IOException, KeeperException {
    	DHTConfiguration	dhtConfig;
    	
    	dhtConfig = DHTConfiguration.parse(rmc.getDHTConfiguration(), 0);
    	displayRings(dhtConfig.getRingName(), displayLimit);
    }
    
    private void displayDHTConfig() throws RemoteException {
    	DHTConfiguration	dhtConfig;
    	
    	dhtConfig = DHTConfiguration.parse(rmc.getDHTConfiguration(), 0);
    	out.printf("%s\n", dhtConfig);
    }
    
    private void displayRing(DisplayTarget target) throws KeeperException {
    	Triple<String,Long,Long>	ring;
    	
		switch (target) {
		case CurrentRing:
			ring = curTargetZK.getCurRingAndVersionPair();
			break;
		case TargetRing:
			ring = curTargetZK.getTargetRingAndVersionPair();
			break;
		default: throw new RuntimeException("Panic");
		}
		out.printf("%s: %s\n", target, ring);
	}
    
	private void doDisplay(DisplayTarget target, String[] args) throws IOException, KeeperException {
		switch (target) {
		case DHTConfig:
			displayDHTConfig();
			break;
		case Rings:
			displayRings(args.length > 0 ? Integer.parseInt(args[0]) : defaultDisplayLimit);
			break;
		case CurrentRing:
			displayRing(DisplayTarget.CurrentRing);
			break;
		case TargetRing:
			displayRing(DisplayTarget.TargetRing);
			break;
		case Mode:
			displayMode();
			break;
		default: throw new RuntimeException("Unsupported target");
		}
	}
	
	/////////////////////////////////////////////////////////////////////////////
    
	private void displayHelpMessage() {
        out.println(SKAdminShellAction.helpMessage());
    }
    
    private void displayMode() throws RemoteException {
    	out.printf("%s\n", rmc.getMode());
    }
    
	private void doMode(String[] args) throws RemoteException {
		if (args.length == 0) {
			displayMode();
		} else {
			rmc.setMode(Mode.valueOf(args[0]));
		}
	}

	private void doWaitForRequest(UUIDBase uuid) throws RemoteException {
		RequestStatus	prevStatus;
		RequestStatus	status;
		
		if (uuid == null) {
			uuid = rmc.getCurrentConvergenceID();
			if (uuid == null) {
				out.printf("No current convergence\n");
				return;
			} else {
				out.printf("Current convergence id: %s\n", uuid);
			}
		}
		
		out.printf("Waiting for %s\n", uuid);
		prevStatus = null;
		do {
			ThreadUtil.sleepSeconds(statusPollIntervalSeconds);
			status = rmc.getStatus(uuid);
			if (status != null) {
				if (prevStatus == null || !status.equals(prevStatus)) {
					out.printf("%s\n", status.getStatusString());
					prevStatus = status;
				}
			}
		} while (status != null && !status.requestComplete());
		out.printf("Complete\n");
	}
	
	private void doTarget(String[] args) throws RemoteException {
		if (args.length != 1) {
			out.println("Too many arguments to target. 1 expected.");
		} else {
			Triple<String,Long,Long>	target;
			String						ringDef;
			
			try {
				int	index;
				
				index = Integer.parseInt(args[0]);
				if (index < 0 || index >= rings.size()) {
					out.printf("Bad ring index\n");
					return;
				}
				ringDef = rings.get(index).getV2();
			} catch (NumberFormatException nfe) {
				ringDef = args[0];
			}
			target = Triple.parseDefault(ringDef, java.lang.String.class.getName(), java.lang.Long.class.getName(), java.lang.Long.class.getName());
			out.printf("Setting target: %s\n", target);
			mostRecentRequest = rmc.setTarget(target);
			out.printf("%s\n", mostRecentRequest);
			out.printf("Target set\n");
			doWaitForRequest(mostRecentRequest);
		}
	}
	
	private void doTestTarget(String[] args) throws RemoteException {
		for (String target : args) {
			String[]	_args;
			Stopwatch	sw;
			
			_args = new String[1];
			_args[0] = target;
			sw = new SimpleStopwatch();
			doTarget(_args);
			sw.stop();
			out.printf("%s\t%s\n", target, sw.toString());
		}
	}	
    
	private void doDisplay(String[] args) throws IOException, KeeperException {
		DisplayTarget	target;
		
		target = DisplayTarget.valueOf(args[0]);
		doDisplay(target, Arrays.copyOfRange(args, 1, args.length));
	}
	
    private void toggleVerbose() {
        verbose = !verbose;
        out.printf("Verbose is now %s\n", verbose);
    }	
	
	/////////////////////////////////////////////////////////////////////////////
    
    private UUIDBase requestUUID(String[] args) {
    	UUIDBase	uuid;
    	
    	if (args.length == 0) {
    		/*
    		 * this section deprecated for now in favor of retrieval from the rmc
    		if (mostRecentRequest == null) {
    			throw new RuntimeException("No previous request");
    		} else {
    			uuid = mostRecentRequest;
    		}
    		*/
    		uuid = null;
    	} else if (args.length == 1) {
    		uuid = UUIDBase.fromString(args[0]);
    	} else {
    		throw new RuntimeException("Too manu arguments");
    	}
    	return uuid;
    }
    
    private void doCommand(SKAdminShellAction action, String[] args) throws IOException, OperationException, KeeperException {
        boolean actionPerformed;
        int		reps = 1;
        
        sw.reset();
        actionPerformed = true;
        switch (action) {
        case Mode: doMode(args); break;
        case Target: doTarget(args); break;
        case TestTarget: doTestTarget(args); break;
        case WaitForConvergence: doWaitForRequest(requestUUID(args)); break;
        case Display: doDisplay(args); break;
        case Help: displayHelpMessage(); break;
        case ToggleVerbose: toggleVerbose(); break;
        case Quit: System.exit(0); break;
        default: 
            out.println("Action not supported: "+ action); 
            actionPerformed = false;
        }
        // Commands that care about precise timing will also reset the sw internally
        // We reset/stop it in this routine to handle commands for which it isn't critical 
        if (sw.getState() == State.running) {
            sw.stop();
        }
        if (verbose && actionPerformed) {
            out.printf("Elapsed:  %f\n", sw.getElapsedSeconds());
            if (action.usesReps()) {
                double  timePerRep;
                
                timePerRep = sw.getElapsedSeconds() / (double)reps;
                out.printf("Time/rep: %f\tReps: %d\n", timePerRep, reps);
            }
        }
    }	
	
	public static void main(String[] args) {
        try {
            CmdLineParser       	parser;
            SKAdminShellOptions	options;
            SKGridConfiguration		gc;
            SKAdminShell			shell;
            
            options = new SKAdminShellOptions();
            parser = new CmdLineParser(options);
            try {            	
                parser.parseArgument(args);
                
                gc = SKGridConfiguration.parseFile(options.gridConfig);
                
                shell = new SKAdminShell(gc, System.in, System.out, System.err, options.server, options.port);
                shell.shellLoop();
                
        		if (options.commands == null && options.commandFile == null) {
        		    shell.shellLoop();
        		} else {
                    if (options.commandFile != null) {
                        shell.runCommands(options.commandFile);
                    }
                    if (options.commands != null) {
                        shell.runCommands(options.commands);
                    }
        		}
            } catch (CmdLineException cle) {
                System.err.println(cle.getMessage());
                parser.printUsage(System.err);
                return;
            }        	
        } catch (Exception e) {
            e.printStackTrace();
            Log.logErrorSevere(e, SKAdminShell.class.getName(), "main");
        }
	}
}