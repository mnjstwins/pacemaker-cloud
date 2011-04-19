install
text
key --skip
keyboard us
lang en_US.UTF-8
skipx
network  @NETWORK@
rootpw --iscrypted $1$g7wJsgcU$gWTT/UE1AhXF86UB8GuuF/
firewall --disabled
authconfig --enableshadow --enablemd5
selinux --enforcing
timezone --utc America/New_York
bootloader --location=mbr --append="console=tty0 console=ttyS0,115200"
zerombr yes
clearpart --all

part /boot --fstype ext4 --size=200
part pv.2 --grow --size=1
volgroup VolGroup00 --pesize=32768 pv.2
logvol swap --fstype swap --name=LogVol01 --vgname=VolGroup00 --size=768 --grow --maxsize=1536
logvol / --fstype ext4 --name=LogVol00 --vgname=VolGroup00 --size=1024 --grow
reboot

%packages
@core
@base
matahari
httpd
%post
