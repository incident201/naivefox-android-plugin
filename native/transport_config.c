/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "transport_config.h"

#include <stdlib.h>
#include <string.h>

/* A bounded JSON syntax walker, not a proxy-config parser. It locates a single
 * top-level field without reserializing credentials, numbers, or unknown fields.
 * UTF-8/surrogate and application-level validation remain NaiveFox's job.
 */
typedef struct Scanner {
  const char* cursor;
  const char* insert;
  const char* transport_start;
  const char* transport_end;
  bool has_members;
} Scanner;

static void Whitespace(Scanner* scanner) {
  while (*scanner->cursor == ' ' || *scanner->cursor == '\t' ||
         *scanner->cursor == '\r' || *scanner->cursor == '\n') {
    ++scanner->cursor;
  }
}

static int Hex(char character) {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  if (character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

static bool String(Scanner* scanner) {
  if (*scanner->cursor != '"') return false;
  ++scanner->cursor;
  while (*scanner->cursor != '"') {
    unsigned char character = (unsigned char)*scanner->cursor;
    if (character < 0x20U) return false;
    ++scanner->cursor;
    if (character != '\\') continue;
    character = (unsigned char)*scanner->cursor;
    if (character == 'u') {
      ++scanner->cursor;
      for (unsigned index = 0; index < 4; ++index) {
        if (Hex(*scanner->cursor) < 0) return false;
        ++scanner->cursor;
      }
    } else {
      if (character == 0U || !strchr("\"\\/bfnrt", (int)character)) return false;
      ++scanner->cursor;
    }
  }
  ++scanner->cursor;
  return true;
}

/* Compare a syntactically valid JSON string to an ASCII field/value, including
 * escaped spellings such as "\u0074ransport". No unescaping of the input itself.
 */
static bool StringEquals(const char* start, const char* end, const char* text) {
  if (end - start < 2 || *start != '"') return false;
  const char* cursor = start + 1;
  while (cursor < end - 1) {
    unsigned character = (unsigned char)*cursor++;
    if (character == '\\') {
      character = (unsigned char)*cursor++;
      if (character == 'u') {
        character = 0;
        for (unsigned index = 0; index < 4; ++index) {
          character = character * 16U + (unsigned)Hex(*cursor++);
        }
      } else if (character != '"' && character != '\\' && character != '/') {
        return false; /* Control escapes cannot occur in our ASCII names. */
      }
    }
    if (!*text || character != (unsigned char)*text++) return false;
  }
  return *text == '\0';
}

static bool Value(Scanner* scanner, unsigned depth);

static bool Object(Scanner* scanner, unsigned depth) {
  if (depth >= 128U || *scanner->cursor != '{') return false;
  ++scanner->cursor;
  if (depth == 0U) scanner->insert = scanner->cursor;
  Whitespace(scanner);
  if (*scanner->cursor == '}') {
    ++scanner->cursor;
    return true;
  }
  if (depth == 0U) scanner->has_members = true;
  for (;;) {
    const char* key_start = scanner->cursor;
    if (!String(scanner)) return false;
    const char* key_end = scanner->cursor;
    Whitespace(scanner);
    if (*scanner->cursor != ':') return false;
    ++scanner->cursor;
    Whitespace(scanner);
    const char* value_start = scanner->cursor;
    if (!Value(scanner, depth + 1U)) return false;
    if (depth == 0U && StringEquals(key_start, key_end, "transport")) {
      if (scanner->transport_start ||
          (!StringEquals(value_start, scanner->cursor, "classic") &&
           !StringEquals(value_start, scanner->cursor, "no-connect"))) {
        return false;
      }
      scanner->transport_start = value_start;
      scanner->transport_end = scanner->cursor;
    }
    Whitespace(scanner);
    if (*scanner->cursor == '}') {
      ++scanner->cursor;
      return true;
    }
    if (*scanner->cursor != ',') return false;
    ++scanner->cursor;
    Whitespace(scanner);
  }
}

static bool Array(Scanner* scanner, unsigned depth) {
  if (depth >= 128U) return false;
  ++scanner->cursor;
  Whitespace(scanner);
  if (*scanner->cursor == ']') {
    ++scanner->cursor;
    return true;
  }
  for (;;) {
    if (!Value(scanner, depth + 1U)) return false;
    Whitespace(scanner);
    if (*scanner->cursor == ']') {
      ++scanner->cursor;
      return true;
    }
    if (*scanner->cursor != ',') return false;
    ++scanner->cursor;
    Whitespace(scanner);
  }
}

static bool Digit(char character) {
  return character >= '0' && character <= '9';
}

static bool Number(Scanner* scanner) {
  if (*scanner->cursor == '-') ++scanner->cursor;
  if (*scanner->cursor == '0') {
    ++scanner->cursor;
  } else {
    if (*scanner->cursor < '1' || *scanner->cursor > '9') return false;
    do { ++scanner->cursor; } while (Digit(*scanner->cursor));
  }
  if (*scanner->cursor == '.') {
    ++scanner->cursor;
    if (!Digit(*scanner->cursor)) return false;
    do { ++scanner->cursor; } while (Digit(*scanner->cursor));
  }
  if (*scanner->cursor == 'e' || *scanner->cursor == 'E') {
    ++scanner->cursor;
    if (*scanner->cursor == '+' || *scanner->cursor == '-') ++scanner->cursor;
    if (!Digit(*scanner->cursor)) return false;
    do { ++scanner->cursor; } while (Digit(*scanner->cursor));
  }
  return true;
}

static bool Value(Scanner* scanner, unsigned depth) {
  switch (*scanner->cursor) {
    case '{': return Object(scanner, depth);
    case '[': return Array(scanner, depth);
    case '"': return String(scanner);
    default: break;
  }
  const char* literals[] = {"true", "false", "null"};
  for (unsigned index = 0; index < 3U; ++index) {
    size_t length = strlen(literals[index]);
    if (strncmp(scanner->cursor, literals[index], length) == 0) {
      scanner->cursor += length;
      return true;
    }
  }
  return Number(scanner);
}

bool SelectTransport(const char* config, const char* transport, char** result) {
  if (!result) return false;
  *result = NULL;
  if (!config || !transport ||
      (strcmp(transport, "classic") != 0 && strcmp(transport, "no-connect") != 0)) {
    return false;
  }
  size_t length = strlen(config);
  if (length > NAIVEFOX_CONFIG_MAXIMUM_BYTES) return false;
  Scanner scanner = {.cursor = config};
  Whitespace(&scanner);
  if (!Object(&scanner, 0)) return false;
  Whitespace(&scanner);
  if (*scanner.cursor != '\0') return false;

  const char* start = scanner.transport_start ? scanner.transport_start : scanner.insert;
  const char* end = scanner.transport_end ? scanner.transport_end : scanner.insert;
  const char* prefix = scanner.transport_start ? "\"" : "\"transport\":\"";
  const char* suffix = !scanner.transport_start && scanner.has_members ? "\"," : "\"";
  size_t inserted = strlen(prefix) + strlen(transport) + strlen(suffix);
  size_t output_length = length - (size_t)(end - start) + inserted;
  if (output_length > NAIVEFOX_CONFIG_MAXIMUM_BYTES) return false;
  char* output = malloc(output_length + 1U);
  if (!output) return false;
  size_t offset = (size_t)(start - config);
  memcpy(output, config, offset);
  memcpy(output + offset, prefix, strlen(prefix));
  offset += strlen(prefix);
  memcpy(output + offset, transport, strlen(transport));
  offset += strlen(transport);
  memcpy(output + offset, suffix, strlen(suffix));
  offset += strlen(suffix);
  memcpy(output + offset, end, length - (size_t)(end - config) + 1U);
  *result = output;
  return true;
}
