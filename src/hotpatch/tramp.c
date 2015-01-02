#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sysexits.h>
#include <sys/mman.h>
#include <err.h>
#include <assert.h>

#include "arch.h"
#include "bin_api.h"
#include "util.h"

#define FREELIST_INLINES 3

/*
 * trampoline specific stuff
 */
static struct tramp_st2_entry *tramp_table;
static size_t tramp_size;

/*
 * inline trampoline specific stuff
 */
static size_t inline_tramp_size;
static struct inline_tramp_st2_entry *inline_tramp_table;
struct memprof_config memprof_config;


void init_memprof_config_base() {
  memset(&memprof_config, 0, sizeof(memprof_config));
  memprof_config.offset_heaps_slot_limit = SIZE_MAX;
  memprof_config.offset_heaps_slot_slot = SIZE_MAX;
  memprof_config.pagesize = getpagesize();
  assert(memprof_config.pagesize);
}




void
create_tramp_table()
{
  size_t i;
  void *region, *ent, *inline_ent;
  size_t tramp_sz = 0, inline_tramp_sz = 0;

  ent = arch_get_st2_tramp(&tramp_sz);
  inline_ent = arch_get_inline_st2_tramp(&inline_tramp_sz);
  assert(ent && inline_ent);

  region = bin_allocate_page();
  if (region == MAP_FAILED)
    errx(EX_SOFTWARE, "Failed to allocate memory for stage 1 trampolines.");

  tramp_table = region;
  inline_tramp_table = region + memprof_config.pagesize / 2;

  for (i = 0; i < (memprof_config.pagesize / 2) / tramp_sz; i++) {
    memcpy(tramp_table + i, ent, tramp_sz);
  }

  for (i = 0; i < (memprof_config.pagesize / 2) / inline_tramp_sz; i++) {
    memcpy(inline_tramp_table + i, inline_ent, inline_tramp_sz);
  }
}

void
insert_tramp(const char *trampee, void *tramp)
{
  void *trampee_addr = bin_find_symbol(trampee, NULL, 1);
  int inline_ent = inline_tramp_size;

  if (trampee_addr == NULL) {
      errx(EX_SOFTWARE, "Failed to locate required symbol %s", trampee);
  } else {
    tramp_table[tramp_size].addr = tramp;
    if (bin_update_image(trampee, &tramp_table[tramp_size], NULL) != 0)
      errx(EX_SOFTWARE, "Failed to insert tramp for %s", trampee);
    tramp_size++;
  }
}
