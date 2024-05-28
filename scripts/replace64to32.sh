#!/usr/bin/ksh

#this is script replaces Linux Toolbox 64 bits binaries for its 32 bits version to avoid segmentation fault error

curr_time=`date +%Y%m%d%H%M%S`
backup=/opt/freeware/bin/backup/$curr_time/
mkdir -p $backup

for i in `ls /opt/freeware/bin | grep "_64"`
do
app=`echo $i | awk -F_ '{print $1}'`
if [ -e "/opt/freeware/bin/$app"_32 ];then
echo "Moving $i to backup..."
mv "/opt/freeware/bin/$app"_64 $backup
echo "Copying /opt/freeware/bin/$app"_32 to _64
cp "/opt/freeware/bin/$app"_32 "/opt/freeware/bin/$app"_64
fi
done
