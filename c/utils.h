#ifndef UTILS_H
#define UTILS_H
#define NODE_API_CALL_DEFAULT(call, def)                                       \
  do {                                                                         \
    napi_status status = (call);                                               \
    if (status != napi_ok) {                                                   \
      const napi_extended_error_info *error_info = NULL;                       \
      napi_get_last_error_info(env, &error_info);                              \
      const char *err_message = error_info->error_message;                     \
      bool is_pending;                                                         \
      napi_is_exception_pending(env, &is_pending);                             \
      if (!is_pending) {                                                       \
        const char *message =                                                  \
            err_message ? err_message : "empty error message";                 \
        int len = snprintf(NULL, 0, "%s\n    at %s (%s:%d)", message,          \
                           __func__, __FILE__, __LINE__) +                     \
                  1;                                                           \
        char *buf = malloc(len);                                               \
        if (buf) {                                                             \
          snprintf(buf, len, "%s\n    at %s (%s:%d)", message, __func__,       \
                   __FILE__, __LINE__);                                        \
          message = buf;                                                       \
        }                                                                      \
        napi_throw_error(env, NULL, message);                                  \
        free(buf);                                                             \
      }                                                                        \
      return def;                                                              \
    }                                                                          \
  } while (0)
#define NODE_API_CALL(call) NODE_API_CALL_DEFAULT(call, UNDEFINED)
#define NODE_SET_PROPERTY(object, name, value)                                 \
  NODE_API_CALL(napi_set_named_property(env, (object), (name), (value)))
#define NODE_LOAD_ARGUMENTS(count, cbinfo)                                     \
  napi_value arguments[(count)];                                               \
  {                                                                            \
    size_t argc = (count);                                                     \
    NODE_API_CALL(                                                             \
        napi_get_cb_info(env, (cbinfo), &argc, arguments, NULL, NULL));        \
  }
#define _GET_MACRO2(_1, _2, NAME, ...) NAME
#define _GET_MACRO3(_1, _2, _3, NAME, ...) NAME
#define STRING(str) String(env, (str))
#define NUMBER(value) Number(env, (value))
#define BIGINT64(value) BigInt64(env, (value))
#define BIGUINT64(value) BigUInt64(env, (value))
#define BIGINTWORDS(value) BigIntWords(env, (value))
#define FUNCTION(name) Function(env, #name, (name))
#define FREEZE(object) NODE_API_CALL(napi_object_freeze(env, (object)))
#define SEAL(object) NODE_API_CALL(napi_object_seal(env, (object)))
#define Array(...)                                                             \
  _GET_MACRO2(__VA_ARGS__, createArrayWithLength, createArray)(__VA_ARGS__)
#define EXTERNAL(data) External(env, (data), NULL, NULL)
#define DECLARE_PROP_ATTR(name, value, attributes)                             \
  {name, NULL, NULL, NULL, NULL, value, attributes, NULL}
#define DECLARE_PROP_NOATTR(name, value)                                       \
  {name, NULL, NULL, NULL, NULL, value, napi_default_jsproperty, NULL}
#define DECLARE_PROPERTY(...)                                                  \
  _GET_MACRO3(__VA_ARGS__, DECLARE_PROP_ATTR, DECLARE_PROP_NOATTR)(__VA_ARGS__)
#define PROP_CONST(name, value) DECLARE_PROP_ATTR(#name, value, napi_enumerable)
#define PROP_GETSET(name, prop, type)                                          \
  {#name,                                                                      \
   NULL,                                                                       \
   NULL,                                                                       \
   get_##type,                                                                 \
   set_##type,                                                                 \
   NULL,                                                                       \
   napi_enumerable | napi_writable,                                            \
   &native->prop}
#define PROP_GET(name, prop, type)                                             \
  {#name, NULL, NULL, get_##type, NULL, NULL, napi_enumerable, &native->prop}
#define LOAD_SET(cbinfo, type)                                                 \
  type *data;                                                                  \
  napi_value argv[1];                                                          \
  size_t argc = 1;                                                             \
  NODE_API_CALL(                                                               \
      napi_get_cb_info(env, cbinfo, &argc, argv, NULL, (void **)&data));       \
  if (!data || !argv[0])                                                       \
    return UNDEFINED;
#define LOAD_GET(cbinfo, type)                                                 \
  type *data;                                                                  \
  NODE_API_CALL(                                                               \
      napi_get_cb_info(env, cbinfo, NULL, NULL, NULL, (void **)&data));
#define UNDEFINED undefined(env)
#define EXPORT_FN(fn) NODE_SET_PROPERTY(exports, #fn, FUNCTION(fn));
#define LOOP_ARRAY(array)                                                      \
  uint32_t length;                                                             \
  NODE_API_CALL(napi_get_array_length(env, array, &length));                   \
  for (uint32_t i = 0; i < length; i++)
#define CHECK_ERR(op, msg)                                                     \
  if ((err = (op)) < 0) {                                                      \
    char buf[256];                                                             \
    av_strerror(err, buf, sizeof(buf));                                        \
    fprintf(stderr, "%s: %s\n", msg, buf);                                     \
    exit(1);                                                                   \
  }

#include <node_api.h>
#include <stdio.h>
#include <stdlib.h>

static inline napi_value undefined(napi_env env) {
  napi_value result;

  NODE_API_CALL(napi_get_undefined(env, &result));
  return result;
}
static inline napi_valuetype nodeTypeof(napi_env env, napi_value value) {
  napi_valuetype result;

  NODE_API_CALL_DEFAULT(napi_typeof(env, value, &result), napi_undefined);
  return result;
};
static inline bool isArray(napi_env env, napi_value value) {
  bool result;

  NODE_API_CALL_DEFAULT(napi_is_array(env, value, &result), 0);
  return result;
};
static inline bool isTypedArray(napi_env env, napi_value value) {
  if (nodeTypeof(env, value) != napi_object)
    return false;
  bool result;

  NODE_API_CALL_DEFAULT(napi_is_typedarray(env, value, &result), 0);
  return result;
};
static inline napi_value String(napi_env env, const char *str) {
  if (!str)
    return UNDEFINED;
  napi_value result;

  NODE_API_CALL(napi_create_string_utf8(env, str, NAPI_AUTO_LENGTH, &result));
  return result;
}
static inline napi_value Number(napi_env env, double value) {
  napi_value result;

  NODE_API_CALL(napi_create_double(env, value, &result));
  return result;
}
static inline napi_value BigInt64(napi_env env, int64_t value) {
  napi_value result;

  NODE_API_CALL(napi_create_bigint_int64(env, value, &result));
  return result;
}
static inline napi_value BigUInt64(napi_env env, uint64_t value) {
  napi_value result;

  NODE_API_CALL(napi_create_bigint_uint64(env, value, &result));
  return result;
}
static inline napi_value BigIntWords(napi_env env, int sign_bit,
                                     size_t word_count, const uint64_t *words) {
  napi_value result;

  NODE_API_CALL(
      napi_create_bigint_words(env, sign_bit, word_count, words, &result));
  return result;
}
static inline napi_value Object(napi_env env) {
  napi_value result;

  NODE_API_CALL(napi_create_object(env, &result));
  return result;
}
static inline napi_value createArrayWithLength(napi_env env, size_t length) {
  napi_value result;

  NODE_API_CALL(napi_create_array_with_length(env, length, &result));
  return result;
}
static inline napi_value createArray(napi_env env) {
  napi_value result;

  NODE_API_CALL(napi_create_array(env, &result));
  return result;
}
static inline napi_value Function(napi_env env, const char *name,
                                  napi_callback cb) {
  napi_value result;

  NODE_API_CALL(
      napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, NULL, &result));
  return result;
}
static inline char *parseString(napi_env env, napi_value value) {
  size_t length;

  if (nodeTypeof(env, value) != napi_string)
    return NULL;
  NODE_API_CALL_DEFAULT(
      napi_get_value_string_utf8(env, value, NULL, 0, &length), NULL);
  if (!length)
    return NULL;
  char *name = malloc(length + 1);
  if (!name) {
    napi_throw_error(env, NULL, "Failed to allocate memory for string");
    return NULL;
  }
  NODE_API_CALL_DEFAULT(
      napi_get_value_string_utf8(env, value, name, length + 1, NULL), NULL);
  return name;
}
static inline int parseInt(napi_env env, napi_value value, bool ignoreNaN,
                           int defaultValue) {
  int result;

  if (ignoreNaN && nodeTypeof(env, value) != napi_number)
    return defaultValue;
  NODE_API_CALL_DEFAULT(napi_get_value_int32(env, value, &result),
                        defaultValue);
  return result;
}
static inline int64_t parseInt64(napi_env env, napi_value value, bool ignoreNaN,
                                 int64_t defaultValue) {
  int64_t result;

  if (ignoreNaN && nodeTypeof(env, value) != napi_number)
    return defaultValue;
  NODE_API_CALL_DEFAULT(napi_get_value_int64(env, value, &result),
                        defaultValue);
  return result;
}
static inline double parseDouble(napi_env env, napi_value value, bool ignoreNaN,
                                 double defaultValue) {
  double result;

  if (ignoreNaN && nodeTypeof(env, value) != napi_number)
    return defaultValue;
  NODE_API_CALL_DEFAULT(napi_get_value_double(env, value, &result),
                        defaultValue);
  return result;
}
static inline bool parseBool(napi_env env, napi_value value,
                             bool defaultValue) {
  bool result;

  if (nodeTypeof(env, value) != napi_boolean)
    return defaultValue;
  NODE_API_CALL_DEFAULT(napi_get_value_bool(env, value, &result), defaultValue);
  return result;
}
#endif
