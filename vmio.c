#include "vmio.h"

void oevent_list_init(Oevent_list *olist) {
  olist->buffer = NULL;
  olist->count = 0;
  olist->capacity = 0;
}
void oevent_list_deinit(Oevent_list *olist) { free(olist->buffer); }
void oevent_list_clear(Oevent_list *olist) { olist->count = 0; }
bool oevent_list_copy(Oevent_list const *src, Oevent_list *dest) {
  Usz src_count = src->count;
  if (dest->capacity < src_count) {
    Usz new_cap = orca_round_up_power2(src_count);
    Oevent *grown = realloc(dest->buffer, new_cap * sizeof(Oevent));
    if (!grown) {
      // As opstate_copy does: the destination is emptied, and keeps its
      // buffer and capacity.
      dest->count = 0;
      return false;
    }
    dest->buffer = grown;
    dest->capacity = new_cap;
  }
  memcpy(dest->buffer, src->buffer, src_count * sizeof(Oevent));
  dest->count = src_count;
  return true;
}
Oevent *oevent_list_alloc_item(Oevent_list *olist) {
  Usz count = olist->count;
  if (olist->capacity == count) {
    // Note: no overflow check, but you're probably out of memory if this
    // happens anyway. A failed growth leaves the list as it was and drops
    // the event into the discard slot, which is never counted or sent.
    Usz capacity = count < 16 ? 16 : orca_round_up_power2(count + 1);
    Oevent *grown = realloc(olist->buffer, capacity * sizeof(Oevent));
    if (!grown)
      return &olist->discard;
    olist->buffer = grown;
    olist->capacity = capacity;
  }
  Oevent *result = olist->buffer + count;
  olist->count = count + 1;
  return result;
}
