#include <quirewise/caj.h>
#include <quirewise/djvu.h>
#include <quirewise/uvz_parser.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FUZZ_SEED UINT32_C(0x51f0bb77)
#define FUZZ_RUNS 200u

static uint32_t rng_state = FUZZ_SEED;

static const uint8_t djvu_seed[] = {
    'A', 'T', '&', 'T', 'F', 'O', 'R', 'M', 0x00, 0x00, 0x00, 0x10,
    'D', 'J', 'V', 'U'
};

static uint32_t next_random(void)
{
    uint32_t value = rng_state;
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    rng_state = value;
    return value;
}

static size_t mutate(const uint8_t *seed, size_t seed_size, uint8_t *buffer,
                     size_t capacity)
{
    size_t length = seed_size > capacity ? capacity : seed_size;
    unsigned int flips = 1u + next_random() % 4u;
    size_t index;
    memcpy(buffer, seed, length);
    if (next_random() % 8u == 0u && length > 1u)
        length = 1u + next_random() % length;
    for (index = 0u; index < flips && length; ++index) {
        size_t position = next_random() % length;
        buffer[position] = (uint8_t)(buffer[position] ^
                                     (uint8_t)(1u << (next_random() % 8u)));
    }
    return length;
}

int main(void)
{
    uint8_t buffer[2048];
    size_t run;
    size_t ok = 0u;
    size_t rejected = 0u;
    for (run = 0u; run < FUZZ_RUNS; ++run) {
        QwCajDocument *document = NULL;
        size_t size = sizeof(djvu_seed) + (size_t)(next_random() % 512u);
        memcpy(buffer, djvu_seed, sizeof(djvu_seed));
        for (size_t i = sizeof(djvu_seed); i < size; ++i)
            buffer[i] = (uint8_t)next_random();
        if (qw_caj_document_open(buffer, size, &document) == QW_CAJ_OK &&
            document) {
            ++ok;
            qw_caj_document_close(document);
        } else {
            ++rejected;
        }
    }
    for (run = 0u; run < FUZZ_RUNS; ++run) {
        QwUvzDocument *document = NULL;
        size_t size = 16u + (size_t)(next_random() % 512u);
        for (size_t i = 0u; i < size; ++i) buffer[i] = (uint8_t)next_random();
        if (qw_uvz_document_open(buffer, size, &document) == QW_UVZ_OK &&
            document) {
            ++ok;
            qw_uvz_document_close(document);
        } else {
            ++rejected;
        }
    }
    for (run = 0u; run < FUZZ_RUNS; ++run) {
        size_t pages = 0u;
        size_t size = mutate(djvu_seed, sizeof(djvu_seed), buffer,
                             sizeof(buffer));
        (void)qw_djvu_is_supported(buffer, size);
        if (qw_djvu_get_page_count(buffer, size, &pages) == QW_DJVU_OK)
            ++ok;
        else
            ++rejected;
    }
    printf("Page fuzz smoke: %zu ok, %zu rejected, seed=0x%08x runs=%u\n", ok,
           rejected, FUZZ_SEED, FUZZ_RUNS);
    return 0;
}
