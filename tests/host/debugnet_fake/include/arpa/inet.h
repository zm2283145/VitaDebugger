#pragma once

#include <netinet/in.h>

int inet_pton(int address_family, const char* source, void* destination);
