#!/bin/bash

if [ "$1" == "stdout" ]; then
		sudo ./src/cmake-build-release/bin/ganesha.nfsd \
		-F -L /dev/stdout -f /home/ubuntu/ganesha.conf \
		-N NIV_EVENT
else
		echo "" > log.txt
		sudo ./src/cmake-build-release/bin/ganesha.nfsd \
		-F -L /var/log/nfs-ganesha.log -f /home/ubuntu/ganesha.conf \
		-N NIV_EVENT > log.txt
fi
