// Copyright 2017 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "absl/strings/escaping.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>

#include "absl/base/internal/endian.h"
#include "absl/base/internal/raw_logging.h"
#include "absl/base/internal/unaligned_access.h"
#include "absl/strings/internal/char_map.h"
#include "absl/strings/internal/resize_uninitialized.h"
#include "absl/strings/internal/utf8.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"

namespace absl {
namespace {

// Digit conversion.
constexpr char kHexChar[] = "0123456789abcdef";

constexpr char kHexTable[513] =
    "000102030405060708090a0b0c0d0e0f"
    "101112131415161718191a1b1c1d1e1f"
    "202122232425262728292a2b2c2d2e2f"
    "303132333435363738393a3b3c3d3e3f"
    "404142434445464748494a4b4c4d4e4f"
    "505152535455565758595a5b5c5d5e5f"
    "606162636465666768696a6b6c6d6e6f"
    "707172737475767778797a7b7c7d7e7f"
    "808182838485868788898a8b8c8d8e8f"
    "909192939495969798999a9b9c9d9e9f"
    "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
    "b0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
    "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
    "d0d1d2d3d4d5d6d7d8d9dadbdcdddedf"
    "e0e1e2e3e4e5e6e7e8e9eaebecedeeef"
    "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff";

// These are used for the leave_nulls_escaped argument to CUnescapeInternal().
constexpr bool kUnescapeNulls = false;

inline bool is_octal_digit(char c) { return ('0' <= c) && (c <= '7'); }

inline int hex_digit_to_int(char c) {
  static_assert('0' == 0x30 && 'A' == 0x41 && 'a' == 0x61,
                "Character set must be ASCII.");
  assert(absl::ascii_isxdigit(c));
  int x = static_cast<unsigned char>(c);
  if (x > '9') {
    x += 9;
  }
  return x & 0xf;
}

inline bool IsSurrogate(char32_t c, absl::string_view src, std::string* error) {
  if (c >= 0xD800 && c <= 0xDFFF) {
    if (error) {
      *error = absl::StrCat("invalid surrogate character (0xD800-DFFF): \\",
                            src);
    }
    return true;
  }
  return false;
}

// ----------------------------------------------------------------------
// CUnescapeInternal()
//    Implements both CUnescape() and CUnescapeForNullTerminatedString().
//
//    Unescapes C escape sequences and is the reverse of CEscape().
//
//    If 'source' is valid, stores the unescaped string and its size in
//    'dest' and 'dest_len' respectively, and returns true. Otherwise
//    returns false and optionally stores the error description in
//    'error'. Set 'error' to nullptr to disable error reporting.
//
//    'dest' should point to a buffer that is at least as big as 'source'.
//    'source' and 'dest' may be the same.
//
//     NOTE: any changes to this function must also be reflected in the older
//     UnescapeCEscapeSequences().
// ----------------------------------------------------------------------
bool CUnescapeInternal(absl::string_view source, bool leave_nulls_escaped,
                       char* dest, ptrdiff_t* dest_len, std::string* error) {
  char* d = dest;
  const char* p = source.data();
  const char* end = p + source.size();
  const char* last_byte = end - 1;

  // Small optimization for case where source = dest and there's no escaping
  while (p == d && p < end && *p != '\\') p++, d++;

  while (p < end) {
    if (*p != '\\') {
      *d++ = *p++;
    } else {
      if (++p > last_byte) {  // skip past the '\\'
        if (error) *error = "String cannot end with \\";
        return false;
      }
      switch (*p) {
        case 'a':  *d++ = '\a';  break;
        case 'b':  *d++ = '\b';  break;
        case 'f':  *d++ = '\f';  break;
        case 'n':  *d++ = '\n';  break;
        case 'r':  *d++ = '\r';  break;
        case 't':  *d++ = '\t';  break;
        case 'v':  *d++ = '\v';  break;
        case '\\': *d++ = '\\';  break;
        case '?':  *d++ = '\?';  break;    // \?  Who knew?
        case '\'': *d++ = '\'';  break;
        case '"':  *d++ = '\"';  break;
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7': {
          // octal digit: 1 to 3 digits
          const char* octal_start = p;
          unsigned int ch = *p - '0';
          if (p < last_byte && is_octal_digit(p[1])) ch = ch * 8 + *++p - '0';
          if (p < last_byte && is_octal_digit(p[1]))
            ch = ch * 8 + *++p - '0';      // now points at last digit
          if (ch > 0xff) {
            if (error) {
              *error = "Value of \\" +
                       std::string(octal_start, p + 1 - octal_start) +
                       " exceeds 0xff";
            }
            return false;
          }
          if ((ch == 0) && leave_nulls_escaped) {
            // Copy the escape sequence for the null character
            const ptrdiff_t octal_size = p + 1 - octal_start;
            *d++ = '\\';
            memcpy(d, octal_start, octal_size);
            d += octal_size;
            break;
          }
          *d++ = ch;
          break;
        }
        case 'x':
        case 'X': {
          if (p >= last_byte) {
            if (error) *error = "String cannot end with \\x";
            return false;
          } else if (!absl::ascii_isxdigit(p[1])) {
            if (error) *error = "\\x cannot be followed by a non-hex digit";
            return false;
          }
          unsigned int ch = 0;
          const char* hex_start = p;
          while (p < last_byte && absl::ascii_isxdigit(p[1]))
            // Arbitrarily many hex digits
            ch = (ch << 4) + hex_digit_to_int(*++p);
          if (ch > 0xFF) {
            if (error) {
              *error = "Value of \\" +
                       std::string(hex_start, p + 1 - hex_start) +
                       " exceeds 0xff";
            }
            return false;
          }
          if ((ch == 0) && leave_nulls_escaped) {
            // Copy the escape sequence for the null character
            const ptrdiff_t hex_size = p + 1 - hex_start;
            *d++ = '\\';
            memcpy(d, hex_start, hex_size);
            d += hex_size;
            break;
          }
          *d++ = ch;
          break;
        }
        case 'u': {
          // \uhhhh => convert 4 hex digits to UTF-8
          char32_t rune = 0;
          const char* hex_start = p;
          if (p + 4 >= end) {
            if (error) {
              *error = "\\u must be followed by 4 hex digits: \\" +
                       std::string(hex_start, p + 1 - hex_start);
            }
            return false;
          }
          for (int i = 0; i < 4; ++i) {
            // Look one char ahead.
            if (absl::ascii_isxdigit(p[1])) {
              rune = (rune << 4) + hex_digit_to_int(*++p);  // Advance p.
            } else {
              if (error) {
                *error = "\\u must be followed by 4 hex digits: \\" +
                         std::string(hex_start, p + 1 - hex_start);
              }
              return false;
            }
          }
          if ((rune == 0) && leave_nulls_escaped) {
            // Copy the escape sequence for the null character
            *d++ = '\\';
            memcpy(d, hex_start, 5);  // u0000
            d += 5;
            break;
          }
          if (IsSurrogate(rune, absl::string_view(hex_start, 5), error)) {
            return false;
          }
          d += strings_internal::EncodeUTF8Char(d, rune);
          break;
        }
        case 'U': {
          // \Uhhhhhhhh => convert 8 hex digits to UTF-8
          char32_t rune = 0;
          const char* hex_start = p;
          if (p + 8 >= end) {
            if (error) {
              *error = "\\U must be followed by 8 hex digits: \\" +
                       std::string(hex_start, p + 1 - hex_start);
            }
            return false;
          }
          for (int i = 0; i < 8; ++i) {
            // Look one char ahead.
            if (absl::ascii_isxdigit(p[1])) {
              // Don't change rune until we're sure this
              // is within the Unicode limit, but do advance p.
              uint32_t newrune = (rune << 4) + hex_digit_to_int(*++p);
              if (newrune > 0x10FFFF) {
                if (error) {
                  *error = "Value of \\" +
                           std::string(hex_start, p + 1 - hex_start) +
                           " exceeds Unicode limit (0x10FFFF)";
                }
                return false;
              } else {
                rune = newrune;
              }
            } else {
              if (error) {
                *error = "\\U must be followed by 8 hex digits: \\" +
                         std::string(hex_start, p + 1 - hex_start);
              }
              return false;
            }
          }
          if ((rune == 0) && leave_nulls_escaped) {
            // Copy the escape sequence for the null character
            *d++ = '\\';
            memcpy(d, hex_start, 9);  // U00000000
            d += 9;
            break;
          }
          if (IsSurrogate(rune, absl::string_view(hex_start, 9), error)) {
            return false;
          }
          d += strings_internal::EncodeUTF8Char(d, rune);
          break;
        }
        default: {
          if (error) *error = std::string("Unknown escape sequence: \\") + *p;
          return false;
        }
      }
      p++;                                 // read past letter we escaped
    }
  }
  *dest_len = d - dest;
  return true;
}

// ----------------------------------------------------------------------
// CUnescapeInternal()
//
//    Same as above but uses a std::string for output. 'source' and 'dest'
//    may be the same.
// ----------------------------------------------------------------------
bool CUnescapeInternal(absl::string_view source, bool leave_nulls_escaped,
                       std::string* dest, std::string* error) {
  strings_internal::STLStringResizeUninitialized(dest, source.size());

  ptrdiff_t dest_size;
  if (!CUnescapeInternal(source,
                         leave_nulls_escaped,
                         &(*dest)[0],
                         &dest_size,
                         error)) {
    return false;
  }
  dest->erase(dest_size);
  return true;
}

// ----------------------------------------------------------------------
// CEscape()
// CHexEscape()
// Utf8SafeCEscape()
// Utf8SafeCHexEscape()
//    Escapes 'src' using C-style escape sequences.  This is useful for
//    preparing query flags.  The 'Hex' version uses hexadecimal rather than
//    octal sequences.  The 'Utf8Safe' version does not touch UTF-8 bytes.
//
//    Escaped chars: \n, \r, \t, ", ', \, and !absl::ascii_isprint().
// ----------------------------------------------------------------------
std::string CEscapeInternal(absl::string_view src, bool use_hex,
                            bool utf8_safe) {
  std::string dest;
  bool last_hex_escape = false;  // true if last output char was \xNN.

  for (unsigned char c : src) {
    bool is_hex_escape = false;
    switch (c) {
      case '\n': dest.append("\\" "n"); break;
      case '\r': dest.append("\\" "r"); break;
      case '\t': dest.append("\\" "t"); break;
      case '\"': dest.append("\\" "\""); break;
      case '\'': dest.append("\\" "'"); break;
      case '\\': dest.append("\\" "\\"); break;
      default:
        // Note that if we emit \xNN and the src character after that is a hex
        // digit then that digit must be escaped too to prevent it being
        // interpreted as part of the character code by C.
        if ((!utf8_safe || c < 0x80) &&
            (!absl::ascii_isprint(c) ||
             (last_hex_escape && absl::ascii_isxdigit(c)))) {
          if (use_hex) {
            dest.append("\\" "x");
            dest.push_back(kHexChar[c / 16]);
            dest.push_back(kHexChar[c % 16]);
            is_hex_escape = true;
          } else {
            dest.append("\\");
            dest.push_back(kHexChar[c / 64]);
            dest.push_back(kHexChar[(c % 64) / 8]);
            dest.push_back(kHexChar[c % 8]);
          }
        } else {
          dest.push_back(c);
          break;
        }
    }
    last_hex_escape = is_hex_escape;
  }

  return dest;
}

/* clang-format off */
constexpr char c_escaped_len[256] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 2, 2, 4, 4, 2, 4, 4,  // \t, \n, \r
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1,  // ", '
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // '0'..'9'
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 'A'..'O'
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1,  // 'P'..'Z', '\'
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 'a'..'o'
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4,  // 'p'..'z', DEL
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
};
/* clang-format on */

// Calculates the length of the C-style escaped version of 'src'.
// Assumes that non-printable characters are escaped using octal sequences, and
// that UTF-8 bytes are not handled specially.
inline size_t CEscapedLength(absl::string_view src) {
  size_t escaped_len = 0;
  for (unsigned char c : src) escaped_len += c_escaped_len[c];
  return escaped_len;
}

void CEscapeAndAppendInternal(absl::string_view src, std::string* dest) {
  size_t escaped_len = CEscapedLength(src);
  if (escaped_len == src.size()) {
    dest->append(src.data(), src.size());
    return;
  }

  size_t cur_dest_len = dest->size();
  strings_internal::STLStringResizeUninitialized(dest,
                                                 cur_dest_len + escaped_len);
  char* append_ptr = &(*dest)[cur_dest_len];

  for (unsigned char c : src) {
    int char_len = c_escaped_len[c];
    if (char_len == 1) {
      *append_ptr++ = c;
    } else if (char_len == 2) {
      switch (c) {
        case '\n':
          *append_ptr++ = '\\';
          *append_ptr++ = 'n';
          break;
        case '\r':
          *append_ptr++ = '\\';
          *append_ptr++ = 'r';
          break;
        case '\t':
          *append_ptr++ = '\\';
          *append_ptr++ = 't';
          break;
        case '\"':
          *append_ptr++ = '\\';
          *append_ptr++ = '\"';
          break;
        case '\'':
          *append_ptr++ = '\\';
          *append_ptr++ = '\'';
          break;
        case '\\':
          *append_ptr++ = '\\';
          *append_ptr++ = '\\';
          break;
      }
    } else {
      *append_ptr++ = '\\';
      *append_ptr++ = '0' + c / 64;
      *append_ptr++ = '0' + (c % 64) / 8;
      *append_ptr++ = '0' + c % 8;
    }
  }
}

bool Base64UnescapeInternal(const char* src_param, size_t szsrc, char* dest,
                            size_t szdest, const signed char* unbase64,
                            size_t* len) {
  static const char kPad64Equals = '=';
  static const char kPad64Dot = '.';

  size_t destidx = 0;
  int decode = 0;
  int state = 0;
  unsigned int ch = 0;
  unsigned int temp = 0;

  // If "char" is signed by default, using *src as an array index results in
  // accessing negative array elements. Treat the input as a pointer to
  // unsigned char to avoid this.
  const unsigned char* src = reinterpret_cast<const unsigned char*>(src_param);

  // The GET_INPUT macro gets the next input character, skipping
  // over any whitespace, and stopping when we reach the end of the
  // std::string or when we read any non-data character.  The arguments are
  // an arbitrary identifier (used as a label for goto) and the number
  // of data bytes that must remain in the input to avoid aborting the
  // loop.
#define GET_INPUT(label, remain)                                \
  label:                                                        \
  --szsrc;                                                      \
  ch = *src++;                                                  \
  decode = unbase64[ch];                                        \
  if (decode < 0) {                                             \
    if (absl::ascii_isspace(ch) && szsrc >= remain) goto label; \
    state = 4 - remain;                                         \
    break;                                                      \
  }

  // if dest is null, we're just checking to see if it's legal input
  // rather than producing output.  (I suspect this could just be done
  // with a regexp...).  We duplicate the loop so this test can be
  // outside it instead of in every iteration.

  if (dest) {
    // This loop consumes 4 input bytes and produces 3 output bytes
    // per iteration.  We can't know at the start that there is enough
    // data left in the std::string for a full iteration, so the loop may
    // break out in the middle; if so 'state' will be set to the
    // number of input bytes read.

    while (szsrc >= 4) {
      // We'll start by optimistically assuming that the next four
      // bytes of the std::string (src[0..3]) are four good data bytes
      // (that is, no nulls, whitespace, padding chars, or illegal
      // chars).  We need to test src[0..2] for nulls individually
      // before constructing temp to preserve the property that we
      // never read past a null in the std::string (no matter how long
      // szsrc claims the std::string is).

      if (!src[0] || !src[1] || !src[2] ||
          ((temp = ((unsigned(unbase64[src[0]]) << 18) |
                    (unsigned(unbase64[src[1]]) << 12) |
                    (unsigned(unbase64[src[2]]) << 6) |
                    (unsigned(unbase64[src[3]])))) &
           0x80000000)) {
        // Iff any of those four characters was bad (null, illegal,
        // whitespace, padding), then temp's high bit will be set
        // (because unbase64[] is -1 for all bad characters).
        //
        // We'll back up and resort to the slower decoder, which knows
        // how to handle those cases.

        GET_INPUT(first, 4);
        temp = decode;
        GET_INPUT(second, 3);
        temp = (temp << 6) | decode;
        GET_INPUT(third, 2);
        temp = (temp << 6) | decode;
        GET_INPUT(fourth, 1);
        temp = (temp << 6) | decode;
      } else {
        // We really did have four good data bytes, so advance four
        // characters in the std::string.

        szsrc -= 4;
        src += 4;
      }

      // temp has 24 bits of input, so write that out as three bytes.

      if (destidx + 3 > szdest) return false;
      dest[destidx + 2] = temp;
      temp >>= 8;
      dest[destidx + 1] = temp;
      temp >>= 8;
      dest[destidx] = temp;
      destidx += 3;
    }
  } else {
    while (szsrc >= 4) {
      if (!src[0] || !src[1] || !src[2] ||
          ((temp = ((unsigned(unbase64[src[0]]) << 18) |
                    (unsigned(unbase64[src[1]]) << 12) |
                    (unsigned(unbase64[src[2]]) << 6) |
                    (unsigned(unbase64[src[3]])))) &
           0x80000000)) {
        GET_INPUT(first_no_dest, 4);
        GET_INPUT(second_no_dest, 3);
        GET_INPUT(third_no_dest, 2);
        GET_INPUT(fourth_no_dest, 1);
      } else {
        szsrc -= 4;
        src += 4;
      }
      destidx += 3;
    }
  }

#undef GET_INPUT

  // if the loop terminated because we read a bad character, return
  // now.
  if (decode < 0 && ch != kPad64Equals && ch != kPad64Dot &&
      !absl::ascii_isspace(ch))
    return false;

  if (ch == kPad64Equals || ch == kPad64Dot) {
    // if we stopped by hitting an '=' or '.', un-read that character -- we'll
    // look at it again when we count to check for the proper number of
    // equals signs at the end.
    ++szsrc;
    --src;
  } else {
    // This loop consumes 1 input byte per iteration.  It's used to
    // clean up the 0-3 input bytes remaining when the first, faster
    // loop finishes.  'temp' contains the data from 'state' input
    // characters read by the first loop.
    while (szsrc > 0) {
      --szsrc;
      ch = *src++;
      decode = unbase64[ch];
      if (decode < 0) {
        if (absl::ascii_isspace(ch)) {
          continue;
        } else if (ch == kPad64Equals || ch == kPad64Dot) {
          // back up one character; we'll read it again when we check
          // for the correct number of pad characters at the end.
          ++szsrc;
          --src;
          break;
        } else {
          return false;
        }
      }

      // Each input character gives us six bits of output.
      temp = (temp << 6) | decode;
      ++state;
      if (state == 4) {
        // If we've accumulated 24 bits of output, write that out as
        // three bytes.
        if (dest) {
          if (destidx + 3 > szdest) return false;
          dest[destidx + 2] = temp;
          temp >>= 8;
          dest[destidx + 1] = temp;
          temp >>= 8;
          dest[destidx] = temp;
        }
        destidx += 3;
        state = 0;
        temp = 0;
      }
    }
  }

  // Process the leftover data contained in 'temp' at the end of the input.
  int expected_equals = 0;
  switch (state) {
    case 0:
      // Nothing left over; output is a multiple of 3 bytes.
      break;

    case 1:
      // Bad input; we have 6 bits left over.
      return false;

    case 2:
      // Produce one more output byte from the 12 input bits we have left.
      if (dest) {
        if (destidx + 1 > szdest) return false;
        temp >>= 4;
        dest[destidx] = temp;
      }
      ++destidx;
      expected_equals = 2;
      break;

    case 3:
      // Produce two more output bytes from the 18 input bits we have left.
      if (dest) {
        if (destidx + 2 > szdest) return false;
        temp >>= 2;
        dest[destidx + 1] = temp;
        temp >>= 8;
        dest[destidx] = temp;
      }
      destidx += 2;
      expected_equals = 1;
      break;

    default:
      // state should have no other values at this point.
      ABSL_RAW_LOG(FATAL, "This can't happen; base64 decoder state = %d",
                   state);
  }

  // The remainder of the std::string should be all whitespace, mixed with
  // exactly 0 equals signs, or exactly 'expected_equals' equals
  // signs.  (Always accepting 0 equals signs is an Abseil extension
  // not covered in the RFC, as is accepting dot as the pad character.)

  int equals = 0;
  while (szsrc > 0) {
    if (*src == kPad64Equals || *src == kPad64Dot)
      ++equals;
    else if (!absl::ascii_isspace(*src))
      return false;
    --szsrc;
    ++src;
  }

  const bool ok = (equals == 0 || equals == expected_equals);
  if (ok) *len = destidx;
  return ok;
}

// The arrays below were generated by the following code
// #include <sys/time.h>
// #include <stdlib.h>
// #include <string.h>
// main()
// {
//   static const char Base64[] =
//     "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
//   char* pos;
//   int idx, i, j;
//   printf("    ");
//   for (i = 0; i < 255; i += 8) {
//     for (j = i; j < i + 8; j++) {
//       pos = strchr(Base64, j);
//       if ((pos == nullptr) || (j == 0))
//         idx = -1;
//       else
//         idx = pos - Base64;
//       if (idx == -1)
//         printf(" %2d,     ", idx);
//       else
//         printf(" %2d/*%c*/,", idx, j);
//     }
//     printf("\n    ");
//   }
// }
//
// where the value of "Base64[]" was replaced by one of the base-64 conversion
// tables from the functions below.
/* clang-format off */
constexpr signed char kUnBase64[] = {
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      62/*+*/, -1,      -1,      -1,      63/*/ */,
    52/*0*/, 53/*1*/, 54/*2*/, 55/*3*/, 56/*4*/, 57/*5*/, 58/*6*/, 59/*7*/,
    60/*8*/, 61/*9*/, -1,      -1,      -1,      -1,      -1,      -1,
    -1,       0/*A*/,  1/*B*/,  2/*C*/,  3/*D*/,  4/*E*/,  5/*F*/,  6/*G*/,
    07/*H*/,  8/*I*/,  9/*J*/, 10/*K*/, 11/*L*/, 12/*M*/, 13/*N*/, 14/*O*/,
    15/*P*/, 16/*Q*/, 17/*R*/, 18/*S*/, 19/*T*/, 20/*U*/, 21/*V*/, 22/*W*/,
    23/*X*/, 24/*Y*/, 25/*Z*/, -1,      -1,      -1,      -1,      -1,
    -1,      26/*a*/, 27/*b*/, 28/*c*/, 29/*d*/, 30/*e*/, 31/*f*/, 32/*g*/,
    33/*h*/, 34/*i*/, 35/*j*/, 36/*k*/, 37/*l*/, 38/*m*/, 39/*n*/, 40/*o*/,
    41/*p*/, 42/*q*/, 43/*r*/, 44/*s*/, 45/*t*/, 46/*u*/, 47/*v*/, 48/*w*/,
    49/*x*/, 50/*y*/, 51/*z*/, -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1
};

constexpr signed char kUnWebSafeBase64[] = {
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      62/*-*/, -1,      -1,
    52/*0*/, 53/*1*/, 54/*2*/, 55/*3*/, 56/*4*/, 57/*5*/, 58/*6*/, 59/*7*/,
    60/*8*/, 61/*9*/, -1,      -1,      -1,      -1,      -1,      -1,
    -1,       0/*A*/,  1/*B*/,  2/*C*/,  3/*D*/,  4/*E*/,  5/*F*/,  6/*G*/,
    07/*H*/,  8/*I*/,  9/*J*/, 10/*K*/, 11/*L*/, 12/*M*/, 13/*N*/, 14/*O*/,
    15/*P*/, 16/*Q*/, 17/*R*/, 18/*S*/, 19/*T*/, 20/*U*/, 21/*V*/, 22/*W*/,
    23/*X*/, 24/*Y*/, 25/*Z*/, -1,      -1,      -1,      -1,      63/*_*/,
    -1,      26/*a*/, 27/*b*/, 28/*c*/, 29/*d*/, 30/*e*/, 31/*f*/, 32/*g*/,
    33/*h*/, 34/*i*/, 35/*j*/, 36/*k*/, 37/*l*/, 38/*m*/, 39/*n*/, 40/*o*/,
    41/*p*/, 42/*q*/, 43/*r*/, 44/*s*/, 45/*t*/, 46/*u*/, 47/*v*/, 48/*w*/,
    49/*x*/, 50/*y*/, 51/*z*/, -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
    -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1
};
/* clang-format on */

size_t CalculateBase64EscapedLenInternal(size_t input_len, bool do_padding) {
  // Base64 encodes three bytes of input at a time. If the input is not
  // divisible by three, we pad as appropriate.
  //
  // (from https://tools.ietf.org/html/rfc3548)
  // Special processing is performed if fewer than 24 bits are available
  // at the end of the data being encoded.  A full encoding quantum is
  // always completed at the end of a quantity.  When fewer than 24 input
  // bits are available in an input group, zero bits are added (on the
  // right) to form an integral number of 6-bit groups.  Padding at the
  // end of the data is performed using the '=' character.  Since all base
  // 64 input is an integral number of octets, only the following cases
  // can arise:

  // Base64 encodes each three bytes of input into four bytes of output.
  size_t len = (input_len / 3) * 4;

  if (input_len % 3 == 0) {
    // (from https://tools.ietf.org/html/rfc3548)
    // (1) the final quantum of encoding input is an integral multiple of 24
    // bits; here, the final unit of encoded output will be an integral
    // multiple of 4 characters with no "=" padding,
  } else if (input_len % 3 == 1) {
    // (from https://tools.ietf.org/html/rfc3548)
    // (2) the final quantum of encoding input is exactly 8 bits; here, the
    // final unit of encoded output will be two characters followed by two
    // "=" padding characters, or
    len += 2;
    if (do_padding) {
      len += 2;
    }
  } else {  // (input_len % 3 == 2)
    // (from https://tools.ietf.org/html/rfc3548)
    // (3) the final quantum of encoding input is exactly 16 bits; here, the
    // final unit of encoded output will be three characters followed by one
    // "=" padding character.
    len += 3;
    if (do_padding) {
      len += 1;
    }
  }

  assert(len >= input_len);  // make sure we didn't overflow
  return len;
}

size_t Base64EscapeInternal(const unsigned char* src, size_t szsrc, char* dest,
                            size_t szdest, const char* base64,
                            bool do_padding) {
  static const char kPad64 = '=';

  if (szsrc * 4 > szdest * 3) return 0;

  char* cur_dest = dest;
  const unsigned char* cur_src = src;

  char* const limit_dest = dest + szdest;
  const unsigned char* const limit_src = src + szsrc;

  // Three bytes of data encodes to four characters of cyphertext.
  // So we can pump through three-byte chunks atomically.
  if (szsrc >= 3) {  // "limit_src - 3" is UB if szsrc < 3.
    while (cur_src < limit_src - 3) {  // While we have >= 32 bits.
      uint32_t in = absl::big_endian::Load32(cur_src) >> 8;

      cur_dest[0] = base64[in >> 18];
      in &= 0x3FFFF;
      cur_dest[1] = base64[in >> 12];
      in &= 0xFFF;
      cur_dest[2] = base64[in >> 6];
      in &= 0x3F;
      cur_dest[3] = base64[in];

      cur_dest += 4;
      cur_src += 3;
    }
  }
  // To save time, we didn't update szdest or szsrc in the loop.  So do it now.
  szdest = limit_dest - cur_dest;
  szsrc = limit_src - cur_src;

  /* now deal with the tail (<=3 bytes) */
  switch (szsrc) {
    case 0:
      // Nothing left; nothing more to do.
      break;
    case 1: {
      // One byte left: this encodes to two characters, and (optionally)
      // two pad characters to round out the four-character cypherblock.
      if (szdest < 2) return 0;
      uint32_t in = cur_src[0];
      cur_dest[0] = base64[in >> 2];
      in &= 0x3;
      cur_dest[1] = base64[in << 4];
      cur_dest += 2;
      szdest -= 2;
      if (do_padding) {
        if (szdest < 2) return 0;
        cur_dest[0] = kPad64;
        cur_dest[1] = kPad64;
        cur_dest += 2;
        szdest -= 2;
      }
      break;
    }
    case 2: {
      // Two bytes left: this encodes to three characters, and (optionally)
      // one pad character to round out the four-character cypherblock.
      if (szdest < 3) return 0;
      uint32_t in = absl::big_endian::Load16(cur_src);
      cur_dest[0] = base64[in >> 10];
      in &= 0x3FF;
      cur_dest[1] = base64[in >> 4];
      in &= 0x00F;
      cur_dest[2] = base64[in << 2];
      cur_dest += 3;
      szdest -= 3;
      if (do_padding) {
        if (szdest < 1) return 0;
        cur_dest[0] = kPad64;
        cur_dest += 1;
        szdest -= 1;
      }
      break;
    }
    case 3: {
      // Three bytes left: same as in the big loop above.  We can't do this in
      // the loop because the loop above always reads 4 bytes, and the fourth
      // byte is past the end of the input.
      if (szdest < 4) return 0;
      uint32_t in = (cur_src[0] << 16) + absl::big_endian::Load16(cur_src + 1);
      cur_dest[0] = base64[in >> 18];
      in &= 0x3FFFF;
      cur_dest[1] = base64[in >> 12];
      in &= 0xFFF;
      cur_dest[2] = base64[in >> 6];
      in &= 0x3F;
      cur_dest[3] = base64[in];
      cur_dest += 4;
      szdest -= 4;
      break;
    }
    default:
      // Should not be reached: blocks of 4 bytes are handled
      // in the while loop before this switch statement.
      ABSL_RAW_LOG(FATAL, "Logic problem? szsrc = %zu", szsrc);
      break;
  }
  return (cur_dest - dest);
}

constexpr char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr char kWebSafeBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

template <typename String>
void Base64EscapeInternal(const unsigned char* src, size_t szsrc, String* dest,
                          bool do_padding, const char* base64_chars) {
  const size_t calc_escaped_size =
      CalculateBase64EscapedLenInternal(szsrc, do_padding);
  strings_internal::STLStringResizeUninitialized(dest, calc_escaped_size);

  const size_t escaped_len = Base64EscapeInternal(
      src, szsrc, &(*dest)[0], dest->size(), base64_chars, do_padding);
  assert(calc_escaped_size == escaped_len);
  dest->erase(escaped_len);
}

template <typename String>
bool Base64UnescapeInternal(const char* src, size_t slen, String* dest,
                            const signed char* unbase64) {
  // Determine the size of the output std::string.  Base64 encodes every 3 bytes into
  // 4 characters.  any leftover chars are added directly for good measure.
  // This is documented in the base64 RFC: http://tools.ietf.org/html/rfc3548
  const size_t dest_len = 3 * (slen / 4) + (slen % 4);

  strings_internal::STLStringResizeUninitialized(dest, dest_len);

  // We are getting the destination buffer by getting the beginning of the
  // std::string and converting it into a char *.
  size_t len;
  const bool ok =
      Base64UnescapeInternal(src, slen, &(*dest)[0], dest_len, unbase64, &len);
  if (!ok) {
    dest->clear();
    return false;
  }

  // could be shorter if there was padding
  assert(len <= dest_len);
  dest->erase(len);

  return true;
}

/* clang-format off */
constexpr char kHexValue[256] = {
    0,  0,  0,  0,  0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,  0,  0,  0,  0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,  0,  0,  0,  0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,  1,  2,  3,  4,  5,  6, 7, 8, 9, 0, 0, 0, 0, 0, 0,  // '0'..'9'
    0, 10, 11, 12, 13, 14, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 'A'..'F'
    0,  0,  0,  0,  0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 10, 11, 12, 13, 14, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 'a'..'f'
    0,  0,  0,  0,  0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,  0,  0,  0,  0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,  0,  0,  0,  0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,  0,  0,  0,  0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,  0,  0,  0,  0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,  0,  0,  0,  0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,  0,  0,  0,  0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,  0,  0,  0,  0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,  0,  0,  0,  0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
/* clang-format on */

// This is a templated function so that T can be either a char*
// or a string.  This works because we use the [] operator to access
// individual characters at a time.
template <typename T>
void HexStringToBytesInternal(const char* from, T to, ptrdiff_t num) {
  for (int i = 0; i < num; i++) {
    to[i] = (kHexValue[from[i * 2] & 0xFF] << 4) +
            (kHexValue[from[i * 2 + 1] & 0xFF]);
  }
}

// This is a templated function so that T can be either a char* or a
// std::string.
template <typename T>
void BytesToHexStringInternal(const unsigned char* src, T dest, ptrdiff_t num) {
  auto dest_ptr = &dest[0];
  for (auto src_ptr = src; src_ptr != (src + num); ++src_ptr, dest_ptr += 2) {
    const char* hex_p = &kHexTable[*src_ptr * 2];
    std::copy(hex_p, hex_p + 2, dest_ptr);
  }
}

}  // namespace

// ----------------------------------------------------------------------
// CUnescape()
//
// See CUnescapeInternal() for implementation details.
// ----------------------------------------------------------------------
bool CUnescape(absl::string_view source, std::string* dest,
               std::string* error) {
  return CUnescapeInternal(source, kUnescapeNulls, dest, error);
}

std::string CEscape(absl::string_view src) {
  std::string dest;
  CEscapeAndAppendInternal(src, &dest);
  return dest;
}

std::string CHexEscape(absl::string_view src) {
  return CEscapeInternal(src, true, false);
}

std::string Utf8SafeCEscape(absl::string_view src) {
  return CEscapeInternal(src, false, true);
}

std::string Utf8SafeCHexEscape(absl::string_view src) {
  return CEscapeInternal(src, true, true);
}

// ----------------------------------------------------------------------
// ptrdiff_t Base64Unescape() - base64 decoder
// ptrdiff_t Base64Escape() - base64 encoder
// ptrdiff_t WebSafeBase64Unescape() - Google's variation of base64 decoder
// ptrdiff_t WebSafeBase64Escape() - Google's variation of base64 encoder
//
// Check out
// http://tools.ietf.org/html/rfc2045 for formal description, but what we
// care about is that...
//   Take the encoded stuff in groups of 4 characters and turn each
//   character into a code 0 to 63 thus:
//           A-Z map to 0 to 25
//           a-z map to 26 to 51
//           0-9 map to 52 to 61
//           +(- for WebSafe) maps to 62
//           /(_ for WebSafe) maps to 63
//   There will be four numbers, all less than 64 which can be represented
//   by a 6 digit binary number (aaaaaa, bbbbbb, cccccc, dddddd respectively).
//   Arrange the 6 digit binary numbers into three bytes as such:
//   aaaaaabb bbbbcccc ccdddddd
//   Equals signs (one or two) are used at the end of the encoded block to
//   indicate that the text was not an integer multiple of three bytes long.
// ----------------------------------------------------------------------

bool Base64Unescape(absl::string_view src, std::string* dest) {
  return Base64UnescapeInternal(src.data(), src.size(), dest, kUnBase64);
}

bool WebSafeBase64Unescape(absl::string_view src, std::string* dest) {
  return Base64UnescapeInternal(src.data(), src.size(), dest, kUnWebSafeBase64);
}

void Base64Escape(absl::string_view src, std::string* dest) {
  Base64EscapeInternal(reinterpret_cast<const unsigned char*>(src.data()),
                       src.size(), dest, true, kBase64Chars);
}

void WebSafeBase64Escape(absl::string_view src, std::string* dest) {
  Base64EscapeInternal(reinterpret_cast<const unsigned char*>(src.data()),
                       src.size(), dest, false, kWebSafeBase64Chars);
}

std::string HexStringToBytes(absl::string_view from) {
  std::string result;
  const auto num = from.size() / 2;
  strings_internal::STLStringResizeUninitialized(&result, num);
  absl::HexStringToBytesInternal<std::string&>(from.data(), result, num);
  return result;
}

std::string BytesToHexString(absl::string_view from) {
  std::string result;
  strings_internal::STLStringResizeUninitialized(&result, 2 * from.size());
  absl::BytesToHexStringInternal<std::string&>(
      reinterpret_cast<const unsigned char*>(from.data()), result, from.size());
  return result;
}

}  // namespace absl
