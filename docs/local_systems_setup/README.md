# Local setup for working with systems.

This is a work in progress.
Soon this should be instructions for creating a local setup with dlsh, dserv, and stim2 on a local Linux or macOS machine.

For now, it's some notes on installing and testing packages created via CI.
I expect these to change as we make the CI better and find/fix issues!

# Clean

```
sudo systemctl stop dserv
systemctl disable dserv
sudo rm /etc/systemd/system/dserv.service
sudo systemctl daemon-reload
sudo systemctl reset-failed

sudo rm -rf /usr/local/dlsh
sudo rm -rf /usr/local/dserv
sudo rm -/usr/local/bin/essctrl
sudo rm -/usr/local/bin/essgui
sudo rm -/usr/local/bin/stim2
sudo rm -/usr/local/bin/install-dserv-service.sh
```

# Install packages from CI

## dlsh

```
# TODO: this was hand-made, we don't have the full dlsh.zip on CI yet.
wget https://github.com/benjamin-heasly/dlsh/releases/download/initial/dlsh.zip
sudo mkdir -p /usr/local/dlsh
sudo cp dlsh.zip /usr/local/dlsh/
```

## dserv

```
wget https://github.com/benjamin-heasly/dserv/releases/download/0.0.37/dserv_0.0.37_arm64.deb
sudo apt install -y ./dserv_0.0.37_arm64.deb

ls -alth /usr/local/bin
ls -alth /usr/local/lib
ls -alth /usr/local/dserv/db

essctrl

/usr/local/dserv/dserv --help
/usr/local/dserv/dserv -c /usr/local/dserv/config/dsconf.tcl -t /usr/local/dserv/config/triggers.tcl

sudo install-dserv-service.sh
sudo systemctl status dserv
```

## essgui

```
https://github.com/benjamin-heasly/dserv/releases/download/0.0.37/essgui_0.0.37_arm64.deb
sudo apt install -y ./essgui_0.0.37_arm64.deb

ls -alth /usr/local/bin
ls -alth /usr/local/lib

essgui
```

## stim2

```
wget https://github.com/benjamin-heasly/stim2/releases/download/0.0.10/stim2_0.0.10_amd64.deb
sudo apt install -y ./stim2_0.0.10_amd64.deb

ls -alth /usr/local/bin
ls -alth /usr/local/lib

stim2 --help
stim2
```

