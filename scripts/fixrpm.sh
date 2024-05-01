#!/usr/bin/ksh

cd /opt/freeware
rm -f *.rpm.packages.tar
tar -chvf `date +"%d%m%Y"`.rpm.packages.tar packages
rm -f /opt/freeware/packages/__*
/usr/bin/rpm --rebuilddb

exit 0
