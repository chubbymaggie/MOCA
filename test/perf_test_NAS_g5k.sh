#!/bin/bash
START_TIME=$(date +%y%m%d_%H%M%S)
CMDLINE="$0 $@"
EXP_NAME=$(basename $0 .sh)
OUTPUT="exp.log"
OWNER=dbeniamine
RUN=30
PREFIX="/home/dbeniamine"
WORKPATH="/tmp"
NAS="NPB3.3-OMP/"
MOCAPATH="Moca"
#MEMPROFPATH="MemProf"
TABARNACPATH="tabarnac"
export PATH=$PATH:/opt/pin
CONFIGS=('Moca' 'Base' 'Pin' ) #'Memprof')
declare -A TARGETS

#report error if needed
function testAndExitOnError
{
    err=$?
    if [ $err -ne 0 ]
    then
        echo "ERROR $err : $1"
        exit $err
    fi
}
function dumpInfos
{

    #Echo start time
    echo "Expe started at $START_TIME"
    #Echo args
    echo "#### Cmd line args : ###"
    echo "$CMDLINE"
    echo "EXP_NAME $EXP_NAME"
    echo "OUTPUT $OUTPUT"
    echo "RUN $RUN"
    echo "FIRST: $FIRSTRUN"
    echo "LAST: $LASTRUN"
    echo "########################"
    # DUMP environement important stuff
    echo "#### Hostname: #########"
    hostname
    echo "#### Path:     #########"
    echo "$PATH"
    echo "########################"
    echo "##### git log: #########"
    git log | head
    echo "########################"
    echo "#### git diff: #########"
    git diff
    echo "########################"
    lstopo --of txt
    cat /proc/cpuinfo
    echo "########################"


    #DUMPING scripts
    cp -v $0 $EXP_DIR/
    cp -v ./*.sh $EXP_DIR/
    cp -v *.pl $EXP_DIR/
    cp -v *.rmd  $EXP_DIR/
    cp -v Makefile  $EXP_DIR/
}
if [ $(whoami) != "root" ]
then
    echo "This script must be run as root"
    exit 1
fi
id=0
#parsing args
while getopts "ho:e:r:i:" opt
do
    case $opt in
        h)
            usage
            exit 0
            ;;
        e)
            EXP_NAME=$OPTARG
            ;;
        o)
            OUTPUT=$OPTARG
            ;;
        r)
            RUN=$OPTARG
            ;;
        i)
            id=$OPTARG
            ;;
        *)
            usage
            exit 1
            ;;
    esac
done
if [ $(whoami) != "root" ]
then
    echo "This script must be run as root"
    exit 1
fi

#post init
EXP_DIR="$WORKPATH/$EXP_NAME"_$(date +%y%m%d_%H%M)
mkdir $EXP_DIR
OUTPUT="$EXP_DIR/$OUTPUT"
FIRSTRUN=$(( $id * $RUN ))
LASTRUN=$(( $FIRSTRUN + $RUN ))
FIRSTRUN=$(( $FIRSTRUN + 1 ))

#Continue but change the OUTPUT
exec > >(tee $OUTPUT) 2>&1
dumpInfos

# Copy files to working dir
if [ $PREFIX != $WORKPATH ]
then
    cp -rv $PREFIX/$NAS $WORKPATH/
    cp -rv $PREFIX/$MOCAPATH $WORKPATH/
    #cp -rv $PREFIX/$MEMPROFPATH $WORKPATH/
    cp -rv $PREFIX/$TABARNACPATH $WORKPATH/
fi

#Do the first compilation
cd $WORKPATH/$NAS
make clean
make suite
#make dc CLASS=A
rm bin/*.x
cd -

cd $WORKPATH/$TABARNACPATH
make clean
make
cd -

for run in $(seq $FIRSTRUN $LASTRUN)
do
    echo "RUN : $run"
    #Actual exp
    for bench in $WORKPATH/$NAS/bin/*
    do

	benchname=$(basename $bench)
        echo "$benchname"
        LOGDIR="$EXP_DIR/$benchname/run-$run"
        mkdir -p $LOGDIR
	TARGETS=([Base]='' [MemProf]="$WORKPATH/$MEMPROFPATH/scripts/profile_app.sh" \
		[Moca]="$WORKPATH/$MOCAPATH/src/utils/moca -d $WORKPATH/$MOCAPATH -G -D $LOGDIR/Moca-$benchname -c" \
    		[Pin]="$WORKPATH/$TABARNACPATH/tabarnac -r --")
        echo $LOGDIR
        #Actual experiment
        for conf in ${CONFIGS[@]}
        do
            cmd="${TARGETS[$conf]} $bench"
            set -x
            $cmd > $LOGDIR/$conf.log 2> $LOGDIR/$conf.err
            #testAndExitOnError "Exec failed $conf $benchname run-$run"
            set +x
            rm $WORKPATH/$NAS/ADC.*
        done
        #echo "Compressing traces"
        mv $LOGDIR/Moca.log $LOGDIR/Moca-$benchname/
        mv $LOGDIR/Moca-$benchname/Moca-$benchname.log $LOGDIR/Moca.log
        #tar cvJf $LOGDIR/traces.tar.xz $LOGDIR/Moca-$benchname *.csv
        mv *.csv $LOGDIR/
        #echo "Done"
    done
    echo "Saving files"
	sudo chmod -R 777 $EXP_DIR
	chown -R $OWNER: $EXP_DIR
	su $OWNER -c "cp -ur $EXP_DIR /home/$OWNER/"
    echo "Done"
done

#cd $EXP_DIR/
#./parseAndPlot.sh
#cd -
#Echo thermal throttle info
echo "retrieving expe files"
sudo chmod -R 777 $EXP_DIR
chown -R $OWNER: $EXP_DIR
su $OWNER -c "cp -ur $EXP_DIR /home/$OWNER/"
echo "thermal_throttle infos :"
cat /sys/devices/system/cpu/cpu0/thermal_throttle/*
END_TIME=$(date +%y%m%d_%H%M%S)
echo "Expe ended at $END_TIME"