/*
 * Copyright (C) 2016 Swift Navigation Inc.
 * Contact: Jacob McNamee <jacob@swiftnav.com>
 *
 * This source is subject to the license found in the file 'LICENSE' which must
 * be be distributed together with this source. All other rights reserved.
 *
 * THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND,
 * EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR PURPOSE.
 */

#include "filter.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <syslog.h>

#include <libpiksi/logging.h>

#include <libsbp/sbp.h>
#include <libsbp/settings.h>

extern bool debug;

#define debug_printf(format, ...) \
  if (debug) \
    fprintf(stdout, "[PID %d] %s+%d(%s) " format, getpid(), \
      __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__);

static bool allow_sensitive_settings_write = false;

typedef struct filter_interface_s {
  const char *name;
  filter_create_fn_t create;
  filter_destroy_fn_t destroy;
  filter_process_fn_t process;
  struct filter_interface_s *next;
} filter_interface_t;

struct filter_s {
  void *state;
  filter_interface_t *interface;
};

static filter_interface_t *filter_interface_list = NULL;

static filter_interface_t * filter_interface_lookup(const char *name)
{
  filter_interface_t *interface;
  for (interface = filter_interface_list; interface != NULL;
       interface = interface->next) {
    if (strcasecmp(name, interface->name) == 0) {
      return interface;
    }
  }
  return NULL;
}

int filter_interface_register(const char *name,
                              filter_create_fn_t create,
                              filter_destroy_fn_t destroy,
                              filter_process_fn_t process)
{
  filter_interface_t *interface = (filter_interface_t *)
                                      malloc(sizeof(*interface));
  if (interface == NULL) {

    syslog(LOG_ERR, "error allocating filter interface");

    return -1;
  }

  *interface = (filter_interface_t) {
    .name = strdup(name),
    .create = create,
    .destroy = destroy,
    .process = process,
    .next = NULL
  };

  if (interface->name == NULL) {
    syslog(LOG_ERR, "error allocating filter interface members");
    free(interface);
    interface = NULL;
    return -1;
  }

  /* Add to list */
  filter_interface_t **p_next = &filter_interface_list;
  while (*p_next != NULL) {
    p_next = &(*p_next)->next;
  }
  *p_next = interface;

  return 0;
}

int filter_interface_valid(const char *name)
{
  filter_interface_t *interface = filter_interface_lookup(name);
  if (interface == NULL) {
    return -1;
  }
  return 0;
}

filter_t * filter_create(const char *name, const char *filename)
{
  /* Look up interface */
  filter_interface_t *interface = filter_interface_lookup(name);
  if (interface == NULL) {
    syslog(LOG_ERR, "unknown filter: %s", name);
    return NULL;
  }

  filter_t *filter = (filter_t *)malloc(sizeof(*filter));
  if (filter == NULL) {
    syslog(LOG_ERR, "error allocating filter");
    return NULL;
  }

  *filter = (filter_t) {
    .state = interface->create(filename),
    .interface = interface
  };

  if (filter->state == NULL) {
    syslog(LOG_ERR, "error creating filter");
    free(filter);
    filter = NULL;
    return NULL;
  }

  return filter;
}

void filter_destroy(filter_t **filter)
{
  (*filter)->interface->destroy(&(*filter)->state);
  free(*filter);
  *filter = NULL;
}

/* Parse SBP message payload into setting parameters */
static bool parse_setting(u8 len, u8 msg[],
                          const char **section,
                          const char **setting,
                          const char **value)
{
  *section = NULL;
  *setting = NULL;
  *value = NULL;

  if (len == 0) {
    return false;
  }

  if (msg[len-1] != '\0') {
    return false;
  }

  if ((len == 0) || (msg[len-1] != '\0')) {
    return false;
  }

  /* Extract parameters from message:
   * 3 null terminated strings: section, setting and value
   */
  *section = (const char *)msg;
  for (int i = 0, tok = 0; i < len; i++) {
    if (msg[i] == '\0') {
      tok++;
      switch (tok) {
      case 1:
        *setting = (const char *)&msg[i+1];
        break;
      case 2:
        if (i + 1 < len)
          *value = (const char *)&msg[i+1];
        break;
      case 3:
        if (i == len-1)
          break;
      default:
        break;
      }
    }
  }

  return true;
}

#define SBP_MSG_TYPE_OFFSET 1
#define SBP_MSG_SIZE_MIN    6

typedef struct {
    const uint8_t *msg;
    uint32_t msg_length;
} read_context_t;

static void write_callback(u16 sender_id, u8 len, u8 msg[], void* context)
{
  (void)sender_id;

  bool* reject = (bool*)context;
  *reject = false; // default value

  const char *section = NULL;
  const char *setting = NULL;
  const char *value = NULL;

  if (!parse_setting(len, msg, &section, &setting, &value)) {
    debug_printf("filter: write_callback: failed to parse setting\n");
    return;
  }

  debug_printf("filter: sensitive write check: section: %s, setting: %s, value: %s\n", section, setting, value);

  if (section == NULL || strcmp(section, "ssh") != 0)
    return;

  if (setting == NULL || strcmp(setting, "enable") != 0)
    return;

  const char* reject_msg = "This interface is not allowed to write sensitive settings";

  syslog(LOG_ERR, reject_msg);
  sbp_log(LOG_ERR, reject_msg);

  *reject = true;
}

static u32 one_buffer_read(u8* buff, u32 n, void* context)
{
  read_context_t* ctx = (read_context_t*) context;
  size_t read_size = (size_t)(n <= ctx->msg_length ? n : ctx->msg_length);

  memcpy(buff, ctx->msg, read_size);

  ctx->msg += read_size;
  ctx->msg_length -= read_size;

  return read_size;
}

static bool reject_sensitive_settings_write(const uint8_t *msg, uint32_t msg_length)
{
  if (allow_sensitive_settings_write) {
    debug_printf("filter: sensitive writes are allowed\n");
    return false;
  }

  // Not an SBP message, reject?
  if (msg_length < SBP_MSG_SIZE_MIN) {
    syslog(LOG_ERR, "filter: rejecting short SBP message");
    return true;
  }

  /* Search for corresponding rule */
  uint16_t msg_type = le16toh(*(uint16_t *)&msg[SBP_MSG_TYPE_OFFSET]);
 
  if (msg_type != SBP_MSG_SETTINGS_WRITE) {
    debug_printf("filter: msg_type: %04hx\n", msg_type);
    debug_printf("filter: msg_type: %02hhx, %02hhx, %02hhx, %02hhx, %02hhx, %02hhx\n",
        msg[0], msg[1], msg[2], msg[3], msg[4], msg[5]);
    return false;
  }

  debug_printf("filter: checking settings write message\n");

  sbp_state_t sbp_state;
  sbp_state_init(&sbp_state);

  read_context_t context = {
    .msg        = msg,
    .msg_length = msg_length,
  };

  bool reject = false;

  sbp_msg_callbacks_node_t callbacks;

  sbp_state_set_io_context(&sbp_state, &context);
  sbp_register_callback(&sbp_state, SBP_MSG_SETTINGS_WRITE, write_callback, &reject, &callbacks);

  s8 retval = sbp_process(&sbp_state, one_buffer_read);

  debug_printf("filter: sensitive write reject: %c, retval: %c\n", reject, retval);

  return reject;
}

int filter_process(filter_t *filter, const uint8_t *msg, uint32_t msg_length)
{
  if (reject_sensitive_settings_write(msg, msg_length))
    return 1;

  return filter->interface->process(filter->state, msg, msg_length);
}

void filter_allow_sensitive_settings_write()
{
  allow_sensitive_settings_write = true;
}
