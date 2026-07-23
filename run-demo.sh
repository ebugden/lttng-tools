#!/bin/sh

sudo killall -w lttng-sessiond
#sudo killall lttng-sessiond # Broken

sudo lttng-sessiond --daemonize
#sudo lttng-sessiond -vvv --verbose-consumer --background > ./logs/sessiond-$(date --iso-8601=ns).log 2>&1
#sudo gdb lttng-sessiond -quiet -ex 'br health-consumerd.cpp:160' -ex 'run'

# Snapshot
#lttng create my_session --snapshot --output /tmp/demo-output
#lttng enable-channel --kernel my_channel
#lttng add-context --kernel --type procname
#lttng enable-event --channel my_channel --kernel --syscall --all
#lttng start
#lttng add-trigger --name openat_EACCES_exit --condition event-rule-matches --type kernel:syscall:exit --name "openat" --filter "ret == -13" --capture '$ctx.procname' --action notify --action snapshot-session my_session
#lttng add-trigger --name openat --condition event-rule-matches --type kernel:syscall:entry --name "openat" --filter '$ctx.procname == "touch"' --capture '$ctx.procname' --action notify
#lttng add-trigger --name openat_EACCES_entry --condition event-rule-matches --type kernel:syscall:entry --name "openat" --capture '$ctx.procname' --action notify 
#lttng list-triggers

# Minimal, no snapshot
lttng create my_session --snapshot --output /tmp/demo-output
lttng list
lttng enable-channel --kernel my_channel
lttng add-context --kernel --type procname
lttng enable-event --channel my_channel --kernel --syscall --all
lttng start
#lttng add-trigger --name openat_exit --condition event-rule-matches --type kernel:syscall:exit --name "openat" --filter "ret == -13" --capture '$ctx.procname' --action stop-session
#lttng add-trigger --name openat_exit --condition event-rule-matches --type kernel:syscall:exit --name "openat" --filter "ret == -13" --capture '$ctx.procname' --action notify 
#lttng add-trigger --name openat_exit --condition event-rule-matches --type kernel:syscall:exit --name "openat" --capture '$ctx.procname' --action notify 
#lttng add-trigger --name openat_entry --condition event-rule-matches --type kernel:syscall:entry --name "openat" --capture '$ctx.procname' --action notify 

#echo '\nStart of lttng listen output'
#lttng listen
#lttng listen openat openat_EACCES_exit

# In another terminal, run `$ touch /etc/passwd` to see the trigger