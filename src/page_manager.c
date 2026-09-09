#include "page_manager.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "PageManager";

/* ---- Platform: memory ---- */

static void *pm_alloc(size_t size) {
#ifdef ESP_PLATFORM
  void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p)
    p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p;
#else
  return malloc(size);
#endif
}

static void pm_free(void *ptr) {
  if (!ptr)
    return;
#ifdef ESP_PLATFORM
  heap_caps_free(ptr);
#else
  free(ptr);
#endif
}

static char *pm_strdup(const char *s) {
  if (!s)
    return NULL;
  size_t len = strlen(s) + 1;
  char *copy = (char *)pm_alloc(len);
  if (copy)
    memcpy(copy, s, len);
  return copy;
}

/* ---- Internal data structures ---- */

typedef struct pm_page_t {
  char *name;
  pm_page_cbs_t callbacks;
  void *user_data;
  bool created;
  bool visible;
  bool keep_alive;
  bool is_instance;
  bool refresh_pending;
  pm_page_t *template_page;
  lv_obj_t *root;
  uint8_t *nav_data;
  size_t nav_data_len;
} pm_page_t;

typedef struct pm_event_node_t {
  int event_id;
  size_t data_len;
  uint8_t *data;
  pm_event_data_release_cb_t release_cb;
  void *release_user_data;
  bool owns_data;
  bool broadcast;
  struct pm_event_node_t *next;
} pm_event_node_t;

typedef struct pm_transition_t {
  bool active;
  pm_nav_type_t type;
  pm_page_t *old_page;
  pm_page_t *new_page;
  bool destroy_old;
  bool keep_old_visible;
} pm_transition_t;

/* ---- Lock-free MPSC event queue (Treiber stack) ---- */

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) && !defined(__STDC_NO_ATOMICS__)

#include <stdatomic.h>

typedef _Atomic(pm_event_node_t *) pm_atomic_event_ptr;

static pm_event_node_t *event_queue_drain(pm_atomic_event_ptr *head) {
  return atomic_exchange_explicit(head, NULL, memory_order_acq_rel);
}

static void event_queue_push(pm_atomic_event_ptr *head, pm_event_node_t *node) {
  pm_event_node_t *old;
  do {
    old = atomic_load_explicit(head, memory_order_acquire);
    node->next = old;
  } while (!atomic_compare_exchange_weak_explicit(
      head, &old, node, memory_order_acq_rel, memory_order_acquire));
}

#else

typedef pm_event_node_t *pm_atomic_event_ptr;

static pm_event_node_t *event_queue_drain(pm_atomic_event_ptr *head) {
  return __atomic_exchange_n(head, NULL, __ATOMIC_ACQ_REL);
}

static void event_queue_push(pm_atomic_event_ptr *head, pm_event_node_t *node) {
  pm_event_node_t *old;
  do {
    old = __atomic_load_n(head, __ATOMIC_ACQUIRE);
    node->next = old;
  } while (!__atomic_compare_exchange_n(
      head, &old, node, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

#endif

static void event_node_free_chain(pm_event_node_t *node) {
  while (node) {
    pm_event_node_t *next = node->next;
    if (node->release_cb)
      node->release_cb(node->data, node->release_user_data);
    else if (node->owns_data)
      pm_free(node->data);
    pm_free(node);
    node = next;
  }
}

static void event_node_release_data(pm_event_node_t *node) {
  if (!node)
    return;
  if (node->release_cb)
    node->release_cb(node->data, node->release_user_data);
  else if (node->owns_data)
    pm_free(node->data);
}

static pm_event_node_t *event_node_create(int event_id, const void *data,
                                          size_t data_len, bool broadcast) {
  pm_event_node_t *node = (pm_event_node_t *)pm_alloc(sizeof(pm_event_node_t));
  if (!node)
    return NULL;

  memset(node, 0, sizeof(pm_event_node_t));
  node->event_id = event_id;
  node->data_len = data_len;
  node->broadcast = broadcast;

  if (data && data_len > 0) {
    node->data = (uint8_t *)pm_alloc(data_len);
    if (!node->data) {
      pm_free(node);
      return NULL;
    }
    memcpy(node->data, data, data_len);
    node->owns_data = true;
  }

  return node;
}

static pm_event_node_t *event_node_create_fast(
    int event_id, void *data, size_t data_len, bool broadcast,
    pm_event_data_release_cb_t release_cb, void *release_user_data) {
  pm_event_node_t *node = (pm_event_node_t *)pm_alloc(sizeof(pm_event_node_t));
  if (!node)
    return NULL;

  memset(node, 0, sizeof(pm_event_node_t));
  node->event_id = event_id;
  node->data_len = data_len;
  node->data = (uint8_t *)data;
  node->release_cb = release_cb;
  node->release_user_data = release_user_data;
  node->broadcast = broadcast;

  return node;
}

static pm_event_node_t *event_list_reverse(pm_event_node_t *list) {
  pm_event_node_t *prev = NULL;
  while (list) {
    pm_event_node_t *next = list->next;
    list->next = prev;
    prev = list;
    list = next;
  }
  return prev;
}

typedef struct pm_t {
  lv_display_t *disp;
  lv_obj_t *host;
  int disp_w;
  int disp_h;
  pm_page_t *stack[PM_MAX_STACK_DEPTH];
  int stack_depth;
  pm_page_t *overlays[PM_MAX_OVERLAY_DEPTH];
  int overlay_depth;
  pm_page_t **registered;
  int reg_count;
  int reg_capacity;
  pm_atomic_event_ptr event_head;
  lv_timer_t *event_timer;
  pm_action_cb_t action_handler;
  void *action_handler_ud;
  pm_event_preprocessor_t event_preprocessor;
  void *event_preprocessor_ud;
  pm_transition_t transition;
  pm_transition_style_t transition_style;
  uint32_t transition_duration_ms;
  pm_transition_start_cb_t custom_transition_start;
  pm_transition_cancel_cb_t custom_transition_cancel;
  void *custom_transition_ud;
} pm_t;

/* ---- Internal helpers ---- */

static bool pm_is_registered_page(pm_t *pm, pm_page_t *page) {
  if (!pm || !page)
    return false;
  for (int i = 0; i < pm->reg_count; i++) {
    if (pm->registered[i] == page)
      return true;
  }
  return false;
}

static bool pm_is_known_page(pm_t *pm, pm_page_t *page) {
  return pm_is_registered_page(pm, page) || (page && page->is_instance);
}

static void page_free(pm_page_t *page) {
  if (!page)
    return;
  pm_free(page->nav_data);
  pm_free(page->name);
  pm_free(page);
}

static void page_release_if_instance(pm_page_t *page) {
  if (page && page->is_instance)
    page_free(page);
}

static lv_obj_t *create_page_root(pm_t *pm) {
  lv_obj_t *root = lv_obj_create(pm->host);
  lv_obj_set_pos(root, 0, 0);
  lv_obj_set_size(root, pm->disp_w, pm->disp_h);
  lv_obj_set_style_pad_all(root, 0, 0);
  lv_obj_set_style_border_width(root, 0, 0);
  lv_obj_set_style_radius(root, 0, 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
  lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
  lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
  return root;
}

static void page_set_x(pm_page_t *page, int32_t x) {
  if (page && page->root)
    lv_obj_set_x(page->root, x);
}

static void page_anim_x_cb(void *var, int32_t x) {
  lv_obj_set_x((lv_obj_t *)var, x);
}

static void page_anim_opa_cb(void *var, int32_t opa) {
  lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)opa, 0);
}

static void page_reset_transition_state(pm_page_t *page) {
  if (!page || !page->root)
    return;
  lv_obj_set_x(page->root, 0);
  lv_obj_set_style_opa(page->root, LV_OPA_COVER, 0);
}

static void page_anim_x_start(pm_page_t *page, int32_t from, int32_t to,
                              uint32_t duration_ms,
                              lv_anim_ready_cb_t ready_cb, void *user_data) {
  if (!page || !page->root)
    return;

  lv_anim_delete(page->root, page_anim_x_cb);
  lv_obj_set_x(page->root, from);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, page->root);
  lv_anim_set_exec_cb(&a, page_anim_x_cb);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_duration(&a, duration_ms);
  lv_anim_set_path_cb(&a, PM_ANIM_PATH_CB);
  lv_anim_set_user_data(&a, user_data);
  if (ready_cb)
    lv_anim_set_ready_cb(&a, ready_cb);
  lv_anim_start(&a);
}

static void page_anim_opa_start(pm_page_t *page, lv_opa_t from, lv_opa_t to,
                                uint32_t duration_ms,
                                lv_anim_ready_cb_t ready_cb, void *user_data) {
  if (!page || !page->root)
    return;

  lv_anim_delete(page->root, page_anim_opa_cb);
  lv_obj_set_style_opa(page->root, from, 0);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, page->root);
  lv_anim_set_exec_cb(&a, page_anim_opa_cb);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_duration(&a, duration_ms);
  lv_anim_set_path_cb(&a, PM_ANIM_PATH_CB);
  lv_anim_set_user_data(&a, user_data);
  if (ready_cb)
    lv_anim_set_ready_cb(&a, ready_cb);
  lv_anim_start(&a);
}

static void page_do_destroy_internal(pm_page_t *page, bool clear_nav_data) {
  if (!page || !page->created) return;
  if (page->visible && page->callbacks.on_hide && page->root)
    page->callbacks.on_hide(page->root, page->user_data);
  if (page->callbacks.on_destroy && page->root)
    page->callbacks.on_destroy(page->root, page->user_data);
  if (page->root) {
    lv_obj_del(page->root);
    page->root = NULL;
  }
  if (clear_nav_data) {
    pm_free(page->nav_data);
    page->nav_data = NULL;
    page->nav_data_len = 0;
  }
  page->created = false;
  page->visible = false;
}

static void page_do_destroy(pm_page_t *page) {
  page_do_destroy_internal(page, true);
}

static void page_do_destroy_and_release(pm_page_t *page) {
  page_do_destroy(page);
  page_release_if_instance(page);
}

static void page_do_create(pm_t *pm, pm_page_t *page) {
  if (page->created) return;
  lv_obj_t *root = create_page_root(pm);
  page->root = root;
  page->created = true;
  if (page->callbacks.on_create)
    page->callbacks.on_create(root, page->user_data);
}

static void page_do_show(pm_page_t *page) {
  if (page->root)
    lv_obj_remove_flag(page->root, LV_OBJ_FLAG_HIDDEN);
  if (page->visible)
    return;
  page->visible = true;
  if (page->callbacks.on_show && page->root)
    page->callbacks.on_show(page->root, page->user_data);
}

static void page_do_show_at(pm_page_t *page, int32_t x) {
  if (page && page->root)
    lv_obj_set_x(page->root, x);
  page_do_show(page);
}

static void page_do_hide(pm_page_t *page) {
  if (!page || !page->created)
    return;
  if (!page->visible) {
    if (page->root)
      lv_obj_add_flag(page->root, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  if (page->callbacks.on_hide && page->root)
    page->callbacks.on_hide(page->root, page->user_data);
  if (page->root)
    lv_obj_add_flag(page->root, LV_OBJ_FLAG_HIDDEN);
  page->visible = false;
}

static void page_set_nav_data(pm_page_t *page, const void *data, size_t len) {
  if (!page) return;
  pm_free(page->nav_data);
  page->nav_data = NULL;
  page->nav_data_len = 0;
  if (data && len > 0) {
    page->nav_data = (uint8_t *)pm_alloc(len);
    if (page->nav_data) {
      memcpy(page->nav_data, data, len);
      page->nav_data_len = len;
    }
  }
}

static bool pm_page_can_receive_event(pm_page_t *page) {
  return page && page->created && page->root && page->callbacks.on_event;
}

static void pm_transition_clear(pm_t *pm) {
  memset(&pm->transition, 0, sizeof(pm->transition));
}

static void pm_transition_finish(pm_t *pm) {
  pm_transition_t t = pm->transition;
  pm_transition_clear(pm);

  page_reset_transition_state(t.old_page);
  page_reset_transition_state(t.new_page);

  if (t.old_page) {
    if (t.destroy_old) {
      page_do_destroy_and_release(t.old_page);
    } else if (t.keep_old_visible) {
      page_reset_transition_state(t.old_page);
    } else {
      page_do_hide(t.old_page);
    }
  }
  page_reset_transition_state(t.new_page);
}

static void pm_transition_ready_cb(lv_anim_t *a) {
  pm_t *pm = (pm_t *)lv_anim_get_user_data(a);
  if (pm)
    pm_transition_finish(pm);
}

static void pm_transition_done_cb(void *user_data) {
  pm_transition_finish((pm_t *)user_data);
}

static void pm_fill_transition_ctx(pm_t *pm, pm_transition_ctx_t *ctx) {
  pm_transition_t *t = &pm->transition;
  memset(ctx, 0, sizeof(*ctx));
  ctx->nav_type = t->type;
  ctx->old_page = t->old_page;
  ctx->new_page = t->new_page;
  ctx->old_root = t->old_page ? t->old_page->root : NULL;
  ctx->new_root = t->new_page ? t->new_page->root : NULL;
  ctx->width = pm->disp_w;
  ctx->height = pm->disp_h;
  ctx->duration_ms = pm->transition_duration_ms;
  ctx->done_cb = pm_transition_done_cb;
  ctx->done_user_data = pm;
  ctx->user_data = pm->custom_transition_ud;
}

static void pm_transition_cancel(pm_t *pm) {
  if (!pm || !pm->transition.active)
    return;

  pm_page_t *old_page = pm->transition.old_page;
  pm_page_t *new_page = pm->transition.new_page;

  if (old_page && old_page->root) {
    lv_anim_delete(old_page->root, page_anim_x_cb);
    lv_anim_delete(old_page->root, page_anim_opa_cb);
  }
  if (new_page && new_page->root) {
    lv_anim_delete(new_page->root, page_anim_x_cb);
    lv_anim_delete(new_page->root, page_anim_opa_cb);
  }

  if (pm->transition_style == PM_TRANSITION_STYLE_CUSTOM &&
      pm->custom_transition_cancel) {
    pm_transition_ctx_t ctx;
    pm_fill_transition_ctx(pm, &ctx);
    pm->custom_transition_cancel(&ctx);
  }

  pm_transition_finish(pm);
}

static bool pm_should_animate(pm_t *pm, pm_page_t *old_page,
                              pm_page_t *new_page) {
  if (!pm || !old_page || !new_page || !old_page->root || !new_page->root)
    return false;
  if (pm->transition_style == PM_TRANSITION_STYLE_NONE ||
      pm->transition_duration_ms == 0)
    return false;
  if (pm->transition_style == PM_TRANSITION_STYLE_CUSTOM &&
      !pm->custom_transition_start)
    return false;
  return true;
}

static void pm_start_slide_transition(pm_t *pm, pm_nav_type_t type,
                                      pm_page_t *old_page,
                                      pm_page_t *new_page) {
  int32_t w = pm->disp_w;
  int32_t new_from = 0;
  int32_t old_to = 0;

  switch (type) {
  case PM_NAV_PUSH:
  case PM_NAV_OVERLAY_SHOW:
    new_from = w;
    old_to = -w / 3;
    break;
  case PM_NAV_POP:
  case PM_NAV_OVERLAY_CLOSE:
    new_from = -w / 3;
    old_to = w;
    break;
  case PM_NAV_REPLACE:
  case PM_NAV_SWITCH:
    new_from = w;
    old_to = -w;
    break;
  default:
    break;
  }

  page_anim_x_start(old_page, 0, old_to, pm->transition_duration_ms, NULL, NULL);
  page_anim_x_start(new_page, new_from, 0, pm->transition_duration_ms,
                    pm_transition_ready_cb, pm);
  lv_obj_move_foreground(new_page->root);
}

static void pm_start_fade_transition(pm_t *pm, pm_page_t *old_page,
                                     pm_page_t *new_page) {
  page_set_x(old_page, 0);
  page_set_x(new_page, 0);
  page_anim_opa_start(old_page, LV_OPA_COVER, LV_OPA_TRANSP,
                      pm->transition_duration_ms, NULL, NULL);
  page_anim_opa_start(new_page, LV_OPA_TRANSP, LV_OPA_COVER,
                      pm->transition_duration_ms, pm_transition_ready_cb, pm);
  lv_obj_move_foreground(new_page->root);
}

static void pm_start_custom_transition(pm_t *pm, pm_nav_type_t type,
                                       pm_page_t *old_page,
                                       pm_page_t *new_page) {
  (void)type;
  (void)old_page;
  (void)new_page;
  pm_transition_ctx_t ctx;
  pm_fill_transition_ctx(pm, &ctx);
  pm->custom_transition_start(&ctx);
}

static void pm_start_transition(pm_t *pm, pm_nav_type_t type,
                                pm_page_t *old_page, pm_page_t *new_page,
                                bool destroy_old) {
  bool keep_old_visible = (type == PM_NAV_OVERLAY_SHOW);
  if (!pm_should_animate(pm, old_page, new_page)) {
    if (old_page) {
      if (destroy_old)
        page_do_destroy_and_release(old_page);
      else if (!keep_old_visible)
        page_do_hide(old_page);
    }
    page_reset_transition_state(old_page);
    page_reset_transition_state(new_page);
    return;
  }

  pm->transition.active = true;
  pm->transition.type = type;
  pm->transition.old_page = old_page;
  pm->transition.new_page = new_page;
  pm->transition.destroy_old = destroy_old;
  pm->transition.keep_old_visible = keep_old_visible;

  switch (pm->transition_style) {
  case PM_TRANSITION_STYLE_SLIDE:
    pm_start_slide_transition(pm, type, old_page, new_page);
    break;
  case PM_TRANSITION_STYLE_FADE:
    pm_start_fade_transition(pm, old_page, new_page);
    break;
  case PM_TRANSITION_STYLE_CUSTOM:
    pm_start_custom_transition(pm, type, old_page, new_page);
    break;
  case PM_TRANSITION_STYLE_NONE:
  default:
    pm_transition_finish(pm);
    break;
  }
}

/* ---- Event system ---- */

int pm_dispatch_events(pm_t *pm) {
  if (!pm)
    return 0;
  pm_event_node_t *batch = event_queue_drain(&pm->event_head);
  if (!batch)
    return 0;

  batch = event_list_reverse(batch);

  pm_event_node_t *node = batch;
  int dispatched = 0;
  while (node) {
    pm_event_node_t *next = node->next;

    if (pm->event_preprocessor &&
        pm->event_preprocessor(node->event_id, node->data, node->data_len,
                               pm->event_preprocessor_ud)) {
      event_node_release_data(node);
      pm_free(node);
      node = next;
      dispatched++;
      continue;
    }

    if (node->broadcast) {
      pm_page_t *targets[PM_MAX_STACK_DEPTH + PM_MAX_OVERLAY_DEPTH];
      int target_count = pm->stack_depth;
      if (target_count > PM_MAX_STACK_DEPTH)
        target_count = PM_MAX_STACK_DEPTH;
      for (int i = 0; i < target_count; i++)
        targets[i] = pm->stack[i];
      int overlay_count = pm->overlay_depth;
      if (overlay_count > PM_MAX_OVERLAY_DEPTH)
        overlay_count = PM_MAX_OVERLAY_DEPTH;
      for (int i = 0; i < overlay_count; i++)
        targets[target_count++] = pm->overlays[i];

      for (int i = 0; i < target_count; i++) {
        pm_page_t *p = targets[i];
        if (pm_page_can_receive_event(p))
          p->callbacks.on_event(
            p->root, node->event_id, node->data, node->data_len, p->user_data);
      }
    } else {
      pm_page_t *top = NULL;
      if (pm->overlay_depth > 0)
        top = pm->overlays[pm->overlay_depth - 1];
      else if (pm->stack_depth > 0)
        top = pm->stack[pm->stack_depth - 1];
      if (pm_page_can_receive_event(top))
        top->callbacks.on_event(
          top->root, node->event_id, node->data, node->data_len, top->user_data);
    }
    event_node_release_data(node);
    pm_free(node);
    node = next;
    dispatched++;

#if PM_EVENT_DISPATCH_LIMIT > 0
    if (dispatched >= PM_EVENT_DISPATCH_LIMIT && node) {
      pm_event_node_t *requeue = node;
      while (requeue) {
        pm_event_node_t *requeue_next = requeue->next;
        event_queue_push(&pm->event_head, requeue);
        requeue = requeue_next;
      }
      break;
    }
#endif
  }

  return dispatched;
}

void pm_clear_events(pm_t *pm) {
  if (!pm)
    return;
  event_node_free_chain(event_queue_drain(&pm->event_head));
}

void pm_set_transition_style(pm_t *pm, pm_transition_style_t style) {
  if (!pm)
    return;
  if (style > PM_TRANSITION_STYLE_CUSTOM)
    style = PM_TRANSITION_STYLE_NONE;
  pm_transition_cancel(pm);
  pm->transition_style = style;
}

pm_transition_style_t pm_get_transition_style(pm_t *pm) {
  return pm ? pm->transition_style : PM_TRANSITION_STYLE_NONE;
}

void pm_set_transition_duration(pm_t *pm, uint32_t duration_ms) {
  if (!pm)
    return;
  pm_transition_cancel(pm);
  pm->transition_duration_ms = duration_ms;
}

uint32_t pm_get_transition_duration(pm_t *pm) {
  return pm ? pm->transition_duration_ms : 0;
}

void pm_set_custom_transition(pm_t *pm, pm_transition_start_cb_t start_cb,
                              pm_transition_cancel_cb_t cancel_cb,
                              void *user_data) {
  if (!pm)
    return;
  pm_transition_cancel(pm);
  pm->custom_transition_start = start_cb;
  pm->custom_transition_cancel = cancel_cb;
  pm->custom_transition_ud = user_data;
}

static void pm_event_poll_cb(lv_timer_t *timer) {
  pm_t *pm = (pm_t *)lv_timer_get_user_data(timer);
  (void)pm_dispatch_events(pm);
}

void pm_send_event(pm_t *pm, int event_id, const void *data, size_t data_len) {
  if (!pm)
    return;

  pm_event_node_t *node = event_node_create(event_id, data, data_len, false);
  if (!node) {
    PM_LOGE(TAG, "post_event: alloc failed");
    return;
  }

  event_queue_push(&pm->event_head, node);
}

bool pm_send_event_fast(pm_t *pm, int event_id, void *data, size_t data_len,
                        pm_event_data_release_cb_t release_cb,
                        void *release_user_data) {
  if (!pm || (data_len > 0 && !data))
    return false;

  pm_event_node_t *node = event_node_create_fast(
      event_id, data, data_len, false, release_cb, release_user_data);
  if (!node) {
    PM_LOGE(TAG, "post_event_fast: alloc failed");
    return false;
  }

  event_queue_push(&pm->event_head, node);
  return true;
}

void pm_on_action(pm_t *pm, pm_action_cb_t handler,
                           void *user_data) {
  if (!pm) return;
  pm->action_handler = handler;
  pm->action_handler_ud = user_data;
}

void pm_set_event_preprocessor(pm_t *pm, pm_event_preprocessor_t handler,
                                void *user_data) {
  if (!pm) return;
  pm->event_preprocessor = handler;
  pm->event_preprocessor_ud = user_data;
}

void pm_send_action(pm_t *pm, int action_id, const void *data,
                    size_t data_len) {
  if (!pm || !pm->action_handler)
    return;
  if (pm->stack_depth == 0)
    return;
  pm_page_t *top = pm->stack[pm->stack_depth - 1];
  pm->action_handler(top, action_id, data, data_len,
                     pm->action_handler_ud);
}

void *pm_get_page_user_data(pm_page_t *page) {
  if (!page)
    return NULL;
  return page->user_data;
}

void pm_page_set_keep_alive(pm_page_t *page, bool enable) {
  if (page) page->keep_alive = enable;
}

bool pm_page_get_keep_alive(pm_page_t *page) {
  return page ? page->keep_alive : false;
}

static void page_mark_for_refresh(pm_page_t *page) {
  if (page != NULL && page->created)
    page->refresh_pending = true;
}

static void page_refresh_if_marked(pm_t *pm, pm_page_t *page) {
  if (page == NULL || !page->refresh_pending)
    return;
  page->refresh_pending = false;
  page_do_create(pm, page);
}

static void page_resize_root(pm_page_t *page, int width, int height) {
  if (page == NULL || page->root == NULL)
    return;
  lv_obj_set_pos(page->root, 0, 0);
  lv_obj_set_size(page->root, width, height);
}

static void pm_display_resolution_changed(lv_event_t *event) {
  pm_t *pm = (pm_t *)lv_event_get_user_data(event);
  int width;
  int height;
  int index;
  if (pm == NULL || pm->disp == NULL)
    return;
  width = lv_display_get_horizontal_resolution(pm->disp);
  height = lv_display_get_vertical_resolution(pm->disp);
  if (width <= 0 || height <= 0 ||
      (width == pm->disp_w && height == pm->disp_h))
    return;

  pm_transition_cancel(pm);
  pm->disp_w = width;
  pm->disp_h = height;
  lv_obj_set_size(pm->host, width, height);
  for (index = 0; index < pm->reg_count; ++index)
    page_resize_root(pm->registered[index], width, height);
  for (index = 0; index < pm->stack_depth; ++index) {
    if (pm->stack[index] != NULL && pm->stack[index]->is_instance)
      page_resize_root(pm->stack[index], width, height);
  }
  for (index = 0; index < pm->overlay_depth; ++index) {
    if (pm->overlays[index] != NULL && pm->overlays[index]->is_instance)
      page_resize_root(pm->overlays[index], width, height);
  }
  if (pm->stack_depth > 0)
    page_reset_transition_state(pm->stack[pm->stack_depth - 1]);
}

void pm_refresh_pages(pm_t *pm) {
  int index;
  if (pm == NULL)
    return;
  pm_transition_cancel(pm);
  for (index = 0; index < pm->reg_count; ++index)
    page_mark_for_refresh(pm->registered[index]);
  for (index = 0; index < pm->stack_depth; ++index) {
    if (pm->stack[index] != NULL && pm->stack[index]->is_instance)
      page_mark_for_refresh(pm->stack[index]);
  }
  for (index = 0; index < pm->overlay_depth; ++index) {
    if (pm->overlays[index] != NULL && pm->overlays[index]->is_instance)
      page_mark_for_refresh(pm->overlays[index]);
  }
  for (index = 0; index < pm->reg_count; ++index) {
    if (pm->registered[index]->refresh_pending)
      page_do_destroy_internal(pm->registered[index], false);
  }
  for (index = 0; index < pm->stack_depth; ++index) {
    pm_page_t *page = pm->stack[index];
    if (page != NULL && page->is_instance && page->refresh_pending)
      page_do_destroy_internal(page, false);
  }
  for (index = 0; index < pm->overlay_depth; ++index) {
    pm_page_t *page = pm->overlays[index];
    if (page != NULL && page->is_instance && page->refresh_pending)
      page_do_destroy_internal(page, false);
  }
  for (index = 0; index < pm->reg_count; ++index)
    page_refresh_if_marked(pm, pm->registered[index]);
  for (index = 0; index < pm->stack_depth; ++index) {
    if (pm->stack[index] != NULL && pm->stack[index]->is_instance)
      page_refresh_if_marked(pm, pm->stack[index]);
  }
  for (index = 0; index < pm->overlay_depth; ++index) {
    if (pm->overlays[index] != NULL && pm->overlays[index]->is_instance)
      page_refresh_if_marked(pm, pm->overlays[index]);
  }
  if (pm->stack_depth > 0)
    page_do_show(pm->stack[pm->stack_depth - 1]);
  for (index = 0; index < pm->overlay_depth; ++index)
    page_do_show(pm->overlays[index]);
}

/* ---- Public API ---- */

pm_t *pm_create(lv_display_t *disp) {
  if (!disp) {
    disp = lv_display_get_default();
  }
  if (!disp) {
    PM_LOGE(TAG, "No display available");
    return NULL;
  }

  int w = lv_display_get_horizontal_resolution(disp);
  int h = lv_display_get_vertical_resolution(disp);

  pm_t *pm = (pm_t *)pm_alloc(sizeof(pm_t));
  if (!pm) {
    PM_LOGE(TAG, "Failed to allocate page manager");
    return NULL;
  }
  memset(pm, 0, sizeof(pm_t));

  pm->disp   = disp;
  pm->disp_w = w;
  pm->disp_h = h;
  pm->transition_style = PM_TRANSITION_STYLE_NONE;
  pm->transition_duration_ms = PM_ANIM_DURATION_MS;

  pm->host = lv_obj_create(lv_display_get_screen_active(disp));
  lv_obj_set_pos(pm->host, 0, 0);
  lv_obj_set_size(pm->host, w, h);
  lv_obj_set_style_pad_all(pm->host, 0, 0);
  lv_obj_set_style_border_width(pm->host, 0, 0);
  lv_obj_set_style_radius(pm->host, 0, 0);
  lv_obj_set_style_bg_opa(pm->host, LV_OPA_TRANSP, 0);
  lv_obj_set_scrollbar_mode(pm->host, LV_SCROLLBAR_MODE_OFF);
  lv_obj_remove_flag(pm->host, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  pm->reg_capacity = 8;
  pm->registered   = (pm_page_t **)pm_alloc(sizeof(pm_page_t *) * (size_t)pm->reg_capacity);
  if (!pm->registered) {
    PM_LOGE(TAG, "Failed to allocate registered array");
    lv_obj_del(pm->host);
    pm_free(pm);
    return NULL;
  }

  pm->event_timer = lv_timer_create(pm_event_poll_cb, PM_EVENT_POLL_MS, pm);
  if (!pm->event_timer) {
    PM_LOGE(TAG, "Failed to create event poll timer");
    pm_free(pm->registered);
    lv_obj_del(pm->host);
    pm_free(pm);
    return NULL;
  }

  lv_display_add_event_cb(disp, pm_display_resolution_changed,
                          LV_EVENT_RESOLUTION_CHANGED, pm);

  PM_LOGI(TAG, "Created page manager %dx%d", w, h);
  return pm;
}

void pm_destroy(pm_t *pm) {
  if (!pm)
    return;

  if (pm->disp != NULL) {
    lv_display_remove_event_cb_with_user_data(
        pm->disp, pm_display_resolution_changed, pm);
  }

  if (pm->event_timer) {
    lv_timer_delete(pm->event_timer);
    pm->event_timer = NULL;
  }

  pm_clear_events(pm);

  pm_close_all_overlays(pm);

  for (int i = pm->stack_depth - 1; i >= 0; i--) {
    page_do_destroy_and_release(pm->stack[i]);
  }
  pm->stack_depth = 0;

  for (int i = 0; i < pm->reg_count; i++) {
    page_free(pm->registered[i]);
  }
  pm_free(pm->registered);

  if (pm->host) {
    lv_obj_del(pm->host);
  }

  pm_free(pm);
}

pm_page_t *
pm_register_page(pm_t *pm, const char *name, const pm_page_cbs_t *callbacks, void *user_data) {
  if (!pm || !name || !callbacks) {
    PM_LOGE(TAG, "register_page: invalid args");
    return NULL;
  }

  pm_page_t *page = (pm_page_t *)pm_alloc(sizeof(pm_page_t));
  if (!page) {
    PM_LOGE(TAG, "Failed to allocate page");
    return NULL;
  }
  memset(page, 0, sizeof(pm_page_t));

  page->name = pm_strdup(name);
  if (!page->name) {
    PM_LOGE(TAG, "Failed to strdup page name");
    pm_free(page);
    return NULL;
  }
  page->callbacks = *callbacks;
  page->user_data = user_data;

  if (pm->reg_count >= pm->reg_capacity) {
    int new_cap         = pm->reg_capacity * 2;
    pm_page_t **new_arr = (pm_page_t **)pm_alloc(sizeof(pm_page_t *) * (size_t)new_cap);
    if (!new_arr) {
      PM_LOGE(TAG, "Failed to grow registered array");
      pm_free(page->name);
      pm_free(page);
      return NULL;
    }
    memcpy(new_arr, pm->registered, sizeof(pm_page_t *) * (size_t)pm->reg_count);
    pm_free(pm->registered);
    pm->registered   = new_arr;
    pm->reg_capacity = new_cap;
  }

  pm->registered[pm->reg_count++] = page;

  PM_LOGI(TAG, "Registered page: %s", name);
  return page;
}

void pm_push(pm_t *pm, pm_page_t *page) {
  if (!pm || !page) {
    PM_LOGE(TAG, "push: invalid args");
    return;
  }
  pm_transition_cancel(pm);
  pm_close_all_overlays(pm);
  if (!pm_is_known_page(pm, page)) {
    PM_LOGE(TAG, "push: page is not registered with this manager");
    return;
  }

  /* Already top — no-op */
  if (pm->stack_depth > 0 && pm->stack[pm->stack_depth - 1] == page)
    return;

  /* Registered pages are singletons by default; instances can appear multiple times. */
  if (!page->is_instance && pm_has_page(pm, page)) {
    pm_pop_to(pm, page);
    return;
  }

  if (pm->stack_depth >= PM_MAX_STACK_DEPTH) {
    PM_LOGE(TAG, "push: stack full (depth=%d)", pm->stack_depth);
    return;
  }

  pm_page_t *old_top = NULL;
  bool destroy_old = false;

  if (pm->stack_depth > 0) {
    old_top = pm->stack[pm->stack_depth - 1];
    destroy_old = !old_top->keep_alive;
  }

  page_do_create(pm, page);

  pm->stack[pm->stack_depth++] = page;

  page_do_show_at(page, old_top ? pm->disp_w : 0);
  pm_start_transition(pm, PM_NAV_PUSH, old_top, page, destroy_old);

  PM_LOGI(TAG, "Pushed page: %s (depth=%d)", page->name, pm->stack_depth);
}

bool pm_pop(pm_t *pm) {
  if (!pm)
    return false;
  pm_transition_cancel(pm);
  pm_close_all_overlays(pm);
  if (pm->stack_depth <= 1) {
    PM_LOGW(TAG, "pop: can't pop last page (depth=%d)", pm->stack_depth);
    return false;
  }

  pm_page_t *top     = pm->stack[pm->stack_depth - 1];
  pm_page_t *new_top = pm->stack[pm->stack_depth - 2];

  pm->stack_depth--;

  page_do_create(pm, new_top);
  page_do_show_at(new_top, -pm->disp_w / 3);
  pm_start_transition(pm, PM_NAV_POP, top, new_top, true);

  PM_LOGI(TAG, "Popped page: %s (depth=%d)", top->name, pm->stack_depth);
  return true;
}

bool pm_pop_to(pm_t *pm, pm_page_t *page) {
  if (!pm || !page)
    return false;
  pm_transition_cancel(pm);
  pm_close_all_overlays(pm);

  int target_idx = -1;
  for (int i = 0; i < pm->stack_depth; i++) {
    if (pm->stack[i] == page) {
      target_idx = i;
      break;
    }
  }

  if (target_idx < 0) {
    PM_LOGW(TAG, "pop_to: page '%s' not in stack", page->name);
    return false;
  }

  pm_page_t *old_top = pm->stack_depth > 0 ? pm->stack[pm->stack_depth - 1] : NULL;
  for (int i = pm->stack_depth - 1; i > target_idx; i--) {
    if (pm->stack[i] != old_top)
      page_do_destroy_and_release(pm->stack[i]);
  }

  pm->stack_depth = target_idx + 1;

  page_do_create(pm, page);
  page_do_show_at(page, -pm->disp_w / 3);
  if (old_top == page) {
    page_set_x(page, 0);
  } else {
    pm_start_transition(pm, PM_NAV_POP, old_top, page, true);
  }

  PM_LOGI(TAG, "pop_to: '%s' (depth=%d)", page->name, pm->stack_depth);
  return true;
}

pm_page_t *pm_get_top_page(pm_t *pm) {
  if (!pm || pm->stack_depth == 0)
    return NULL;
  return pm->stack[pm->stack_depth - 1];
}

lv_obj_t *pm_get_page_root(pm_page_t *page) {
  if (!page)
    return NULL;
  return page->root;
}

int pm_get_stack_depth(pm_t *pm) {
  if (!pm)
    return 0;
  return pm->stack_depth;
}

const char *pm_get_page_name(pm_page_t *page) {
  if (!page)
    return NULL;
  return page->name;
}

bool pm_has_page(pm_t *pm, pm_page_t *page) {
  if (!pm || !page) return false;
  for (int i = 0; i < pm->stack_depth; i++) {
    if (pm->stack[i] == page) return true;
  }
  return false;
}

bool pm_is_top(pm_t *pm, pm_page_t *page) {
  if (!pm || !page || pm->stack_depth == 0) return false;
  return pm->stack[pm->stack_depth - 1] == page;
}

pm_page_t *pm_get_page_at(pm_t *pm, int index) {
  if (!pm || index < 0 || index >= pm->stack_depth) return NULL;
  return pm->stack[index];
}

void pm_replace_top(pm_t *pm, pm_page_t *page) {
  if (!pm || !page) return;
  pm_transition_cancel(pm);
  pm_close_all_overlays(pm);
  if (!pm_is_known_page(pm, page)) {
    PM_LOGE(TAG, "replace_top: page is not registered with this manager");
    return;
  }
  if (pm->stack_depth == 0) {
    pm_push(pm, page);
    return;
  }
  pm_page_t *old = pm->stack[pm->stack_depth - 1];
  if (old == page) return;
  if (!page->is_instance && pm_has_page(pm, page)) {
    pm_pop_to(pm, page);
    return;
  }

  pm->stack_depth--;

  page_do_create(pm, page);
  pm->stack[pm->stack_depth++] = page;
  page_do_show_at(page, pm->disp_w);
  pm_start_transition(pm, PM_NAV_REPLACE, old, page, true);

  PM_LOGI(TAG, "replace_top: '%s' -> '%s' (depth=%d)",
          old->name, page->name, pm->stack_depth);
}

bool pm_switch_to(pm_t *pm, pm_page_t *page) {
  if (!pm || !page)
    return false;
  pm_transition_cancel(pm);
  if (!pm_is_known_page(pm, page)) {
    PM_LOGE(TAG, "switch_to: page is not registered with this manager");
    return false;
  }

  pm_close_all_overlays(pm);

  pm_page_t *old = pm->stack_depth > 0 ? pm->stack[pm->stack_depth - 1] : NULL;
  if (old == page)
    return true;

  pm_page_t *new_stack[PM_MAX_STACK_DEPTH];
  int new_depth = 0;

  for (int i = 0; i < pm->stack_depth; i++) {
    pm_page_t *p = pm->stack[i];
    if (p == page)
      continue;
    if (p == old || p->keep_alive) {
      if (new_depth >= PM_MAX_STACK_DEPTH) {
        PM_LOGE(TAG, "switch_to: stack full while preserving pages");
        return false;
      }
      if (p != old)
        page_do_hide(p);
      new_stack[new_depth++] = p;
    } else {
      page_do_destroy_and_release(p);
    }
  }

  pm->stack_depth = 0;
  for (int i = 0; i < new_depth; i++)
    pm->stack[pm->stack_depth++] = new_stack[i];
  if (pm->stack_depth >= PM_MAX_STACK_DEPTH) {
    PM_LOGE(TAG, "switch_to: stack full (depth=%d)", pm->stack_depth);
    return false;
  }
  pm->stack[pm->stack_depth++] = page;

  page_do_create(pm, page);
  page_do_show_at(page, old ? pm->disp_w : 0);
  pm_start_transition(pm, PM_NAV_SWITCH, old, page, false);

  PM_LOGI(TAG, "switch_to: '%s' (depth=%d)", page->name, pm->stack_depth);
  return true;
}

bool pm_switch_to_with_data(pm_t *pm, pm_page_t *page,
                            const void *data, size_t data_len) {
  if (!pm || !page)
    return false;
  if (!pm_is_known_page(pm, page)) {
    PM_LOGE(TAG, "switch_to_with_data: page is not registered with this manager");
    return false;
  }
  page_set_nav_data(page, data, data_len);
  return pm_switch_to(pm, page);
}

pm_page_t *pm_create_page_instance(pm_t *pm, pm_page_t *template_page,
                                   const void *data, size_t data_len) {
  if (!pm || !template_page || !pm_is_registered_page(pm, template_page)) {
    PM_LOGE(TAG, "create_page_instance: invalid template page");
    return NULL;
  }

  pm_page_t *page = (pm_page_t *)pm_alloc(sizeof(pm_page_t));
  if (!page) {
    PM_LOGE(TAG, "create_page_instance: alloc failed");
    return NULL;
  }
  memset(page, 0, sizeof(pm_page_t));

  page->name = pm_strdup(template_page->name);
  if (!page->name) {
    pm_free(page);
    return NULL;
  }
  page->callbacks = template_page->callbacks;
  page->user_data = template_page->user_data;
  page->keep_alive = template_page->keep_alive;
  page->is_instance = true;
  page->template_page = template_page;
  page_set_nav_data(page, data, data_len);

  PM_LOGI(TAG, "Created page instance: %s", page->name);
  return page;
}

void pm_destroy_page_instance(pm_t *pm, pm_page_t *page) {
  (void)pm;
  if (!page || !page->is_instance)
    return;
  page_do_destroy_and_release(page);
}

pm_page_t *pm_push_instance(pm_t *pm, pm_page_t *template_page,
                            const void *data, size_t data_len) {
  pm_page_t *page = pm_create_page_instance(pm, template_page, data, data_len);
  if (!page)
    return NULL;
  pm_push(pm, page);
  return page;
}

bool pm_show_overlay(pm_t *pm, pm_page_t *page) {
  if (!pm || !page)
    return false;
  pm_transition_cancel(pm);
  if (!pm_is_known_page(pm, page)) {
    PM_LOGE(TAG, "show_overlay: page is not registered with this manager");
    return false;
  }
  if (!page->is_instance && pm_has_page(pm, page)) {
    PM_LOGE(TAG, "show_overlay: page is already in the main stack");
    return false;
  }
  for (int i = 0; i < pm->overlay_depth; i++) {
    if (pm->overlays[i] == page) {
      PM_LOGW(TAG, "show_overlay: page is already an overlay");
      return false;
    }
  }
  if (pm->overlay_depth >= PM_MAX_OVERLAY_DEPTH) {
    PM_LOGE(TAG, "show_overlay: overlay stack full (depth=%d)", pm->overlay_depth);
    return false;
  }

  pm_page_t *under = NULL;
  if (pm->overlay_depth > 0)
    under = pm->overlays[pm->overlay_depth - 1];
  else if (pm->stack_depth > 0)
    under = pm->stack[pm->stack_depth - 1];

  page_do_create(pm, page);
  pm->overlays[pm->overlay_depth++] = page;
  page_do_show_at(page, pm->disp_w);
  if (page->root)
    lv_obj_move_foreground(page->root);
  if (under && under->root)
    pm_start_transition(pm, PM_NAV_OVERLAY_SHOW, under, page, false);
  else
    page_reset_transition_state(page);

  PM_LOGI(TAG, "show_overlay: '%s' (overlay_depth=%d)",
          page->name, pm->overlay_depth);
  return true;
}

bool pm_show_overlay_with_data(pm_t *pm, pm_page_t *page,
                               const void *data, size_t data_len) {
  if (!pm || !page)
    return false;
  if (!pm_is_known_page(pm, page)) {
    PM_LOGE(TAG, "show_overlay_with_data: page is not registered with this manager");
    return false;
  }
  page_set_nav_data(page, data, data_len);
  return pm_show_overlay(pm, page);
}

bool pm_close_overlay(pm_t *pm) {
  if (!pm)
    return false;
  pm_transition_cancel(pm);
  if (pm->overlay_depth <= 0)
    return false;

  pm_page_t *top = pm->overlays[pm->overlay_depth - 1];
  pm->overlay_depth--;
  pm->overlays[pm->overlay_depth] = NULL;

  pm_page_t *under = NULL;
  if (pm->overlay_depth > 0)
    under = pm->overlays[pm->overlay_depth - 1];
  else if (pm->stack_depth > 0)
    under = pm->stack[pm->stack_depth - 1];

  if (under) {
    page_do_create(pm, under);
    page_do_show_at(under, -pm->disp_w / 3);
  }
  if (under && under->root)
    pm_start_transition(pm, PM_NAV_OVERLAY_CLOSE, top, under, true);
  else
    page_do_destroy_and_release(top);

  PM_LOGI(TAG, "close_overlay: '%s' (overlay_depth=%d)",
          top->name, pm->overlay_depth);
  return true;
}

void pm_close_all_overlays(pm_t *pm) {
  if (!pm)
    return;
  pm_transition_cancel(pm);
  while (pm->overlay_depth > 0) {
    pm_page_t *top = pm->overlays[--pm->overlay_depth];
    pm->overlays[pm->overlay_depth] = NULL;
    page_do_destroy_and_release(top);
  }
}

pm_page_t *pm_get_top_overlay(pm_t *pm) {
  if (!pm || pm->overlay_depth <= 0)
    return NULL;
  return pm->overlays[pm->overlay_depth - 1];
}

int pm_get_overlay_depth(pm_t *pm) {
  return pm ? pm->overlay_depth : 0;
}

void pm_broadcast_event(pm_t *pm, int event_id, const void *data, size_t data_len) {
  if (!pm) return;

  pm_event_node_t *node = event_node_create(event_id, data, data_len, true);
  if (!node) {
    PM_LOGE(TAG, "broadcast_event: alloc failed");
    return;
  }

  event_queue_push(&pm->event_head, node);
}

void pm_push_with_data(pm_t *pm, pm_page_t *page,
                       const void *data, size_t data_len) {
  if (!pm || !page) return;
  if (!pm_is_known_page(pm, page)) {
    PM_LOGE(TAG, "push_with_data: page is not registered with this manager");
    return;
  }
  page_set_nav_data(page, data, data_len);
  pm_push(pm, page);
}

void pm_replace_top_with_data(pm_t *pm, pm_page_t *page,
                              const void *data, size_t data_len) {
  if (!pm || !page) return;
  if (!pm_is_known_page(pm, page)) {
    PM_LOGE(TAG, "replace_top_with_data: page is not registered with this manager");
    return;
  }
  page_set_nav_data(page, data, data_len);
  pm_replace_top(pm, page);
}

const void *pm_get_nav_data(pm_page_t *page, size_t *out_len) {
  if (!page || !page->nav_data || page->nav_data_len == 0) {
    if (out_len) *out_len = 0;
    return NULL;
  }
  if (out_len) *out_len = page->nav_data_len;
  return page->nav_data;
}
