# ttyPos driver for PAX bank pinpads

Fork of alex-eri/ttypos to patch if for Linux kernel >6.1

To build localy

```
cd src
make all
sudo make install
```

To make package for Debian or Ubuntu

```
apt install build-essential dkms debhelper
dpkg-buildpackage
```
