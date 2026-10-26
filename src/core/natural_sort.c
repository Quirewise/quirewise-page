#include "natural_sort.h"
#include "byte_utils.h"

#include <string.h>

int qw_natural_name_compare(const char *left, const char *right)
{
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    size_t i = 0;
    size_t j = 0;

    while (a[i] && b[j]) {
        if (a[i] >= '0' && a[i] <= '9' && b[j] >= '0' && b[j] <= '9') {
            size_t ai = i;
            size_t bj = j;
            size_t a_significant;
            size_t b_significant;
            int order;
            while (a[i] >= '0' && a[i] <= '9') ++i;
            while (b[j] >= '0' && b[j] <= '9') ++j;
            a_significant = ai;
            b_significant = bj;
            while (a_significant < i && a[a_significant] == '0') ++a_significant;
            while (b_significant < j && b[b_significant] == '0') ++b_significant;
            order = (int)((i - a_significant) > (j - b_significant)) -
                    (int)((i - a_significant) < (j - b_significant));
            if (order) return order;
            if (i - a_significant) {
                order = memcmp(a + a_significant, b + b_significant,
                               i - a_significant);
                if (order) return order < 0 ? -1 : 1;
            }
            continue;
        }
        {
            int ca = qw_ascii_lower(a[i]);
            int cb = qw_ascii_lower(b[j]);
            if (ca != cb) return ca < cb ? -1 : 1;
        }
        ++i;
        ++j;
    }
    if (a[i] || b[j]) {
        return a[i] ? 1 : -1;
    }
    while (*left && *right) {
        int ca = qw_ascii_lower((unsigned char)*left++);
        int cb = qw_ascii_lower((unsigned char)*right++);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return 0;
}
