#
# Regular cron jobs for the wwvsim package.
#
0 4	* * *	root	[ -x /usr/bin/wwvsim_maintenance ] && /usr/bin/wwvsim_maintenance
