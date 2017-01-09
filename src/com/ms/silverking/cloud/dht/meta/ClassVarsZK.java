package com.ms.silverking.cloud.dht.meta;
    public ClassVarsZK(MetaClient mc) throws KeeperException {
        super(mc, mc.getMetaPaths().getClassVarsBasePath());
    }
    
    @Override
    public ClassVars readFromFile(File file, long version) throws IOException {
        return ClassVars.parse(file, version);
    }

    @Override
    public ClassVars readFromZK(long version, MetaToolOptions options) throws KeeperException {
    	vBase = getVBase(options.name, version);
    }
    
    @Override
    public void writeToFile(File file, ClassVars instance) throws IOException {

    @Override
    public String writeToZK(ClassVars classVars, MetaToolOptions options) throws IOException, KeeperException {
    }
}