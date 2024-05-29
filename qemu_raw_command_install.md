## Installation

Create a qcow2 disk:
```bash
qemu-img create -f qcow2 hdisk0.qcow2 20G
```

Run the following command to install AIX from the iso:

```bash
qemu-system-ppc64 -cpu POWER8 -machine pseries -m 4096 -serial stdio -drive file=hdisk0.qcow2,if=none,id=drive-virtio-disk0 -device virtio-scsi-pci,id=scsi -device scsi-hd,drive=drive-virtio-disk0 -cdrom aix_7200-04-02-2027_1of2_072020.iso -prom-env "boot-command=boot cdrom:" -prom-env "input-device=/vdevice/vty@71000000" -prom-env "output-device=/vdevice/vty@71000000"
```
\
Then run the following command to boot AIX from disk:

```bash
qemu-system-ppc64 -cpu POWER8 -machine pseries -m 2048 -serial mon:stdio -drive file=hdisk0.qcow2,if=none,id=drive-virtio-disk0 -device virtio-scsi-pci,id=scsi -device scsi-hd,drive=drive-virtio-disk0 -cdrom aix_7200-04-02-2027_1of2_072020.iso -prom-env "boot-command=boot disk:" -net nic,macaddr=56:44:45:30:31:31 -net tap,script=no,ifname=tap0 -nographic
```

To share disks between two machines in order to create a shared Volume Group just add file.locking=off to the drive file options for each disk:

```bash
-drive file=01_shared_pv.qcow2,file.locking=off,if=none,id=drive-virtio-disk1 -device scsi-hd,drive=drive-virtio-disk1
```
