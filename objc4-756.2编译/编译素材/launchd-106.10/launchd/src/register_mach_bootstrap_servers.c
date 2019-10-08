#include <CoreFoundation/CoreFoundation.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <servers/bootstrap.h>
#include <unistd.h>
#include <dirent.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>

static void regServ(uid_t u, bool on_demand, bool is_kunc, const char *serv_name, const char *serv_cmd);
static void handleConfigFile(const char *file);
static CFPropertyListRef CreateMyPropertyListFromFile(const char *posixfile);

int main(int argc, char *argv[])
{
	DIR *d;
	struct dirent *de;
	struct stat sb;

	if (argc != 2) {
		fprintf(stderr, "usage: %s: <configdir|configfile>\n", getprogname());
		exit(EXIT_FAILURE);
	}

	stat(argv[1], &sb);

	if (S_ISREG(sb.st_mode)) {
		handleConfigFile(argv[1]);
		exit(EXIT_SUCCESS);
	}

	if (getenv("SECURITYSESSIONID")) {
		if (fork() == 0) {
			const char *h = getenv("HOME");
			struct passwd *pwe = getpwuid(getuid());
			char *buf;
			asprintf(&buf, "%s/%s", h ? h : pwe->pw_dir, "Library/LaunchAgents");
			execlp("launchctl", "launchctl", "load", buf, "/Library/LaunchAgents", "/System/Library/LaunchAgents", NULL);
			exit(EXIT_SUCCESS);
		}
	}

	if ((d = opendir(argv[1])) == NULL) {
		fprintf(stderr, "%s: opendir() failed to open the directory\n", getprogname());
		exit(EXIT_FAILURE);
	}

	while ((de = readdir(d)) != NULL) {
		if ((de->d_name[0] != '.')) {
			char *foo;
			if (asprintf(&foo, "%s/%s", argv[1], de->d_name))
				handleConfigFile(foo);
			free(foo);
		}
	}

	exit(EXIT_SUCCESS);
}

static void handleConfigFile(const char *file)
{
	bool on_demand = true, is_kunc = false;
	uid_t	u = getuid();
	struct passwd *pwe;
	char usr[4096];
	char serv_name[4096];
	char serv_cmd[4096];
	CFPropertyListRef plist = CreateMyPropertyListFromFile(file);

	if (plist) {
		if (CFDictionaryContainsKey(plist, CFSTR("Username"))) {
			const void *v = CFDictionaryGetValue(plist, CFSTR("Username"));

			if (v) CFStringGetCString(v, usr, sizeof(usr), kCFStringEncodingUTF8);
			else goto out;

			if ((pwe = getpwnam(usr))) {
				u = pwe->pw_uid;
			} else {
			     fprintf(stderr, "%s: user not found\n", getprogname());
			     goto out;
			}
		}
		if (CFDictionaryContainsKey(plist, CFSTR("OnDemand"))) {
			const void *v = CFDictionaryGetValue(plist, CFSTR("OnDemand"));
			if (v)
				on_demand = CFBooleanGetValue(v);
			else goto out;
		}
		if (CFDictionaryContainsKey(plist, CFSTR("ServiceName"))) {
			const void *v = CFDictionaryGetValue(plist, CFSTR("ServiceName"));

			if (v) CFStringGetCString(v, serv_name, sizeof(serv_name), kCFStringEncodingUTF8);
			else goto out;
		}
		if (CFDictionaryContainsKey(plist, CFSTR("Command"))) {
			const void *v = CFDictionaryGetValue(plist, CFSTR("Command"));

			if (v) CFStringGetCString(v, serv_cmd, sizeof(serv_cmd), kCFStringEncodingUTF8);
			else goto out;
		}
		if (CFDictionaryContainsKey(plist, CFSTR("isKUNCServer"))) {
			const void *v = CFDictionaryGetValue(plist, CFSTR("isKUNCServer"));
			if (v && CFBooleanGetValue(v)) is_kunc = true;
			else goto out;
		}
		regServ(u, on_demand, is_kunc, serv_name, serv_cmd);
		goto out_good;
out:
		fprintf(stdout, "%s: failed to register: %s\n", getprogname(), file);
out_good:
		CFRelease(plist);
	} else {
		fprintf(stderr, "%s: no plist was returned for: %s\n", getprogname(), file);
	}
}

static void regServ(uid_t u, bool on_demand, bool is_kunc, const char *serv_name, const char *serv_cmd)
{
	kern_return_t kr;
	mach_port_t msr, msv, mhp;

	if ((kr = bootstrap_create_server(bootstrap_port, (char*)serv_cmd, u, on_demand, &msr)) != KERN_SUCCESS) {
		fprintf(stderr, "%s: bootstrap_create_server(): %d\n", getprogname(), kr);
		return;
	}
	if ((kr = bootstrap_create_service(msr, (char*)serv_name, &msv)) != KERN_SUCCESS) {
		fprintf(stderr, "%s: bootstrap_register(): %d\n", getprogname(), kr);
		return;
	}
	if (is_kunc) {
		mhp = mach_host_self();
		if ((kr = host_set_UNDServer(mhp, msv)) != KERN_SUCCESS) {
			fprintf(stderr, "%s: host_set_UNDServer(): %s\n", getprogname(), mach_error_string(kr));
			return;
		}
		mach_port_deallocate(mach_task_self(), mhp);
	}
}

static CFPropertyListRef CreateMyPropertyListFromFile(const char *posixfile)
{
	CFPropertyListRef propertyList;
	CFStringRef       errorString;
	CFDataRef         resourceData;
	SInt32            errorCode;
	CFURLRef          fileURL;

	fileURL = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)posixfile, strlen(posixfile), false);
	if (!fileURL)
		fprintf(stderr, "%s: CFURLCreateFromFileSystemRepresentation(%s) failed\n", getprogname(), posixfile);
	if (!CFURLCreateDataAndPropertiesFromResource(kCFAllocatorDefault, fileURL, &resourceData, NULL, NULL, &errorCode))
		fprintf(stderr, "%s: CFURLCreateDataAndPropertiesFromResource(%s) failed: %d\n", getprogname(), posixfile, (int)errorCode);
	propertyList = CFPropertyListCreateFromXMLData(kCFAllocatorDefault, resourceData, kCFPropertyListImmutable, &errorString);
	if (!propertyList) {
		fprintf(stderr, "%s: propertyList is NULL\n", getprogname());
		if (errorString)
			CFRelease(errorString);
        }
	if (resourceData)
		CFRelease(resourceData);
        if (fileURL)
		CFRelease(fileURL);
        
	return propertyList;
}
