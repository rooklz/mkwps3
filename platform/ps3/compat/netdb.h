#pragma once
// POSIX <netdb.h> on PSL1GHT lives under net/, and has struct addrinfo but no
// getaddrinfo(): an IPv4 one over gethostbyname() is in compat_lv2.cpp.
#include <net/netdb.h>

#ifndef EAI_NONAME
#define EAI_NONAME -2
#endif
#ifndef EAI_MEMORY
#define EAI_MEMORY -10
#endif

extern "C" {
int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res);
void freeaddrinfo(struct addrinfo* res);
const char* gai_strerror(int code);
}
