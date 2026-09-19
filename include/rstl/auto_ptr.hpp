#ifndef _RSTL_AUTO_PTR
#define _RSTL_AUTO_PTR

#include "types.h"
#include "rstl/pointer_deleter.hpp"

namespace rstl {
template < typename T >
class auto_ptr {
  typedef typename pointer_deleter< T >::element_type element_type;
  mutable bool x0_has;
  element_type* x4_item;

public:
  auto_ptr() : x0_has(false), x4_item(nullptr) {}
  auto_ptr(element_type* ptr) : x0_has(ptr != nullptr), x4_item(ptr) {}
  ~auto_ptr() {
    if (x0_has) {
      pointer_deleter< T >::destroy(x4_item);
    }
  }
  // TODO check
  auto_ptr(const auto_ptr& other) : x0_has(other.x0_has), x4_item(other.x4_item) {
    other.x0_has = false;
  }
  auto_ptr& operator=(const auto_ptr& other) {
    if (&other != this) {
      if (x0_has) {
        pointer_deleter< T >::destroy(x4_item);
      }
      x0_has = other.x0_has;
      x4_item = other.x4_item;
      other.x0_has = false;
    }
    return *this;
  }
  element_type* get() { return x4_item; }
  element_type* get() const { return x4_item; }
  bool owner() const { return x0_has; }
  element_type* operator->() const { return x4_item; }
  element_type& operator*() const { return *x4_item; }
  element_type* release() const {
    x0_has = false;
    return x4_item;
  }
  bool null() const { return x4_item == nullptr; }
  void reset() {
    x0_has = false;
    x4_item = nullptr;
  }
};
} // namespace rstl

#endif // _RSTL_AUTO_PTR
