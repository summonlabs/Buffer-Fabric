#pragma once

// Buffer Fabric -- public linkage configuration.
//
// The library builds as a static archive by default. When built as a shared
// library (BUFFER_FABRIC_SHARED) the public surface is annotated with BF_API.

#if defined(_WIN32)
#  if defined(BUFFER_FABRIC_SHARED)
#    if defined(BUFFER_FABRIC_BUILDING_LIBRARY)
#      define BF_API __declspec(dllexport)
#    else
#      define BF_API __declspec(dllimport)
#    endif
#  else
#    define BF_API
#  endif
#else
#  if defined(BUFFER_FABRIC_SHARED) && defined(BUFFER_FABRIC_BUILDING_LIBRARY)
#    define BF_API __attribute__((visibility("default")))
#  elif defined(BUFFER_FABRIC_SHARED)
#    define BF_API __attribute__((visibility("default")))
#  else
#    define BF_API
#  endif
#endif

#define BF_UNUSED(x) (void)(x)
