#!/bin/ksh

# order is important, build_sk_client.vars actually calls a function in common.lib...
source lib/common.lib
source lib/build_sk_client.vars

f_clearOutEnvVariables
f_checkAndSetBuildTimestamp

function f_checkParams {
	f_printHeader "PARAM CHECK"
	  
	echo "       cc=$CC"
	echo " cc_flags=$CC_FLAGS"
	  
	if [[ -z $CC ]] ; then
		echo "Need to pass in a C compiler"
		exit 1
	fi
	  
	if [[ -z $CC_FLAGS ]] ; then
		# By default, debug, not optimized
		CC_FLAGS="-g"
		echo "Set CC_FLAGS=$CC_FLAGS"
	fi
}

function f_compileAndLinkProxiesIntoLib {
	# params
	typeset cc=$1
	typeset cc_opts=$2
	typeset inc_opts=$3
	typeset ld=$4
	typeset ld_opts=$5
	typeset lib_opts=$6
		
	f_printSection "Compiling and linking proxies into lib"

	typeset buildObjDir=$SILVERKING_BUILD_ARCH_DIR/jaceLib
	typeset  objDirStatic=$buildObjDir/static
	typeset objDirDynamic=$buildObjDir/dynamic
	f_cleanOrMakeDirectory "$buildObjDir"
	f_cleanOrMakeDirectory "$objDirStatic"
	f_cleanOrMakeDirectory "$objDirDynamic"
		
	typeset libDir=$INSTALL_ARCH_LIB_DIR/jace/
	typeset  libDirStaticLoad=$libDir/static
	typeset libDirDynamicLoad=$libDir/dynamic
	f_cleanOrMakeDirectory "$libDir"
	f_cleanOrMakeDirectory "$libDirStaticLoad"
	f_cleanOrMakeDirectory "$libDirDynamicLoad"
	
	PROXY_SRC="../src/jace/source"
	### STATIC LOAD
	f_compileAssembleDirectoryTree "$PROXY_SRC" "$objDirStatic" "$cc" "$cc_opts" "$inc_opts"
	# if [[ $CREATE_STATIC_LIBS == $TRUE ]]; then
		f_createStaticLibrary "$JACE_LIB_NAME" "$libDirStaticLoad" "$objDirStatic/$ALL_DOT_O_FILES" ""
	# fi
	f_createSharedLibrary "$JACE_LIB_NAME" "$libDirStaticLoad" "$objDirStatic/$ALL_DOT_O_FILES" "$ld" "$ld_opts" "$lib_opts"

	typeset expectedObjCount=36
	f_testEquals "$objDirStatic"  "$ALL_DOT_O_FILES" "$expectedObjCount"
	# if [[ $CREATE_STATIC_LIBS == $TRUE ]]; then
		f_testEquals "$libDirStaticLoad" "$JACE_LIB_STATIC_NAME" "1"
	# fi
	f_testEquals "$libDirStaticLoad" "$JACE_LIB_SHARED_NAME" "1"
	
	### DYNAMIC LOAD
	f_compileAssembleDirectoryTree "$PROXY_SRC" "$objDirDynamic" "$cc" "$cc_opts -DJACE_WANT_DYNAMIC_LOAD" "$inc_opts"
	# if [[ $CREATE_STATIC_LIBS == $TRUE ]]; then
		f_createStaticLibrary "$JACE_LIB_NAME" "$libDirDynamicLoad" "$objDirDynamic/$ALL_DOT_O_FILES" ""
	# fi
	f_createSharedLibrary "$JACE_LIB_NAME" "$libDirDynamicLoad" "$objDirDynamic/$ALL_DOT_O_FILES" "$ld" "$ld_opts" "$lib_opts"

	f_testEquals "$objDirDynamic" "$ALL_DOT_O_FILES" "$expectedObjCount"
	# if [[ $CREATE_STATIC_LIBS == $TRUE ]]; then
		f_testEquals "$libDirDynamicLoad" "$JACE_LIB_STATIC_NAME" "1"
	# fi
	f_testEquals "$libDirDynamicLoad" "$JACE_LIB_SHARED_NAME" "1"
}

## params
	     CC=$1
output_filename=$(f_getBuildJace_RunOutputFilename "$CC")
{
	CC_FLAGS=$2
	f_checkParams;

	LD=$CC
	
	CC_OPTS="$CC_FLAGS $LD_OPTS -enable-threads=posix -pipe -Wall ${BOOST_NS} -D_GNU_SOURCE -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64 -DBOOST_SPIRIT_THREADSAFE -D_REENTRANT -DJACE_EXPORTS -D__STDC_LIMIT_MACROS -D__STDC_FORMAT_MACROS"
	PROXY_INC="../src/jace/include"
	# LIB_OPTS=" -L${BOOST_LIB} -l${BOOST_LIB_THREAD} -l${BOOST_LIB_DT} -Wl,--rpath -Wl,${BOOST_LIB} -L${JAVA_LIB} -lrt -lpthread -ljvm -Wl,--rpath -Wl,${JAVA_LIB}"  ##works
	f_startLocalTimer;
	date;
	
	f_compileAndLinkProxiesIntoLib "$CC" "$CC_FLAGS $LD_OPTS" "$INC_OPTS_NO_JACE -I${PROXY_INC}" "$LD" "$LD_OPTS" "$LIB_OPTS_NO_JACE";
	f_printSummary "$output_filename"
	f_printLocalElapsed;
} 2>&1 | tee $output_filename