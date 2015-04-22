#!/usr/bin/env bash
unset LANG
unset ${!LC_*}
set -x
set -e

modprobe --version
targetcli --version
udevadm --version
blockdev --version
parted --version
sfdisk --version
mkswap --version

configfs=/sys/kernel/config
target_path=$configfs/target

num_luns=4
num_hosts=4

get_wwn() {
	sed '
	s@-@@g
	s@^\(.\{16\}\)\(.*\)@\1@
	' /proc/sys/kernel/random/uuid
}

if test ! -d "${target_path}"
then
	modprobe -v configfs
	mount -vt configfs configfs $configfs
	modprobe -v target_core_mod
fi
modprobe -v xen-scsiback

host=0
while test $host -lt $num_hosts
do
	host=$(( $host + 1 ))
	lun=0
	loopback_wwn="naa.`get_wwn`"
	pvscsi_wwn="naa.`get_wwn`"
	targetcli /loopback create ${loopback_wwn}
	targetcli /xen-pvscsi create ${pvscsi_wwn}
	while test $lun -lt $num_luns
	do
		: h $host l $lun
		f_file=/dev/shm/Fileio.${host}.${lun}.file
		f_uuid=/dev/shm/Fileio.${host}.${lun}.uuid
		f_link=/dev/shm/Fileio.${host}.${lun}.link
		fileio_name="fio_${host}.${lun}"
		pscsi_name="ps_${host}.${lun}"

		targetcli /backstores/fileio create name=${fileio_name} "file_or_dev=${f_file}" size=$((1024*1024 * 8 )) sparse=true
		targetcli /loopback/${loopback_wwn}/luns create /backstores/fileio/${fileio_name} $lun

		udevadm settle --timeout=4

		vpd_uuid="`sed -n '/^T10 VPD Unit Serial Number:/s@^[^:]\+:[[:blank:]]\+@@p' /sys/kernel/config/target/core/fileio_*/${fileio_name}/wwn/vpd_unit_serial`"
		if test -z "${vpd_uuid}"
		then
			exit 1
		fi
		echo "${vpd_uuid}" > "${f_uuid}"
		by_id="`echo ${vpd_uuid} | sed 's@-@@g;s@^\(.\{25\}\)\(.*\)@scsi-36001405\1@'`"
		ln -sfvbn "/dev/disk/by-id/${by_id}" "${f_link}"

		f_major=$((`stat --dereference --format=0x%t "${f_link}"`))
		f_minor=$((`stat --dereference --format=0x%T "${f_link}"`))
		if test -z "${f_major}" || test -z "${f_minor}"
		then
			exit 1
		fi
		f_alias=`ls -d /sys/dev/block/${f_major}:${f_minor}/device/scsi_device/*:*:*:*`
		if test -z "${f_alias}"
		then
			exit 1
		fi
		f_alias=${f_alias##*/}

		blockdev --rereadpt "${f_link}"
		udevadm settle --timeout=4
		echo 1,12,S | sfdisk "${f_link}"
		udevadm settle --timeout=4
		blockdev --rereadpt "${f_link}"
		udevadm settle --timeout=4
		parted -s "${f_link}" unit s print

		d_link="`readlink \"${f_link}\"`"
		if test -n "${d_link}"
		then
			p_link="${d_link}-part1"
			ls -l "${p_link}"
			mkswap -L "swp_${fileio_name}" "${p_link}"
			udevadm settle --timeout=4
			blockdev --rereadpt "${f_link}"
			udevadm settle --timeout=4
			parted -s "${f_link}" unit s print
		fi

		targetcli /backstores/pscsi create "dev=${f_link}" "${pscsi_name}"
		targetcli /xen-pvscsi/${pvscsi_wwn}/tpg1/luns create "/backstores/pscsi/${pscsi_name}" $lun
		targetcli /xen-pvscsi/${pvscsi_wwn}/tpg1  set parameter alias=${f_alias%:*}

		lun=$(( $lun + 1 ))
	done
done

