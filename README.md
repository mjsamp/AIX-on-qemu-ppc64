# AIX on qemu-system-ppc64

My experience running AIX and related software on Linux qemu-system-ppc64

## Environment

Lenovo ThinkPad E480 Intel® Core™ i3-8130U CPU @ 2.20GHz × 4 8G Mem\
XrayDisk 240GB SSD\
Ubuntu 20.04.6 LTS (Focal Fossa) 64-bit\
Kernel Linux 5.4.0-181-generic x86_64\
QEMU emulator version 4.2.1 (Debian 1:4.2-3ubuntu6.28)\
libvirt QEMU Driver 6.0.0-0ubuntu8.20\
AIX aix_7200-04-02-2027_1of2_072020.iso

## Installation

Create a qcow2 disk:
```bash
qemu-img create -f qcow2 hdisk0.qcow2 20G
```

Define the virtual machine using virsh (see libvirt folder for XML files and edit it to reflect your ISO and virtual hard disks files path):

```bash
virsh define AIX_qemu_install.xml
```
\
Then check the boot options and set CDROM as first boot:\
![IMAGE ALT TEXT HERE](./images/boot_options.png)\
\
Start the machine and wait for the initial install screen so press 1 and enter:\
![IMAGE ALT TEXT HERE](./images/tela_01.png)\
\
On the next screen choose option 2:\
![IMAGE ALT TEXT HERE](./images/tela_02.png)\
\
On the next screen choose option 4:\
![IMAGE ALT TEXT HERE](./images/tela_02_mais_opcoes.png)\
\
On the next screen define install options (disable Enable System Backups... so it finishes faster) then press 0 and Enter then confirm install:\
![IMAGE ALT TEXT HERE](./images/tela_02_opcoes_instalacao.png)\

Wait till install finishes then follow instructions. If everything goes ok it will reboot the machine at the end. Then wait for the install screen again and choose Maintenance Mode (option 3).\
![IMAGE ALT TEXT HERE](./images/tela_03_modo_manutencao.png)\
\
Access the rootvg and follow the steps to fix boot and inittab services.\
![IMAGE ALT TEXT HERE](./images/tela_03_modo_manutencao_acesso_rootvg.png)\
\
After fix boot and services halt the machine using command halt and wait it stop.\
If halt fails force stop then define the disk as the first boot option.\
Watch my video where I show you all these steps to install it using virsh and virt-manager for more details.

[![IMAGE ALT TEXT HERE](https://img.youtube.com/vi/EFftKpKCj_Y/0.jpg)](https://www.youtube.com/watch?v=EFftKpKCj_Y)\

## Fixing boot
The first boot hangs trying to run fsck64. Then you need to replace the 64 bits by the 32 bits.


Move or remove the old one
```bash
mv /sbin/helpers/jfs2/fsck64 sbin/helpers/jfs2/fsck64.old
```

then create a link to the 32 bits fsck
```bash
ln -s /sbin/helpers/jfs2/fsck /sbin/helpers/jfs2/fsck64
```
Do the same for logredo64 because it also hangs while moving a shared VG to another node (yes, PowerHA 6.1 worked on it after some issues fixed) so the varyon failed.

```bash
mv /sbin/helpers/jfs2/logredo64 sbin/helpers/jfs2/logredo64.old
ln -s /sbin/helpers/jfs2/logredo /sbin/helpers/jfs2/logredo64
```
## Fixing Segmentation Faults
AIX binary compatibility
Last Updated: 2024-03-06

AIX® binary compatibility allows applications that were created on earlier releases or technology levels of AIX to run unchanged and without recompilation on later releases or technology levels of AIX. For example, an application that is created on AIX Version 6.1 can be run on AIX Version 7.2, or later.

The best way to fix the segmentation faults is copy the binary from an old version of AIX.

First you need to find the fileset where the binary is provided.

```bash
root@node01:/>which crontab
/usr/bin/crontab
root@node01:/>lslpp -w /usr/bin/crontab
  File                                        Fileset               Type
  ----------------------------------------------------------------------------
  /usr/bin/crontab                            bos.rte.cron          File
```
Now copy the fileset from an old AIX install media to a temporary folder and list it then extract if it contains the binary:
```bash
#list the content
root@node01:/>restore -Tqvf bos.rte.cron

#extract all files
root@node01:/>restore -xqvf bos.rte.cron

#extract only the binary
root@node01:/>restore -xqvf bos.rte.cron ./usr/bin/crontab
```
In my tests I used AIX53_ML10_01_CD1.ISO and the following worked well.
```bash
at
cron
crontab
ps
qdaemon
tprof
vmstat --> replace with vmstat64
```

## The following are alternatives if you don't have an older AIX install media.
### Fixing ps command

Command ps is broken (Segmentation Fault). Fortunately there is another ps that works but isn't the same so you may need replace -o arguments like %c by comm for example in some scripts.
This is the alternative ps.

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
ln -s /usr/sysv/bin/ps /usr/bin/ps

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

### Fix cron and crontab

cron and crontab didn't work also and there is no alternative version.

The solution is to compile it yourself.

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
Then you can run it after finish the install and get login prompt. It works after fix ps.

```bash
to start it
root@node01:/>startsrc -g nfs
create an export
root@node01:/>mknfsexp -d /yourfsname
to list exports
root@node01:/>showmount -e localhost
export list for localhost:
/yourfsname (everyone)

to stop it
root@node01:/>startsrc -g nfs

```

Remove DSO
```bash
rmitab aso
```

You may have this RPM issue
```bash
error: rpmdb: Thread/process 5177770/1 failed: Thread died in Berkeley DB library
error: db4 error(-30974) from dbenv->failchk: DB_RUNRECOVERY: Fatal error, run database recovery
error: cannot open Packages index using db4 -  (-30974)
error: cannot open Packages database in /opt/freeware/packages
error: rpmdb: Thread/process 5177770/1 failed: Thread died in Berkeley DB library
error: db4 error(-30974) from dbenv->failchk: DB_RUNRECOVERY: Fatal error, run database recovery
error: cannot open Packages index using db4 -  (-30974)
error: cannot open Packages database in /opt/freeware/packages
```
To fix it add the following line to your /etc/initab after tcpip service (you find the script in the folder scripts) 

```bash
fixrpm:23456789:once:/fixrpm.sh > /dev/null 2>&1
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

See PowerHA compatibilty matrix for more details.
https://www.ibm.com/docs/en/powerha-aix/7.2?topic=reference-information

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

To share disks between two machines in order to create a shared Volume Group create raw disk images for each disk:\
The following command will create a sparse raw disk image
```bash
dd if=/dev/zero of=sparse.img bs=1k count=0 seek=10485760
```
See my video Updates on Install AIX and run PowerHA using virsh and virt-manager for more details.\
[![IMAGE ALT TEXT HERE](https://img.youtube.com/vi/_zZIwsy8JLs/0.jpg)](https://www.youtube.com/watch?v=_zZIwsy8JLs)\

## Observations
Take a look into the videos I've done before trying install AIX if you don't have experience doing it.\
https://www.youtube.com/playlist?list=PLWNnbCzUTMSY6c6rjKtGuSAzHCPONExv2

This environment isn't recommended for production but it worth a try if you want to learn AIX and PowerHA for example.

Testing PowerHA I was able to create a Resource Group with a service IP and a shared Volume Group successfully and move and failover worked very well in my tests.

You can create an install ISO after fix everything so you don't need to do these steps everytime you need to install it on a new machine.

First create a mksysb image:
```bash
mksysb /images/mksysb/aix72qemu_fixed.mksysb
```
Then create the iso image:
```bash
mkcd -L -S -I /images/iso/ -m /images/mksysb/aix72qemu_fixed.mksysb
```

[![IMAGE ALT TEXT HERE](https://img.youtube.com/vi/SNvnmmXTENk/0.jpg)](https://www.youtube.com/watch?v=SNvnmmXTENk)


## Contacts
Linkedin: https://www.linkedin.com/in/marcos-jean-sampaio-05bb621b/

## Buy me a coffee

BTC bc1qd7c9mcvs0dgpd2nv59jsmm2j3qzjhq4a7mramu

ETH 0x7cF556FFA4F3ffD57554D89A19DD92F546598544

LTC LTJwUWPLdDDgK8AYNb419C7UGZgpxuy9qK

DOGE DUBiQaGs4WKJFNc6Adq86QWU3aXgBJR3zW

