By default this will install the following:

# executable for essctrl
/usr/local/bin/essctrl

# executable for dserv
/usr/local/dserv/dserv

# additional files for dserv
/usr/local/dserv/dserv/*

After installation you can check what was installed:

pkgutil --pkgs | grep sheinberglab
pkgutil --files org.sheinberglab.dserv
pkgutil --files org.sheinberglab.essctrl

To uninstall:

sudo rm /usr/local/bin/essctrl
sudo rm -rf /usr/local/dserv
sudo pkgutil --forget org.sheinberglab.dserv
sudo pkgutil --forget org.sheinberglab.essctrl
