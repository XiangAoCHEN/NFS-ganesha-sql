#!/bin/bash

if [ "$1" == "log" ]; then
		echo "" > log.txt
		sudo ./src/cmake-build-release/bin/ganesha.nfsd \
		-F -L /var/log/nfs-ganesha.log -f /home/ubuntu/ganesha.conf \
		-N NIV_EVENT > log.txt
else
		sudo ./src/cmake-build-release/bin/ganesha.nfsd \
		-F -L /dev/stdout -f /home/ubuntu/ganesha.conf \
		-N NIV_EVENT
fi
