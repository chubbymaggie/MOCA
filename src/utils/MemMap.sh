#!/bin/bash

usage()
{
    echo "Usage : $0 [options] -c \"command\""
    echo "Loads MemMap kernel module and configure it to monitor command, then
    execute the command given by the user"
    echo "-c    command     The command to be executed"
    echo "Options:"
    echo "-w    interval    Set the wakeup interval for MemMap to interval ms,
                        default: $interval"
    echo "-a    \"args\"    The arguments for command"
    echo "-h                Display this help and exit"
    echo "-p prio           Schedtool priority for the kernel module, the user
                        program priority will be prio-1, default: $prio"
    echo "-d dir       Path to the MemMap dir default $install_dir"
}

which schedtool > /dev/null
if [ $? -ne 0 ]
then
    echo "$0 requires the schedtool which is not find in your PATH"
    echo "Please install schedtool or update your PATH"
    exit 1
fi

if [ $(whoami) != "root"  ]
then
    echo "$0 must be run as root"
    exit 1
fi

interval=200
prio=$(schedtool -r | grep FIFO | sed -e 's/.*prio_max \([0-9]*\)/\1/')
args=""
cmd=""
install_dir="~/install/MemMap"

while getopts "w:c:a:hp:d:" opt
do
    case $opt in
        h)
            usage
            exit 0
            ;;
        w)
            interval=$OPTARG
            ;;
        a)
            args="$OPTARG"
            ;;
        c)
            cmd=$OPTARG
            ;;
        p)
            prio=$OPTARG
            ;;
        d)
            install_dir="$OPTARG"
            ;;
        *)
            usage
            exit 1
            ;;
    esac
done

if [ -z "$cmd" ]
then
    echo "A command is required"
    usage
    exit 1
fi

start()
{
    exec $cmd $args
}

# Wait for the kernel module to start
child()
{
    trap start SIGUSR1
    while true
    do
        echo "Waiting for a signal from my parent"
        sleep 3
    done
}

abort_on_error()
{
    if [ $1 -ne 0 ]
    then
        kill -9 $pid
        echo "Fatal error: $2"
        echo "aborting"
        exit 1
    fi
}

child &
let user_prio=$(( $prio - 1 ))
schedtool -F -p $user_prio $pid
pid=$!
cd $install_dir/src/module/
make clean && make
abort_on_error $? "make fail"
make install
abort_on_error $? "Install fail"
modprobe memmap MemMap_mainPid=$pid MemMap_wakeupInterval=$interval \
    MemMap_schedulerPriority=$prio
abort_on_error $? "unable to load module"
kill -s SIGUSR1 $pid
wait $pid
rmmod memmap
