/*
 * sweetnetcfg: publish the USB Ethernet (ue0) IPv4 + DNS state into
 * configd's dynamic store so IPMonitor makes it the primary service:
 * default route via the Mac and the DNS resolver configuration.
 *
 * configd sees ue0 (patch 0110) but its IPConfiguration agent never
 * configures it, so no State:/Network/Service/<id>/IPv4 exists and
 * IPMonitor deletes the default route and publishes no DNS. This writes
 * the keys IPConfiguration would have written. Run as root:
 *   sweetnetcfg            publish (and keep the session open with -w)
 *   sweetnetcfg -d         remove the keys
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SERVICE "5EE7C0DE-0000-4000-8000-0000000000E0"

/*
 * The iOS SDK marks the SCDynamicStore API unavailable, but iOS's
 * SystemConfiguration.framework exports it (configd's own clients use it):
 * resolve it at run time. Keys are the plain schema strings.
 */
typedef const struct __SCDynamicStore *SCDynamicStoreRef;
static SCDynamicStoreRef (*SCDynamicStoreCreate)(CFAllocatorRef, CFStringRef, void *, void *);
static Boolean (*SCDynamicStoreSetValue)(SCDynamicStoreRef, CFStringRef, CFPropertyListRef);
static Boolean (*SCDynamicStoreRemoveValue)(SCDynamicStoreRef, CFStringRef);
static CFArrayRef (*SCDynamicStoreCopyKeyList)(SCDynamicStoreRef, CFStringRef);
static CFPropertyListRef (*SCDynamicStoreCopyValue)(SCDynamicStoreRef, CFStringRef);
static int (*SCError)(void);
static const char *(*SCErrorString)(int);
#define kSCPropNetIPv4Addresses         CFSTR("Addresses")
#define kSCPropNetIPv4SubnetMasks       CFSTR("SubnetMasks")
#define kSCPropNetIPv4Router            CFSTR("Router")
#define kSCPropInterfaceName            CFSTR("InterfaceName")
#define kSCPropNetDNSServerAddresses    CFSTR("ServerAddresses")

static int
sc_load(void)
{
	void *h = dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration", RTLD_NOW);
	if (h == NULL) {
		fprintf(stderr, "sweetnetcfg: dlopen: %s\n", dlerror());
		return -1;
	}
	SCDynamicStoreCreate = dlsym(h, "SCDynamicStoreCreate");
	SCDynamicStoreSetValue = dlsym(h, "SCDynamicStoreSetValue");
	SCDynamicStoreRemoveValue = dlsym(h, "SCDynamicStoreRemoveValue");
	SCError = dlsym(h, "SCError");
	SCErrorString = dlsym(h, "SCErrorString");
	SCDynamicStoreCopyKeyList = dlsym(h, "SCDynamicStoreCopyKeyList");
	SCDynamicStoreCopyValue = dlsym(h, "SCDynamicStoreCopyValue");
	if (!SCDynamicStoreCreate || !SCDynamicStoreSetValue || !SCDynamicStoreRemoveValue ||
	    !SCError || !SCErrorString || !SCDynamicStoreCopyKeyList || !SCDynamicStoreCopyValue) {
		fprintf(stderr, "sweetnetcfg: SCDynamicStore symbols missing\n");
		return -1;
	}
	return 0;
}

static CFArrayRef
arr1(const char *s)
{
	CFStringRef v = CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
	CFArrayRef a = CFArrayCreate(NULL, (const void **)&v, 1, &kCFTypeArrayCallBacks);
	CFRelease(v);
	return a;
}

static void
set(CFMutableDictionaryRef d, CFStringRef k, CFTypeRef v)
{
	CFDictionarySetValue(d, k, v);
	CFRelease(v);
}

/*
 * iOS 18: configd creates its own hidden service for ue0 (Setup:/Network/Service/<random id>/Interface,
 * DeviceName ue0, HiddenConfiguration 1) and IPMonitor takes DNS only from services that have a Setup entry,
 * so the fixed SERVICE above gets an IPv4 route but no resolver (mDNSResponder: "no DNS service"). Publish the
 * same DNS dictionary under every service whose Setup interface is ue0, when it is missing or different.
 * Returns the number of services found.
 */
static int
dns_for_ue0_services(SCDynamicStoreRef st, CFDictionaryRef dns, int verbose)
{
	CFArrayRef keys = SCDynamicStoreCopyKeyList(st, CFSTR("Setup:/Network/Service/[^/]+/Interface"));
	CFIndex n = keys ? CFArrayGetCount(keys) : 0;
	int found = 0;

	for (CFIndex i = 0; i < n; i++) {
		CFStringRef k = CFArrayGetValueAtIndex(keys, i);
		CFPropertyListRef v = SCDynamicStoreCopyValue(st, k);
		if (v == NULL) {
			continue;
		}
		CFTypeRef dev = CFGetTypeID(v) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(v, CFSTR("DeviceName")) : NULL;
		if (dev && CFGetTypeID(dev) == CFStringGetTypeID() && CFEqual(dev, CFSTR("ue0"))) {
			/* "Setup:/Network/Service/<id>/Interface" -> "State:/Network/Service/<id>/DNS" */
			CFIndex len = CFStringGetLength(k);
			CFStringRef id = CFStringCreateWithSubstring(NULL, k, CFRangeMake(23, len - 23 - 10));
			CFStringRef kd = CFStringCreateWithFormat(NULL, NULL, CFSTR("State:/Network/Service/%@/DNS"), id);
			CFPropertyListRef cur = SCDynamicStoreCopyValue(st, kd);
			if (cur == NULL || !CFEqual(cur, dns)) {
				Boolean ok = SCDynamicStoreSetValue(st, kd, dns);
				if (verbose || !ok) {
					char b[128] = "";
					CFStringGetCString(id, b, sizeof(b), kCFStringEncodingUTF8);
					printf("sweetnetcfg: DNS for ue0 service %s: %s\n", b, ok ? "published" : "FAILED");
				}
			}
			if (cur) {
				CFRelease(cur);
			}
			CFRelease(kd);
			CFRelease(id);
			found++;
		}
		CFRelease(v);
	}
	if (keys) {
		CFRelease(keys);
	}
	return found;
}

int
main(int argc, char **argv)
{
	int del = argc > 1 && strcmp(argv[1], "-d") == 0;
	/* Default: publish and stay (session keys die with the process; the boot job runs it without arguments).
	 * -1: publish once and exit (the old behaviour); -w: accepted, same as the default. */
	int wait = !(argc > 1 && strcmp(argv[1], "-1") == 0);
	if (sc_load() != 0) {
		return 1;
	}
	SCDynamicStoreRef st = SCDynamicStoreCreate(NULL, CFSTR("sweetnetcfg"), NULL, NULL);
	if (st == NULL) {
		fprintf(stderr, "sweetnetcfg: SCDynamicStoreCreate: %s\n", SCErrorString(SCError()));
		return 1;
	}
	CFStringRef kv4 = CFSTR("State:/Network/Service/" SERVICE "/IPv4");
	CFStringRef kdns = CFSTR("State:/Network/Service/" SERVICE "/DNS");
	CFStringRef ksvc = CFSTR("State:/Network/Service/" SERVICE);
	if (del) {
		SCDynamicStoreRemoveValue(st, kv4);
		SCDynamicStoreRemoveValue(st, kdns);
		SCDynamicStoreRemoveValue(st, ksvc);
		printf("sweetnetcfg: removed\n");
		return 0;
	}

	CFMutableDictionaryRef v4 = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	set(v4, kSCPropNetIPv4Addresses, arr1("192.168.7.2"));
	set(v4, kSCPropNetIPv4SubnetMasks, arr1("255.255.255.0"));
	CFDictionarySetValue(v4, kSCPropNetIPv4Router, CFSTR("192.168.7.1"));
	CFDictionarySetValue(v4, kSCPropInterfaceName, CFSTR("ue0"));

	CFMutableDictionaryRef dns = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	/* the host first (a Linux host sharing its connection answers DNS on 192.168.7.1), then public resolvers */
	CFStringRef servers[] = { CFSTR("192.168.7.1"), CFSTR("1.1.1.1"), CFSTR("8.8.8.8") };
	set(dns, kSCPropNetDNSServerAddresses,
	    CFArrayCreate(NULL, (const void **)servers, 3, &kCFTypeArrayCallBacks));

	CFMutableDictionaryRef svc = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(svc, kSCPropInterfaceName, CFSTR("ue0"));

	Boolean ok = SCDynamicStoreSetValue(st, ksvc, svc) &&
	    SCDynamicStoreSetValue(st, kv4, v4) &&
	    SCDynamicStoreSetValue(st, kdns, dns);
	printf("sweetnetcfg: %s (%s)\n", ok ? "published ue0 IPv4 192.168.7.2 via 192.168.7.1, DNS 192.168.7.1 1.1.1.1 8.8.8.8" :
	    "FAILED", ok ? "ok" : SCErrorString(SCError()));
	printf("sweetnetcfg: %d iOS service(s) on ue0\n", dns_for_ue0_services(st, dns, 1));
	fflush(stdout);
	if (ok && wait) {
		/* Session keys live as long as the session: keep it open, and put back what IPMonitor or a
		 * re-plug of the cable removed, and DNS on a ue0 service configd creates later. */
		for (;;) {
			sleep(15);
			CFPropertyListRef cur = SCDynamicStoreCopyValue(st, kdns);
			if (cur == NULL) {
				SCDynamicStoreSetValue(st, ksvc, svc);
				SCDynamicStoreSetValue(st, kv4, v4);
				SCDynamicStoreSetValue(st, kdns, dns);
			} else {
				CFRelease(cur);
			}
			dns_for_ue0_services(st, dns, 0);
		}
	}
	CFRelease(v4);
	CFRelease(dns);
	CFRelease(svc);
	return ok ? 0 : 1;
}
