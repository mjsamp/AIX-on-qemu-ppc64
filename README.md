# AIX-on-qemu-ppc64

My experience running AIX and relate software on Linux qemu-system-ppc64

## Environment

Lenovo ThinkPad E480 Intel® Core™ i3-8130U CPU @ 2.20GHz × 4 8G Mem\
XrayDisk 240GB SSD\
Ubuntu 20.04.6 LTS (Focal Fossa) 64-bit\
Kernel Linux 5.4.0-181-generic x86_64\
QEMU emulator version 4.2.1 (Debian 1:4.2-3ubuntu6.28)\
AIX aix_7200-04-02-2027_1of2_072020.iso

## Installation

Run the following command to install AIX from the iso:

```bash
qemu-system-ppc64 -cpu POWER8 -machine pseries -m 4096 -serial stdio -drive file=hdisk0.qcow2,if=none,id=drive-virtio-disk0 -device virtio-scsi-pci,id=scsi -device scsi-hd,drive=drive-virtio-disk0 -cdrom aix_7200-04-02-2027_1of2_072020.iso -prom-env "boot-command=boot cdrom:" -prom-env "input-device=/vdevice/vty@71000000" -prom-env "output-device=/vdevice/vty@71000000"
```
\
Then run the following command to boot AIX from disk:

```bash
qemu-system-ppc64 -cpu POWER8 -machine pseries -m 2048 -serial mon:stdio -drive file=hdisk0.qcow2,if=none,id=drive-virtio-disk0 -device virtio-scsi-pci,id=scsi -device scsi-hd,drive=drive-virtio-disk0 -cdrom aix_7200-04-02-2027_1of2_072020.iso -prom-env "boot-command=boot disk:" -net nic,macaddr=56:44:45:30:31:31 -net tap,script=no,ifname=tap0 -nographic
```

## Fixing issues
The first boot hangs trying to run fsck64. Then you need to replace the 64 bits by the 32 bits.


Move or remove the old one
```bash
mv /sbin/helpers/jfs2/fsck64 sbin/helpers/jfs2/fsck64.old
```

then create a link to the 32 bits fsck
```bash
ln -s /sbin/helpers/jfs2/fsck64 /sbin/helpers/jfs2/fsck
```
Do the same for logredo64 because it also hangs while moving a shared VG to another node (yes, PowerHA 6.1 worked on it after some issues fixed) so the varyon failed.

## Fixing ps command

ps is broken also (Segmentation Fault). Fortunately there is another ps that works but isn't the same so you may need replace -o flags like %c by comm in some scripts. This is the alternative ps.

```bash
root@node01:/>lslpp -w /usr/sysv/bin/ps
  File                                        Fileset               Type
  ----------------------------------------------------------------------------
  /usr/sysv/bin/ps                            bos.rte.control       File


root@node01:/>lslpp -l bos.rte.control
  Fileset                      Level  State      Description         
  ----------------------------------------------------------------------------
Path: /usr/lib/objrepos
  bos.rte.control            7.2.4.1  COMMITTED  System Control Commands
```  
move or remove the old one
```bash
 mv /usr/bin/ps /usr/bin/ps.old
```
then link it to the one that works

```bash
ln -s /usr/bin/ps /usr/sysv/bin/ps

# ps -eo %c
ps: Flag -o was used with invalid list
ps: Unknown output format %c
Usage: ps [ -acdefjlyALPX ] [ -g pgrplist ] [ -o format ]
        [ -p proclist ] [ -s sidlist ] [ -t termlist ]
        [ -u uidlist ] [ -G grplist ] [ -U uidlist ]
# ps -eo comm
COMMAND
init
syncd
shlap64
errdemon
srcmstr
ksh
```
## Fixing inittab services 
The '/usr/lpp/diagnostics/bin/diagd' command is not supported on this system.
Run rmitab to remove it from inittab
```bash
rmitab diagd
```

NFS daemon hangs during the boot then remove it
```bash
rmitab rcnfs
```
## Fix cron and crontab

cron and crontab didn't work also and there is no alternative version.

The only solution is to compile it your self.

If you don't want to download xlc and compile them you just need to copy the binaries (see binaries folder) to the server and replace the old ones with them.

This is the xlc version that worked in my configuration: 

xlcpp.12.1.0.0.aix.eval.tar.Z

vac.12.1.0.23.aix53TL7-72.may2021.ptf.tar.Z

Copy the crontab_src folder to the server then run:
```bash
/usr/vac/bin/cc cron.c -o cron

/usr/vac/bin/cc crontab.c -o crontab
```

Then you can replace the old ones with the ones generated by cc command.

```bash
mv /usr/sbin/cron /usr/sbin/cron.old
cp cron /usr/sbin/cron

mv /usr/bin/crontab /usr/bin/crontab.old
cp crontab /usr/bin/crontab
```
## Java

If you need java then uninstall the 64 bit version (it doesn't worked for me) and install 32 bit version


```bash
root@node01:/>java -version
java version "1.8.0_401"
Java(TM) SE Runtime Environment (build 8.0.8.21 - pap3280sr8fp21-20240221_01(SR8 FP21))
IBM J9 VM (build 2.9, JRE 1.8.0 AIX ppc-32-Bit 20240216_65882 (JIT enabled, AOT enabled)
OpenJ9   - 6a2a245
OMR      - 9440e34
IBM      - 7394519)
JCL - 20231221_01 based on Oracle jdk8u401-b10

```
## PowerHA

PowerHA SystemMirror 6.1 worked but you need to fix some scripts.
Look for the errors in hacmp.out then fix them yourself or search for the fixes on the internet.

Trying to run PowerHA 5.4 I had to downgrade perl to fix some issues so I keep it for 6.1 once it is working well.

```bash
root@node01:/>perl -version

This is perl, v5.8.8 built for ppc-thread-multi
(with 1 registered patch, see perl -V for more detail)

Copyright 1987-2007, Larry Wall

Perl may be copied only under the terms of either the Artistic License or the
GNU General Public License, which may be found in the Perl 5 source kit.

Complete documentation for Perl, including FAQ lists, should be found on
this system using "man perl" or "perldoc perl".  If you have access to the
Internet, point your browser at http://www.perl.org/, the Perl Home Page.

root@node01:/>ls -l /usr/bin/perl
lrwxrwxrwx    1 root     system           22 Apr 27 09:31 /usr/bin/perl -> /opt/freeware/bin/perl

root@node01:/>rpm -qa
gettext-0.10.40-8.ppc
libgcc-4.8.5-1.ppc
gdbm-1.18-1.ppc
AIX-rpm-7.2.4.1-2.ppc
perl-5.8.8-2.ppc
```
\
PowerHA Filesets

```bash
root@node01:/>lslpp -l cluster*
  Fileset                      Level  State      Description         
  ----------------------------------------------------------------------------
Path: /usr/lib/objrepos
  cluster.doc.en_US.es.html  6.1.0.0  COMMITTED  HAES Web-based HTML
                                                 Documentation - U.S. English
  cluster.doc.en_US.es.pdf   6.1.0.0  COMMITTED  HAES PDF Documentation - U.S.
                                                 English
  cluster.es.client.clcomd   6.1.0.0  COMMITTED  ES Cluster Communication
                                                 Infrastructure
  cluster.es.client.lib      6.1.0.0  COMMITTED  ES Client Libraries
  cluster.es.client.rte      6.1.0.0  COMMITTED  ES Client Runtime
  cluster.es.client.utils    6.1.0.0  COMMITTED  ES Client Utilities
  cluster.es.client.wsm      6.1.0.0  COMMITTED  Web based Smit
  cluster.es.cspoc.cmds      6.1.0.0  COMMITTED  ES CSPOC Commands
  cluster.es.cspoc.dsh       6.1.0.0  COMMITTED  ES CSPOC dsh
  cluster.es.cspoc.rte       6.1.0.0  COMMITTED  ES CSPOC Runtime Commands
  cluster.es.server.cfgast   6.1.0.0  COMMITTED  ES Two-Node Configuration
                                                 Assistant
  cluster.es.server.diag     6.1.0.0  COMMITTED  ES Server Diags
  cluster.es.server.events   6.1.0.0  COMMITTED  ES Server Events
  cluster.es.server.rte      6.1.0.0  COMMITTED  ES Base Server Runtime
  cluster.es.server.testtool
                             6.1.0.0  COMMITTED  ES Cluster Test Tool
  cluster.es.server.utils    6.1.0.0  COMMITTED  ES Server Utilities

Path: /etc/objrepos
  cluster.es.client.clcomd   6.1.0.0  COMMITTED  ES Cluster Communication
                                                 Infrastructure
  cluster.es.client.lib      6.1.0.0  COMMITTED  ES Client Libraries
  cluster.es.client.rte      6.1.0.0  COMMITTED  ES Client Runtime
  cluster.es.client.wsm      6.1.0.0  COMMITTED  Web based Smit
  cluster.es.cspoc.rte       6.1.0.0  COMMITTED  ES CSPOC Runtime Commands
  cluster.es.server.diag     6.1.0.0  COMMITTED  ES Server Diags
  cluster.es.server.events   6.1.0.0  COMMITTED  ES Server Events
  cluster.es.server.rte      6.1.0.0  COMMITTED  ES Base Server Runtime
  cluster.es.server.utils    6.1.0.0  COMMITTED  ES Server Utilities
```

To share a disk between two machines in order to create a shared Volume Group just add file.locking=off to the drive file options for each disk:

```bash
-drive file=01_shared_pv.qcow2,file.locking=off,if=none,id=drive-virtio-disk1 -device scsi-hd,drive=drive-virtio-disk1
```
## Observations
Take a look into the console output files before trying install AIX if you don't have experience doing it.

This environment isn't recommended for production but it worth a try if you want to learn AIX and PowerHA for example.

Testing PowerHA I successfully created a Resource Group with a service IP and a shared Volume Group. The move and failover worked very well in my tests.

## Buy me a coffee
BTC: 329nUAQRxJPEybo5K1dgJdFteCojHimTcA

ETH: 
0x4c69ac113D3a8d8ECF91ab023Bd675a56A5c6529

LTC:
LTJwUWPLdDDgK8AYNb419C7UGZgpxuy9qK

## Contacts
Linkedin: https://www.linkedin.com/in/marcos-jean-sampaio-05bb621b/


