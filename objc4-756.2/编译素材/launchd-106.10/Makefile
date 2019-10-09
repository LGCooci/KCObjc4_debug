Project	= launchd
Extra_Configure_Flags = --sbindir=/sbin --bindir=/bin --sysconfdir=/private/etc
GnuAfterInstall = launchd_after_install
include $(MAKEFILEPATH)/CoreOS/ReleaseControl/GNUSource.make
Install_Flags = DESTDIR=$(DSTROOT)

launchd_after_install::
ifeq ($(RC_ProjectName),launchd_libs)
	-find -d $(DSTROOT) -type f | grep -v /usr/local/lib/system | xargs rm
	-find -d $(DSTROOT) -type l | grep -v /usr/local/lib/system | xargs rm
	-find -d $(DSTROOT) -type d | grep -v /usr/local/lib/system | xargs rmdir
else
	mkdir -p $(DSTROOT)/Library/StartupItems
	chmod 755 $(DSTROOT)/Library/StartupItems
	rm -rf $(DSTROOT)/usr/local/lib/system
endif

launchd_libs:: install
