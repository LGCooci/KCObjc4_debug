AM_CFLAGS = -no-cpp-precomp -F/System/Library/PrivateFrameworks -Wall -W -Wshadow -Wpadded -Werror -fconstant-cfstrings

sbin_SCRIPTS = service

noinst_LIBRARIES = liblaunch.a liblaunch_profile.a

bin_PROGRAMS = launchctl wait4path
sbin_PROGRAMS = launchd SystemStarter launchd_debugd
libexec_PROGRAMS = ConsoleMessage register_mach_bootstrap_servers StartupItemContext launchproxy

sysconf_DATA = hostconfig rc rc.common rc.netboot rc.shutdown

ConsoleMessage_LDFLAGS = -framework CoreFoundation

launchctl_LDFLAGS = -framework CoreFoundation -weak_library /usr/lib/libedit.dylib

register_mach_bootstrap_servers_LDFLAGS = -framework CoreFoundation

launchd_debugd_CFLAGS = -mdynamic-no-pic $(AM_CFLAGS)
launchd_debugd_SOURCES = launchdebugd.c

liblaunch_a_SOURCES = liblaunch.c

liblaunch_profile_a_CFLAGS = -pg $(AM_CFLAGS)
liblaunch_profile_a_SOURCES = liblaunch.c

SystemStarter_CFLAGS = -mdynamic-no-pic $(AM_CFLAGS)
SystemStarter_LDFLAGS = -framework CoreFoundation
SystemStarter_SOURCES = StartupItems.c IPC.c SystemStarter.c

launchd_CFLAGS = -DPID1_REAP_ADOPTED_CHILDREN -mdynamic-no-pic $(AM_CFLAGS) -Wno-unused-parameter
launchd_LDFLAGS = -lbsm
launchd_SOURCES = launchd.c init.c bootstrap.c lists.c rpc_services.c bootstrapServer.c

launchproxy_LDFLAGS = -weak_framework Security

CLEANFILES = bootstrap.h bootstrapServer.c bootstrapUser.c

bootstrap.c:: bootstrap.h

bootstrap.h bootstrapServer.c bootstrapUser.c: $(srcdir)/bootstrap.defs
	mig $(MIGFLAGS) $(srcdir)/bootstrap.defs

man1_MANS = wait4path.1 launchctl.1

man5_MANS = launchd.plist.5 launchd.conf.5

man8_MANS = ConsoleMessage.8 StartupItemContext.8 SystemStarter.8 init.8 rc.8 launchd.8 service.8 launchproxy.8 launchd_debugd.8

STARTUPITEMS = $(basename $(notdir $(wildcard $(srcdir)/StartupItems/*.plist)))

$(addprefix $(DESTDIR)/System/Library/StartupItems/, $(STARTUPITEMS)):
	mkdir -p $@
	cp $(srcdir)/StartupItems/$(notdir $@) $@
	chmod 755 $@/$(notdir $@)
	cp $(srcdir)/StartupItems/$(notdir $@).plist $@/StartupParameters.plist

install-startupitems: $(addprefix $(DESTDIR)/System/Library/StartupItems/, $(STARTUPITEMS))

install-data-hook: install-startupitems
	mkdir -p $(DESTDIR)/usr/local/lib/system
	cp liblaunch.a $(DESTDIR)/usr/local/lib/system
	cp liblaunch.a $(DESTDIR)/usr/local/lib/system/liblaunch_debug.a
	cp liblaunch_profile.a $(DESTDIR)/usr/local/lib/system/liblaunch_profile.a
	mkdir -p $(DESTDIR)/usr/include
	cp $(srcdir)/launch.h $(DESTDIR)/usr/include
	mkdir -p $(DESTDIR)/usr/local/include
	cp $(srcdir)/launch_priv.h $(DESTDIR)/usr/local/include
	mkdir -p $(DESTDIR)/$(sysconfdir)/mach_init.d
	mkdir -p $(DESTDIR)/$(sysconfdir)/mach_init_per_user.d
	mkdir -p $(DESTDIR)/Library/LaunchDaemons
	mkdir -p $(DESTDIR)/Library/LaunchAgents
	mkdir -p $(DESTDIR)/System/Library/LaunchDaemons
	mkdir -p $(DESTDIR)/System/Library/LaunchAgents

install-exec-hook:
	chmod u+s $(DESTDIR)/$(sbindir)/launchd
