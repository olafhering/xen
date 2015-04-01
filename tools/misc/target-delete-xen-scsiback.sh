#!/usr/bin/env bash
unset LANG
unset ${!LC_*}
set -x
set -e

targetcli --version

configfs=/sys/kernel/config
target_path=$configfs/target

cd "${target_path}"
cd xen-pvscsi
for wwn in `ls -d naa.*`
do
	targetcli /xen-pvscsi delete $wwn
done
cd "${target_path}"
cd core
for name in `ls -d pscsi_*/*/wwn`
do
	name=${name%/wwn}
	name=${name##*/}
	targetcli /backstores/pscsi delete $name
done
cd "${target_path}"
cd loopback
for wwn in `ls -d naa.*`
do
	targetcli /loopback delete $wwn
done
cd "${target_path}"
cd core
for name in `ls -d fileio_*/*/wwn`
do
	name=${name%/wwn}
	name=${name##*/}
	targetcli /backstores/fileio delete $name
done
