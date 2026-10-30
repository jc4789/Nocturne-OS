/* C:'s blocks in memory: the store's supply (struct store_supply, store/volume.h) over the device
   C: is on. Any CPU may call it at once. */
#pragma once
#include "store/volume.h"

/* A supply over dev that keeps up to limit blocks nothing holds (the least recently used go
   first). NULL without memory. */
struct store_supply *cache_create(struct store_device *dev, size_t limit);
