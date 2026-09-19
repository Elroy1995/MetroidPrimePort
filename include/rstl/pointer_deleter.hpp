#ifndef _RSTL_POINTER_DELETER
#define _RSTL_POINTER_DELETER

namespace rstl {
// Keep scalar and array ownership distinct without changing the legacy
// transfer-on-copy semantics used by resource factories.
template < typename T > struct pointer_deleter {
  typedef T element_type;
  static void destroy(T* ptr) { delete ptr; }
};
template < typename T > struct pointer_deleter< T[] > {
  typedef T element_type;
  static void destroy(T* ptr) { delete[] ptr; }
};
} // namespace rstl

#endif
