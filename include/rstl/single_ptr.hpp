#ifndef _RSTL_SINGLE_PTR
#define _RSTL_SINGLE_PTR

#include "types.h"
#include "rstl/allocator.hpp"
#include "rstl/pointer_deleter.hpp"

namespace rstl {
template < typename T >
class single_ptr {
  typedef typename pointer_deleter< T >::element_type element_type;
  mutable element_type* x0_ptr;

public:
  single_ptr() : x0_ptr(nullptr) {}
  single_ptr(element_type* ptr) : x0_ptr(ptr) {}
  single_ptr(const single_ptr& other) : x0_ptr(other.x0_ptr) { other.x0_ptr = nullptr; }
  ~single_ptr() { pointer_deleter< T >::destroy(x0_ptr); }
  single_ptr& operator=(single_ptr& other) {
    if (&other == this) {
      return *this;
    }
    pointer_deleter< T >::destroy(x0_ptr);
    x0_ptr = other.x0_ptr;
    other.x0_ptr = nullptr;
    return *this;
  }

  single_ptr& operator=(element_type* const ptr) {
    if (x0_ptr == ptr) {
      return *this;
    }
    pointer_deleter< T >::destroy(x0_ptr);
    x0_ptr = ptr;
    return *this;
  }

  element_type* get() const { return x0_ptr; }
  // const T* get() const { return x0_ptr; }
  element_type* operator->() const { return x0_ptr; }
  element_type& operator*() { return *x0_ptr; }
  const element_type& operator*() const { return *x0_ptr; }

  bool null() const { return x0_ptr == nullptr; }
  element_type* release() {
    element_type* ptr = x0_ptr;
    x0_ptr = nullptr;
    return ptr;
  }

  // This is certainly not real, but handy to force not-inline
  single_ptr& reset(element_type* ptr);
};

template < typename T >
single_ptr< T >& single_ptr< T >::reset(element_type* ptr) {
  return *this = ptr;
}

typedef single_ptr< char > unk_singleptr;
CHECK_SIZEOF(unk_singleptr, 0x4);
} // namespace rstl

#endif // _RSTL_SINGLE_PTR
