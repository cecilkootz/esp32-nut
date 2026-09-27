#ifndef HOSTNAME_H
#define HOSTNAME_H

#include <Arduino.h>

// The UPS name as a hostname label (RFC 1123): lowercase letters, digits and single
// hyphens. NUT allows characters a hostname can't have, such as '_' and '.', so
// those become hyphens. At most 31 characters, because the Arduino core truncates
// longer hostnames. Returns fallback when nothing usable is left.
inline String hostnameFromUpsName(const String& ups_name, const String& fallback) {
    char out[32];
    size_t n = 0;
    for (size_t i = 0; i < ups_name.length() && n < sizeof(out) - 1; i++) {
        char c = ups_name[i];
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[n++] = c;
        } else if (n > 0 && out[n - 1] != '-') {
            out[n++] = '-';
        }
    }
    while (n > 0 && out[n - 1] == '-') {
        n--;
    }
    out[n] = '\0';
    return n > 0 ? String(out) : fallback;
}

#endif // HOSTNAME_H
