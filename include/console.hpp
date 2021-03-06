///////
#ifndef AZE_CONSOLE_HPP
#define AZE_CONSOLE_HPP
#include <cstdio>
#include <string_view>
#include <absl/strings/str_format.h>
#ifdef _WIN32
#include "details/console_win.hpp"
#endif

namespace aze {
#ifdef _WIN32
template <typename... Args>
int FPrintF(std::FILE *out, const absl::FormatSpec<Args...> &format,
            const Args &... args) {
  auto msg = absl::str_format_internal::FormatPack(
      absl::str_format_internal::UntypedFormatSpecImpl::Extract(format),
      {absl::str_format_internal::FormatArgImpl(args)...});
  return internal::console_write(out, msg);
}
#else
template <typename... Args>
int FPrintF(std::FILE *out, const absl::FormatSpec<Args...> &format,
            const Args &... args) {
  return absl::str_format_internal::FprintF(
      out, absl::str_format_internal::UntypedFormatSpecImpl::Extract(format),
      {absl::str_format_internal::FormatArgImpl(args)...});
}
#endif
} // namespace aze

#endif
