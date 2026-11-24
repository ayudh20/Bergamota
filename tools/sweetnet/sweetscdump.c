/*
 * sweetscdump: READ-ONLY dump of what configd/IPMonitor holds for networking, to see why iOS 18
 * builds no DNS configuration from what sweetnetcfg publishes (mDNSResponder: "no DNS service").
 *   sweetscdump [REGEX]     keys of the dynamic store matching REGEX (default: State:/Network and
 *                           Setup:/Network), each with its value; then the DNS configuration that
 *                           mDNSResponder receives (dns_configuration_copy, libsystem_info's dnsinfo)
 * Nothing is written. SCDynamicStore is resolved at run time as in sweetnetcfg.c (the iOS SDK marks
 * it unavailable); dnsinfo's structures are those of configd's public dnsinfo.h.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

typedef const struct __SCDynamicStore *SCDynamicStoreRef;
static SCDynamicStoreRef (*SCDynamicStoreCreate)(CFAllocatorRef, CFStringRef, void *, void *);
static CFArrayRef (*SCDynamicStoreCopyKeyList)(SCDynamicStoreRef, CFStringRef);
static CFPropertyListRef (*SCDynamicStoreCopyValue)(SCDynamicStoreRef, CFStringRef);

/* configd dnsinfo.h: every pointer is a DNS_PTR union, 8 bytes on arm64 */
typedef struct {
	char *domain;
	int32_t n_nameserver;
	struct sockaddr **nameserver;
	uint16_t port;
	int32_t n_search;
	char **search;
	int32_t n_sortaddr;
	void **sortaddr;
	char *options;
	uint32_t timeout;
	uint32_t search_order;
	uint32_t if_index;
	uint32_t flags;
	uint32_t reach_flags;
	uint32_t service_identifier;
	char *cid;
	char *if_name;
} dns_resolver_t;

typedef struct {
	int32_t n_resolver;
	dns_resolver_t **resolver;
	int32_t n_scoped_resolver;
	dns_resolver_t **scoped_resolver;
	uint64_t generation;
	int32_t n_service_specific_resolver;
	dns_resolver_t **service_specific_resolver;
	uint32_t version;
} dns_config_t;

static void
pr(CFTypeRef v)
{
	CFStringRef d = CFCopyDescription(v);
	char buf[8192];
	if (d && CFStringGetCString(d, buf, sizeof(buf), kCFStringEncodingUTF8)) {
		printf("%s\n", buf);
	} else {
		printf("(undescribable)\n");
	}
	if (d) {
		CFRelease(d);
	}
}

static void
resolvers(const char *what, int32_t n, dns_resolver_t **r)
{
	printf("  %s: %d\n", what, n);
	for (int32_t i = 0; i < n && r; i++) {
		dns_resolver_t *x = r[i];
		printf("    [%d] domain=%s if=%s(%u) flags=0x%x reach=0x%x order=%u nameservers=%d:", i,
		    x->domain ? x->domain : "-", x->if_name ? x->if_name : "-", x->if_index, x->flags,
		    x->reach_flags, x->search_order, x->n_nameserver);
		for (int32_t k = 0; k < x->n_nameserver && x->nameserver; k++) {
			struct sockaddr *sa = x->nameserver[k];
			char a[64] = "?";
			if (sa && sa->sa_family == AF_INET) {
				inet_ntop(AF_INET, &((struct sockaddr_in *)sa)->sin_addr, a, sizeof(a));
			} else if (sa && sa->sa_family == AF_INET6) {
				inet_ntop(AF_INET6, &((struct sockaddr_in6 *)sa)->sin6_addr, a, sizeof(a));
			}
			printf(" %s", a);
		}
		printf("\n");
	}
}

int
main(int argc, char **argv)
{
	void *h = dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration", RTLD_NOW);
	if (!h) {
		fprintf(stderr, "sweetscdump: dlopen: %s\n", dlerror());
		return 1;
	}
	SCDynamicStoreCreate = dlsym(h, "SCDynamicStoreCreate");
	SCDynamicStoreCopyKeyList = dlsym(h, "SCDynamicStoreCopyKeyList");
	SCDynamicStoreCopyValue = dlsym(h, "SCDynamicStoreCopyValue");
	if (!SCDynamicStoreCreate || !SCDynamicStoreCopyKeyList || !SCDynamicStoreCopyValue) {
		fprintf(stderr, "sweetscdump: SCDynamicStore symbols missing\n");
		return 1;
	}
	SCDynamicStoreRef st = SCDynamicStoreCreate(NULL, CFSTR("sweetscdump"), NULL, NULL);
	if (!st) {
		fprintf(stderr, "sweetscdump: SCDynamicStoreCreate failed\n");
		return 1;
	}
	CFStringRef pat = argc > 1 ? CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8)
	                           : CFSTR("(State|Setup):/Network/.*");
	CFArrayRef keys = SCDynamicStoreCopyKeyList(st, pat);
	CFIndex n = keys ? CFArrayGetCount(keys) : 0;
	printf("== %ld keys\n", (long)n);
	for (CFIndex i = 0; i < n; i++) {
		CFStringRef k = CFArrayGetValueAtIndex(keys, i);
		char kb[512] = "";
		CFStringGetCString(k, kb, sizeof(kb), kCFStringEncodingUTF8);
		printf("-- %s\n", kb);
		CFPropertyListRef v = SCDynamicStoreCopyValue(st, k);
		if (v) {
			pr(v);
			CFRelease(v);
		} else {
			printf("(no value)\n");
		}
	}

	dns_config_t *(*copy)(void) = dlsym(RTLD_DEFAULT, "dns_configuration_copy");
	void (*free_)(dns_config_t *) = dlsym(RTLD_DEFAULT, "dns_configuration_free");
	printf("== dns_configuration_copy: %s\n", copy ? "found" : "symbol missing");
	if (copy) {
		dns_config_t *c = copy();
		if (!c) {
			printf("  NULL (configd has published no DNS configuration)\n");
		} else {
			printf("  version %u generation %llu\n", c->version, (unsigned long long)c->generation);
			resolvers("resolvers", c->n_resolver, c->resolver);
			resolvers("scoped", c->n_scoped_resolver, c->scoped_resolver);
			resolvers("service-specific", c->n_service_specific_resolver, c->service_specific_resolver);
			if (free_) {
				free_(c);
			}
		}
	}
	return 0;
}
